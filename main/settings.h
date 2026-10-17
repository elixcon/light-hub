/* Global preferences, persisted in NVS.
 *
 * A leaf module on purpose: lighting.c, zb_coordinator.c and http_server.c all
 * read from it, so it must not depend on any of them.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How long a lamp should take to move between states, in tenths of a second.
 * 0 is instant; the ZCL field is a uint16 of the same unit. */
/* 0.8 s, which is what switching off actually takes: OffWithEffect fades over a
 * fixed 0.8 s and has no duration field, so matching it here makes on and off
 * symmetric out of the box instead of one being visibly slower than the other. */
#define ZBC_TRANSITION_DS_DEFAULT 8
#define ZBC_TRANSITION_DS_MAX     100   /* 10 s; longer is a scene, not a transition */

/* What this coordinator calls itself. One setting, two forms: the name as it was
 * typed, and the DNS label derived from it. 32 is what both ends allow - Matter's
 * NodeLabel caps there, and wifi_mgr_status_t::hostname is char[32]. */
#define ZBC_COORD_NAME_LEN     32
#define ZBC_COORD_NAME_DEFAULT "Elixcon Light Hub"
#define ZBC_COORD_HOST_DEFAULT "zigbee"

esp_err_t settings_init(void);

uint16_t  settings_transition_ds(void);
esp_err_t settings_set_transition_ds(uint16_t ds);

/* Whether the Wi-Fi radio dozes between the access point's DTIM beacons
 * (WIFI_PS_MIN_MODEM) or listens without pause (WIFI_PS_NONE). On this chip the
 * two radios share one antenna and 802.15.4 *receive* has the lowest priority
 * of anything the arbiter sees, so a Wi-Fi that never sleeps is a Zigbee
 * receiver that is deaf whenever Wi-Fi feels like listening - which is what a
 * remote's press runs into: one frame, a few MAC retries inside ten
 * milliseconds, and nothing above that to say it again. Espressif's own gateway
 * example picks MIN_MODEM exactly when software coexistence is on. The default
 * is therefore on; off is kept because a dozing station answers a few tens of
 * milliseconds later, and somebody may prefer the snappier page to the remote.
 * The updater never reads this: it uploads firmware and has no 802.15.4 to
 * protect. */
bool      settings_wifi_sleep(void);
esp_err_t settings_set_wifi_sleep(bool on);

/* Leave a lamp at its warmest white while it is switched off, so it lights warm
 * next time instead of coming back at whatever the day left it at. The colour
 * is sent while the lamp is still lit, as one command with the off (see
 * off_endpoint in lighting.c). Off by default: it is a change to what every
 * colour-temperature lamp does. */
bool      settings_warm_off(void);
esp_err_t settings_set_warm_off(bool on);

/* The WLED interface (wled.c): whether it answers, and the room it drives - a
 * group id, 0 for none. Off by default, because while it is on anything on the
 * network can switch that room and stream to it: WLED has no keys.
 * `others_off`: the room's lights that cannot take part are switched off while
 * it streams and back on after it (lighting.c); off by default. */
bool      settings_wled_on(void);
uint16_t  settings_wled_room(void);
bool      settings_wled_others_off(void);
esp_err_t settings_set_wled(bool on, uint16_t room, bool others_off);

/* The Zigbee channel the network was last moved to (zb_move_channel), 11-26, or
 * 0 while it has never been moved - it is then on whatever it was formed on.
 * Forming a network uses it too, ahead of CONFIG_ZBC_ZIGBEE_CHANNEL.
 *
 * This, not the stack's own record, is the authority: esp-zigbee-lib does not
 * save a channel changed under a running network, and comes back on the old one
 * after a restart, so the coordinator moves itself here again at every boot.
 * `update_id` is the nwkUpdateId the move was announced with; the stack cannot
 * advance its own while on a network, so the next move counts on from this.
 * Channel 0 forgets the channel and keeps the id. */
uint8_t   settings_zigbee_channel(void);
uint8_t   settings_zigbee_update_id(void);
esp_err_t settings_set_zigbee_channel(uint8_t channel, uint8_t update_id);

/* Both are cached and never empty, so they can be handed straight to mdns and
 * esp_netif without a length check. */
const char *settings_name(void);       /* as typed: "Living Room Hub" */
const char *settings_hostname(void);   /* derived: "living-room-hub", for .local and DHCP */
/* An empty or NULL name restores the defaults above - the only way back to
 * zigbee.local once a name has been set. */
esp_err_t   settings_set_name(const char *name);

/* The time zone, as a POSIX TZ rule (tz.h) and the name it was made from, for
 * Adaptive Lighting's sunrise and sunset limits (al_db.h) and nothing else. Both
 * empty while none is set. The page writes the rule from the browser's own zone. */
#define ZBC_TZ_NAME_LEN 48
bool      settings_tz(char *posix, size_t posix_len, char *name, size_t name_len);
/* NULL or "" forgets the zone. ESP_ERR_INVALID_ARG for a rule tz.c cannot read
 * or a name that is not an IANA-style one. */
esp_err_t settings_set_tz(const char *posix, const char *name);
/* The test settings_set_tz applies to a zone it is to store. */
bool      settings_tz_ok(const char *posix, const char *name);
/* The stored zone's offset east of UTC at `utc_s`. False, and 0, without one. */
bool      settings_tz_offset(int64_t utc_s, int32_t *offset_s);

/* Where the coordinator is, in millionths of a degree, north and east positive.
 * False while nobody has entered one - Adaptive Lighting waits until then. No
 * timezone: the sun model works in UTC. */
bool      settings_location(int32_t *lat_udeg, int32_t *lon_udeg);
/* `set` false forgets the location. */
esp_err_t settings_set_location(bool set, int32_t lat_udeg, int32_t lon_udeg);

/* Overheat protection: an NTC thermistor on an ADC pin (thermal.c, ntc.h). While
 * it reads too hot, or reads as broken, every wired light is held off. `gpio` -1
 * is off, the default; the rest defaults to a converted Shelly Gen3/Gen4 (NTC on
 * GPIO 3 to GND, 10 kOhm to 3V3, B 3350, shutoff 75 °C), which the page offers
 * as a preset. */
#define ZBC_THERMAL_B_MIN       1000
#define ZBC_THERMAL_B_MAX       10000
#define ZBC_THERMAL_OHM_MIN     100
#define ZBC_THERMAL_OHM_MAX     1000000
#define ZBC_THERMAL_TRIP_MIN_C  40
#define ZBC_THERMAL_TRIP_MAX_C  120

typedef struct {
    int8_t   gpio;          /* -1: no sensor */
    uint16_t b;             /* B constant, kelvin */
    uint32_t r0_ohm;        /* NTC at 25 °C */
    uint32_t rs_ohm;        /* the divider's fixed resistor */
    bool     ntc_to_3v3;    /* false: NTC between the pin and GND */
    uint8_t  trip_c;        /* shutoff temperature */
} zbc_thermal_cfg_t;

void      settings_thermal(zbc_thermal_cfg_t *out);
/* ESP_ERR_INVALID_ARG for a value out of range; nothing is stored then. The pin
 * itself is checked by the caller, which knows the other pins in use. */
esp_err_t settings_set_thermal(const zbc_thermal_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
