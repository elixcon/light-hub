#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"

#include "captive_dns.h"
#include "settings.h"
#include "zb_coordinator.h"
#include "wifi_manager.h"

static const char *TAG = "wifi";

#define WIFI_NVS_NAMESPACE "zbc"
/* Both in config_backup.c's list too (coordinator only): a backup carries them. */
#define WIFI_NVS_SSID      "wifi_ssid"
#define WIFI_NVS_PASS      "wifi_pass"
/* One-shot: set by ten button presses, erased by the boot that reads it. */
#define WIFI_NVS_SETUP_ONCE "wifi_setup1"

/* Give up on the stored network after this many failures and fall back to the
 * provisioning AP, so a wrong password never leaves the box unreachable. */
#define STA_MAX_FAILS_BEFORE_AP 8

/* Below this the link costs retries, and a retried Wi-Fi frame is airtime the
 * mesh does not get: Wi-Fi and 802.15.4 share one antenna. A weak uplink shows
 * up as lamps that answer late or not at all, so it is logged. */
#define WIFI_WEAK_RSSI_DBM (-75)

static wifi_mgr_status_t s_status = { .mode = WIFI_MGR_MODE_BOOTING };
static esp_netif_t      *s_netif_sta;
static esp_netif_t      *s_netif_ap;
static int               s_fail_count;
static bool              s_ever_connected;
static esp_timer_handle_t s_retry_timer;
static esp_timer_handle_t s_dhcp_timer;
static bool              s_mdns_started;
static bool              s_wifi_started;
/* -1 until the signal has been judged once, so a association that is weak from
 * the start still says so. Reset on disconnect: the next one is a fresh link. */
static int               s_weak_logged = -1;
/* A hotspot started by the button, while a network is stored, goes back to that
 * network by itself: at s_return_at_us, pushed back a minute at a time while a
 * device is still on it, but never past s_return_deadline_us. */
static esp_timer_handle_t s_return_timer;
static int64_t           s_return_at_us;
static int64_t           s_return_deadline_us;

/* The provisioning AP opened because the stored network could not be joined -
 * as opposed to there being no network stored, or the button asking for it.
 * While this is set the STA half keeps trying, and the box restarts onto the
 * network the moment a join succeeds, so a router that comes back slower than
 * the box after a power cut does not leave it stuck as a hotspot. */
#define FALLBACK_RETRY_S 60
static esp_timer_handle_t s_fallback_timer;
static bool              s_fallback;

static void start_ap_mode(bool fallback);
static bool load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

/* How every join is set up - the boot's, the fallback AP's retries and a check
 * from the setup page alike, so that a check tries exactly what the boot will.
 * Scan every channel and take the strongest BSSID: the default fast scan stops
 * at the first match, which on a multi-AP network happily picks a far one that
 * then loses the DHCP handshake. */
static void sta_config(wifi_config_t *cfg, const char *ssid, const char *pass)
{
    *cfg = (wifi_config_t){
        .sta = {
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
        },
    };
    strlcpy((char *)cfg->sta.ssid, ssid, sizeof(cfg->sta.ssid));
    strlcpy((char *)cfg->sta.password, pass, sizeof(cfg->sta.password));
}

/* --------------------------------------- checking a network from the hotspot */

/* A network typed into the setup page is tried from the hotspot's station half
 * before anything is stored - the way Espressif's own provisioning does it - so
 * a mistyped password is said on the page instead of costing a restart, two
 * minutes of failed joins and the hotspot coming back. The hotspot stays up
 * throughout, but not on its channel: the chip has one, and as soon as the
 * station associates the SoftAP moves to the network's. That happens with a
 * wrong password too, since association comes before the handshake, so a phone
 * may drop off for a moment; the page keeps asking.
 *
 * The verdicts are Espressif's: a failed 4-way handshake or authentication is a
 * wrong password, "no AP found" is a name that is not there (or not 2.4 GHz),
 * anything else is tried again up to WIFI_TEST_TRIES. Past the handshake the
 * password is right, and the rest is DHCP: s_dhcp_timer gives it the same 20 s
 * a boot does. Success stores the network and restarts onto it after
 * WIFI_TEST_RESTART_MS, which is what the page's poll has to read "joined".
 *
 * Written by the HTTP task (the start) and the event loop (the outcome), so
 * under s_test_mux, with nothing that blocks inside it. */
#define WIFI_TEST_TRIES      3
#define WIFI_TEST_RESTART_MS 8000
static struct {
    wifi_test_status_t st;
    char               pass[65];
    bool               associated;   /* past the handshake: the password is right */
    uint8_t            tries;
} s_test;
static portMUX_TYPE s_test_mux = portMUX_INITIALIZER_UNLOCKED;

static wifi_test_state_t test_verdict(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return WIFI_TEST_WRONG_PASSWORD;
    case WIFI_REASON_NO_AP_FOUND:
        return WIFI_TEST_NOT_FOUND;
    default:
        return WIFI_TEST_RUNNING;    /* no verdict: try again */
    }
}

/* The hotspot as it was before the check: a fallback AP goes back to retrying
 * the stored network, which the check's configuration replaced. */
static void test_finished(void)
{
    if (!s_fallback) {
        return;
    }
    char ssid[33] = {0}, pass[65] = {0};
    if (load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        wifi_config_t cfg;
        sta_config(&cfg, ssid, pass);
        esp_wifi_set_config(WIFI_IF_STA, &cfg);
    }
    esp_timer_start_once(s_fallback_timer, (uint64_t)FALLBACK_RETRY_S * 1000000ULL);
}

/* 3072 bytes, not 2048: in the coordinator this restart is what confirms a
 * freshly flashed image (ota.c's confirm_on_restart runs on this stack). */
static void test_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(WIFI_TEST_RESTART_MS));
    esp_restart();
}

static void test_note_associated(void)
{
    portENTER_CRITICAL(&s_test_mux);
    if (s_test.st.state == WIFI_TEST_RUNNING) {
        s_test.associated = true;
    }
    portEXIT_CRITICAL(&s_test_mux);
}

/* True when the disconnect was the check's own. */
static bool test_note_disconnect(uint8_t reason)
{
    portENTER_CRITICAL(&s_test_mux);
    if (s_test.st.state != WIFI_TEST_RUNNING) {
        portEXIT_CRITICAL(&s_test_mux);
        return false;
    }
    s_test.st.reason = reason;
    const wifi_test_state_t v     = s_test.associated ? WIFI_TEST_NO_ADDRESS : test_verdict(reason);
    const bool              again = v == WIFI_TEST_RUNNING && ++s_test.tries < WIFI_TEST_TRIES;
    if (!again) {
        s_test.st.state = v == WIFI_TEST_RUNNING ? WIFI_TEST_FAILED : v;
        memset(s_test.pass, 0, sizeof(s_test.pass));
    }
    const wifi_test_state_t now = s_test.st.state;
    char                    ssid[33];
    memcpy(ssid, s_test.st.ssid, sizeof(ssid));
    portEXIT_CRITICAL(&s_test_mux);

    if (again) {
        ESP_LOGI(TAG, "Check of \"%s\": disconnected (reason %d), trying again", ssid, reason);
        esp_wifi_connect();
    } else {
        ESP_LOGW(TAG, "Check of \"%s\" failed: %s (reason %d)", ssid,
                 now == WIFI_TEST_WRONG_PASSWORD ? "wrong password"
                 : now == WIFI_TEST_NOT_FOUND    ? "network not found"
                 : now == WIFI_TEST_NO_ADDRESS   ? "no address from DHCP"
                                                 : "could not join",
                 reason);
        test_finished();
    }
    return true;
}

/* True when the address was the check's: the network is stored, and the box
 * restarts onto it once the page has had time to hear so. */
static bool test_note_joined(const esp_ip4_addr_t *ip)
{
    char ssid[33], pass[65];
    portENTER_CRITICAL(&s_test_mux);
    const bool running = s_test.st.state == WIFI_TEST_RUNNING;
    if (running) {
        memcpy(ssid, s_test.st.ssid, sizeof(ssid));
        memcpy(pass, s_test.pass, sizeof(pass));
        snprintf(s_test.st.ip, sizeof(s_test.st.ip), IPSTR, IP2STR(ip));
    }
    portEXIT_CRITICAL(&s_test_mux);
    if (!running) {
        return false;
    }
    esp_timer_stop(s_dhcp_timer);
    const esp_err_t err = wifi_manager_set_credentials(ssid, pass);
    memset(pass, 0, sizeof(pass));
    portENTER_CRITICAL(&s_test_mux);
    s_test.st.state = err == ESP_OK ? WIFI_TEST_JOINED : WIFI_TEST_NOT_SAVED;
    memset(s_test.pass, 0, sizeof(s_test.pass));
    portEXIT_CRITICAL(&s_test_mux);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Check of \"%s\" joined, but it could not be stored (%s)", ssid, esp_err_to_name(err));
        esp_wifi_disconnect();
        test_finished();
        return true;
    }
    ESP_LOGW(TAG, "Check of \"%s\" joined (got " IPSTR "); stored, restarting onto it in %d s",
             ssid, IP2STR(ip), WIFI_TEST_RESTART_MS / 1000);
    xTaskCreate(test_restart_task, "wifi_test_rst", 3072, NULL, 5, NULL);
    return true;
}

static void retry_connect_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void schedule_reconnect(void)
{
    /* 1, 2, 4 ... up to 30 s between attempts. */
    int delay_s = 1 << (s_fail_count > 5 ? 5 : s_fail_count);
    if (delay_s > 30) {
        delay_s = 30;
    }
    ESP_LOGI(TAG, "Reconnecting in %d s (attempt %d)", delay_s, s_fail_count + 1);
    esp_timer_stop(s_retry_timer);
    ESP_ERROR_CHECK(esp_timer_start_once(s_retry_timer, (uint64_t)delay_s * 1000000ULL));
}

/* An association that never turns into a lease leaves the coordinator on the
 * network but unreachable, and nothing in the Wi-Fi stack complains. Give DHCP
 * a deadline, say what state it is in, and start over on another BSSID. */
static void dhcp_deadline_cb(void *arg)
{
    (void)arg;
    if (s_status.connected) {
        return;
    }
    esp_netif_dhcp_status_t status = ESP_NETIF_DHCP_INIT;
    esp_netif_dhcpc_get_status(s_netif_sta, &status);
    ESP_LOGW(TAG, "No IP address after 20 s (DHCP client status %d), reconnecting", status);
    esp_wifi_disconnect();
}

static void start_mdns(void)
{
    if (s_mdns_started) {
        return;
    }
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed");
        return;
    }
    /* Both come from settings.c, which is read once at boot: a rename restarts
     * the box rather than trying to move a live name. mDNS could be renamed in
     * place, but the DHCP hostname below cannot, and matter_bridge.c's
     * keep_hostname() has no way to withdraw the delegated name it registered. */
    mdns_hostname_set(settings_hostname());
    mdns_instance_name_set(settings_name());
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    s_mdns_started = true;
    ESP_LOGI(TAG, "Reachable at http://%s.local", settings_hostname());
}

/* Logs everything the radio can hear, once a join has failed a couple of times.
 * "No AP found" is nearly always a network the C6 cannot see at all: 5 GHz only,
 * hidden, or out of the scanned channel range. Runs as its own task: a blocking
 * scan from the Wi-Fi event handler deadlocks, because scan-done is delivered by
 * that same task, and one issued during startup answers before the radio has
 * settled and reports nothing at all. */
static void log_visible_networks(void *arg)
{
    (void)arg;
    wifi_ap_record_t *records = NULL;
    uint16_t          count   = 0;
    if (wifi_manager_scan(&records, &count) != ESP_OK) {
        ESP_LOGW(TAG, "Diagnostic scan failed");
        return;
    }
    ESP_LOGW(TAG, "%d network(s) visible on 2.4 GHz:", count);
    for (uint16_t i = 0; i < count; i++) {
        ESP_LOGW(TAG, "  \"%s\" channel %d, %d dBm, auth %d",
                 (const char *)records[i].ssid, records[i].primary, records[i].rssi, records[i].authmode);
    }
    free(records);
    vTaskDelete(NULL);
}

/* esp_timer context: esp_wifi_connect() only posts to the Wi-Fi task, and the
 * driver call is the same one the return timer makes. */
static void fallback_retry_cb(void *arg)
{
    (void)arg;
    wifi_sta_list_t sta;
    /* Somebody is on the hotspot, most likely typing a password; a scan now
     * would take the AP off its channel under them. Look again in a minute. */
    if (esp_wifi_ap_get_sta_list(&sta) == ESP_OK && sta.num > 0) {
        esp_timer_start_once(s_fallback_timer, (uint64_t)FALLBACK_RETRY_S * 1000000ULL);
        return;
    }
    ESP_LOGI(TAG, "Trying the stored network \"%s\" again from the fallback AP", s_status.home_ssid);
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            /* Only in station mode. The interface also starts under the
             * provisioning AP (APSTA, so the setup page can scan), and a fallback
             * AP still carries the stored credentials; joining from under the AP
             * would fail and re-enter start_ap_mode. The fallback's own retries
             * come from fallback_retry_cb. */
            if (s_status.mode == WIFI_MGR_MODE_STA) {
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_STA_CONNECTED: {
            wifi_event_sta_connected_t *ev = data;
            ESP_LOGI(TAG, "Associated with \"%.*s\" on channel %d", ev->ssid_len,
                     (const char *)ev->ssid, ev->channel);
            esp_timer_stop(s_dhcp_timer);
            esp_timer_start_once(s_dhcp_timer, 20 * 1000000ULL);
            test_note_associated();
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *ev = data;
            s_status.connected = false;
            s_status.ip[0]     = '\0';
            s_weak_logged      = -1;
            ESP_LOGW(TAG, "Disconnected (reason %d)", ev->reason);
            if (s_status.mode == WIFI_MGR_MODE_AP) {
                if (test_note_disconnect(ev->reason)) {
                    break;
                }
                /* A fallback AP's own attempt at the stored network. The AP
                 * stays up; ask again in a minute. Any other AP has no network
                 * to try and nothing to do here. */
                if (s_fallback) {
                    esp_timer_start_once(s_fallback_timer, (uint64_t)FALLBACK_RETRY_S * 1000000ULL);
                }
                break;
            }
            if (!s_ever_connected && s_fail_count == 2) {
                xTaskCreate(log_visible_networks, "wifi_scan_log", 4096, NULL, 4, NULL);
            }
            /* Counted after a loss as well as before the first join, so reconnects
             * back off too: each attempt scans all thirteen channels with Wi-Fi
             * holding the shared antenna for most of a second, and a retry every
             * second during a router outage starves the 802.15.4 radio, remotes
             * and buttons with it. Only the AP fallback is reserved for a box that
             * has never got on: a network that worked is coming back, and a
             * hotspot would take Zigbee down. */
            if (s_fail_count < STA_MAX_FAILS_BEFORE_AP) {
                s_fail_count++;
            }
            if (!s_ever_connected && s_fail_count >= STA_MAX_FAILS_BEFORE_AP) {
                ESP_LOGW(TAG, "Could not join stored network, opening provisioning AP");
                start_ap_mode(true);
                break;
            }
            schedule_reconnect();
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "A client joined the provisioning AP");
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        /* Before the hotspot's two cases, which both restart: an address from
         * either is what ota.c waits for before it confirms a new image, and
         * nothing reads this in AP mode otherwise. */
        s_ever_connected = true;
        if (s_status.mode == WIFI_MGR_MODE_AP) {
            if (test_note_joined(&ev->ip_info.ip)) {
                return;
            }
            /* The fallback AP's retry got through: the router is back. Restart
             * rather than switch over live, for the reason the button hotspot
             * does - the boot path into station mode is the one that starts
             * Zigbee and Matter, and the stack stopped for the AP cannot simply
             * be started again. The credentials are stored, so the next boot
             * lands on this network. */
            ESP_LOGW(TAG, "Stored network \"%s\" is reachable again (got " IPSTR "); restarting onto it",
                     s_status.home_ssid, IP2STR(&ev->ip_info.ip));
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
            return;
        }
        s_fail_count          = 0;
        s_status.connected    = true;
        snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&ev->ip_info.ip));
        esp_timer_stop(s_dhcp_timer);
        ESP_LOGI(TAG, "Got IP %s", s_status.ip);
        start_mdns();
    }
}

/* `fallback`: the stored network could not be joined, so keep trying it from
 * under the AP - see s_fallback. */
static void start_ap_mode(bool fallback)
{
    char     ssid[33];
    uint8_t  mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(ssid, sizeof(ssid), "elixcon-%02X%02X", mac[4], mac[5]);

    wifi_config_t cfg = {
        .ap = {
            .channel        = 1,
            .max_connection = 4,
            .authmode       = WIFI_AUTH_OPEN,
            .beacon_interval = 100,
        },
    };
    size_t ssid_len = strlcpy((char *)cfg.ap.ssid, ssid, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = (uint8_t)ssid_len;

    const char *pass = CONFIG_ZBC_AP_PASSWORD;
    if (strlen(pass) >= 8) {
        strlcpy((char *)cfg.ap.password, pass, sizeof(cfg.ap.password));
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    esp_timer_stop(s_retry_timer);
    /* The 802.15.4 receiver never idles, and clients cannot associate with the
     * SoftAP while it holds the shared antenna. */
    zb_coordinator_stop();

    /* The AP config has to be in place before the interface comes up, otherwise
     * it beacons with an empty SSID. When we fall back here from a failed STA
     * attempt, Wi-Fi is already running and has to be stopped first. */
    if (s_wifi_started) {
        esp_wifi_disconnect();
        ESP_ERROR_CHECK(esp_wifi_stop());
        s_wifi_started = false;
    }
    /* The mode is recorded before the interface comes up: the event handler
     * reads it when WIFI_EVENT_STA_START arrives, and that event is delivered on
     * another task as soon as esp_wifi_start() returns. */
    if (fallback) {
        /* The network being retried, for the log and the status page. */
        strlcpy(s_status.home_ssid, s_status.ssid, sizeof(s_status.home_ssid));
    }
    s_fallback         = fallback;
    s_status.mode      = WIFI_MGR_MODE_AP;
    s_status.connected = false;
    strlcpy(s_status.ssid, ssid, sizeof(s_status.ssid));
    strlcpy(s_status.ip, "192.168.4.1", sizeof(s_status.ip));

    /* APSTA rather than AP so the setup page can still scan for networks. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_started = true;

    wifi_config_t readback = {0};
    esp_wifi_get_config(WIFI_IF_AP, &readback);
    ESP_LOGI(TAG, "Provisioning AP \"%s\" (channel %d, auth %d) is up, open http://192.168.4.1",
             (const char *)readback.ap.ssid, readback.ap.channel, readback.ap.authmode);

    captive_dns_start();

    if (fallback) {
        if (!s_fallback_timer) {
            const esp_timer_create_args_t args = {
                .callback = fallback_retry_cb,
                .name     = "wifi_fallback",
            };
            ESP_ERROR_CHECK(esp_timer_create(&args, &s_fallback_timer));
        }
        esp_timer_start_once(s_fallback_timer, (uint64_t)FALLBACK_RETRY_S * 1000000ULL);
        ESP_LOGW(TAG, "Will keep trying \"%s\" every %d s and restart onto it when it answers",
                 s_status.home_ssid, FALLBACK_RETRY_S);
    }
}

static bool load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t nvs;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t    len = ssid_len;
    esp_err_t err = nvs_get_str(nvs, WIFI_NVS_SSID, ssid, &len);
    if (err == ESP_OK) {
        len = pass_len;
        if (nvs_get_str(nvs, WIFI_NVS_PASS, pass, &len) != ESP_OK) {
            pass[0] = '\0';
        }
    }
    nvs_close(nvs);
    return err == ESP_OK && ssid[0] != '\0';
}

bool wifi_manager_has_credentials(void)
{
    char ssid[33] = {0}, pass[65] = {0};
    return load_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
}

esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    esp_err_t    err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs, WIFI_NVS_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, WIFI_NVS_PASS, password ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t wifi_manager_erase_credentials(void)
{
    nvs_handle_t nvs;
    esp_err_t    err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_key(nvs, WIFI_NVS_SSID);
    nvs_erase_key(nvs, WIFI_NVS_PASS);
    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

/* ------------------------------------------------ the button-started hotspot */

/* The fallback AP above opens only when a join fails, so a box associated with a
 * network nobody can reach - the wrong one, a guest VLAN, an address nobody
 * knows - needs another way back. Ten presses at the board are that way.
 *
 * It restarts rather than switching over live because the boot path into the
 * hotspot never starts Zigbee or Matter, so there is nothing to stop and nothing
 * sharing the antenna with the SoftAP. */
esp_err_t wifi_manager_setup_hotspot(void)
{
    nvs_handle_t nvs;
    esp_err_t    err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, WIFI_NVS_SETUP_ONCE, 1);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not store the setup hotspot request (%s)", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(200));   /* let the log line reach the console */
    esp_restart();
    return ESP_OK;
}

/* Read and erased in one go, before anything else at boot can go wrong: a flag
 * that survived a crash would bring the box up as a hotspot at every boot, and
 * the only way out would be the setup page. So a power cut always ends up back on
 * the stored network. */
static bool take_setup_flag(void)
{
    nvs_handle_t nvs;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    uint8_t    v   = 0;
    const bool set = nvs_get_u8(nvs, WIFI_NVS_SETUP_ONCE, &v) == ESP_OK && v;
    if (set) {
        nvs_erase_key(nvs, WIFI_NVS_SETUP_ONCE);
        nvs_commit(nvs);
    }
    nvs_close(nvs);
    return set;
}

/* esp_timer context. No lock is taken, so the rule about the button sampler does
 * not bite; a restart is the end of everything anyway. */
static void hotspot_return_cb(void *arg)
{
    (void)arg;
    const int64_t   now = esp_timer_get_time();
    wifi_sta_list_t sta;
    /* Somebody is on it, most likely half way through typing a password. Wait a
     * minute at a time - but not for ever: a phone that auto-joins an open network
     * and stays would otherwise keep the box off its own network indefinitely. */
    if (now + 60 * 1000000LL <= s_return_deadline_us && esp_wifi_ap_get_sta_list(&sta) == ESP_OK &&
        sta.num > 0) {
        s_return_at_us = now + 60 * 1000000LL;
        esp_timer_start_once(s_return_timer, 60 * 1000000ULL);
        ESP_LOGI(TAG, "%d device(s) still on the setup hotspot; going back in a minute", sta.num);
        return;
    }
    ESP_LOGW(TAG, "No new network was saved; restarting onto \"%s\"", s_status.home_ssid);
    esp_restart();
}

esp_err_t wifi_manager_scan(wifi_ap_record_t **records, uint16_t *count)
{
    if (!records || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    *records = NULL;
    *count   = 0;

    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        return err;
    }
    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        return ESP_OK;
    }
    if (found > 32) {
        found = 32;
    }
    wifi_ap_record_t *buf = calloc(found, sizeof(wifi_ap_record_t));
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    err = esp_wifi_scan_get_ap_records(&found, buf);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    *records = buf;
    *count   = found;
    return ESP_OK;
}

wifi_mgr_mode_t wifi_manager_mode(void)
{
    return s_status.mode;
}

bool wifi_manager_station_joined(void)
{
    return s_ever_connected;
}

esp_err_t wifi_manager_test(const char *ssid, const char *password)
{
    if (!password) {
        password = "";
    }
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || strlen(password) > 64) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_status.mode != WIFI_MGR_MODE_AP) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_test_mux);
    const bool busy = s_test.st.state == WIFI_TEST_RUNNING || s_test.st.state == WIFI_TEST_JOINED;
    if (!busy) {
        memset(&s_test, 0, sizeof(s_test));
        s_test.st.state = WIFI_TEST_RUNNING;
        strlcpy(s_test.st.ssid, ssid, sizeof(s_test.st.ssid));
        strlcpy(s_test.pass, password, sizeof(s_test.pass));
    }
    portEXIT_CRITICAL(&s_test_mux);
    if (busy) {
        return ESP_ERR_NOT_FINISHED;
    }

    if (s_fallback) {
        /* Its retries stop for the check, and one in flight is ended - which
         * reads as one more try of the check's own, not as a verdict. */
        esp_timer_stop(s_fallback_timer);
        esp_wifi_disconnect();
    }
    wifi_config_t cfg;
    sta_config(&cfg, ssid, password);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Check of \"%s\" could not start (%s)", ssid, esp_err_to_name(err));
        portENTER_CRITICAL(&s_test_mux);
        s_test.st.state = WIFI_TEST_FAILED;
        memset(s_test.pass, 0, sizeof(s_test.pass));
        portEXIT_CRITICAL(&s_test_mux);
        test_finished();
        return err;
    }
    ESP_LOGI(TAG, "Checking \"%s\" from the setup hotspot", ssid);
    return ESP_OK;
}

void wifi_manager_test_status(wifi_test_status_t *out)
{
    portENTER_CRITICAL(&s_test_mux);
    *out = s_test.st;
    portEXIT_CRITICAL(&s_test_mux);
}

void wifi_manager_get_status(wifi_mgr_status_t *out)
{
    if (!out) {
        return;
    }
    *out = s_status;
    strlcpy(out->hostname, settings_hostname(), sizeof(out->hostname));
    if (s_status.temporary) {
        const int64_t left = s_return_at_us - esp_timer_get_time();
        out->returns_in_s  = left > 0 ? (int)(left / 1000000) : 0;
    }
    if (s_status.mode == WIFI_MGR_MODE_STA && s_status.connected) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out->rssi    = ap.rssi;
            out->channel = ap.primary;
            out->weak    = ap.rssi < WIFI_WEAK_RSSI_DBM;

            /* Edge-triggered: the UI polls this every 2 s and a line per poll
             * would bury everything else. Read from more than one task, so a
             * torn update costs at most a repeated line - it is a log, not state. */
            if (out->weak != (s_weak_logged == 1)) {
                s_weak_logged = out->weak ? 1 : 0;
                if (out->weak) {
                    ESP_LOGW(TAG, "Uplink is weak: %d dBm on channel %d. Wi-Fi and "
                                  "802.15.4 share one antenna here, so this costs the "
                                  "Zigbee mesh airtime and reads as unreliable lamps",
                             ap.rssi, ap.primary);
                } else {
                    ESP_LOGI(TAG, "Uplink signal recovered to %d dBm", ap.rssi);
                }
            }
        }
    }
}

esp_err_t wifi_manager_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    s_netif_sta = esp_netif_create_default_wifi_sta();
    s_netif_ap  = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_netif_sta, settings_hostname());

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    /* Scan the whole 2.4 GHz band the way a phone does. ESP-IDF's default world
     * safe mode covers only channels 1-11, so an access point on 12 or 13 does
     * not exist as far as we are concerned and every join ends in "no AP found".
     * The country stays unclaimed ("01") and the policy stays AUTO, so once we
     * associate, the regulatory domain from the AP's own beacon takes over. */
    const wifi_country_t country = {
        .cc     = "01",
        .schan  = 1,
        .nchan  = 13,
        .policy = WIFI_COUNTRY_POLICY_AUTO,
    };
    ESP_ERROR_CHECK(esp_wifi_set_country(&country));

    const esp_timer_create_args_t timer_args = {
        .callback = retry_connect_cb,
        .name     = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_retry_timer));

    const esp_timer_create_args_t dhcp_timer_args = {
        .callback = dhcp_deadline_cb,
        .name     = "wifi_dhcp",
    };
    ESP_ERROR_CHECK(esp_timer_create(&dhcp_timer_args, &s_dhcp_timer));

    char ssid[33] = {0}, pass[65] = {0};
    bool have_creds = load_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
    /* Taken even when nothing is stored, so it cannot linger until something is. */
    const bool setup_once = take_setup_flag();

    if (have_creds && !setup_once) {
        wifi_config_t cfg;
        sta_config(&cfg, ssid, pass);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
        s_status.mode = WIFI_MGR_MODE_STA;
        strlcpy(s_status.ssid, ssid, sizeof(s_status.ssid));
        ESP_LOGI(TAG, "Joining \"%s\"", ssid);
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
    } else {
        start_ap_mode(false);
        if (have_creds) {
            /* Asked for by ten button presses. Saving a network through the setup
             * page restarts onto that one; doing nothing restarts onto this one. */
            const esp_timer_create_args_t return_args = {
                .callback = hotspot_return_cb,
                .name     = "wifi_return",
            };
            ESP_ERROR_CHECK(esp_timer_create(&return_args, &s_return_timer));
            const int64_t now    = esp_timer_get_time();
            const int64_t wait   = (int64_t)CONFIG_ZBC_SETUP_HOTSPOT_S * 1000000;
            s_return_at_us       = now + wait;
            s_return_deadline_us = now + wait * 3;
            s_status.temporary   = true;
            strlcpy(s_status.home_ssid, ssid, sizeof(s_status.home_ssid));
            ESP_ERROR_CHECK(esp_timer_start_once(s_return_timer, (uint64_t)wait));
            ESP_LOGW(TAG, "Setup hotspot started by the button; back to \"%s\" in %d s unless a network is saved",
                     ssid, CONFIG_ZBC_SETUP_HOTSPOT_S);
        }
    }

    /* Listening without pause to begin with. The updater wants exactly that - a
     * dozing station makes every TCP round trip of an upload wait for a beacon -
     * and the coordinator decides for itself right after this returns: see
     * wifi_manager_set_modem_sleep(). Modem sleep does not cost DHCP replies;
     * declined leases come from lwIP's ARP check (see
     * CONFIG_LWIP_DHCP_DOES_NOT_CHECK_OFFERED_IP). */
    esp_wifi_set_ps(WIFI_PS_NONE);
    return ESP_OK;
}

esp_err_t wifi_manager_set_modem_sleep(bool on)
{
    esp_err_t err = esp_wifi_set_ps(on ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not set Wi-Fi power save (0x%x)", err);
        return err;
    }
    /* Said in the same words as the settings page, because this is the line to
     * look for when deciding whether a remote's lost presses are the antenna. */
    ESP_LOGI(TAG, "Wi-Fi %s between beacons, so the Zigbee radio %s",
             on ? "dozes" : "never sleeps", on ? "gets the antenna in between" : "listens only when Wi-Fi is idle");
    return ESP_OK;
}
