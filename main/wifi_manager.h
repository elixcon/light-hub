/* Wi-Fi bring-up: joins the stored network, or opens a provisioning SoftAP when
 * there are no credentials (or the stored ones don't work). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MGR_MODE_BOOTING = 0,
    WIFI_MGR_MODE_STA,        /* joining / joined the user's network */
    WIFI_MGR_MODE_AP,         /* provisioning portal is up */
} wifi_mgr_mode_t;

typedef struct {
    wifi_mgr_mode_t mode;
    bool            connected;   /* STA has an IP */
    char            ssid[33];
    char            ip[16];
    int8_t          rssi;
    uint8_t         channel;     /* 2.4 GHz channel the AP is on, 0 when unknown */
    bool            weak;        /* associated, but with little signal to spare */
    char            hostname[32];
    /* The setup hotspot was started on purpose (ten button presses) while a
     * network is stored, and the box goes back to that network on its own. */
    bool            temporary;
    char            home_ssid[33];   /* the stored network it will go back to */
    int             returns_in_s;    /* until it does; 0 when not temporary */
} wifi_mgr_status_t;

esp_err_t wifi_manager_start(void);
/* Let the station doze between DTIM beacons (WIFI_PS_MIN_MODEM) or keep it
 * listening (WIFI_PS_NONE). wifi_manager_start() leaves it listening; the
 * coordinator switches to what settings_wifi_sleep() says, and the updater -
 * which compiles this file and has no 802.15.4 to protect - never calls this.
 * Takes effect at once, connected or not. */
esp_err_t wifi_manager_set_modem_sleep(bool on);
void      wifi_manager_get_status(wifi_mgr_status_t *out);

/* Just the mode, for callers that ask on every request: get_status also asks the
 * driver for the AP's signal and may log about it. */
wifi_mgr_mode_t wifi_manager_mode(void);

/* True once the station has had an address in this boot, in whichever mode:
 * from the stored network, the setup page's check of a new one, or the fallback
 * hotspot's retry. Stays true after the link drops. */
bool wifi_manager_station_joined(void);

/* Stores credentials in NVS. The caller reboots to apply them. */
esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password);

/* A network typed into the setup page, tried from the hotspot before anything
 * is stored. */
typedef enum {
    WIFI_TEST_IDLE = 0,
    WIFI_TEST_RUNNING,
    WIFI_TEST_JOINED,          /* stored; the box restarts onto it in a few seconds */
    WIFI_TEST_WRONG_PASSWORD,
    WIFI_TEST_NOT_FOUND,
    WIFI_TEST_NO_ADDRESS,      /* the password was taken, and DHCP never answered */
    WIFI_TEST_FAILED,          /* anything else: `reason` says what */
    WIFI_TEST_NOT_SAVED,       /* joined, and the credentials could not be stored */
} wifi_test_state_t;

typedef struct {
    wifi_test_state_t state;
    uint8_t           reason;  /* the last disconnect reason (wifi_err_reason_t), 0 none */
    char              ssid[33];
    char              ip[16];  /* the address it was given, once JOINED */
} wifi_test_status_t;

/* Only while the setup hotspot is up (ESP_ERR_INVALID_STATE otherwise - from a
 * station, trying another network would take the page's own network away), and
 * one at a time (ESP_ERR_NOT_FINISHED). Returns once the attempt has started;
 * the outcome is read with wifi_manager_test_status(). On success the
 * credentials are stored and the box restarts onto them by itself. */
esp_err_t wifi_manager_test(const char *ssid, const char *password);
void      wifi_manager_test_status(wifi_test_status_t *out);
esp_err_t wifi_manager_erase_credentials(void);
bool      wifi_manager_has_credentials(void);

/* Restarts into the setup hotspot once, keeping the stored network: the flag is
 * erased as the next boot reads it, so a power cut always ends up back on the
 * stored Wi-Fi. Does not return unless the flag could not be stored. Blocks on
 * flash - never call it from an esp_timer callback. */
esp_err_t wifi_manager_setup_hotspot(void);

/* Blocking scan (~2 s). Caller frees *records with free(). */
esp_err_t wifi_manager_scan(wifi_ap_record_t **records, uint16_t *count);

#ifdef __cplusplus
}
#endif
