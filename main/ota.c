#include <ctype.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_bootloader_desc.h"
#include "esp_flash.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

#include "ota.h"
#include "wifi_manager.h"

static const char *TAG = "ota";

/* Read this much from the socket at a time. Bigger buffers do not help: the
 * flash write is the slow half, and this one lives on the heap because the httpd
 * task only has 6 KiB of stack. */
#define RECV_CHUNK    4096
#define MAX_BOUNDARY  70                    /* RFC 2046 caps it here */
#define DELIM_MAX     (MAX_BOUNDARY + 4)    /* "\r\n--" + boundary */
#define PART_HDR_MAX  1024
#define WORK_SIZE     (RECV_CHUNK + PART_HDR_MAX)
#define RECV_RETRIES  3

/* esp_image_header_t + esp_image_segment_header_t come to 32 bytes, and the
 * esp_app_desc_t sits right behind them. Hold back all of it so the descriptor
 * can be both checked and reported before anything reaches flash. */
#define APP_DESC_OFFSET   32
#define IMAGE_HDR_LEN     (APP_DESC_OFFSET + sizeof(esp_app_desc_t))

/* ESPHome answers with exactly these, so anything already scripted against an
 * ESPHome device keeps working. The reason for a failure goes in a header
 * instead of the body, which has to stay byte for byte what it was. */
#define BODY_OK   "Update Successful!"
#define BODY_FAIL "Update Failed!"

#define CONFIRM_INTERVAL_MS 15000
#define CONFIRM_DEADLINE_US (5 * 60 * 1000000LL)

static bool s_pending_verify;

/* ------------------------------------------------------- small utilities */

static const uint8_t *find_bytes(const uint8_t *hay, size_t hn, const uint8_t *needle, size_t nn)
{
    if (nn == 0 || hn < nn) {
        return NULL;
    }
    for (size_t i = 0; i + nn <= hn; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nn) == 0) {
            return hay + i;
        }
    }
    return NULL;
}

/* Header names and parameter names are case-insensitive, and browsers do not
 * agree on the spelling of either. */
static const char *find_ci(const char *hay, size_t hn, const char *needle)
{
    size_t nn = strlen(needle);
    if (nn == 0 || hn < nn) {
        return NULL;
    }
    for (size_t i = 0; i + nn <= hn; i++) {
        size_t j = 0;
        while (j < nn && tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j])) {
            j++;
        }
        if (j == nn) {
            return hay + i;
        }
    }
    return NULL;
}

/* httpd_req_recv() reports a quiet socket as a timeout rather than an error. A
 * client uploading over a link shared with the 802.15.4 radio goes quiet often
 * enough that treating the first one as failure would abort real uploads. */
static int recv_some(httpd_req_t *req, char *buf, size_t len)
{
    for (int tries = 0; tries < RECV_RETRIES; tries++) {
        int n = httpd_req_recv(req, buf, len);
        if (n != HTTPD_SOCK_ERR_TIMEOUT) {
            return n;
        }
    }
    return HTTPD_SOCK_ERR_TIMEOUT;
}

/* Content-Type: multipart/form-data; boundary=----WebKitFormBoundaryAbc123 */
static bool parse_boundary(httpd_req_t *req, char *out, size_t out_size)
{
    char ct[192];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) != ESP_OK) {
        return false;
    }
    size_t ct_len = strlen(ct);
    if (!find_ci(ct, ct_len, "multipart/")) {
        return false;
    }
    const char *b = find_ci(ct, ct_len, "boundary=");
    if (!b) {
        return false;
    }
    b += strlen("boundary=");
    bool quoted = (*b == '"');
    if (quoted) {
        b++;
    }
    size_t n = 0;
    while (b[n] && !(quoted ? (b[n] == '"') : (b[n] == ';' || b[n] == ' '))) {
        n++;
    }
    if (n == 0 || n > MAX_BOUNDARY || n >= out_size) {
        return false;
    }
    memcpy(out, b, n);
    out[n] = '\0';
    return true;
}

/* ------------------------------------------------------ the multipart body */

bool ota_is_multipart(httpd_req_t *req)
{
    char boundary[MAX_BOUNDARY + 1];
    return parse_boundary(req, boundary, sizeof(boundary));
}

const char *ota_multipart_receive(httpd_req_t *req, ota_sink_fn sink, void *ctx)
{
    char boundary[MAX_BOUNDARY + 1];
    if (!parse_boundary(req, boundary, sizeof(boundary))) {
        return "expected a multipart/form-data body with a boundary";
    }

    /* A part ends at CRLF followed by the boundary. Nothing within dlen-1 bytes
     * of the end of a chunk can be written yet: that terminator is free to
     * straddle two reads, and writing half of it into flash corrupts the image
     * in a way that only shows up as a device that will not boot. */
    uint8_t delim[DELIM_MAX];
    memcpy(delim, "\r\n--", 4);
    size_t dlen = 4 + strlen(boundary);
    memcpy(delim + 4, boundary, dlen - 4);

    uint8_t *work = malloc(WORK_SIZE);
    if (!work) {
        return "out of memory";
    }

    enum { ST_HEADERS, ST_SKIP, ST_BODY, ST_DONE } state = ST_HEADERS;
    const char *why       = NULL;
    size_t      hold      = 0;
    int         remaining = req->content_len;

    while (state != ST_DONE) {
        size_t room = WORK_SIZE - hold;
        if (room > RECV_CHUNK) {
            room = RECV_CHUNK;
        }
        if (room == 0) {
            why = "part headers are too long";
            break;
        }
        if (remaining <= 0) {
            /* Which of these it is says whether the request carried a firmware
             * file at all, which is the useful half of the answer. */
            why = (state == ST_BODY) ? "body ended before the closing boundary"
                                     : "no firmware file in the request";
            break;
        }
        int n = recv_some(req, (char *)work + hold, room);
        if (n <= 0) {
            why = "upload was cut short";
            break;
        }
        remaining -= n;
        hold      += n;

        /* One received chunk can carry the end of the part headers and the
         * start of the file, so the states are re-entered without reading. */
        bool progressed = true;
        while (progressed && state != ST_DONE) {
            progressed = false;

            if (state == ST_HEADERS) {
                const uint8_t *end = find_bytes(work, hold, (const uint8_t *)"\r\n\r\n", 4);
                if (!end) {
                    if (hold > PART_HDR_MAX) {
                        why = "part headers are too long";
                        goto done;
                    }
                    continue;
                }
                size_t consumed = (size_t)(end - work) + 4;
                /* Only a part with a filename is a file. Anything else is an
                 * ordinary form field, so a client that sends other fields
                 * before the firmware still works. */
                bool is_file = find_ci((const char *)work, consumed, "filename=") != NULL;
                memmove(work, work + consumed, hold - consumed);
                hold -= consumed;
                state = is_file ? ST_BODY : ST_SKIP;
                progressed = true;

            } else if (state == ST_SKIP) {
                const uint8_t *end = find_bytes(work, hold, delim, dlen);
                if (!end) {
                    if (hold > dlen) {
                        /* Keep only what could still turn out to be the start
                         * of a delimiter. */
                        memmove(work, work + hold - (dlen - 1), dlen - 1);
                        hold = dlen - 1;
                    }
                    continue;
                }
                size_t consumed = (size_t)(end - work) + dlen;
                memmove(work, work + consumed, hold - consumed);
                hold -= consumed;
                state = ST_HEADERS;
                progressed = true;

            } else { /* ST_BODY */
                const uint8_t *end = find_bytes(work, hold, delim, dlen);
                size_t emit = end ? (size_t)(end - work)
                                  : (hold > dlen - 1 ? hold - (dlen - 1) : 0);
                if (emit > 0) {
                    why = sink(ctx, work, emit);
                    if (why) {
                        goto done;
                    }
                }
                if (end) {
                    state = ST_DONE;
                } else {
                    memmove(work, work + emit, hold - emit);
                    hold -= emit;
                }
            }
        }
    }

done:
    free(work);
    return why;
}

/* ------------------------------------------------------- the partition sink */

/* Erase ahead of the writes a block at a time. One erase of the whole
 * partition takes seconds with the flash cache off - and with it every task
 * that is not in IRAM, the Zigbee mainloop included. A block is a fraction of
 * that, spread over the upload. */
#define PART_ERASE_BLOCK 0x10000u

static const char *part_write(ota_part_sink_t *s, const uint8_t *data, size_t len)
{
    size_t limit = s->limit ? s->limit : s->part->size;
    if (s->written + len > limit) {
        return s->too_big ? s->too_big : "the file is larger than its partition";
    }
    while (s->erased < s->written + len) {
        size_t n = s->part->size - s->erased < PART_ERASE_BLOCK ? s->part->size - s->erased : PART_ERASE_BLOCK;
        if (esp_partition_erase_range(s->part, s->erased, n) != ESP_OK) {
            return "flash erase failed";
        }
        s->erased += n;
    }
    if (esp_partition_write(s->part, s->written, data, len) != ESP_OK) {
        return "flash write failed";
    }
    s->written += len;
    return NULL;
}

const char *ota_part_sink_write(void *ctx, const uint8_t *data, size_t len)
{
    ota_part_sink_t *s = ctx;
    if (s->head_len < s->head_want) {
        size_t take = s->head_want - s->head_len;
        if (take > len) {
            take = len;
        }
        memcpy(s->head + s->head_len, data, take);
        s->head_len += take;
        data += take;
        len  -= take;
        if (s->head_len < s->head_want) {
            return NULL;
        }
        const char *why = s->check ? s->check(s->check_ctx, s->head, s->head_len) : NULL;
        if (!why) {
            why = part_write(s, s->head, s->head_len);
        }
        if (why) {
            return why;
        }
    }
    return len ? part_write(s, data, len) : NULL;
}

/* ------------------------------------------------ the application image */

typedef struct {
    esp_ota_handle_t handle;
    bool             open;
    size_t           written;
    size_t           size;
    uint8_t          head[IMAGE_HDR_LEN];
    size_t           head_len;
    size_t           next_log;
} app_sink_t;

/* The first bytes decide whether this is an application image at all. Both the
 * app image and a merged firmware.factory.bin start with 0xE9 - the factory
 * image just starts with the bootloader's copy of that header - so only the app
 * descriptor tells them apart, and picking the wrong file is the mistake anyone
 * arriving from ESPHome will actually make. */
static const char *check_image_head(const uint8_t *head)
{
    if (head[0] != ESP_IMAGE_HEADER_MAGIC) {
        return "not an ESP32 application image";
    }
    uint32_t magic;
    memcpy(&magic, head + APP_DESC_OFFSET, sizeof(magic));
    if (magic != ESP_APP_DESC_MAGIC_WORD) {
        return "no app descriptor - this looks like firmware.factory.bin; use firmware.ota.bin";
    }
    return NULL;
}

/* >>> host-test region: idf-ver <<< */
/* The ESP-IDF release series in a descriptor's idf_ver, as major * 1000 +
 * minor: "v6.0.3" and "v6.0.3-dirty" are 6000, "v6.1-dev-12-gabc" is 6001. -1
 * for anything that does not start that way. `size` bounds a field that need
 * not end in a NUL. */
static int idf_series(const char *ver, size_t size)
{
    size_t i = 0;
    int    part[2];
    if (i < size && ver[i] == 'v') {
        i++;
    }
    for (int k = 0; k < 2; k++) {
        if (k > 0) {
            if (i >= size || ver[i] != '.') {
                return -1;
            }
            i++;
        }
        if (i >= size || ver[i] < '0' || ver[i] > '9') {
            return -1;
        }
        int n = 0;
        while (i < size && ver[i] >= '0' && ver[i] <= '9') {
            n = n * 10 + (ver[i++] - '0');
            if (n > 999) {
                return -1;
            }
        }
        part[k] = n;
    }
    return part[0] * 1000 + part[1];
}

/* Whether a bootloader from ESP-IDF `boot` would not start an image from ESP-IDF
 * `image`. Espressif's bootloaders start images from their own ESP-IDF or a
 * newer one, not from an older one. Bugfix releases only fix bugs, so the series
 * decides: a v6.0.3 bootloader takes a v6.0.2 image, a v6.1 one does not. A
 * version that cannot be read refuses nothing - a bootloader without a
 * description predates ESP-IDF 6, and starts newer images. */
static bool idf_image_too_old(const char *image, size_t image_size, const char *boot, size_t boot_size)
{
    const int img = idf_series(image, image_size);
    const int bl  = idf_series(boot, boot_size);
    return img >= 0 && bl >= 0 && img < bl;
}
/* >>> end host-test region: idf-ver <<< */

/* The bootloader's ESP-IDF, read once: only a USB flash or a conversion writes
 * a bootloader, and both restart afterwards. Empty without a description. Asked
 * from the httpd task only. */
static const char *boot_idf(void)
{
    static char ver[33];
    static bool read;
    if (!read) {
        esp_bootloader_desc_t desc;
        if (esp_ota_get_bootloader_description(NULL, &desc) == ESP_OK) {
            snprintf(ver, sizeof(ver), "%.32s", desc.idf_ver);
        }
        read = true;
    }
    return ver;
}

/* Why this board's bootloader would not start the image `desc` describes, or
 * NULL. Asked before anything is written, so a refused file leaves the board as
 * it was. The answer lasts until the next call. */
static const char *check_image_idf(const esp_app_desc_t *desc)
{
    static char why[160];
    const char *boot = boot_idf();
    if (!idf_image_too_old(desc->idf_ver, sizeof(desc->idf_ver), boot, strlen(boot))) {
        return NULL;
    }
    snprintf(why, sizeof(why), "this image is from ESP-IDF %.32s, older than this board's bootloader (%s), which would not start it",
             desc->idf_ver, boot);
    return why;
}

static const char *app_sink_write(void *ctx, const uint8_t *data, size_t len)
{
    app_sink_t *s = ctx;
    /* Hold the first IMAGE_HDR_LEN bytes back until they can be checked, so a
     * wrong file never reaches flash at all. */
    if (s->head_len < IMAGE_HDR_LEN) {
        size_t take = IMAGE_HDR_LEN - s->head_len;
        if (take > len) {
            take = len;
        }
        memcpy(s->head + s->head_len, data, take);
        s->head_len += take;
        data += take;
        len  -= take;
        if (s->head_len < IMAGE_HDR_LEN) {
            return NULL;
        }
        const char *why = check_image_head(s->head);
        if (why) {
            return why;
        }
        const esp_app_desc_t *desc = (const esp_app_desc_t *)(s->head + APP_DESC_OFFSET);
        ESP_LOGI(TAG, "Incoming image: %.32s %.32s, ESP-IDF %.32s", desc->project_name, desc->version,
                 desc->idf_ver);
        why = check_image_idf(desc);
        if (why) {
            return why;
        }

        if (esp_ota_write(s->handle, s->head, IMAGE_HDR_LEN) != ESP_OK) {
            return "flash write failed";
        }
        s->written = IMAGE_HDR_LEN;
    }
    if (len == 0) {
        return NULL;
    }
    if (s->written + len > s->size) {
        return "image is larger than the update partition";
    }
    if (esp_ota_write(s->handle, data, len) != ESP_OK) {
        return "flash write failed";
    }
    s->written += len;
    if (s->written >= s->next_log) {
        ESP_LOGI(TAG, "%u KiB written", (unsigned)(s->written / 1024));
        s->next_log = s->written + 128 * 1024;
    }
    return NULL;
}

/* ------------------------------------------------------------- the handler */

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t respond(httpd_req_t *req, const char *status, const char *why)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Connection", "close");
    if (why) {
        ESP_LOGE(TAG, "Update failed: %s", why);
        httpd_resp_set_hdr(req, "X-Ota-Error", why);
        return httpd_resp_sendstr(req, BODY_FAIL);
    }
    return httpd_resp_sendstr(req, BODY_OK);
}

esp_err_t ota_handle_update(httpd_req_t *req)
{
    if (!ota_is_multipart(req)) {
        return respond(req, "400 Bad Request", "expected a multipart/form-data body with a boundary");
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return respond(req, "500 Internal Server Error", "no free OTA partition");
    }

    /* The boot we are running from has just served an HTTP request, which is as
     * good as it is going to get. esp_ota_begin() refuses to start while the
     * running image is still on trial, so this has to happen first.
     *
     * Never from the updater. The call marks otadata's *active* entry valid,
     * whichever partition is running, and in the updater that entry can be the
     * app the bootloader has just rolled back. Blessing it there and then
     * rejecting the file would leave a broken image marked good: the updater
     * would start it, and nothing would ever roll it back again. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running && running->subtype != ESP_PARTITION_SUBTYPE_APP_FACTORY) {
        esp_ota_mark_app_valid_cancel_rollback();
        s_pending_verify = false;
    }

    ESP_LOGI(TAG, "Update starting into '%s' (%u KiB free), body %d bytes",
             part->label, (unsigned)(part->size / 1024), (int)req->content_len);

    app_sink_t sink = { .size = part->size, .next_log = 128 * 1024 };
    esp_err_t  err  = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &sink.handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return respond(req, "500 Internal Server Error", "could not open the update partition");
    }
    sink.open = true;

    const char *why = ota_multipart_receive(req, app_sink_write, &sink);
    if (!why && sink.head_len < IMAGE_HDR_LEN) {
        why = "the file is too short to be an application image";
    }
    if (why) {
        esp_ota_abort(sink.handle);
        return respond(req, "400 Bad Request", why);
    }

    err = esp_ota_end(sink.handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        return respond(req, "400 Bad Request",
                       err == ESP_ERR_OTA_VALIDATE_FAILED ? "image failed validation"
                                                          : "could not finish the update");
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        return respond(req, "500 Internal Server Error", "could not select the new image");
    }

    ESP_LOGW(TAG, "Update complete: %u bytes into '%s', rebooting",
             (unsigned)sink.written, part->label);
    esp_err_t sent = respond(req, "200 OK", NULL);
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return sent;
}

/* -------------------------------------------------- confirming a new image */

/* With rollback enabled a freshly written image boots once as PENDING_VERIFY:
 * if it reboots without confirming, the bootloader goes back to the slot it came
 * from. ESPHome confirms on a plain uptime timer, which it can afford because an
 * ESPHome device that cannot reach the network still has a serial console. Here
 * Wi-Fi *is* the only way back in, so having been on the network is the thing
 * worth waiting for - the stored one, or a new one the setup page checked from
 * the hotspot - with an uptime fallback so a box that has legitimately dropped
 * into the provisioning AP is not rolled back at the next power cut. */
static int64_t s_trial_started_us;

static bool boot_is_good(void)
{
    return wifi_manager_station_joined() ||
           esp_timer_get_time() - s_trial_started_us >= CONFIRM_DEADLINE_US;
}

/* confirm_task and the restart hook can both get here; the exchange lets only
 * the first of them write otadata. */
static void confirm_running_image(void)
{
    /* esp_ota_mark_app_valid_cancel_rollback() acts on the partition selected
     * for the *next* boot, not the running one. Between an upload finishing and
     * the reboot those differ, and confirming there would bless an image that
     * has never run. */
    if (esp_ota_get_running_partition() != esp_ota_get_boot_partition()) {
        return;
    }
    if (!__atomic_exchange_n(&s_pending_verify, false, __ATOMIC_ACQ_REL)) {
        return;
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "New image confirmed; rollback cancelled");
    } else {
        ESP_LOGE(TAG, "Could not confirm the new image: %s", esp_err_to_name(err));
    }
}

static void confirm_task(void *arg)
{
    (void)arg;
    while (s_pending_verify) {
        if (boot_is_good()) {
            confirm_running_image();
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIRM_INTERVAL_MS));
    }
    vTaskDelete(NULL);
}

/* A restart of an image still on trial is a failed update, whatever asked for
 * it, and two restarts come straight after the join boot_is_good() waits for:
 * the setup page's check restarts onto the network it stored 8 s later, and the
 * fallback hotspot restarts the moment the stored network answers. Both beat
 * confirm_task's next look. A box flashed over USB is always on trial (the
 * updater selects the app, which makes its first boot one), so setting it up
 * from the hotspot rolled it back to the updater. Hence the same test once more
 * on the way out. A crash does not come through esp_restart(), so an image that
 * panics is still rolled back.
 *
 * It runs on the stack of whichever task restarts, and a confirmation writes
 * flash and logs: the restart tasks that can get here take 3072 bytes. This
 * file's own reboot_task stays at 2048 because it only ever follows selecting
 * the updater, where confirm_running_image() returns before doing anything. */
static void confirm_on_restart(void)
{
    if (s_pending_verify && boot_is_good()) {
        confirm_running_image();
    }
}

void ota_init(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!run) {
        return;
    }
    ESP_LOGI(TAG, "Running from '%s' at 0x%06" PRIx32, run->label, (uint32_t)run->address);

    esp_ota_img_states_t state;
    /* No otadata entry for the running app is not an error and not a trial. It
     * is not what a serial flash leaves, though: with otadata blank the
     * bootloader starts the updater, whose selecting this app puts its first
     * boot on trial. */
    if (esp_ota_get_state_partition(run, &state) != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    s_pending_verify   = true;
    s_trial_started_us = esp_timer_get_time();
    ESP_LOGW(TAG, "This image is on trial - it is rolled back on the next boot unless Wi-Fi comes up");
    if (esp_register_shutdown_handler(confirm_on_restart) != ESP_OK) {
        ESP_LOGW(TAG, "No restart hook: restarting right after joining Wi-Fi still rolls this image back");
    }
    xTaskCreate(confirm_task, "ota_confirm", 3072, NULL, 3, NULL);
}

const char *ota_running_label(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    return run ? run->label : "unknown";
}

/* ------------------------------------------- handing over to the updater */

static const esp_partition_t *updater_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
}

/* Both read once and then kept, and both forgotten when the updater is
 * replaced. esp_http_server answers one request at a time, and these are only
 * asked from its task, so nothing else can be halfway through either. */
static char        s_updater_version[33];
static bool        s_updater_version_read;
static bool        s_updater_checked;
static const char *s_updater_problem;
static const uint8_t *s_own_table;
static size_t         s_own_table_len;

void ota_set_own_table(const uint8_t *table, size_t len)
{
    s_own_table     = table;
    s_own_table_len = len;
    s_updater_checked = false;
}

/* Whether `needle` occurs in the first `limit` bytes of a partition. Read in
 * pieces with an overlap, since mapping a whole app partition costs MMU pages
 * the Zigbee image may need. */
static bool partition_contains(const esp_partition_t *part, size_t limit, const uint8_t *needle, size_t nlen)
{
    enum { CHUNK = 4096 };
    uint8_t *buf = malloc(CHUNK + nlen);
    if (!buf || nlen == 0) {
        free(buf);
        return false;
    }
    bool   found = false;
    size_t have  = 0;
    for (size_t off = 0; off < limit && !found;) {
        size_t n = limit - off < CHUNK ? limit - off : CHUNK;
        if (esp_partition_read(part, off, buf + have, n) != ESP_OK) {
            break;
        }
        off  += n;
        have += n;
        for (size_t i = 0; i + nlen <= have; i++) {
            if (buf[i] == needle[0] && memcmp(buf + i, needle, nlen) == 0) {
                found = true;
                break;
            }
        }
        if (have >= nlen) {
            memmove(buf, buf + have - (nlen - 1), nlen - 1);
            have = nlen - 1;
        }
    }
    free(buf);
    return found;
}

const char *ota_updater_version(void)
{
    if (!s_updater_version_read) {
        const esp_partition_t *upd = updater_partition();
        esp_app_desc_t         desc;
        s_updater_version[0] = '\0';
        if (upd && esp_ota_get_partition_description(upd, &desc) == ESP_OK) {
            strlcpy(s_updater_version, desc.version, sizeof(s_updater_version));
        }
        s_updater_version_read = true;
    }
    return s_updater_version[0] ? s_updater_version : NULL;
}

/* Two things make an updater safe to restart into.
 *
 * It is whole: a verification - checksum and SHA-256 over the image - rather
 * than the descriptor ota_updater_version reads, because a write cut short
 * keeps its descriptor and only the hash at the end tells it is incomplete.
 *
 * And it knows this flash's partition table. The updater carries the table it
 * was built with, and one that finds any other at 0x8000 takes it for a foreign
 * firmware's: it starts nothing and takes no upload - a box that needs a cable.
 * So its embedded table must be the one on flash, or the one this coordinator
 * was built with (which it will write - see cv_relayout_check). An updater from
 * an older build on a box that already has the newer table is refused here. */
const char *ota_updater_problem(void)
{
    if (s_updater_checked) {
        return s_updater_problem;
    }
    const esp_partition_t *upd  = updater_partition();
    const char            *why  = NULL;
    esp_image_metadata_t   meta = { 0 };
    if (!upd) {
        why = "there is no updater partition";
    } else {
        const esp_partition_pos_t pos = { .offset = upd->address, .size = upd->size };
        if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) != ESP_OK) {
            why = "the updater image is damaged";
        }
    }
    if (!why) {
        enum { TABLE_OFFSET = 0x8000, TABLE_LEN = 0xC00 };
        uint8_t   *on_flash = malloc(TABLE_LEN);
        const bool read     = on_flash && esp_flash_read(esp_flash_default_chip, on_flash, TABLE_OFFSET, TABLE_LEN) == ESP_OK;
        const bool knows_flash = read && partition_contains(upd, meta.image_len, on_flash, TABLE_LEN);
        const bool knows_build = !knows_flash && s_own_table && s_own_table_len == TABLE_LEN &&
                                 partition_contains(upd, meta.image_len, s_own_table, s_own_table_len);
        free(on_flash);
        if (!read) {
            why = "the partition table could not be read";
        } else if (!knows_flash && !knows_build) {
            why = "the updater was built for another partition layout";
        }
    }
    if (why) {
        ESP_LOGE(TAG, "Updater: %s", why);
    }
    s_updater_problem = why;
    s_updater_checked = true;
    return why;
}

bool ota_updater_valid(void)
{
    return ota_updater_problem() == NULL;
}

static void set_updater_flag(uint8_t on)
{
    nvs_handle_t nvs;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    if (on) {
        nvs_set_u8(nvs, OTA_NVS_UPDATER_FLAG, 1);
    } else {
        nvs_erase_key(nvs, OTA_NVS_UPDATER_FLAG);
    }
    nvs_commit(nvs);
    nvs_close(nvs);
}

/* Leaves the flag and selects the updater for the next boot; the caller
 * restarts. Selecting the factory app erases otadata, and that is also what
 * makes a power cut in the updater harmless: no flag, no otadata entry and a
 * valid image are "start the coordinator" to it. NULL on success. */
static const char *request_updater(void)
{
    const esp_partition_t *upd = updater_partition();
    if (!upd) {
        return "this flash has no updater partition - flash it once over USB";
    }
    /* Everything that writes the coordinator runs in the updater. Restarting
     * into one that does not verify would leave the bootloader nothing to fall
     * back to but the coordinator's own trial - so a damaged updater is
     * replaced first, from here, while the coordinator still runs. */
    if (!ota_updater_valid()) {
        return "the updater cannot be trusted - upload this build's elixcon_light_hub_updater.bin first";
    }
    set_updater_flag(1);
    esp_err_t err = esp_ota_set_boot_partition(upd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not select the updater: %s", esp_err_to_name(err));
        set_updater_flag(0);
        return "could not select the updater";
    }
    ESP_LOGW(TAG, "Restarting into the updater %s", ota_updater_version() ? ota_updater_version() : "");
    return NULL;
}

esp_err_t ota_handle_prepare(httpd_req_t *req)
{
    const char *why = request_updater();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (why) {
        char body[160];
        snprintf(body, sizeof(body), "{\"error\":\"%s\"}", why);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, body);
    }
    httpd_resp_set_status(req, "202 Accepted");
    esp_err_t sent = httpd_resp_sendstr(req, "{\"ok\":true}");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return sent;
}

/* The upload arrived here, where it cannot be written. The body is read to the
 * end and thrown away before answering: a server that answers and closes with
 * the body still coming makes curl report a send failure rather than the 503,
 * and a 503 is what `curl --retry` retries. It costs the time one upload takes,
 * during which the box would be restarting anyway. */
esp_err_t ota_handle_handoff(httpd_req_t *req)
{
    const char *why = request_updater();
    if (why) {
        return respond(req, "500 Internal Server Error", why);
    }
    char buf[1024];
    int  left = req->content_len;
    while (left > 0) {
        int n = recv_some(req, buf, left < (int)sizeof(buf) ? (size_t)left : sizeof(buf));
        if (n <= 0) {
            break;
        }
        left -= n;
    }
    httpd_resp_set_hdr(req, "Retry-After", "15");
    esp_err_t sent = respond(req, "503 Service Unavailable",
                             "restarting into the updater - send the file again");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return sent;
}

bool ota_pending_verify(void)
{
    return s_pending_verify;
}

/* ------------------------------------------ replacing the updater itself */

/* The updater is otherwise only ever written over USB, because a half-written
 * one cannot be recovered without a cable - if something restarts into it. Here
 * nothing does: the coordinator keeps running while it is written, and
 * request_updater() refuses to hand over until the image verifies again. A
 * power cut in the middle costs one more upload, not a trip to the board.
 *
 * The factory partition cannot go through esp_ota_begin() (it only takes OTA
 * slots), so this is a raw partition write, verified as an image afterwards. */

#define UPDATER_PROJECT "elixcon_light_hub_updater"

static esp_err_t json_reply(httpd_req_t *req, const char *status, const char *why)
{
    char body[200];
    if (why) {
        ESP_LOGE(TAG, "Updater: %s", why);
        snprintf(body, sizeof(body), "{\"error\":\"%s\"}", why);
    } else {
        snprintf(body, sizeof(body), "{\"ok\":true}");
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static const char *check_updater_head(void *ctx, const uint8_t *head, size_t len)
{
    (void)ctx;
    (void)len;
    const char *why = check_image_head(head);
    if (why) {
        return why;
    }
    const esp_app_desc_t *desc = (const esp_app_desc_t *)(head + APP_DESC_OFFSET);
    if (strncmp(desc->project_name, UPDATER_PROJECT, sizeof(desc->project_name)) != 0) {
        return "this is not the updater - use build/updater/elixcon_light_hub_updater.bin";
    }
    why = check_image_idf(desc);
    if (why) {
        return why;
    }
    ESP_LOGW(TAG, "Writing the updater %.32s", desc->version);
    return NULL;
}

esp_err_t ota_handle_updater_image(httpd_req_t *req)
{
    const esp_partition_t *upd = updater_partition();
    if (!upd) {
        return json_reply(req, "409 Conflict", "this flash has no updater partition");
    }
    if (esp_ota_get_running_partition() == upd) {
        return json_reply(req, "409 Conflict", "the updater cannot replace itself");
    }
    if (!ota_is_multipart(req)) {
        return json_reply(req, "400 Bad Request", "expected a multipart/form-data body with a boundary");
    }
    uint8_t *head = malloc(IMAGE_HDR_LEN);
    if (!head) {
        return json_reply(req, "500 Internal Server Error", "out of memory");
    }
    ota_part_sink_t sink = {
        .part      = upd,
        .too_big   = "the file is larger than the updater partition",
        .head      = head,
        .head_want = IMAGE_HDR_LEN,
        .check     = check_updater_head,
    };
    const char *why = ota_multipart_receive(req, ota_part_sink_write, &sink);
    if (!why && sink.head_len < sink.head_want) {
        why = "the file is too short to be an application image";
    }
    free(head);

    /* Whatever happened, what is in the partition now is what gets verified. A
     * rejected head never erased anything, and verifies as before. */
    s_updater_checked      = false;
    s_updater_version_read = false;
    const char *problem    = ota_updater_problem();
    if (!why && problem) {
        why = problem;
    }
    if (why) {
        return json_reply(req, "400 Bad Request", why);
    }
    ESP_LOGW(TAG, "Updater %s written (%u bytes) and verified", ota_updater_version() ? ota_updater_version() : "",
             (unsigned)sink.written);
    return json_reply(req, "200 OK", NULL);
}

esp_err_t ota_handle_updater_run(httpd_req_t *req)
{
    const esp_partition_t *upd = updater_partition();
    if (!upd || esp_ota_get_running_partition() == upd) {
        return json_reply(req, "409 Conflict", "no updater to run");
    }
    if (!ota_updater_valid()) {
        return json_reply(req, "409 Conflict", ota_updater_problem());
    }
    /* No flag: the updater does what it does at every boot - bring the
     * partition table up to date if it is this project's older one - and then
     * starts the coordinator straight back up. */
    set_updater_flag(0);
    esp_err_t err = esp_ota_set_boot_partition(upd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not select the updater: %s", esp_err_to_name(err));
        return json_reply(req, "500 Internal Server Error", "could not select the updater");
    }
    ESP_LOGW(TAG, "Restarting through the updater %s", ota_updater_version() ? ota_updater_version() : "");
    esp_err_t sent = json_reply(req, "202 Accepted", NULL);
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return sent;
}
