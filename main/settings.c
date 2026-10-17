#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "settings.h"
#include "tz.h"

static const char *TAG = "settings";

/* Single scalars rather than one versioned blob: a blob would have to be
 * discarded whenever a setting is added, and there is nothing here worth
 * losing. Same namespace as the device and group tables.
 *
 * Every key is also in config_backup.c's CB_ENTRIES, with its type - all but
 * the Zigbee channel and its update id, which belong to the network. A new
 * setting belongs there too, or a backup silently leaves it out. */
#define SETTINGS_NVS_NAMESPACE "zbc"
#define KEY_TRANSITION_DS      "trans_ds"
#define KEY_NAME               "name"
#define KEY_LAT                "loc_lat"
#define KEY_LON                "loc_lon"
#define KEY_TZ                 "tz"
#define KEY_TZ_NAME            "tz_name"
#define KEY_WIFI_SLEEP         "wifi_sleep"
#define KEY_WARM_OFF           "warm_off"
#define KEY_TH_GPIO            "th_gpio"
#define KEY_TH_B               "th_b"
#define KEY_TH_R0              "th_r0"
#define KEY_TH_RS              "th_rs"
#define KEY_TH_UP              "th_up"
#define KEY_TH_TRIP            "th_trip"
#define KEY_ZB_CHANNEL         "zb_chan"
#define KEY_ZB_UPDATE_ID       "zb_upd_id"
#define KEY_WLED_ON            "wled_on"
#define KEY_WLED_ROOM          "wled_room"
#define KEY_WLED_OTHERS_OFF    "wled_others"

static uint16_t s_transition_ds = ZBC_TRANSITION_DS_DEFAULT;
static bool     s_wifi_sleep    = true;
/* Off by default: it changes what every colour-temperature lamp does when it is
 * switched off, and an update arrives with nobody watching. */
static bool     s_warm_off;
static uint8_t  s_zb_channel;      /* 0: never moved */
static uint8_t  s_zb_update_id;
/* Off by default: while it is on, anything on the network can drive the room. */
static bool     s_wled_on;
static uint16_t s_wled_room;       /* 0: none chosen */
/* Off by default: the room's other lights were always left as they are. */
static bool     s_wled_others_off;

/* No zone until the page sends one: Adaptive Lighting's limits wait for it. */
static char s_tz[ZBC_TZ_MAX_LEN + 1];
static char s_tz_name[ZBC_TZ_NAME_LEN];
/* The zone is copied whole by the Adaptive Lighting task and the HTTP handlers,
 * and written by a POST: a torn copy could pair one rule with another's name. */
static portMUX_TYPE s_tz_mux = portMUX_INITIALIZER_UNLOCKED;

/* A converted Shelly Gen3/Gen4, with no pin until somebody picks one. */
static zbc_thermal_cfg_t s_thermal = {
    .gpio = -1, .b = 3350, .r0_ohm = 10000, .rs_ohm = 10000, .ntc_to_3v3 = false, .trip_c = 75,
};

static bool thermal_valid(const zbc_thermal_cfg_t *c)
{
    return c->gpio >= -1 && c->b >= ZBC_THERMAL_B_MIN && c->b <= ZBC_THERMAL_B_MAX &&
           c->r0_ohm >= ZBC_THERMAL_OHM_MIN && c->r0_ohm <= ZBC_THERMAL_OHM_MAX &&
           c->rs_ohm >= ZBC_THERMAL_OHM_MIN && c->rs_ohm <= ZBC_THERMAL_OHM_MAX &&
           c->trip_c >= ZBC_THERMAL_TRIP_MIN_C && c->trip_c <= ZBC_THERMAL_TRIP_MAX_C;
}

static bool    s_located;
static int32_t s_lat_udeg, s_lon_udeg;

/* What tz_name may hold: an IANA zone name ("Europe/Berlin", "America/Port-au-Prince",
 * "Etc/GMT+1"), only ever shown. */
static bool tz_name_valid(const char *name)
{
    if (strlen(name) >= ZBC_TZ_NAME_LEN) {
        return false;
    }
    for (const char *c = name; *c; c++) {
        const bool ok = (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') ||
                        *c == '/' || *c == '_' || *c == '-' || *c == '+';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* The two defaults are deliberately not a pair: the slug of "Elixcon Light Hub"
 * is "elixcon-light-hub", and deriving it would move every box that has never
 * been named from zigbee.local to a new address. Once a name is set, the two are
 * derived from each other and stay in step. */
static char s_name[ZBC_COORD_NAME_LEN] = ZBC_COORD_NAME_DEFAULT;
static char s_host[ZBC_COORD_NAME_LEN] = ZBC_COORD_HOST_DEFAULT;

/* >>> host-test region: slug <<< */
/* A name is one field and has to serve two masters: it is shown to people (the
 * mDNS instance name, the Matter node label) and it is a DNS label (.local, the
 * DHCP client name). Deriving the label rather than asking for it separately is
 * what keeps that one field - and the derivation is the whole of the rule, so it
 * is worth being exact about the edges:
 *
 *  - anything that is not a-z0-9 becomes a hyphen, including UTF-8 bytes, which
 *    is why "Küche" comes out as "k-che" rather than as mojibake;
 *  - runs of hyphens collapse, and a leading one is never emitted;
 *  - trailing hyphens are trimmed *after* the truncation, because cutting the
 *    name at the buffer is itself a way to expose one;
 *  - a name with no letters or digits in it at all slugs to nothing, and that is
 *    reported rather than quietly replaced with a fallback.
 */
static bool slugify(const char *in, char *out, size_t cap)
{
    if (!in || !out || cap < 2) {
        return false;
    }
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && n + 1 < cap; p++) {
        char c;
        if (*p >= 'A' && *p <= 'Z') {
            c = (char)(*p - 'A' + 'a');
        } else if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) {
            c = (char)*p;
        } else {
            c = '-';
        }
        if (c == '-' && (n == 0 || out[n - 1] == '-')) {
            continue;
        }
        out[n++] = c;
    }
    while (n > 0 && out[n - 1] == '-') {
        n--;
    }
    out[n] = '\0';
    return n > 0;
}
/* >>> end host-test region: slug <<< */

esp_err_t settings_init(void)
{
    nvs_handle_t nvs;
    if (nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return ESP_OK;   /* nothing stored yet; the defaults stand */
    }
    uint16_t v = 0;
    if (nvs_get_u16(nvs, KEY_TRANSITION_DS, &v) == ESP_OK && v <= ZBC_TRANSITION_DS_MAX) {
        s_transition_ds = v;
        ESP_LOGI(TAG, "Transition time %u.%u s", v / 10, v % 10);
    }

    char   name[ZBC_COORD_NAME_LEN] = "";
    char   host[ZBC_COORD_NAME_LEN];
    size_t len = sizeof(name);
    if (nvs_get_str(nvs, KEY_NAME, name, &len) == ESP_OK && slugify(name, host, sizeof(host))) {
        strlcpy(s_name, name, sizeof(s_name));
        strlcpy(s_host, host, sizeof(s_host));
    }
    ESP_LOGI(TAG, "Called \"%s\", reachable at http://%s.local", s_name, s_host);

    int32_t lat = 0, lon = 0;
    if (nvs_get_i32(nvs, KEY_LAT, &lat) == ESP_OK && nvs_get_i32(nvs, KEY_LON, &lon) == ESP_OK &&
        lat >= -90000000 && lat <= 90000000 && lon >= -180000000 && lon <= 180000000) {
        s_located  = true;
        s_lat_udeg = lat;
        s_lon_udeg = lon;
    }
    uint8_t sleep = 1;
    if (nvs_get_u8(nvs, KEY_WIFI_SLEEP, &sleep) == ESP_OK) {
        s_wifi_sleep = sleep != 0;
    }
    uint8_t warm = 0;
    if (nvs_get_u8(nvs, KEY_WARM_OFF, &warm) == ESP_OK) {
        s_warm_off = warm != 0;
    }
    uint8_t ch = 0;
    if (nvs_get_u8(nvs, KEY_ZB_CHANNEL, &ch) == ESP_OK && ch >= 11 && ch <= 26) {
        s_zb_channel = ch;
        ESP_LOGI(TAG, "Zigbee channel %u", ch);
    }
    nvs_get_u8(nvs, KEY_ZB_UPDATE_ID, &s_zb_update_id);
    uint8_t wled = 0;
    if (nvs_get_u8(nvs, KEY_WLED_ON, &wled) == ESP_OK) {
        s_wled_on = wled != 0;
    }
    nvs_get_u16(nvs, KEY_WLED_ROOM, &s_wled_room);
    uint8_t others = 0;
    if (nvs_get_u8(nvs, KEY_WLED_OTHERS_OFF, &others) == ESP_OK) {
        s_wled_others_off = others != 0;
    }

    char tz[ZBC_TZ_MAX_LEN + 1];
    len = sizeof(tz);
    if (nvs_get_str(nvs, KEY_TZ, tz, &len) == ESP_OK && zbc_tz_valid(tz)) {
        strlcpy(s_tz, tz, sizeof(s_tz));
        len = sizeof(s_tz_name);
        if (nvs_get_str(nvs, KEY_TZ_NAME, s_tz_name, &len) != ESP_OK || !tz_name_valid(s_tz_name)) {
            s_tz_name[0] = '\0';
        }
        ESP_LOGI(TAG, "Time zone %s (%s)", s_tz_name[0] ? s_tz_name : "unnamed", s_tz);
    }

    zbc_thermal_cfg_t th = s_thermal;
    uint8_t           up = 0;
    nvs_get_i8(nvs, KEY_TH_GPIO, &th.gpio);
    nvs_get_u16(nvs, KEY_TH_B, &th.b);
    nvs_get_u32(nvs, KEY_TH_R0, &th.r0_ohm);
    nvs_get_u32(nvs, KEY_TH_RS, &th.rs_ohm);
    if (nvs_get_u8(nvs, KEY_TH_UP, &up) == ESP_OK) {
        th.ntc_to_3v3 = up != 0;
    }
    nvs_get_u8(nvs, KEY_TH_TRIP, &th.trip_c);
    if (thermal_valid(&th)) {
        s_thermal = th;
    }

    nvs_close(nvs);
    return ESP_OK;
}

uint16_t settings_transition_ds(void)
{
    return s_transition_ds;
}

esp_err_t settings_set_transition_ds(uint16_t ds)
{
    if (ds > ZBC_TRANSITION_DS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    s_transition_ds = ds;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u16(nvs, KEY_TRANSITION_DS, ds);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

bool settings_wifi_sleep(void)
{
    return s_wifi_sleep;
}

esp_err_t settings_set_wifi_sleep(bool on)
{
    s_wifi_sleep = on;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(nvs, KEY_WIFI_SLEEP, on ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

bool settings_warm_off(void)
{
    return s_warm_off;
}

bool settings_wled_on(void)
{
    return s_wled_on;
}

uint16_t settings_wled_room(void)
{
    return s_wled_room;
}

bool settings_wled_others_off(void)
{
    return s_wled_others_off;
}

esp_err_t settings_set_wled(bool on, uint16_t room, bool others_off)
{
    s_wled_on         = on;
    s_wled_room       = room;
    s_wled_others_off = others_off;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(nvs, KEY_WLED_ON, on ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, KEY_WLED_ROOM, room);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, KEY_WLED_OTHERS_OFF, others_off ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t settings_set_warm_off(bool on)
{
    s_warm_off = on;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(nvs, KEY_WARM_OFF, on ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

uint8_t settings_zigbee_channel(void)
{
    return s_zb_channel;
}

uint8_t settings_zigbee_update_id(void)
{
    return s_zb_update_id;
}

esp_err_t settings_set_zigbee_channel(uint8_t channel, uint8_t update_id)
{
    if (channel != 0 && (channel < 11 || channel > 26)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    if (channel) {
        err = nvs_set_u8(nvs, KEY_ZB_CHANNEL, channel);
    } else {
        err = nvs_erase_key(nvs, KEY_ZB_CHANNEL);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, KEY_ZB_UPDATE_ID, update_id);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    /* Only what reached flash counts: a move announced on a channel that would
     * be forgotten at the next restart strands every lamp. */
    if (err == ESP_OK) {
        s_zb_channel   = channel;
        s_zb_update_id = update_id;
    }
    return err;
}

const char *settings_name(void)
{
    return s_name;
}

const char *settings_hostname(void)
{
    return s_host;
}

/* An empty name means "back to the default", and it is the only way to get
 * there: once anything is stored, the default *pair* above stops applying, and
 * typing "Elixcon Light Hub" would give the hostname elixcon-light-hub rather
 * than zigbee. Erasing the key rather than storing the default string is what
 * keeps that one special case in one place. */
esp_err_t settings_set_name(const char *name)
{
    bool clear = !name || name[0] == '\0';
    char host[ZBC_COORD_NAME_LEN];
    if (!clear && (strlen(name) >= ZBC_COORD_NAME_LEN || !slugify(name, host, sizeof(host)))) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = clear ? nvs_erase_key(nvs, KEY_NAME) : nvs_set_str(nvs, KEY_NAME, name);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;   /* clearing a name that was never set */
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }

    /* Only after the store succeeds: the caches are what everything else reads,
     * and a name that is live but not persisted comes back changed at the next
     * boot. The hostname does not take effect until then in any case - see the
     * three use sites in wifi_manager.c. */
    strlcpy(s_name, clear ? ZBC_COORD_NAME_DEFAULT : name, sizeof(s_name));
    strlcpy(s_host, clear ? ZBC_COORD_HOST_DEFAULT : host, sizeof(s_host));
    ESP_LOGI(TAG, "Renamed to \"%s\"; http://%s.local after the restart", s_name, s_host);
    return ESP_OK;
}

/* An absent key is how "none" is stored, so a backup made without a zone
 * erases one on restore rather than leaving it behind. */
static esp_err_t erase_key(nvs_handle_t nvs, const char *key)
{
    const esp_err_t err = nvs_erase_key(nvs, key);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

bool settings_tz(char *posix, size_t posix_len, char *name, size_t name_len)
{
    portENTER_CRITICAL(&s_tz_mux);
    if (posix) {
        strlcpy(posix, s_tz, posix_len);
    }
    if (name) {
        strlcpy(name, s_tz_name, name_len);
    }
    const bool set = s_tz[0] != '\0';
    portEXIT_CRITICAL(&s_tz_mux);
    return set;
}

bool settings_tz_ok(const char *posix, const char *name)
{
    return zbc_tz_valid(posix) && tz_name_valid(name ? name : "");
}

esp_err_t settings_set_tz(const char *posix, const char *name)
{
    const bool forget = !posix || !*posix;
    if (!name) {
        name = "";
    }
    if (!forget && !settings_tz_ok(posix, name)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    if (forget) {
        err = erase_key(nvs, KEY_TZ);
        if (err == ESP_OK) err = erase_key(nvs, KEY_TZ_NAME);
    } else {
        err = nvs_set_str(nvs, KEY_TZ, posix);
        if (err == ESP_OK) err = *name ? nvs_set_str(nvs, KEY_TZ_NAME, name) : erase_key(nvs, KEY_TZ_NAME);
    }
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_tz_mux);
        strlcpy(s_tz, forget ? "" : posix, sizeof(s_tz));
        strlcpy(s_tz_name, forget ? "" : name, sizeof(s_tz_name));
        portEXIT_CRITICAL(&s_tz_mux);
        if (forget) {
            ESP_LOGI(TAG, "Time zone forgotten");
        } else {
            ESP_LOGI(TAG, "Time zone %s (%s)", *name ? name : "unnamed", posix);
        }
    }
    return err;
}

bool settings_tz_offset(int64_t utc_s, int32_t *offset_s)
{
    char tz[ZBC_TZ_MAX_LEN + 1];
    const bool set = settings_tz(tz, sizeof(tz), NULL, 0);
    const int32_t off = set ? zbc_tz_offset(tz, utc_s) : 0;
    if (offset_s) {
        *offset_s = off;
    }
    return set;
}

bool settings_location(int32_t *lat_udeg, int32_t *lon_udeg)
{
    if (lat_udeg) {
        *lat_udeg = s_lat_udeg;
    }
    if (lon_udeg) {
        *lon_udeg = s_lon_udeg;
    }
    return s_located;
}

esp_err_t settings_set_location(bool set, int32_t lat_udeg, int32_t lon_udeg)
{
    if (set && (lat_udeg < -90000000 || lat_udeg > 90000000 || lon_udeg < -180000000 || lon_udeg > 180000000)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    if (set) {
        err = nvs_set_i32(nvs, KEY_LAT, lat_udeg);
        if (err == ESP_OK) err = nvs_set_i32(nvs, KEY_LON, lon_udeg);
    } else {
        err = nvs_erase_key(nvs, KEY_LAT);
        if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) err = nvs_erase_key(nvs, KEY_LON);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) {
        s_located  = set;
        s_lat_udeg = set ? lat_udeg : 0;
        s_lon_udeg = set ? lon_udeg : 0;
    }
    return err;
}

void settings_thermal(zbc_thermal_cfg_t *out)
{
    *out = s_thermal;
}

esp_err_t settings_set_thermal(const zbc_thermal_cfg_t *c)
{
    if (!c || !thermal_valid(c)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_i8(nvs, KEY_TH_GPIO, c->gpio);
    if (err == ESP_OK) err = nvs_set_u16(nvs, KEY_TH_B, c->b);
    if (err == ESP_OK) err = nvs_set_u32(nvs, KEY_TH_R0, c->r0_ohm);
    if (err == ESP_OK) err = nvs_set_u32(nvs, KEY_TH_RS, c->rs_ohm);
    if (err == ESP_OK) err = nvs_set_u8(nvs, KEY_TH_UP, c->ntc_to_3v3 ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(nvs, KEY_TH_TRIP, c->trip_c);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) {
        s_thermal = *c;
    }
    return err;
}
