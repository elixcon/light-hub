/* The node's startup. For now only the first test image: Wi-Fi stays up so it can
 * be watched, since pwm-test has no cable, and the image is on trial.
 *
 * It never confirms itself, and restarts after TEST_WINDOW_US. A restart while on
 * trial is a rollback, which lands in the updater, so the board stays reachable. */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "soc/soc_caps.h"

#include "buttons.h"
#include "light_db.h"
#include "local_lights.h"
#include "node_debug.h"
#include "node_zb.h"
#include "settings.h"
#include "thermal.h"
#include "wifi_manager.h"

static const char *TAG = "node";

/* -DNODE_TEST_MODE=zigbee: no Wi-Fi at all, the way a paired node runs. Then nothing
 * can reach it but the hub, so the window is short. */
#ifdef NODE_TEST_ZIGBEE_ONLY
#define TEST_WINDOW_US (5 * 60 * 1000000LL)
#else
#define TEST_WINDOW_US (30 * 60 * 1000000LL)
#endif

#define NODE_NVS_NAMESPACE "node"
#define NODE_NVS_ROLE      "role"

/* Where this image found a coordinator's stored network, from a hub that used to
 * run on this flash. An end device cannot use one; the lights, the Wi-Fi network
 * and the keys are in other namespaces and stay. */
static void adopt_flash(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NODE_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    uint8_t role = 0;
    if (nvs_get_u8(nvs, NODE_NVS_ROLE, &role) != ESP_OK || role != 1) {
        const esp_err_t err = node_zb_forget_network();
        ESP_LOGW(TAG, "First boot on this flash: stored Zigbee network forgotten (%s)", esp_err_to_name(err));
        if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
            nvs_set_u8(nvs, NODE_NVS_ROLE, 1);
            nvs_commit(nvs);
        }
    }
    nvs_close(nvs);
}

static void window_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "Test window over: restarting, which rolls back to the updater");
    esp_restart();
}

const char *buttons_pin_problem(int gpio)
{
    if (gpio == -1) {
        return NULL;
    }
    if (gpio < 0 || gpio >= SOC_GPIO_PIN_COUNT) {
        return "that pin does not exist on this chip";
    }
    if (gpio >= 24 && gpio <= 30) {
        return "GPIO 24-30 are wired to the flash chip";
    }
    return NULL;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    ESP_ERROR_CHECK(err);
    node_debug_note_namespaces();
    adopt_flash();

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(light_db_init());

#ifndef NODE_TEST_ZIGBEE_ONLY
    ESP_ERROR_CHECK(wifi_manager_start());
    wifi_manager_set_modem_sleep(settings_wifi_sleep());
    ESP_ERROR_CHECK(node_debug_start());
#endif

    const esp_timer_create_args_t window = { .callback = window_cb, .name = "test_window" };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&window, &timer));
    ESP_ERROR_CHECK(esp_timer_start_once(timer, TEST_WINDOW_US));

    thermal_start();
    ESP_ERROR_CHECK(local_lights_start());

#if !defined(NODE_TEST_HOTSPOT) && !defined(NODE_TEST_ZIGBEE_ONLY)
    wifi_mgr_status_t wifi;
    wifi_manager_get_status(&wifi);
    if (wifi.mode == WIFI_MGR_MODE_AP) {
        ESP_LOGW(TAG, "Setup hotspot is up: Zigbee stays off");
        return;
    }
#endif
    ESP_ERROR_CHECK(node_zb_start());
}
