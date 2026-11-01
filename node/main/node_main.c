/* The node's startup.
 *
 * Which mode this boot is in comes first (node_mode.c), and everything else follows
 * from it. Normally that is Zigbee alone: Wi-Fi, the web server, mDNS and the clock
 * never start, and neither do their tasks. Wi-Fi first when it runs at all, in the
 * hub's order, so coexistence is configured before 802.15.4 starts using the radio.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "button_db.h"
#include "crash_log.h"
#include "light_db.h"
#include "local_lights.h"
#include "node_buttons.h"
#include "node_debug.h"
#include "node_main.h"
#include "node_proto.h"
#include "node_store.h"
#include "node_zb.h"
#include "settings.h"
#include "thermal.h"
#include "wifi_manager.h"

static const char *TAG = "node";

/* Until Wi-Fi mode can hand an update over (node_http.c), a test image does not confirm
 * itself and restarts after this long. On trial that is a rollback to the updater, which
 * is how a board with no cable stays reachable. */
#define TEST_WINDOW_US (20 * 60 * 1000000LL)

static node_mode_t      s_mode;
static uint8_t          s_minutes;
static volatile int64_t s_last_use;

node_mode_t node_mode(void)
{
    return s_mode;
}

void node_touch(void)
{
    s_last_use = esp_timer_get_time();
}

int node_window_left_s(void)
{
    if (s_mode == NODE_MODE_ZIGBEE) {
        return -1;
    }
    /* Asked of the rule rather than worked out twice: the first second it says over. */
    const int64_t now = esp_timer_get_time();
    for (int s = 0; s <= 3600; s += 5) {
        if (node_window_over(s_mode, now + s * 1000000LL, s_last_use, node_zb_joined_at(), s_minutes)) {
            return s;
        }
    }
    return -1;
}

void node_restart_into(node_ask_t ask, uint8_t minutes)
{
    local_lights_save_all();
    node_store_set_resume();
    node_store_set_ask(ask, minutes);
    if (ask == NODE_ASK_HOTSPOT) {
        /* Its own one-shot, so it opens the hotspot whatever network is stored. Does
         * not return. */
        wifi_manager_setup_hotspot();
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

/* Only in a mode with Wi-Fi: normally nothing here wakes the chip. */
static void window_task(void *arg)
{
    (void)arg;
    uint8_t last_state = 0xFF;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (node_window_over(s_mode, esp_timer_get_time(), s_last_use, node_zb_joined_at(), s_minutes)) {
            ESP_LOGW(TAG, "Wi-Fi's time is up: back to Zigbee alone");
            node_restart_into(NODE_ASK_NONE, 0);
        }
        /* The hub shows the address, so tell it as it changes. Every minute too: a
         * frame may be lost, and the hub keeps this in RAM. */
        wifi_mgr_status_t w;
        wifi_manager_get_status(&w);
        const uint8_t state = w.mode == WIFI_MGR_MODE_AP ? NODE_WIFI_HOTSPOT
                              : w.connected              ? NODE_WIFI_STATION
                                                         : NODE_WIFI_JOINING;
        static int ticks;
        if (state != last_state || ++ticks >= 60) {
            ticks = 0;
            uint8_t ip[4] = { 0 };
            esp_ip4_addr_t addr = { 0 };
            if (esp_netif_str_to_ip4(w.ip, &addr) == ESP_OK) {
                memcpy(ip, &addr.addr, 4);
            }
            node_zb_send_wifi_state(state, ip);
            last_state = state;
        }
    }
}

static void test_window_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    crash_log_init();
    node_debug_note_namespaces();

    /* A flash that held a hub keeps its lights, Wi-Fi and keys; only the coordinator's
     * network has to go. Erased directly, since the stack's own reset restarts. */
    if (node_store_foreign()) {
        err = node_zb_forget_network();
        ESP_LOGW(TAG, "First boot on this flash: stored Zigbee network forgotten (%s)", esp_err_to_name(err));
        if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
            node_store_claim();
        }
    }

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(light_db_init());
    ESP_ERROR_CHECK(button_db_init());

    node_ask_t ask = node_store_take_ask(&s_minutes);
#ifdef NODE_TEST_WIFI
    if (ask == NODE_ASK_NONE) {
        ask = NODE_ASK_WIFI;   /* so a test image can be watched */
    }
#endif
    s_mode = node_mode_decide(node_store_paired(), ask, wifi_manager_has_credentials());
    ESP_LOGW(TAG, "Mode %d", s_mode);

    /* Before the lights are set up: a mode change should not flash or lose them. */
    if (node_store_take_resume()) {
        local_lights_resume_once();
    }
    thermal_start();
    ESP_ERROR_CHECK(local_lights_start());
    ESP_ERROR_CHECK(node_buttons_start());

    const bool wifi = s_mode != NODE_MODE_ZIGBEE;
    if (wifi) {
        ESP_ERROR_CHECK(wifi_manager_start());
        wifi_manager_set_modem_sleep(settings_wifi_sleep());
        ESP_ERROR_CHECK(node_debug_start());
        s_last_use = esp_timer_get_time();
        xTaskCreate(window_task, "node_window", 3072, NULL, 3, NULL);
    }

    const esp_timer_create_args_t tw = { .callback = test_window_cb, .name = "test_window" };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&tw, &timer));
    ESP_ERROR_CHECK(esp_timer_start_once(timer, TEST_WINDOW_US));

    /* A paired node in its hotspot keeps quiet on Zigbee: a listening receiver holds the
     * antenna against hotspot clients. Unpaired, it does listen, only while it looks. */
    if (s_mode == NODE_MODE_HOTSPOT) {
        ESP_LOGW(TAG, "Setup hotspot is up: Zigbee stays off");
        return;
    }
    ESP_ERROR_CHECK(node_zb_start(wifi));
}
