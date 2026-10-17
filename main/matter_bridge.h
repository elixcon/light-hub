/* Matter over Wi-Fi: every light group appears as one dimmable light, and - when the
 * group asks for it - its Adaptive Lighting mode as a Mode Select on that light or
 * as a second on/off endpoint (see al_db.h).
 *
 * The coordinator is a Matter bridge - an Aggregator endpoint with one bridged
 * Dimmable Light per group, named after it. A Matter command becomes a call to
 * lighting_group_set_onoff() / lighting_group_set_brightness(), exactly as a
 * press in the web UI does, so curves, deadbands, the command gate and the
 * settle reads all apply and nothing below lighting.c learns Matter exists.
 *
 * Commissioned on the network only: the board is already on Wi-Fi, so it is
 * found over DNS-SD and BLE stays off the antenna Wi-Fi and 802.15.4 share.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "sdkconfig.h"

#define MATTER_MAX_LISTED_FABRICS 8

/* What the bridge last saw and did for one group, so that "the controller does not
 * follow" can be answered from the network: which of the group's state, Matter's
 * copy of it, and the write between them is the one that is wrong. -1 is
 * "unknown" throughout. */
typedef struct {
    uint16_t group_id;
    uint16_t endpoint_id;
    int8_t   group_on;        /* what lighting_group_state() reports */
    int16_t  group_pct;
    int8_t   matter_on;       /* what the Matter attributes hold */
    int16_t  matter_level;    /* CurrentLevel, 1-254 */
    bool     held;            /* sync holding off: a Matter command or sweep is settling */
    uint32_t writes;          /* attribute writes that succeeded */
    int32_t  last_err;        /* esp_err_t of the last write that failed, 0 if none has */
    uint8_t  expose;          /* zbc_al_expose_t the group asks for */
    bool     mode_select_live; /* the light endpoint has the Mode Select cluster - fixed until a restart */
    uint16_t al_endpoint;     /* the "<name> Adaptive" endpoint, 0 when there is none */
    int8_t   al_active;       /* the mode as lighting.c reports it, -1 unknown */
    int8_t   al_matter_on;    /* that endpoint's OnOff, -1 unknown or no endpoint */
    int8_t   matter_mode;     /* the light's Mode Select CurrentMode, -1 unknown or no cluster */
} matter_group_t;

/* One controller the bridge has been added to. A controller may add more than one
 * fabric - so the count alone does not say who is there, and this does. */
typedef struct {
    uint8_t  index;
    uint16_t vendor_id;       /* who commissioned it, as the controller declared */
    char     label[33];       /* the controller's own name for it, often "" */
    bool     ours;            /* this box's own fabric, for its Matter lamps (matter_ctl.h) */
    /* Whether the controller can see the bridge: a change reaches it only through
     * a live subscription. `subs_kept` are those CHIP keeps in nvs_matter and
     * resumes itself after a restart, with the largest intervals the controller
     * asked for and the most failed resumption attempts among them. */
    uint8_t  subs_live;
    uint8_t  subs_kept;
    uint16_t sub_min_s;
    uint16_t sub_max_s;
    uint8_t  resume_tries;
    int32_t  live_after_s;    /* uptime at which one was first live, -1 not yet */
} matter_fabric_t;

/* A command a controller sent to one of the bridge's endpoints, as it arrived.
 * Matter ids throughout. `level` is 0 unless the command moves to a level, and
 * `transition_ds` is MATTER_NO_TRANSITION unless the controller named one. */
#define MATTER_RECENT_COMMANDS 12
#define MATTER_NO_TRANSITION   0xFFFF
typedef struct {
    uint32_t at_ms;           /* esp_timer milliseconds */
    uint16_t endpoint;
    uint16_t cluster;
    uint8_t  command;
    uint8_t  fabric;          /* which controller, as in `fabrics` */
    uint8_t  level;
    uint16_t transition_ds;
} matter_command_t;

typedef struct {
    bool    running;
    uint8_t fabric_count;     /* controllers it has been added to */
    uint8_t fabric_listed;    /* entries used in `fabrics` */
    matter_fabric_t fabrics[MATTER_MAX_LISTED_FABRICS];
    bool    window_open;      /* a new controller can commission it now */
    uint8_t groups;           /* groups currently bridged */
    matter_group_t group_list[CONFIG_ZBC_MAX_GROUPS];
    char    manual_code[24];  /* 11-digit manual pairing code, "" until known */
    char    qr_code[64];      /* "MT:..." payload for a QR code, "" until known */
    uint32_t         commands_seen;  /* the next is written at this modulo the ring */
    matter_command_t commands[MATTER_RECENT_COMMANDS];
} matter_status_t;

/* Call after zb_coordinator_start(), and never in provisioning-AP mode. */
esp_err_t matter_bridge_start(void);

void      matter_bridge_get_status(matter_status_t *out);

/* Opens a commissioning window for 15 minutes, for adding the bridge to a
 * controller after the one opened at boot has timed out.
 * ESP_ERR_INVALID_STATE when Matter is not running. */
esp_err_t matter_bridge_open_window(void);

/* Forgets every controller and every bridged endpoint. The caller reboots. */
esp_err_t matter_bridge_erase(void);

#ifdef __cplusplus
}
#endif
