#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "nvs_flash.h"

#include "al_db.h"
#include "auth.h"
#include "button_db.h"
#include "buttons.h"
#include "config_backup.h"
#include "crash_log.h"
#include "curve_db.h"
#include "device_db.h"
#include "group_db.h"
#include "heap_watch.h"
#include "http_server.h"
#include "light_db.h"
#include "lighting.h"
#include "local_lights.h"
#include "thermal.h"
#include "matter_bridge.h"
#include "matter_ctl.h"
#include "mlight_db.h"
#include "ota.h"
#include "settings.h"
#include "wifi_manager.h"
#include "wled.h"
#include "zb_coordinator.h"
#include "zb_ota.h"

/* partitions.csv as it is flashed, updater row included; main/CMakeLists.txt. */
extern const uint8_t own_table_start[] asm("_binary_partition_table_full_bin_start");
extern const uint8_t own_table_end[]   asm("_binary_partition_table_full_bin_end");

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* The Zigbee stack keeps its network state in this same partition. If it
     * fills up, forming a network fails in a way that looks like a radio
     * problem, so say how much room is left. */
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) == ESP_OK) {
        ESP_LOGI(TAG, "NVS: %d used, %d free, %d total entries (%d namespaces)",
                 st.used_entries, st.free_entries, st.total_entries, st.namespace_count);
    }

    /* First after NVS, so a crash anywhere later in this boot cannot replace the
     * record of the one before it unread. */
    crash_log_init();
    /* A restore uploaded before the restart, carried out before any table
     * loads: they read NVS once, and nothing has read it yet. */
    config_backup_apply();

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    /* Before anything allocates much, so the lows of the start are dated too.
     * A diagnostic: a failure is logged, not fatal. Nothing without
     * CONFIG_ZBC_HEAP_WATCH. */
    if ((err = heap_watch_start()) != ESP_OK) {
        ESP_LOGW(TAG, "heap_watch: %s", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(device_db_init());
    ESP_ERROR_CHECK(group_db_init());
    ESP_ERROR_CHECK(al_db_init());
    ESP_ERROR_CHECK(curve_db_init());
    ESP_ERROR_CHECK(button_db_init());
    ESP_ERROR_CHECK(light_db_init());
    /* Before the AP-mode return, like the other tables: a Matter lamp is still a
     * known device, offline, while the setup portal runs. */
    ESP_ERROR_CHECK(mlight_db_init());
    /* The Zigbee device image, before the stack that serves it and the web
     * server that reports it. Never fatal: a flash on the older layout has no
     * partition for it, and runs on without device updates. */
    zb_ota_init();
    /* The table this build was made for, so the updater it ships with is
     * trusted before that table is on flash - see ota_updater_problem. */
    ota_set_own_table(own_table_start, (size_t)(own_table_end - own_table_start));
    /* Before the web server, which asks it about every request. */
    ESP_ERROR_CHECK(auth_init());

    /* Wi-Fi first so the radio coexistence software is configured before the
     * 802.15.4 stack starts using the shared radio. */
    ESP_ERROR_CHECK(wifi_manager_start());
    /* The two radios share one antenna and 802.15.4 receive has the lowest
     * priority of anything the arbiter sees, so a Wi-Fi that never sleeps is a
     * Zigbee receiver that misses a remote's press whenever Wi-Fi is listening.
     * The updater, which shares wifi_manager.c, stays awake on purpose. */
    wifi_manager_set_modem_sleep(settings_wifi_sleep());
    ESP_ERROR_CHECK(http_server_start());

    /* After the web server, because confirming a freshly uploaded image is a
     * claim that this build can still be reached - and before the AP-mode
     * return below, so a trial image running the setup portal still confirms
     * on its uptime fallback rather than being rolled back. */
    ota_init();

    /* Before the wired lights are driven to their power-on state: a board that
     * starts too hot, or with a broken overheat sensor, never switches one on. */
    thermal_start();

    /* Before the AP-mode return below, so the pins are sampled while somebody is
     * still standing over the board with a wire in hand and the setup page can
     * show whether a button is reaching the pin. Pressing one does nothing until
     * Zigbee is up - buttons.c checks zb_is_running() rather than calling into a
     * stack that has not been initialised, which asserts. */
    /* Wired lights first: they are driven to their power-on state here, and
     * like the buttons they do not wait for Zigbee - or for Wi-Fi to leave the
     * setup portal. */
    ESP_ERROR_CHECK(local_lights_start());
    ESP_ERROR_CHECK(buttons_start());

    /* Wi-Fi and 802.15.4 share one radio. Coexistence (enabled in
     * zb_coordinator.c) lets them run together once we are on a network, but
     * the provisioning AP is left alone with the antenna: getting the user onto
     * their Wi-Fi is the only thing that matters in that state, and there are no
     * lights to serve yet anyway. */
    wifi_mgr_status_t wifi;
    wifi_manager_get_status(&wifi);
    if (wifi.mode == WIFI_MGR_MODE_AP) {
        ESP_LOGW(TAG, "No Wi-Fi credentials yet - running the setup portal, Zigbee stays off");
        return;
    }

    /* Adaptive Lighting carries on where it was before the restart. Flags only,
     * so before Matter: the bridge reports the mode from its first pass. */
    lighting_al_resume();

    /* The time of day, for Adaptive Lighting and nothing else. UTC is all it
     * needs - the sun model has no timezone in it. Here rather than in
     * wifi_manager.c because the updater compiles that file and has no use for a
     * clock. lwIP keeps asking until the network answers, so starting it before
     * the station has an address is fine. */
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp.sync_cb           = crash_log_clock_synced;   /* and for dating a crash */
    if (esp_netif_sntp_init(&sntp) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP did not start; Adaptive Lighting waits for a clock");
    }

    ESP_ERROR_CHECK(zb_coordinator_start());
    ESP_LOGI(TAG, "Zigbee coordinator running");

    /* After Zigbee, so the coexistence it enables is in place before Matter's
     * traffic joins the antenna, and after the AP-mode return above for the
     * reason Zigbee is: the setup portal has the radio to itself. Not fatal - the
     * lights, the buttons and the web UI do not depend on Matter. */
    if (matter_bridge_start() != ESP_OK) {
        ESP_LOGE(TAG, "Matter bridge did not start; lights stay reachable over HTTP");
    } else if (mctl_start() != ESP_OK) {
        /* On the bridge's own stack, so only once it is running - and skipped,
         * like the rest of Matter, in the setup portal. */
        ESP_LOGE(TAG, "Matter lamps did not start");
    }

    /* Last: it drives groups through lighting.c, which needs Zigbee running,
     * and it listens for Apple Home's switch, which Matter reports. */
    if (lighting_al_start() != ESP_OK) {
        ESP_LOGE(TAG, "Adaptive Lighting did not start");
    }

    /* After everything it streams through, and never in the setup portal: the
     * WLED interface feeds lighting.c, and its lamps are Zigbee lamps. */
    wled_start();
}
