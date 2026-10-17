/* Converting another firmware's flash to this project's layout.
 *
 * The way in is the other firmware's own OTA: ESPHome's web_server takes the
 * updater image as it would take any firmware.ota.bin, writes it to its other
 * app slot and boots it there, on trial. So this code runs from an app slot of a
 * partition table it did not write, and has to replace that table, the
 * bootloader and the data partitions without ever writing over itself - and
 * without a serial console, because on a device like a Shelly there is none.
 *
 * The order is chosen so the device stays bootable for as long as possible:
 *
 *   1. copy the updater to the factory address of the new table, and verify it;
 *   2. erase the new data partitions that the old otadata does not overlap;
 *   3. write the bootloader and read it back;          <- a power cut here or
 *   4. write the partition table and read it back;     <- here needs a cable
 *   5. reload the partition list, erase the rest, format NVS and store the
 *      Wi-Fi the person typed in; restart.
 *
 * Until step 3 a power cut boots whatever valid image the old table still
 * points at - this updater, in the worst case. After step 4 the bootloader finds
 * blank otadata and starts the factory app: this updater again, now at home,
 * with no coordinator yet, on the network it was given.
 *
 * Steps 3 and 4 are Espressif's own "unsafe" bootloader and partition table
 * update (examples/system/ota/partitions_ota): the C6 has no recovery bootloader
 * for the ROM to fall back to, so those few seconds cannot be made safe.
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_bootloader_desc.h"
#include "esp_flash.h"
#include "esp_flash_encrypt.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "convert.h"
#include "wifi_manager.h"

static const char *TAG = "convert";

/* The updater build's own table and bootloader, embedded by main/CMakeLists.txt.
 * The table is byte for byte the coordinator's full table, and the bootloader is
 * built from the same options as the coordinator's. */
extern const uint8_t table_start[] asm("_binary_partition_table_bin_start");
extern const uint8_t table_end[]   asm("_binary_partition_table_bin_end");
extern const uint8_t boot_start[]  asm("_binary_bootloader_bin_start");
extern const uint8_t boot_end[]    asm("_binary_bootloader_bin_end");

/* >>> host-test region: convert-plan <<< */
#define CV_MAX_PARTS       32
#define CV_MAX_RANGES      8
#define CV_TABLE_OFFSET    0x8000u
#define CV_TABLE_LEN       0xC00u
#define CV_SECTOR          0x1000u
#define CV_TYPE_APP        0x00
#define CV_TYPE_DATA       0x01
#define CV_SUBTYPE_FACTORY 0x00
#define CV_SUBTYPE_OTA_0   0x10
#define CV_SUBTYPE_OTADATA 0x00

typedef struct {
    uint32_t start;
    uint32_t end;   /* exclusive */
} cv_range_t;

typedef struct {
    char     label[17];
    uint8_t  type;
    uint8_t  subtype;
    uint32_t addr;
    uint32_t size;
} cv_part_t;

typedef enum {
    CV_GO = 0,      /* nothing the conversion writes overlaps the running updater */
    CV_MOVE,        /* it does, but the other app slot does not: move there first */
    CV_BLOCKED,
} cv_verdict_t;

typedef struct {
    cv_verdict_t verdict;
    const char  *why;
    cv_range_t   factory_write;                 /* where the updater copy goes */
    cv_range_t   erase_before[CV_MAX_RANGES];   /* new data partitions, before the table switch */
    int          n_before;
    cv_range_t   erase_after[CV_MAX_RANGES];    /* the ones the old otadata overlaps */
    int          n_after;
} cv_plan_t;

static uint32_t cv_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t cv_round_up(uint32_t v)
{
    return (v + CV_SECTOR - 1) & ~(CV_SECTOR - 1);
}

static bool cv_overlap(cv_range_t a, cv_range_t b)
{
    return a.start < a.end && b.start < b.end && a.start < b.end && b.start < a.end;
}

/* The entries of a binary partition table, or -1 when the bytes are not one. The
 * MD5 entry is skipped; the table ends at the first erased entry. */
static int cv_parse_table(const uint8_t *bin, size_t len, cv_part_t *out, int max)
{
    int n = 0;
    for (size_t off = 0; off + 32 <= len; off += 32) {
        const uint8_t *e     = bin + off;
        const uint16_t magic = (uint16_t)(e[0] | e[1] << 8);
        if (magic == 0xFFFF) {
            return n > 0 ? n : -1;
        }
        if (magic == 0xEBEB) {
            continue;
        }
        if (magic != 0x50AA || n >= max) {
            return -1;
        }
        cv_part_t *p = &out[n++];
        p->type      = e[2];
        p->subtype   = e[3];
        p->addr      = cv_rd32(e + 4);
        p->size      = cv_rd32(e + 8);
        memcpy(p->label, e + 12, 16);
        p->label[16] = '\0';
    }
    return -1;
}

/* What converting would take, from nothing but the two tables, where the
 * updater runs, where it could move to, and how big it is.
 *
 * The rule the whole plan rests on: nothing is ever written or erased over the
 * running image, and the old otadata - which is what a power cut before the
 * table switch boots from - is left alone until the new table is in place. */
static cv_plan_t cv_make_plan(const cv_part_t *nt, int n_new, const cv_part_t *ot, int n_old,
                              cv_range_t running, cv_range_t other_slot, uint32_t updater_len,
                              uint32_t flash_size, bool encrypted)
{
    cv_plan_t plan = { .verdict = CV_BLOCKED };

    if (encrypted) {
        plan.why = "flash encryption is on, and a new bootloader would not carry its keys";
        return plan;
    }
    if (n_old <= 0) {
        plan.why = "there is no readable partition table on the flash";
        return plan;
    }
    if (n_new <= 0) {
        plan.why = "the updater carries no valid partition table";
        return plan;
    }
    if (running.end <= running.start) {
        plan.why = "the updater cannot tell where it is running from";
        return plan;
    }

    const cv_part_t *factory   = NULL;
    uint32_t         table_end = 0;
    for (int i = 0; i < n_new; i++) {
        if (nt[i].type == CV_TYPE_APP && nt[i].subtype == CV_SUBTYPE_FACTORY) {
            factory = &nt[i];
        }
        if (nt[i].addr + nt[i].size > table_end) {
            table_end = nt[i].addr + nt[i].size;
        }
    }
    if (!factory) {
        plan.why = "the new partition table has no place for the updater";
        return plan;
    }
    if (table_end > flash_size) {
        plan.why = "the flash is too small for the new layout";
        return plan;
    }
    if (updater_len == 0 || updater_len > factory->size) {
        plan.why = "the updater does not fit its partition";
        return plan;
    }
    plan.factory_write = (cv_range_t){ factory->addr, factory->addr + cv_round_up(updater_len) };

    /* Unless it is already there. A foreign installer that places an app by the
     * name of a row in its own table can only put the updater in that slot -
     * and on a Shelly Gen4 that slot begins exactly where the new table's
     * factory partition does. Then there is nothing to copy, the running image
     * is not in the way of anything, and what would otherwise be a move (into a
     * slot that may be past the flash size this build knows) is a plain go.
     *
     * The image must fit inside the new factory partition, which is checked
     * above, and be wholly inside the partition it is running from. */
    if (running.start == factory->addr && running.end >= plan.factory_write.end) {
        plan.factory_write = (cv_range_t){ 0, 0 };
    }

    cv_range_t forbidden[CV_MAX_RANGES + 2];
    int        n_forbidden  = 0;
    forbidden[n_forbidden++] = (cv_range_t){ 0, CV_TABLE_OFFSET + CV_SECTOR };   /* bootloader and table */
    forbidden[n_forbidden++] = plan.factory_write;

    for (int i = 0; i < n_new; i++) {
        if (nt[i].type != CV_TYPE_DATA) {
            continue;
        }
        const cv_range_t r = { nt[i].addr, nt[i].addr + cv_round_up(nt[i].size) };
        bool under_otadata = false;
        for (int j = 0; j < n_old; j++) {
            const cv_range_t old = { ot[j].addr, ot[j].addr + ot[j].size };
            if (ot[j].type == CV_TYPE_DATA && ot[j].subtype == CV_SUBTYPE_OTADATA && cv_overlap(r, old)) {
                under_otadata = true;
            }
        }
        if (plan.n_before >= CV_MAX_RANGES || plan.n_after >= CV_MAX_RANGES || n_forbidden >= CV_MAX_RANGES + 2) {
            plan.why = "the new partition table has too many data partitions";
            return plan;
        }
        if (under_otadata) {
            plan.erase_after[plan.n_after++] = r;
        } else {
            plan.erase_before[plan.n_before++] = r;
        }
        forbidden[n_forbidden++] = r;
    }

    bool running_clear = true;
    bool other_clear   = other_slot.end > other_slot.start && other_slot.end - other_slot.start >= updater_len;
    for (int i = 0; i < n_forbidden; i++) {
        if (cv_overlap(running, forbidden[i])) {
            running_clear = false;
        }
        if (cv_overlap(other_slot, forbidden[i])) {
            other_clear = false;
        }
    }
    if (running_clear) {
        plan.verdict = CV_GO;
        plan.why     = NULL;
    } else if (other_clear) {
        plan.verdict = CV_MOVE;
        plan.why     = "the updater is running where the new layout needs to write, so it moves to the other app slot first";
    } else {
        plan.why = "the updater is running where the new layout needs to write, and there is no other app slot clear of it";
    }
    return plan;
}

typedef enum {
    CV_RELAYOUT_NOT_OURS = 0,   /* another firmware's table: that is a conversion */
    CV_RELAYOUT_GO,             /* our own older table: rewrite the table, nothing else */
    CV_RELAYOUT_KEEP,           /* our own older table, but the installed coordinator is longer than the new `app` */
} cv_relayout_t;

/* Whether the table on the flash is this project's own from before a partition
 * was cut from the end of `app`, so that writing the new table is all it takes:
 *
 *  - `app` (the one ota_0) keeps its label and offset, and ends earlier;
 *  - every other old row is in the new table unchanged - label, type, subtype,
 *    offset and size;
 *  - every new row lies in the span `app` gave up.
 *
 * Then nothing moves and nothing is erased: the Zigbee network, the paired
 * devices, the keys, the Wi-Fi and the Matter fabrics stay exactly where they
 * are. Anything else is not ours to rewrite this way.
 *
 * `app_image_len` is the length of the coordinator installed in the old `app`,
 * or 0 when none verifies. A table that cut through it would leave the updater
 * nothing to start, so that one waits for a shorter image (KEEP) and runs on the
 * old table meanwhile. */
static cv_relayout_t cv_relayout_check(const cv_part_t *nt, int n_new, const cv_part_t *ot, int n_old,
                                       uint32_t app_image_len)
{
    if (n_new <= 0 || n_old <= 0) {
        return CV_RELAYOUT_NOT_OURS;
    }
    const cv_part_t *new_app = NULL;
    const cv_part_t *old_app = NULL;
    for (int i = 0; i < n_new; i++) {
        if (nt[i].type == CV_TYPE_APP && nt[i].subtype == CV_SUBTYPE_OTA_0) {
            if (new_app) {
                return CV_RELAYOUT_NOT_OURS;
            }
            new_app = &nt[i];
        }
    }
    for (int i = 0; i < n_old; i++) {
        if (ot[i].type == CV_TYPE_APP && ot[i].subtype == CV_SUBTYPE_OTA_0) {
            if (old_app) {
                return CV_RELAYOUT_NOT_OURS;
            }
            old_app = &ot[i];
        }
    }
    if (!new_app || !old_app || strcmp(new_app->label, old_app->label) != 0 || new_app->addr != old_app->addr ||
        new_app->size >= old_app->size) {
        return CV_RELAYOUT_NOT_OURS;
    }
    const cv_range_t gap = { new_app->addr + new_app->size, old_app->addr + old_app->size };

    int kept = 0;
    for (int i = 0; i < n_old; i++) {
        if (&ot[i] == old_app) {
            continue;
        }
        const cv_part_t *match = NULL;
        for (int j = 0; j < n_new; j++) {
            if (&nt[j] != new_app && strcmp(nt[j].label, ot[i].label) == 0) {
                match = &nt[j];
                break;
            }
        }
        if (!match || match->type != ot[i].type || match->subtype != ot[i].subtype || match->addr != ot[i].addr ||
            match->size != ot[i].size) {
            return CV_RELAYOUT_NOT_OURS;
        }
        kept++;
    }
    int added = 0;
    for (int j = 0; j < n_new; j++) {
        if (&nt[j] == new_app) {
            continue;
        }
        bool known = false;
        for (int i = 0; i < n_old; i++) {
            if (&ot[i] != old_app && strcmp(ot[i].label, nt[j].label) == 0) {
                known = true;
            }
        }
        if (known) {
            continue;
        }
        if (nt[j].addr < gap.start || nt[j].size > gap.end - nt[j].addr) {
            return CV_RELAYOUT_NOT_OURS;
        }
        added++;
    }
    /* Two old rows sharing a label would both have matched one new row. */
    if (kept + added + 1 != n_new) {
        return CV_RELAYOUT_NOT_OURS;
    }
    return app_image_len > new_app->size ? CV_RELAYOUT_KEEP : CV_RELAYOUT_GO;
}
/* >>> end host-test region: convert-plan <<< */

_Static_assert(CONFIG_PARTITION_TABLE_OFFSET == CV_TABLE_OFFSET, "convert-plan assumes the table at 0x8000");

typedef enum {
    PH_IDLE = 0,
    PH_MOVING,
    PH_MOVED,
    PH_UPDATER,
    PH_DATA,
    PH_BOOTLOADER,
    PH_TABLE,
    PH_SETTINGS,
    PH_DONE,
    PH_FAILED,
} phase_t;

static const char *const PHASE_NAMES[] = {
    "idle", "moving", "moved", "updater", "data", "bootloader", "table", "settings", "done", "failed",
};

static bool                   s_foreign;
static cv_part_t              s_new[CV_MAX_PARTS];
static cv_part_t              s_old[CV_MAX_PARTS];
static int                    s_n_new;
static int                    s_n_old;
static const esp_partition_t *s_running;
static const esp_partition_t *s_other;
static uint32_t               s_updater_len;
static uint32_t               s_flash_size;
static cv_plan_t              s_plan;
static volatile phase_t       s_phase;
static const char            *s_error;
static char                   s_ssid[33];
static char                   s_pass[65];
static char                   s_boot_idf[33];

static cv_range_t range_of(const esp_partition_t *p)
{
    return p ? (cv_range_t){ p->address, p->address + p->size } : (cv_range_t){ 0, 0 };
}

static bool write_verified(uint32_t addr, const uint8_t *data, size_t len);

/* The coordinator installed in the old table's `app`, or 0 when none verifies. */
static uint32_t installed_app_len(void)
{
    const esp_partition_t *app =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (!app) {
        return 0;
    }
    const esp_partition_pos_t pos = { .offset = app->address, .size = app->size };
    esp_image_metadata_t      meta;
    return esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) == ESP_OK ? meta.image_len : 0;
}

/* This project's own older table: bring it up to date by writing the table and
 * nothing else, then restart onto it. See cv_relayout_check. Returns only when
 * the table was not rewritten; the flash is then treated as ours, on its old
 * layout.
 *
 * The table write is one sector and takes milliseconds, but a power cut inside
 * it leaves a device that needs a cable - the exposure the conversion's own
 * table write already accepts. Nothing before it is at risk: the updater, the
 * coordinator and every data partition stay where they are. */
static void relayout_older_table(size_t embedded_len)
{
    const uint32_t      app_len = installed_app_len();
    const cv_relayout_t r       = cv_relayout_check(s_new, s_n_new, s_old, s_n_old, app_len);
    if (r == CV_RELAYOUT_KEEP) {
        ESP_LOGW(TAG, "This project's older partition layout, but the installed coordinator (%" PRIu32
                      " bytes) does not fit the new app partition; keeping the old layout",
                 app_len);
        return;
    }
    ESP_LOGW(TAG, "This project's older partition layout; writing the new table (coordinator %" PRIu32 " bytes)",
             app_len);
    for (int attempt = 0; attempt < 3; attempt++) {
        if (write_verified(CV_TABLE_OFFSET, table_start, embedded_len)) {
            ESP_LOGW(TAG, "Partition table updated; restarting onto it");
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
        ESP_LOGE(TAG, "Writing the partition table failed (attempt %d)", attempt + 1);
    }
    /* Whatever is at 0x8000 now, the partition list in RAM is still the old
     * one, and it is the only thing left to run on. */
    ESP_LOGE(TAG, "Could not write the partition table; the next boot may need a cable");
}

void convert_init(void)
{
    static uint8_t flash_table[CV_TABLE_LEN];
    const size_t   embedded_len = (size_t)(table_end - table_start);

    s_n_new = cv_parse_table(table_start, embedded_len, s_new, CV_MAX_PARTS);
    const bool read_ok =
        esp_flash_read(esp_flash_default_chip, flash_table, CV_TABLE_OFFSET, sizeof(flash_table)) == ESP_OK;
    s_n_old   = read_ok ? cv_parse_table(flash_table, sizeof(flash_table), s_old, CV_MAX_PARTS) : -1;
    s_foreign = !(read_ok && embedded_len <= sizeof(flash_table) &&
                  memcmp(flash_table, table_start, embedded_len) == 0);
    if (!s_foreign) {
        return;
    }
    if (cv_relayout_check(s_new, s_n_new, s_old, s_n_old, 0) != CV_RELAYOUT_NOT_OURS) {
        relayout_older_table(embedded_len);
        s_foreign = false;
        return;
    }

    s_running = esp_ota_get_running_partition();
    s_other   = esp_ota_get_next_update_partition(NULL);
    if (s_other == s_running) {
        s_other = NULL;
    }
    if (s_running) {
        const esp_partition_pos_t pos = { .offset = s_running->address, .size = s_running->size };
        esp_image_metadata_t      meta;
        if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) == ESP_OK) {
            s_updater_len = meta.image_len;
        }
    }
    esp_flash_get_size(esp_flash_default_chip, &s_flash_size);

    esp_bootloader_desc_t desc;
    if (esp_ota_get_bootloader_description(NULL, &desc) == ESP_OK) {
        strlcpy(s_boot_idf, desc.idf_ver, sizeof(s_boot_idf));
    }

    s_plan = cv_make_plan(s_new, s_n_new, s_old, s_n_old, range_of(s_running), range_of(s_other),
                          s_updater_len, s_flash_size, esp_flash_encryption_enabled());

    ESP_LOGW(TAG, "Another firmware's partition layout: running from '%s' at 0x%06" PRIx32 ", %" PRIu32
                  " MB flash, verdict %d%s%s",
             s_running ? s_running->label : "?", s_running ? s_running->address : 0, s_flash_size >> 20,
             s_plan.verdict, s_plan.why ? ": " : "", s_plan.why ? s_plan.why : "");
    for (int i = 0; i < s_n_old; i++) {
        ESP_LOGI(TAG, "  %-16s type %02x/%02x at 0x%06" PRIx32 ", 0x%06" PRIx32 " bytes", s_old[i].label,
                 s_old[i].type, s_old[i].subtype, s_old[i].addr, s_old[i].size);
    }
}

bool convert_needed(void)
{
    return s_foreign;
}

bool convert_busy(void)
{
    return s_phase != PH_IDLE && s_phase != PH_FAILED;
}

/* ------------------------------------------------------------ flash writing */

static void fail(const char *why)
{
    ESP_LOGE(TAG, "Conversion failed: %s", why);
    s_error = why;
    s_phase = PH_FAILED;
}

/* Erases, writes and reads back. The data is copied into RAM first: it comes
 * from this image's rodata, which is mapped from flash, and nothing mapped from
 * flash can be read while the flash is being written. */
static bool write_verified(uint32_t addr, const uint8_t *data, size_t len)
{
    uint8_t *copy = malloc(len);
    uint8_t *back = malloc(CV_SECTOR);
    bool     ok   = copy && back;
    if (ok) {
        memcpy(copy, data, len);
        ok = esp_flash_erase_region(esp_flash_default_chip, addr, cv_round_up((uint32_t)len)) == ESP_OK &&
             esp_flash_write(esp_flash_default_chip, copy, addr, (uint32_t)len) == ESP_OK;
    }
    for (size_t off = 0; ok && off < len; off += CV_SECTOR) {
        size_t n = len - off < CV_SECTOR ? len - off : CV_SECTOR;
        ok       = esp_flash_read(esp_flash_default_chip, back, addr + off, (uint32_t)n) == ESP_OK &&
             memcmp(back, copy + off, n) == 0;
    }
    free(copy);
    free(back);
    return ok;
}

static bool flash_equals(uint32_t addr, const uint8_t *data, size_t len)
{
    uint8_t *back = malloc(CV_SECTOR);
    bool     same = back != NULL;
    for (size_t off = 0; same && off < len; off += CV_SECTOR) {
        size_t n = len - off < CV_SECTOR ? len - off : CV_SECTOR;
        same     = esp_flash_read(esp_flash_default_chip, back, addr + off, (uint32_t)n) == ESP_OK &&
               memcmp(back, data + off, n) == 0;
    }
    free(back);
    return same;
}

/* The running image, copied to `to` and verified there as an image. */
static bool copy_self(uint32_t to, uint32_t part_size)
{
    uint8_t *buf = malloc(CV_SECTOR);
    bool     ok  = buf && esp_flash_erase_region(esp_flash_default_chip, to, cv_round_up(s_updater_len)) == ESP_OK;
    for (uint32_t off = 0; ok && off < s_updater_len; off += CV_SECTOR) {
        uint32_t n = s_updater_len - off < CV_SECTOR ? s_updater_len - off : CV_SECTOR;
        ok         = esp_partition_read(s_running, off, buf, n) == ESP_OK &&
             esp_flash_write(esp_flash_default_chip, buf, to + off, n) == ESP_OK;
    }
    free(buf);
    if (!ok) {
        return false;
    }
    const esp_partition_pos_t pos = { .offset = to, .size = part_size };
    esp_image_metadata_t      meta;
    return esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) == ESP_OK;
}

/* Through the ordinary OTA calls, so the old bootloader boots the copy. */
static bool move_to_other_slot(void)
{
    esp_ota_handle_t h;
    uint8_t         *buf = malloc(CV_SECTOR);
    bool             ok  = buf && esp_ota_begin(s_other, OTA_WITH_SEQUENTIAL_WRITES, &h) == ESP_OK;
    bool             begun = ok;
    for (uint32_t off = 0; ok && off < s_updater_len; off += CV_SECTOR) {
        uint32_t n = s_updater_len - off < CV_SECTOR ? s_updater_len - off : CV_SECTOR;
        ok         = esp_partition_read(s_running, off, buf, n) == ESP_OK && esp_ota_write(h, buf, n) == ESP_OK;
    }
    free(buf);
    if (begun && !ok) {
        esp_ota_abort(h);
    }
    return ok && esp_ota_end(h) == ESP_OK && esp_ota_set_boot_partition(s_other) == ESP_OK;
}

static void convert_task(void *arg)
{
    (void)arg;

    /* From here on the old firmware is not coming back: a rollback would land on
     * a half-converted flash, and esp_ota_begin() refuses to start while the
     * running image is still on trial. */
    esp_ota_mark_app_valid_cancel_rollback();

    if (s_plan.verdict == CV_MOVE) {
        s_phase = PH_MOVING;
        ESP_LOGW(TAG, "Moving the updater to '%s' before converting", s_other->label);
        if (!move_to_other_slot()) {
            fail("could not move the updater to the other app slot");
            vTaskDelete(NULL);
            return;
        }
        s_phase = PH_MOVED;
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    /* Nothing in the updater touches NVS again before step 5, and its partition
     * may be about to be erased from under the library. */
    nvs_flash_deinit();

    const cv_part_t *factory = NULL;
    for (int i = 0; i < s_n_new; i++) {
        if (s_new[i].type == CV_TYPE_APP && s_new[i].subtype == CV_SUBTYPE_FACTORY) {
            factory = &s_new[i];
        }
    }

    s_phase = PH_UPDATER;
    if (s_plan.factory_write.end <= s_plan.factory_write.start) {
        ESP_LOGW(TAG, "1/5 The updater is already at 0x%06" PRIx32, factory->addr);
    } else {
        ESP_LOGW(TAG, "1/5 Copying the updater to 0x%06" PRIx32, factory->addr);
        if (!copy_self(factory->addr, factory->size)) {
            fail("could not copy the updater to its new address");
            goto out;
        }
    }

    s_phase = PH_DATA;
    ESP_LOGW(TAG, "2/5 Erasing %d data partition(s)", s_plan.n_before);
    for (int i = 0; i < s_plan.n_before; i++) {
        if (esp_flash_erase_region(esp_flash_default_chip, s_plan.erase_before[i].start,
                                   s_plan.erase_before[i].end - s_plan.erase_before[i].start) != ESP_OK) {
            fail("could not erase the new data partitions");
            goto out;
        }
    }

    s_phase = PH_BOOTLOADER;
    const size_t boot_len = (size_t)(boot_end - boot_start);
    if (boot_len > CONFIG_PARTITION_TABLE_OFFSET - CONFIG_BOOTLOADER_OFFSET_IN_FLASH) {
        fail("the embedded bootloader does not fit before the partition table");
        goto out;
    }
    if (flash_equals(CONFIG_BOOTLOADER_OFFSET_IN_FLASH, boot_start, boot_len)) {
        ESP_LOGW(TAG, "3/5 The bootloader is already this one");
    } else {
        ESP_LOGW(TAG, "3/5 Writing the bootloader (%u bytes) - do not cut the power", (unsigned)boot_len);
        if (!write_verified(CONFIG_BOOTLOADER_OFFSET_IN_FLASH, boot_start, boot_len)) {
            fail("the bootloader did not read back as written - the device may not start again");
            goto out;
        }
    }

    s_phase = PH_TABLE;
    ESP_LOGW(TAG, "4/5 Writing the partition table - do not cut the power");
    if (!write_verified(CV_TABLE_OFFSET, table_start, (size_t)(table_end - table_start))) {
        fail("the partition table did not read back as written - the device may not start again");
        goto out;
    }

    s_phase = PH_SETTINGS;
    ESP_LOGW(TAG, "5/5 The new layout is in place; preparing its data partitions");
    /* Every esp_partition_t pointer handed out so far describes the old table;
     * nothing may use one after this. */
    esp_partition_unload_all();
    for (int i = 0; i < s_plan.n_after; i++) {
        esp_flash_erase_region(esp_flash_default_chip, s_plan.erase_after[i].start,
                               s_plan.erase_after[i].end - s_plan.erase_after[i].start);
    }
    if (nvs_flash_init() == ESP_OK && s_ssid[0]) {
        if (wifi_manager_set_credentials(s_ssid, s_pass) == ESP_OK) {
            ESP_LOGI(TAG, "Stored the Wi-Fi network \"%s\"", s_ssid);
        } else {
            ESP_LOGE(TAG, "Could not store the Wi-Fi network; the device opens its setup hotspot");
        }
    }

    s_phase = PH_DONE;
    ESP_LOGW(TAG, "Converted. Restarting into the updater");
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();

out:
    vTaskDelete(NULL);
}

esp_err_t convert_start(const char *ssid, const char *password)
{
    if (!s_foreign || s_plan.verdict == CV_BLOCKED || convert_busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(s_ssid, ssid ? ssid : "", sizeof(s_ssid));
    strlcpy(s_pass, password ? password : "", sizeof(s_pass));
    s_error = NULL;
    s_phase = s_plan.verdict == CV_MOVE ? PH_MOVING : PH_UPDATER;
    if (xTaskCreate(convert_task, "convert", 6144, NULL, 5, NULL) != pdPASS) {
        s_phase = PH_IDLE;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ status */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} jbuf_t;

static void jb(jbuf_t *b, const char *fmt, ...)
{
    if (b->len + 1 >= b->cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        b->len += (size_t)n;
        if (b->len >= b->cap) {
            b->len = b->cap - 1;
        }
    }
}

/* Labels and messages are ours or come from a partition table; either way
 * nothing in them needs escaping, so anything that would is replaced. */
static const char *safe(const char *in, char *out, size_t cap)
{
    size_t i = 0;
    for (; in && in[i] && i + 1 < cap; i++) {
        const char c = in[i];
        out[i]       = (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') ? c : '?';
    }
    out[i] = '\0';
    return out;
}

void convert_status_json(char *buf, size_t cap)
{
    static const char *const VERDICTS[] = { "go", "move", "blocked" };
    jbuf_t b = { .buf = buf, .cap = cap, .len = 0 };
    char   s1[96], s2[200];
    buf[0] = '\0';

    jb(&b, "{\"needed\":%s", s_foreign ? "true" : "false");
    if (!s_foreign) {
        jb(&b, "}");
        return;
    }
    jb(&b, ",\"verdict\":\"%s\",\"why\":", VERDICTS[s_plan.verdict]);
    if (s_plan.why) {
        jb(&b, "\"%s\"", safe(s_plan.why, s2, sizeof(s2)));
    } else {
        jb(&b, "null");
    }
    jb(&b, ",\"flash_mb\":%" PRIu32 ",\"running\":\"%s\",\"running_address\":%" PRIu32
           ",\"updater_bytes\":%" PRIu32 ",\"bootloader_idf\":\"%s\",\"partitions\":[",
       s_flash_size >> 20, safe(s_running ? s_running->label : "?", s1, sizeof(s1)),
       s_running ? s_running->address : 0, s_updater_len, safe(s_boot_idf, s2, 33));
    for (int i = 0; i < s_n_old; i++) {
        jb(&b, "%s{\"label\":\"%s\",\"type\":%u,\"subtype\":%u,\"address\":%" PRIu32 ",\"size\":%" PRIu32 "}",
           i ? "," : "", safe(s_old[i].label, s1, sizeof(s1)), s_old[i].type, s_old[i].subtype, s_old[i].addr,
           s_old[i].size);
    }
    jb(&b, "],\"phase\":\"%s\",\"error\":", PHASE_NAMES[s_phase]);
    if (s_error) {
        jb(&b, "\"%s\"}", safe(s_error, s2, sizeof(s2)));
    } else {
        jb(&b, "null}");
    }
}
