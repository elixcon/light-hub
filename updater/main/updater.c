/* The updater: the factory app in partitions.csv.
 *
 * The coordinator runs from the one application partition, and a running app
 * cannot overwrite itself. So an update is a handover: the coordinator stores a
 * flag and restarts into this; this writes the app partition and restarts into
 * that. It is also where a failed update lands - the bootloader rolls an image
 * that never confirmed back to the factory app - so it has to keep the box at
 * the address people know, behind the keys they already have.
 *
 * Wi-Fi, a web server and the flash writer, and nothing else: no Zigbee, no
 * Matter, no lights. The modules it shares with the coordinator are compiled
 * from ../../main by path (see main/CMakeLists.txt here).
 */
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_coexist.h"
#include "esp_event.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "auth.h"
#include "convert.h"
#include "ota.h"
#include "settings.h"
#include "updater.h"
#include "wifi_manager.h"

static const char *TAG = "updater";

/* How long a handover waits for a file before going back to the coordinator,
 * and how long any use of the page pushes that back. */
#define RETURN_AFTER_US (10 * 60 * 1000000LL)
#define TOUCH_GRACE_US  (3 * 60 * 1000000LL)

/* >>> host-test region: updater-decide <<< */
typedef enum {
    UPD_APP_NO_STATE = 0,   /* no otadata entry: a USB flash, or the handover erased it */
    UPD_APP_ACCEPTED,       /* an entry the bootloader has not rejected */
    UPD_APP_REJECTED,       /* rolled back: ABORTED or INVALID */
} upd_app_state_t;

typedef enum {
    UPD_BOOT_APP = 0,       /* nothing to do here - start the coordinator */
    UPD_WAIT_REQUESTED,     /* the coordinator asked for an update; go back if none comes */
    UPD_STAY_FAILED,        /* the last image never confirmed */
    UPD_STAY_EMPTY,         /* no bootable image at all */
} upd_decision_t;

/* Why this boot is in the updater, from three facts nothing can fake: whether
 * the coordinator left the one-shot flag, what otadata says about the app
 * partition, and whether the image there verifies.
 *
 * A fresh USB flash and a power cut after a handover look identical - no flag
 * (it is erased as it is read), no otadata entry - and both rightly start the
 * coordinator, which is what keeps a box from being stuck here with a working
 * app beside it. */
static upd_decision_t updater_decide(bool requested, upd_app_state_t state, bool image_valid)
{
    /* Checked first: an upload cut short leaves exactly this, and there is
     * nothing to go back to whatever else is true. */
    if (!image_valid) {
        return UPD_STAY_EMPTY;
    }
    if (requested) {
        return UPD_WAIT_REQUESTED;
    }
    if (state == UPD_APP_REJECTED) {
        return UPD_STAY_FAILED;
    }
    return UPD_BOOT_APP;
}
/* >>> end host-test region: updater-decide <<< */

static const esp_partition_t *s_app;
static upd_decision_t         s_decision;
static bool                   s_uploading;
static int64_t                s_return_at_us;
static char                   s_app_version[33];

static bool image_valid(const esp_partition_t *part)
{
    if (!part) {
        return false;
    }
    const esp_partition_pos_t pos = { .offset = part->address, .size = part->size };
    esp_image_metadata_t      meta;
    return esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) == ESP_OK;
}

/* An NVS that will not mount, on a flash with no coordinator installed.
 *
 * Never erasing it is the rule for a box that has been running: what it holds
 * is worth more than any update. A flash written from outside - a USB flash
 * over whatever was on the board before - is the other case, and there an
 * updater that cannot store Wi-Fi credentials never leaves its setup hotspot,
 * so the box is stranded with nothing it can be told to do next. Nothing anyone
 * typed in can be in a partition that will not mount on a board with no
 * coordinator in it, which is the narrow case this covers; the coordinator
 * itself formats on the same two errors, unconditionally (main.c).
 *
 * Only ever called once the flash is known to be on this project's layout. */
static bool format_empty_nvs(void)
{
    if (image_valid(s_app)) {
        return false;
    }
    ESP_LOGW(TAG, "NVS will not mount and no coordinator is installed; formatting it");
    return nvs_flash_erase() == ESP_OK && nvs_flash_init() == ESP_OK;
}

static upd_app_state_t app_state(const esp_partition_t *part)
{
    esp_ota_img_states_t st;
    if (!part || esp_ota_get_state_partition(part, &st) != ESP_OK) {
        return UPD_APP_NO_STATE;
    }
    return (st == ESP_OTA_IMG_ABORTED || st == ESP_OTA_IMG_INVALID) ? UPD_APP_REJECTED
                                                                     : UPD_APP_ACCEPTED;
}

/* Read and erased in one go, like wifi_manager.c's setup flag: a flag that
 * survived would bring every later boot back here. */
static bool take_update_flag(void)
{
    nvs_handle_t nvs;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    uint8_t    v   = 0;
    const bool set = nvs_get_u8(nvs, OTA_NVS_UPDATER_FLAG, &v) == ESP_OK && v;
    if (set) {
        nvs_erase_key(nvs, OTA_NVS_UPDATER_FLAG);
        nvs_commit(nvs);
    }
    nvs_close(nvs);
    return set;
}

static void refresh_decision(bool requested)
{
    const bool valid = image_valid(s_app);
    s_decision       = updater_decide(requested, app_state(s_app), valid);

    esp_app_desc_t desc;
    s_app_version[0] = '\0';
    if (valid && esp_ota_get_partition_description(s_app, &desc) == ESP_OK) {
        strlcpy(s_app_version, desc.version, sizeof(s_app_version));
    }
}

/* Returns only when the app could not be selected. */
static void start_app(void)
{
    esp_err_t err = esp_ota_set_boot_partition(s_app);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not select the coordinator image (%s); staying here", esp_err_to_name(err));
        s_decision = UPD_STAY_EMPTY;
        return;
    }
    ESP_LOGI(TAG, "Starting the coordinator %s", s_app_version);
    vTaskDelay(pdMS_TO_TICKS(200));   /* let the log line out */
    esp_restart();
}

/* A handover nobody followed up goes back to the coordinator, which is what a
 * mistaken click on "Update firmware" should cost: ten minutes of dark lights,
 * not a trip to the board. */
static void return_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (s_uploading || esp_timer_get_time() < s_return_at_us) {
            continue;
        }
        refresh_decision(false);
        if (s_decision != UPD_BOOT_APP) {
            ESP_LOGW(TAG, "No bootable coordinator image to go back to; staying here");
            break;
        }
        ESP_LOGW(TAG, "No firmware was uploaded; going back to the coordinator");
        start_app();
        break;
    }
    vTaskDelete(NULL);
}

const char *updater_reason(void)
{
    if (convert_needed()) {
        return "convert";
    }
    switch (s_decision) {
    case UPD_WAIT_REQUESTED: return "requested";
    case UPD_STAY_FAILED:    return "failed";
    case UPD_STAY_EMPTY:     return "empty";
    default:                 return "starting";
    }
}

const char *updater_message(void)
{
    if (convert_needed()) {
        return "This device still has another firmware's flash layout (ESPHome, for example). Convert it below before installing the coordinator.";
    }
    switch (s_decision) {
    case UPD_WAIT_REQUESTED:
        return "Ready for the new firmware. Without one, the coordinator starts again in a few minutes.";
    case UPD_STAY_FAILED:
        return "The last update did not start, so the coordinator is not running. Upload a working image.";
    case UPD_STAY_EMPTY:
        return "No working coordinator firmware is installed. Upload one to start it.";
    default:
        return "Starting the coordinator.";
    }
}

const char *updater_app_version(void)
{
    return s_app_version[0] ? s_app_version : NULL;
}

void updater_touch(void)
{
    const int64_t floor = esp_timer_get_time() + TOUCH_GRACE_US;
    if (s_return_at_us < floor) {
        s_return_at_us = floor;
    }
}

void updater_upload_started(void)
{
    s_uploading = true;
}

void updater_upload_finished(void)
{
    /* A successful upload has selected the app and is about to restart into it.
     * Anything else may have erased part of the app partition on the way. */
    if (esp_ota_get_boot_partition() != s_app) {
        refresh_decision(s_decision == UPD_WAIT_REQUESTED);
    }
    updater_touch();
    s_uploading = false;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Updater %s", esp_app_get_description()->version);

    /* Never erased here, unlike in the coordinator: this partition holds the
     * Zigbee network, the paired devices, the keys and the Wi-Fi credentials,
     * and an updater that wiped them to get going would do more damage than any
     * failed update. The one exception is format_empty_nvs(), where there is
     * demonstrably nothing in it yet. */
    esp_err_t nvs_err = nvs_flash_init();
    bool      nvs_ok  = nvs_err == ESP_OK;
    if (!nvs_ok) {
        ESP_LOGE(TAG, "NVS unavailable (%s); running without stored settings", esp_err_to_name(nvs_err));
    }

    /* Before anything decides what to boot. On another firmware's layout the
     * app partition belongs to that firmware, and "start the coordinator" would
     * hand the device straight back to it - so no flag is read, nothing is
     * decided and nothing is started until the flash has been converted. */
    convert_init();
    if (convert_needed()) {
        ESP_LOGW(TAG, "This flash has another firmware's layout; waiting to be converted");
    } else {
        s_app = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
        if (!nvs_ok) {
            nvs_ok = format_empty_nvs();
        }
        const bool requested = nvs_ok && take_update_flag();
        refresh_decision(requested);
        ESP_LOGI(TAG, "Update requested: %s, coordinator image: %s -> %s",
                 requested ? "yes" : "no", s_app_version[0] ? s_app_version : "none", updater_reason());

        if (s_decision == UPD_BOOT_APP) {
            start_app();
        }
    }

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    settings_init();
    auth_init();
    ESP_ERROR_CHECK(wifi_manager_start());
    /* Not optional, although nothing here uses 802.15.4. Without it Wi-Fi hears
     * nothing after a hardware reset - no AP found, a scan of zero networks -
     * and a failed update would leave the box unreachable, which is the one
     * thing the updater exists to prevent. The coordinator makes the same call
     * in zb_coordinator.c, and the coex state survives a software restart, so a
     * handover from the coordinator hides the fault: test from a cold boot. Why
     * the arbitration is needed is not known, only that it is. Called where the
     * coordinator calls it, after Wi-Fi. */
    esp_err_t coex = esp_coex_wifi_i154_enable();
    if (coex != ESP_OK) {
        ESP_LOGE(TAG, "Could not enable Wi-Fi coexistence (%s); Wi-Fi may hear nothing", esp_err_to_name(coex));
    }
    ESP_ERROR_CHECK(updater_http_start());

    if (s_decision == UPD_WAIT_REQUESTED) {
        s_return_at_us = esp_timer_get_time() + RETURN_AFTER_US;
        xTaskCreate(return_task, "upd_return", 3072, NULL, 3, NULL);
    }
}
