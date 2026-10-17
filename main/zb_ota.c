/* See zb_ota.h. */
#include "zb_ota.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "nvs.h"

#include "ota.h"
#include "state_rev.h"

static const char *TAG = "zb_ota";

/* partitions.csv: type 0x40 (application-defined), subtype 0. */
#define PART_TYPE    0x40
#define PART_SUBTYPE 0x00
#define PART_LABEL   "zb_ota"

/* The image is believed only while both match what is in flash. They are
 * erased before the first byte of a new upload, so a write cut short by a power
 * cut comes back as no image rather than as a torn one. */
#define NVS_NAMESPACE "zbc"
#define NVS_KEY_LEN   "zota_len"
#define NVS_KEY_CRC   "zota_crc"

/* A device that has asked for no block in this long has given up, or lost its
 * parent. Only a label in the status: nothing is cancelled, and a device that
 * resumes carries on. It does stop a transfer from blocking a new upload. */
#define STALL_US (120 * 1000000LL)

typedef enum {
    XFER_NONE = 0,
    XFER_SENDING,
    XFER_DONE,
} xfer_phase_t;

typedef struct {
    bool            used;
    uint8_t         ieee[ZBC_IEEE_LEN];
    zb_ota_device_t d;
} ident_t;

static portMUX_TYPE           s_mux = portMUX_INITIALIZER_UNLOCKED;
static const esp_partition_t *s_part;

/* The image. s_ptr/s_len stay valid from mapping to unmapping; the stack only
 * gets them through zb_ota_image, which refuses while s_writing. */
static bool                        s_have;
static bool                        s_writing;
static bool                        s_offered;
static const void                 *s_ptr;
static uint32_t                    s_len;
static uint32_t                    s_crc;
static esp_partition_mmap_handle_t s_map;
static zbc_ota_hdr_t               s_hdr;
static char                        s_error[96];

static bool    s_has_target;
static uint8_t s_target[ZBC_IEEE_LEN];

typedef struct {
    xfer_phase_t phase;
    uint8_t      ieee[ZBC_IEEE_LEN];
    uint16_t     short_addr;
    uint8_t      ep;
    uint32_t     from_version;
    uint32_t     to_version;
    uint32_t     offset;
    uint32_t     size;
    int64_t      started_us;
    int64_t      progress_us;
    uint8_t      status;
    uint8_t      next_log_pct;
} xfer_t;
static xfer_t s_xfer;

/* The last query the stack asked us about, with what it was told: the one
 * place to see why a device that should be updating is not. */
typedef struct {
    bool          seen;
    bool          ieee_known;
    uint8_t       ieee[ZBC_IEEE_LEN];
    uint16_t      short_addr;
    zbc_ota_qni_t q;
    int64_t       at_us;
    const char   *answer;
} last_query_t;
static last_query_t s_last;
static uint32_t s_queries;
static uint32_t s_refused;

static ident_t s_ident[CONFIG_ZBC_MAX_DEVICES];

/* >>> host-test region: zota-wake <<< */
/* A notify for a device that sleeps, held until it is next heard from.
 *
 * A unicast to a sleepy device waits at its parent for about 7.7 s, and a
 * remote polls for it only when it has something of its own to send - so a
 * notify sent when somebody clicks in the browser has expired before they have
 * found the remote. It is sent again when a frame from the device arrives,
 * which is the moment it is awake and about to poll: at most once per
 * ZOTA_WAKE_GAP_US (a turning wheel is dozens of frames), until the device asks
 * or ZOTA_WAKE_ARM_US has passed. Pure: the caller holds the lock and the time. */
#define ZOTA_WAKE_ROWS   4
#define ZOTA_WAKE_ARM_US (10 * 60 * 1000000LL)
#define ZOTA_WAKE_GAP_US (10 * 1000000LL)

typedef struct {
    bool    used;
    bool    due;              /* heard: the work task is to send a notify */
    uint8_t ieee[ZBC_IEEE_LEN];
    int64_t until_us;
    int64_t last_sent_us;
    uint8_t notifies;         /* sent because it was heard, for the status */
} zota_wake_t;

static zota_wake_t *zota_wake_find(zota_wake_t *rows, size_t n, const uint8_t *ieee)
{
    for (size_t i = 0; i < n; i++) {
        if (rows[i].used && memcmp(rows[i].ieee, ieee, ZBC_IEEE_LEN) == 0) {
            return &rows[i];
        }
    }
    return NULL;
}

/* A notify was just sent directly: count it as the last one, so hearing the
 * device a moment later does not send a second on its heels. */
static void zota_wake_arm(zota_wake_t *rows, size_t n, const uint8_t *ieee, int64_t now)
{
    zota_wake_t *r = zota_wake_find(rows, n, ieee);
    for (size_t i = 0; !r && i < n; i++) {
        if (!rows[i].used || rows[i].until_us <= now) {
            r = &rows[i];
        }
    }
    if (!r) {
        /* All taken and running: the one closest to giving up anyway. */
        r = &rows[0];
        for (size_t i = 1; i < n; i++) {
            if (rows[i].until_us < r->until_us) {
                r = &rows[i];
            }
        }
    }
    memcpy(r->ieee, ieee, ZBC_IEEE_LEN);
    r->used         = true;
    r->due          = false;
    r->until_us     = now + ZOTA_WAKE_ARM_US;
    r->last_sent_us = now;
    r->notifies     = 0;
}

/* A frame from the device. True when a notify is now due. */
static bool zota_wake_heard(zota_wake_t *rows, size_t n, const uint8_t *ieee, int64_t now)
{
    zota_wake_t *r = zota_wake_find(rows, n, ieee);
    if (!r) {
        return false;
    }
    if (now >= r->until_us) {
        r->used = false;
        return false;
    }
    if (r->due || now - r->last_sent_us < ZOTA_WAKE_GAP_US) {
        return false;
    }
    r->due          = true;
    r->last_sent_us = now;
    if (r->notifies < UINT8_MAX) {
        r->notifies++;
    }
    return true;
}

/* One device a notify is due for, taken off the list of due ones. */
static bool zota_wake_take(zota_wake_t *rows, size_t n, uint8_t *ieee_out)
{
    for (size_t i = 0; i < n; i++) {
        if (rows[i].used && rows[i].due) {
            rows[i].due = false;
            memcpy(ieee_out, rows[i].ieee, ZBC_IEEE_LEN);
            return true;
        }
    }
    return false;
}

/* The device asked: what the notify was for has happened. */
static void zota_wake_clear(zota_wake_t *rows, size_t n, const uint8_t *ieee)
{
    zota_wake_t *r = zota_wake_find(rows, n, ieee);
    if (r) {
        r->used = false;
        r->due  = false;
    }
}
/* >>> end host-test region: zota-wake <<< */

static zota_wake_t s_wake[ZOTA_WAKE_ROWS];
static uint32_t    s_wake_sent;
static uint32_t    s_wake_failed;

/* Delivery of unicast notifies, direct and on wake alike. */
typedef struct {
    bool     seen;
    uint16_t short_addr;
    uint8_t  status;
    int64_t  at_us;
} notify_cnf_t;
static uint32_t     s_notify_delivered;
static uint32_t     s_notify_undelivered;
static notify_cnf_t s_notify_last;

/* ------------------------------------------------------------------ flash */

static bool load_keys(uint32_t *len, uint32_t *crc)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    const bool ok = nvs_get_u32(nvs, NVS_KEY_LEN, len) == ESP_OK && nvs_get_u32(nvs, NVS_KEY_CRC, crc) == ESP_OK;
    nvs_close(nvs);
    return ok;
}

static bool store_keys(uint32_t len, uint32_t crc)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    const bool ok = nvs_set_u32(nvs, NVS_KEY_LEN, len) == ESP_OK && nvs_set_u32(nvs, NVS_KEY_CRC, crc) == ESP_OK &&
                    nvs_commit(nvs) == ESP_OK;
    nvs_close(nvs);
    return ok;
}

static void erase_keys(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_erase_key(nvs, NVS_KEY_LEN);
    nvs_erase_key(nvs, NVS_KEY_CRC);
    nvs_commit(nvs);
    nvs_close(nvs);
}

/* Takes the image away from zb_ota_image and unmaps it. The caller has made
 * sure the stack no longer holds the pointer. */
static void drop_image(void)
{
    portENTER_CRITICAL(&s_mux);
    esp_partition_mmap_handle_t map = s_map;
    const bool                  had = s_have;
    s_have                          = false;
    s_offered                       = false;
    s_ptr                           = NULL;
    s_len                           = 0;
    s_map                           = 0;
    portEXIT_CRITICAL(&s_mux);
    if (had) {
        esp_partition_munmap(map);
    }
}

/* Maps `len` bytes, checks the header against that length and computes the
 * CRC. On success the image is the one zb_ota_image hands out. */
static const char *map_image(uint32_t len, uint32_t *crc_out)
{
    if (len < ZBC_OTA_HDR_MIN || len > s_part->size) {
        return "the stored length does not fit the partition";
    }
    const void                 *ptr;
    esp_partition_mmap_handle_t map;
    esp_err_t                   err = esp_partition_mmap(s_part, 0, len, ESP_PARTITION_MMAP_DATA, &ptr, &map);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_partition_mmap(%" PRIu32 "): %s", len, esp_err_to_name(err));
        return "could not map the image into memory";
    }
    zbc_ota_hdr_t hdr;
    const size_t  have = len < ZBC_OTA_HDR_MAX ? len : ZBC_OTA_HDR_MAX;
    const char   *why  = zbc_ota_header_check(ptr, have, len, &hdr);
    if (why) {
        esp_partition_munmap(map);
        return why;
    }
    const uint32_t crc = esp_rom_crc32_le(0, ptr, len);

    portENTER_CRITICAL(&s_mux);
    s_have    = true;
    s_offered = false;
    s_ptr     = ptr;
    s_len     = len;
    s_crc     = crc;
    s_map     = map;
    s_hdr     = hdr;
    portEXIT_CRITICAL(&s_mux);
    *crc_out = crc;
    return NULL;
}

static void set_error(const char *why)
{
    char msg[sizeof(s_error)];
    strlcpy(msg, why ? why : "", sizeof(msg));
    portENTER_CRITICAL(&s_mux);
    memcpy(s_error, msg, sizeof(s_error));
    portEXIT_CRITICAL(&s_mux);
}

bool zb_ota_has_partition(void)
{
    return s_part != NULL;
}

esp_err_t zb_ota_init(void)
{
    s_part = esp_partition_find_first(PART_TYPE, PART_SUBTYPE, PART_LABEL);
    if (!s_part) {
        ESP_LOGW(TAG, "No '%s' partition: the flash is on the older layout, so Zigbee devices cannot be updated",
                 PART_LABEL);
        return ESP_OK;
    }
    uint32_t len, stored_crc, crc = 0;
    if (!load_keys(&len, &stored_crc)) {
        ESP_LOGI(TAG, "No Zigbee device image stored (%u KB of room)", (unsigned)(s_part->size / 1024));
        return ESP_OK;
    }
    const char *why = map_image(len, &crc);
    if (!why && crc != stored_crc) {
        drop_image();
        why = "its CRC no longer matches - the upload was cut short";
    }
    if (why) {
        ESP_LOGW(TAG, "Stored Zigbee device image dropped: %s", why);
        char msg[sizeof(s_error)];
        snprintf(msg, sizeof(msg), "the stored image was dropped: %s", why);
        set_error(msg);
        erase_keys();
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Zigbee device image: manufacturer 0x%04x, type 0x%04x, version 0x%08" PRIx32 ", %" PRIu32
                  " bytes, '%s'",
             s_hdr.manuf_code, s_hdr.image_type, s_hdr.file_version, s_len, s_hdr.hdr_string);
    return ESP_OK;
}

bool zb_ota_image(const void **ptr, uint32_t *len)
{
    portENTER_CRITICAL(&s_mux);
    const bool ok = s_have && !s_writing;
    if (ok) {
        *ptr = s_ptr;
        *len = s_len;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

void zb_ota_set_offered(bool offered)
{
    portENTER_CRITICAL(&s_mux);
    const bool changed = s_offered != offered;
    s_offered          = offered;
    portEXIT_CRITICAL(&s_mux);
    if (changed) {
        zbc_state_bump();
    }
}

static bool xfer_busy_locked(int64_t now)
{
    return s_xfer.phase == XFER_SENDING && now - s_xfer.progress_us < STALL_US;
}

bool zb_ota_begin_write(const char **why)
{
    const int64_t now = esp_timer_get_time();
    const char   *no  = NULL;
    portENTER_CRITICAL(&s_mux);
    if (!s_part) {
        no = "this flash has no zb_ota partition yet - see Settings";
    } else if (s_writing) {
        no = "another upload is being written";
    } else if (xfer_busy_locked(now)) {
        no = "a device is downloading the stored image right now";
    } else {
        s_writing = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (why) {
        *why = no;
    }
    return no == NULL;
}

void zb_ota_end_write(void)
{
    portENTER_CRITICAL(&s_mux);
    s_writing = false;
    portEXIT_CRITICAL(&s_mux);
    zbc_state_bump();
}

void zb_ota_forget(void)
{
    erase_keys();
    drop_image();
    portENTER_CRITICAL(&s_mux);
    s_error[0] = '\0';
    if (s_xfer.phase != XFER_SENDING) {
        s_xfer.phase = XFER_NONE;
    }
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "Zigbee device image removed");
    zbc_state_bump();
}

/* ----------------------------------------------------------------- upload */

typedef struct {
    ota_part_sink_t *sink;
    zbc_ota_hdr_t    hdr;
    char             reason[96];
} upload_t;

static const char *upload_check(void *ctx, const uint8_t *head, size_t len)
{
    upload_t   *u   = ctx;
    const char *why = zbc_ota_header_check(head, len, 0, &u->hdr);
    if (why) {
        return why;
    }
    if (u->hdr.total > s_part->size) {
        snprintf(u->reason, sizeof(u->reason), "the image is %u KB, and the coordinator holds up to %u KB",
                 (unsigned)((u->hdr.total + 1023) / 1024), (unsigned)(s_part->size / 1024));
        return u->reason;
    }
    /* From here on the header decides the length: a longer file is refused
     * rather than served with its tail cut off. */
    u->sink->limit = u->hdr.total;
    snprintf(u->reason, sizeof(u->reason), "the file is longer than its OTA header says (%u bytes)",
             (unsigned)u->hdr.total);
    u->sink->too_big = u->reason;
    ESP_LOGI(TAG, "Incoming Zigbee image: manufacturer 0x%04x, type 0x%04x, version 0x%08" PRIx32 ", %" PRIu32
                  " bytes, '%s'",
             u->hdr.manuf_code, u->hdr.image_type, u->hdr.file_version, u->hdr.total, u->hdr.hdr_string);
    return NULL;
}

static esp_err_t reply(httpd_req_t *req, const char *status, cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

esp_err_t zb_ota_handle_upload(httpd_req_t *req)
{
    if (!ota_is_multipart(req)) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "error", "expected a multipart/form-data body with a boundary");
        return reply(req, "400 Bad Request", root);
    }

    /* Whatever was stored stops being believed before the first erase. */
    erase_keys();
    drop_image();
    portENTER_CRITICAL(&s_mux);
    s_error[0] = '\0';
    if (s_xfer.phase != XFER_SENDING) {
        s_xfer.phase = XFER_NONE;
    }
    portEXIT_CRITICAL(&s_mux);
    zbc_state_bump();

    uint8_t         head[ZBC_OTA_HDR_MAX];
    upload_t        u    = { 0 };
    ota_part_sink_t sink = {
        .part      = s_part,
        .head      = head,
        .head_want = sizeof(head),
        .check     = upload_check,
        .check_ctx = &u,
    };
    u.sink = &sink;

    const int64_t started = esp_timer_get_time();
    const char   *why     = ota_multipart_receive(req, ota_part_sink_write, &sink);
    if (!why && sink.head_len < sink.head_want) {
        why = "too short to be a Zigbee OTA file";
    }
    if (!why && sink.written != u.hdr.total) {
        why = "the file is shorter than its OTA header says";
    }
    uint32_t crc = 0;
    if (!why) {
        why = map_image(u.hdr.total, &crc);
    }
    if (!why && !store_keys(u.hdr.total, crc)) {
        drop_image();
        why = "could not record the image in NVS";
    }
    if (why) {
        ESP_LOGE(TAG, "Zigbee image upload refused: %s", why);
        set_error(why);
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "error", why);
        return reply(req, "400 Bad Request", root);
    }
    ESP_LOGI(TAG, "Zigbee image stored: %" PRIu32 " bytes in %lld ms, CRC 0x%08" PRIx32, u.hdr.total,
             (long long)((esp_timer_get_time() - started) / 1000), crc);

    /* The stored image as it was checked, not zb_ota_status_json(): that would
     * still say "writing", since the caller ends the write after this reply. */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON *img = cJSON_AddObjectToObject(root, "image");
    cJSON_AddNumberToObject(img, "manufacturer_code", u.hdr.manuf_code);
    cJSON_AddNumberToObject(img, "image_type", u.hdr.image_type);
    cJSON_AddNumberToObject(img, "file_version", u.hdr.file_version);
    cJSON_AddNumberToObject(img, "size", u.hdr.total);
    cJSON_AddStringToObject(img, "header", u.hdr.hdr_string);
    return reply(req, "200 OK", root);
}

/* ----------------------------------------------------------------- target */

void zb_ota_set_target(const uint8_t *ieee)
{
    portENTER_CRITICAL(&s_mux);
    s_has_target = ieee != NULL;
    if (ieee) {
        memcpy(s_target, ieee, ZBC_IEEE_LEN);
        /* A new request starts a new record, unless that very transfer is
         * still running - notifying it again must not hide its progress. */
        if (!(s_xfer.phase == XFER_SENDING && memcmp(s_xfer.ieee, ieee, ZBC_IEEE_LEN) == 0)) {
            s_xfer.phase = XFER_NONE;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    zbc_state_bump();
}

bool zb_ota_target(uint8_t ieee_out[ZBC_IEEE_LEN])
{
    portENTER_CRITICAL(&s_mux);
    const bool has = s_has_target;
    if (has) {
        memcpy(ieee_out, s_target, ZBC_IEEE_LEN);
    }
    portEXIT_CRITICAL(&s_mux);
    return has;
}

/* --------------------------------------------------------------- identity */

/* The row for a device, a free one, or the one heard from longest ago. Under
 * the lock. */
static ident_t *ident_slot_locked(const uint8_t ieee[ZBC_IEEE_LEN], bool create)
{
    ident_t *free_row = NULL;
    ident_t *oldest   = NULL;
    for (size_t i = 0; i < sizeof(s_ident) / sizeof(s_ident[0]); i++) {
        ident_t *r = &s_ident[i];
        if (r->used && memcmp(r->ieee, ieee, ZBC_IEEE_LEN) == 0) {
            return r;
        }
        if (!r->used && !free_row) {
            free_row = r;
        }
        if (r->used && (!oldest || r->d.queried_us < oldest->d.queried_us)) {
            oldest = r;
        }
    }
    if (!create) {
        return NULL;
    }
    ident_t *r = free_row ? free_row : oldest;
    if (r) {
        memset(r, 0, sizeof(*r));
        r->used = true;
        memcpy(r->ieee, ieee, ZBC_IEEE_LEN);
    }
    return r;
}

void zb_ota_note_client(uint16_t short_addr, uint8_t ep)
{
    uint8_t ieee[ZBC_IEEE_LEN];
    if (!device_db_ieee_by_short(short_addr, ieee)) {
        return;
    }
    bool changed = false;
    portENTER_CRITICAL(&s_mux);
    ident_t *r = ident_slot_locked(ieee, true);
    if (r && r->d.ep != ep) {
        r->d.ep = ep;
        changed = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (changed) {
        zbc_state_bump();
    }
}

void zb_ota_note_query(uint16_t short_addr, uint8_t ep, const zbc_ota_qni_t *q)
{
    if (!q) {
        return;
    }
    uint8_t    ieee[ZBC_IEEE_LEN];
    const bool known   = device_db_ieee_by_short(short_addr, ieee);
    bool       changed = false;
    portENTER_CRITICAL(&s_mux);
    /* Every query, whether or not the stack asks zb_ota_admit about it: the
     * stack answers a query for another manufacturer or image type by itself,
     * and without this the last one visible would be an older, admitted one. If
     * the stack does call zb_ota_admit, its answer replaces this one. */
    s_queries++;
    s_last.seen       = true;
    s_last.ieee_known = known;
    if (known) {
        memcpy(s_last.ieee, ieee, ZBC_IEEE_LEN);
    }
    s_last.short_addr = short_addr;
    s_last.q          = *q;
    s_last.at_us      = esp_timer_get_time();
    s_last.answer     = (!s_have || s_writing) ? "no image stored"
                        : (q->manuf_code != s_hdr.manuf_code || q->image_type != s_hdr.image_type)
                            ? "the stored image is for another manufacturer or model"
                            : "the stored image matches; the stack did not ask whether to offer it";
    if (known) {
        zota_wake_clear(s_wake, ZOTA_WAKE_ROWS, ieee);
    }
    ident_t *r = known ? ident_slot_locked(ieee, true) : NULL;
    if (r) {
        const zb_ota_device_t was = r->d;
        r->d.ep                   = ep;
        r->d.manuf_known          = true;
        r->d.manuf_code           = q->manuf_code;
        r->d.type_known           = true;
        r->d.image_type           = q->image_type;
        r->d.version_known        = true;
        r->d.file_version         = q->file_version;
        if (q->hw_present) {
            r->d.hw_known   = true;
            r->d.hw_version = q->hw_version;
        }
        r->d.queried_us = esp_timer_get_time();
        /* A fresh answer is news to the page even when nothing in it changed:
         * "Check for updates" waits for it, and asking a remote twice within
         * half a minute must show the second answer too. Devices ask on their
         * own about once a day, so this costs nothing; the two seconds only
         * fold a query the device repeats at once into one poll. */
        changed = was.queried_us == 0 || r->d.queried_us - was.queried_us > 2 * 1000000LL || was.ep != r->d.ep || !was.manuf_known || was.manuf_code != r->d.manuf_code ||
                  !was.type_known || was.image_type != r->d.image_type || !was.version_known ||
                  was.file_version != r->d.file_version || was.hw_known != r->d.hw_known ||
                  was.hw_version != r->d.hw_version;
    }
    portEXIT_CRITICAL(&s_mux);
    if (changed) {
        zbc_state_bump();
    }
}

void zb_ota_note_attr(uint16_t short_addr, uint8_t ep, uint16_t attr_id, uint32_t value)
{
    uint8_t ieee[ZBC_IEEE_LEN];
    if ((attr_id != 0x0002 && attr_id != 0x0007 && attr_id != 0x0008) || !device_db_ieee_by_short(short_addr, ieee)) {
        return;
    }
    /* The "not known" values the attributes default to. A Hue LST002 answers
     * ImageTypeID 0xFFFF here while its own Query Next Image says 0x010F, and
     * recording it would hide the image type every match needs. */
    if ((attr_id == 0x0002 && value == 0xFFFFFFFFu) || (attr_id != 0x0002 && value == 0xFFFFu)) {
        return;
    }
    bool changed = false;
    portENTER_CRITICAL(&s_mux);
    ident_t *r = ident_slot_locked(ieee, true);
    if (r) {
        const zb_ota_device_t was = r->d;
        if (r->d.ep == 0) {
            r->d.ep = ep;
        }
        if (attr_id == 0x0002) {
            r->d.version_known = true;
            r->d.file_version  = value;
        } else if (attr_id == 0x0007) {
            r->d.manuf_known = true;
            r->d.manuf_code  = (uint16_t)value;
        } else {
            r->d.type_known = true;
            r->d.image_type = (uint16_t)value;
        }
        changed = memcmp(&was, &r->d, sizeof(was)) != 0;
    }
    portEXIT_CRITICAL(&s_mux);
    if (changed) {
        zbc_state_bump();
    }
}

void zb_ota_wake_arm(const uint8_t ieee[ZBC_IEEE_LEN])
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    zota_wake_arm(s_wake, ZOTA_WAKE_ROWS, ieee, now);
    portEXIT_CRITICAL(&s_mux);
}

bool zb_ota_wake_heard(uint16_t short_addr)
{
    /* Every frame from every device comes through here, and nearly always
     * nothing is armed: answer that without a registry lookup. */
    bool armed = false;
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < ZOTA_WAKE_ROWS; i++) {
        armed |= s_wake[i].used;
    }
    portEXIT_CRITICAL(&s_mux);
    uint8_t ieee[ZBC_IEEE_LEN];
    if (!armed || !device_db_ieee_by_short(short_addr, ieee)) {
        return false;
    }
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    const bool due = zota_wake_heard(s_wake, ZOTA_WAKE_ROWS, ieee, now);
    portEXIT_CRITICAL(&s_mux);
    return due;
}

bool zb_ota_wake_take(uint8_t ieee_out[ZBC_IEEE_LEN])
{
    portENTER_CRITICAL(&s_mux);
    const bool got = zota_wake_take(s_wake, ZOTA_WAKE_ROWS, ieee_out);
    portEXIT_CRITICAL(&s_mux);
    return got;
}

void zb_ota_notify_confirmed(uint16_t short_addr, uint8_t status)
{
    portENTER_CRITICAL(&s_mux);
    if (status == 0) {
        s_notify_delivered++;
    } else {
        s_notify_undelivered++;
    }
    s_notify_last = (notify_cnf_t){ .seen = true, .short_addr = short_addr, .status = status,
                                    .at_us = esp_timer_get_time() };
    portEXIT_CRITICAL(&s_mux);
}

void zb_ota_wake_sent(bool ok)
{
    portENTER_CRITICAL(&s_mux);
    if (ok) {
        s_wake_sent++;
    } else {
        s_wake_failed++;
    }
    portEXIT_CRITICAL(&s_mux);
}

bool zb_ota_device_info(const uint8_t ieee[ZBC_IEEE_LEN], zb_ota_device_t *out)
{
    portENTER_CRITICAL(&s_mux);
    const ident_t *r = ident_slot_locked(ieee, false);
    if (r) {
        *out = r->d;
    }
    portEXIT_CRITICAL(&s_mux);
    return r != NULL;
}

/* --------------------------------------------------------------- transfer */

bool zb_ota_admit(uint16_t short_addr, uint8_t ep, const zbc_ota_qni_t *q)
{
    uint8_t       ieee[ZBC_IEEE_LEN];
    const bool    known = device_db_ieee_by_short(short_addr, ieee);
    const int64_t now   = esp_timer_get_time();
    const char   *no    = NULL;

    portENTER_CRITICAL(&s_mux);
    /* The hardware version as the raw frame had it (zb_ota_note_query). */
    const ident_t *who = known ? ident_slot_locked(ieee, false) : NULL;
    const bool     hw_known = who && who->d.hw_known;
    const uint16_t hw       = hw_known ? who->d.hw_version : 0;
    if (!s_have || s_writing) {
        no = "no image stored";
    } else if (!s_offered) {
        no = "the image is not attached to the stack";
    } else if (!s_has_target) {
        no = "no device has been chosen for the image";
    } else if (!known || memcmp(ieee, s_target, ZBC_IEEE_LEN) != 0) {
        no = "the image is for another device";
    } else if (q->manuf_code != s_hdr.manuf_code || q->image_type != s_hdr.image_type) {
        no = "the image is for another manufacturer or model";
    } else if (q->file_version == s_hdr.file_version) {
        no = "the device already runs this version";
    } else if (s_hdr.hw_range && hw_known && (hw < s_hdr.hw_min || hw > s_hdr.hw_max)) {
        no = "the image is for other hardware versions";
    }
    if (no) {
        s_refused++;
    } else {
        memset(&s_xfer, 0, sizeof(s_xfer));
        s_xfer.phase        = XFER_SENDING;
        memcpy(s_xfer.ieee, ieee, ZBC_IEEE_LEN);
        s_xfer.short_addr   = short_addr;
        s_xfer.ep           = ep;
        s_xfer.from_version = q->file_version;
        s_xfer.to_version   = s_hdr.file_version;
        s_xfer.size         = s_len;
        s_xfer.started_us   = now;
        s_xfer.progress_us  = now;
        s_xfer.status       = 0xFF;
        s_xfer.next_log_pct = 10;
    }
    s_last.seen       = true;
    s_last.ieee_known = known;
    if (known) {
        memcpy(s_last.ieee, ieee, ZBC_IEEE_LEN);
    }
    s_last.short_addr = short_addr;
    s_last.q          = *q;
    s_last.at_us      = now;
    s_last.answer     = no;
    portEXIT_CRITICAL(&s_mux);

    if (no) {
        ESP_LOGI(TAG, "0x%04x asked for firmware (manufacturer 0x%04x, type 0x%04x, version 0x%08" PRIx32 "): %s",
                 short_addr, q->manuf_code, q->image_type, q->file_version, no);
    } else {
        ESP_LOGW(TAG, "0x%04x starts downloading version 0x%08" PRIx32 " over 0x%08" PRIx32 " (%" PRIu32 " bytes)",
                 short_addr, s_hdr.file_version, q->file_version, s_len);
        zbc_state_bump();
    }
    return no == NULL;
}

bool zb_ota_note_block(uint16_t short_addr, uint32_t offset)
{
    uint8_t pct = 0;
    portENTER_CRITICAL(&s_mux);
    /* Blocks only for the device admitted, and only while the image it was
     * admitted to is still the one attached. */
    const bool ours = s_xfer.phase == XFER_SENDING && s_xfer.short_addr == short_addr && s_have && !s_writing;
    if (ours) {
        s_xfer.offset      = offset;
        s_xfer.progress_us = esp_timer_get_time();
        const uint32_t now_pct = s_xfer.size ? (uint32_t)((uint64_t)offset * 100 / s_xfer.size) : 0;
        if (now_pct >= s_xfer.next_log_pct) {
            pct                 = (uint8_t)now_pct;
            s_xfer.next_log_pct = (uint8_t)((now_pct / 10 + 1) * 10);
        }
    }
    portEXIT_CRITICAL(&s_mux);
    if (pct) {
        ESP_LOGI(TAG, "0x%04x has %u %% of the image", short_addr, pct);
    }
    return ours;
}

bool zb_ota_note_end(uint16_t short_addr, uint8_t status, uint32_t *from_version)
{
    portENTER_CRITICAL(&s_mux);
    const bool ours = s_xfer.phase == XFER_SENDING && s_xfer.short_addr == short_addr;
    if (ours && from_version) {
        *from_version = s_xfer.from_version;
    }
    if (ours) {
        s_xfer.phase       = XFER_DONE;
        s_xfer.status      = status;
        s_xfer.progress_us = esp_timer_get_time();
        if (status == 0x00) {
            /* It has the image and says it will switch to it. Serving it again
             * after the device restarts would be for nothing - it will ask with
             * the new version - but a device that failed to boot the new image
             * and came back on the old one must not start over by itself. */
            s_has_target = false;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    if (ours) {
        if (status == 0x00) {
            ESP_LOGW(TAG, "0x%04x downloaded and verified the image and is switching to it", short_addr);
        } else {
            ESP_LOGW(TAG, "0x%04x ended the download with status 0x%02x", short_addr, status);
        }
        zbc_state_bump();
    }
    return ours;
}

/* ------------------------------------------------------------------- JSON */

static void add_ieee(cJSON *obj, const char *name, bool known, const uint8_t ieee[ZBC_IEEE_LEN])
{
    if (!known) {
        cJSON_AddNullToObject(obj, name);
        return;
    }
    char s[19];
    zbc_ieee_to_str(ieee, s);
    cJSON_AddStringToObject(obj, name, s);
}

static const char *state_name_locked(int64_t now)
{
    if (!s_part) {
        return "no_partition";
    }
    if (s_writing) {
        return "writing";
    }
    if (s_xfer.phase == XFER_SENDING) {
        return xfer_busy_locked(now) ? "serving" : "stalled";
    }
    if (!s_have) {
        return "idle";
    }
    if (s_xfer.phase == XFER_DONE) {
        return s_xfer.status == 0x00 ? "finished" : "failed";
    }
    return s_offered ? "ready" : "stored";
}

cJSON *zb_ota_status_json(void)
{
    const int64_t now = esp_timer_get_time();

    /* Copied out under the lock, built into JSON outside it. */
    portENTER_CRITICAL(&s_mux);
    const char         *state      = state_name_locked(now);
    const bool          have       = s_have && !s_writing;
    const zbc_ota_hdr_t hdr        = s_hdr;
    const uint32_t      len        = s_len;
    const uint32_t      crc        = s_crc;
    const bool          has_target = s_has_target;
    uint8_t             target[ZBC_IEEE_LEN];
    memcpy(target, s_target, sizeof(target));
    const xfer_t       xfer    = s_xfer;
    const last_query_t last    = s_last;
    const uint32_t     queries = s_queries;
    const uint32_t     refused = s_refused;
    zota_wake_t        wake[ZOTA_WAKE_ROWS];
    memcpy(wake, s_wake, sizeof(wake));
    const uint32_t wake_sent   = s_wake_sent;
    const uint32_t wake_failed = s_wake_failed;
    const uint32_t     n_delivered   = s_notify_delivered;
    const uint32_t     n_undelivered = s_notify_undelivered;
    const notify_cnf_t n_last        = s_notify_last;
    char               error[sizeof(s_error)];
    memcpy(error, s_error, sizeof(error));
    const uint32_t room = s_part ? (uint32_t)s_part->size : 0;
    portEXIT_CRITICAL(&s_mux);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", state);
    cJSON_AddNumberToObject(o, "room", room);
    if (have) {
        cJSON *img = cJSON_AddObjectToObject(o, "image");
        cJSON_AddNumberToObject(img, "manufacturer_code", hdr.manuf_code);
        cJSON_AddNumberToObject(img, "image_type", hdr.image_type);
        cJSON_AddNumberToObject(img, "file_version", hdr.file_version);
        cJSON_AddNumberToObject(img, "stack_version", hdr.stack_version);
        cJSON_AddNumberToObject(img, "size", len);
        cJSON_AddStringToObject(img, "header", hdr.hdr_string);
        char crc_s[9];
        snprintf(crc_s, sizeof(crc_s), "%08" PRIx32, crc);
        cJSON_AddStringToObject(img, "crc", crc_s);
    } else {
        cJSON_AddNullToObject(o, "image");
    }
    if (error[0]) {
        cJSON_AddStringToObject(o, "error", error);
    } else {
        cJSON_AddNullToObject(o, "error");
    }
    add_ieee(o, "target", has_target, target);

    if (xfer.phase != XFER_NONE) {
        cJSON *t = cJSON_AddObjectToObject(o, "transfer");
        add_ieee(t, "ieee", true, xfer.ieee);
        cJSON_AddNumberToObject(t, "short_addr", xfer.short_addr);
        cJSON_AddNumberToObject(t, "from_version", xfer.from_version);
        cJSON_AddNumberToObject(t, "to_version", xfer.to_version);
        /* What the device says it runs now, so a finished transfer can be told
         * from a device that is actually running the image. */
        zb_ota_device_t now_info;
        if (zb_ota_device_info(xfer.ieee, &now_info) && now_info.version_known) {
            cJSON_AddNumberToObject(t, "device_version", now_info.file_version);
        } else {
            cJSON_AddNullToObject(t, "device_version");
        }
        cJSON_AddNumberToObject(t, "offset", xfer.offset);
        cJSON_AddNumberToObject(t, "size", xfer.size);
        cJSON_AddNumberToObject(t, "started_s_ago", (double)((now - xfer.started_us) / 1000000));
        cJSON_AddNumberToObject(t, "progress_s_ago", (double)((now - xfer.progress_us) / 1000000));
        if (xfer.phase == XFER_DONE) {
            cJSON_AddNumberToObject(t, "status", xfer.status);
        } else {
            cJSON_AddNullToObject(t, "status");
        }
    } else {
        cJSON_AddNullToObject(o, "transfer");
    }

    cJSON_AddNumberToObject(o, "queries", queries);
    cJSON_AddNumberToObject(o, "refused", refused);
    /* The sleepy devices a notify is held for, and how many were sent because
     * one was heard from - the one way to tell a remote that never woke from
     * one that was notified and did not ask. */
    cJSON *w = cJSON_AddArrayToObject(o, "waiting");
    for (size_t i = 0; i < ZOTA_WAKE_ROWS; i++) {
        if (!wake[i].used || wake[i].until_us <= now) {
            continue;
        }
        cJSON *e = cJSON_CreateObject();
        add_ieee(e, "ieee", true, wake[i].ieee);
        cJSON_AddNumberToObject(e, "s_left", (double)((wake[i].until_us - now) / 1000000));
        cJSON_AddNumberToObject(e, "notifies", wake[i].notifies);
        cJSON_AddNumberToObject(e, "last_s_ago", (double)((now - wake[i].last_sent_us) / 1000000));
        cJSON_AddItemToArray(w, e);
    }
    cJSON_AddNumberToObject(o, "wake_notifies_sent", wake_sent);
    cJSON_AddNumberToObject(o, "wake_notifies_refused", wake_failed);
    cJSON_AddNumberToObject(o, "notifies_delivered", n_delivered);
    cJSON_AddNumberToObject(o, "notifies_undelivered", n_undelivered);
    if (n_last.seen) {
        cJSON *c = cJSON_AddObjectToObject(o, "last_notify");
        cJSON_AddNumberToObject(c, "short_addr", n_last.short_addr);
        cJSON_AddNumberToObject(c, "status", n_last.status);
        cJSON_AddNumberToObject(c, "s_ago", (double)((now - n_last.at_us) / 1000000));
    } else {
        cJSON_AddNullToObject(o, "last_notify");
    }
    if (last.seen) {
        cJSON *l = cJSON_AddObjectToObject(o, "last_query");
        add_ieee(l, "ieee", last.ieee_known, last.ieee);
        cJSON_AddNumberToObject(l, "short_addr", last.short_addr);
        cJSON_AddNumberToObject(l, "manufacturer_code", last.q.manuf_code);
        cJSON_AddNumberToObject(l, "image_type", last.q.image_type);
        cJSON_AddNumberToObject(l, "file_version", last.q.file_version);
        cJSON_AddNumberToObject(l, "s_ago", (double)((now - last.at_us) / 1000000));
        cJSON_AddStringToObject(l, "answer", last.answer ? last.answer : "image offered");
    } else {
        cJSON_AddNullToObject(o, "last_query");
    }
    return o;
}

void zb_ota_add_device_json(cJSON *device, const uint8_t ieee[ZBC_IEEE_LEN])
{
    /* Every Zigbee device gets the object, with nothing known when it has not
     * said anything since boot: the table is not kept across a restart, and a
     * remote does not rejoin after one - yet it can still be asked. */
    zb_ota_device_t d;
    if (!zb_ota_device_info(ieee, &d)) {
        memset(&d, 0, sizeof(d));
    }
    cJSON *o = cJSON_AddObjectToObject(device, "ota");
#define ADD_OPT(name, known, value)                     \
    do {                                                \
        if (known) {                                    \
            cJSON_AddNumberToObject(o, name, (value));  \
        } else {                                        \
            cJSON_AddNullToObject(o, name);             \
        }                                               \
    } while (0)
    ADD_OPT("ep", d.ep != 0, d.ep);
    ADD_OPT("manufacturer_code", d.manuf_known, d.manuf_code);
    ADD_OPT("image_type", d.type_known, d.image_type);
    ADD_OPT("file_version", d.version_known, d.file_version);
    ADD_OPT("hw_version", d.hw_known, d.hw_version);
    ADD_OPT("queried_s_ago", d.queried_us != 0, (double)((esp_timer_get_time() - d.queried_us) / 1000000));
#undef ADD_OPT
}
