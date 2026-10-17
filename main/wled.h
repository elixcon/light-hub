/* The WLED interface: the coordinator seen by Hyperion.ng, HyperHDR, LedFx and
 * other WLED clients as a WLED strip, one pixel per Hue colour lamp of one room
 * (lighting_stream_lamps). Pixels arrive by DDP (UDP 4048) or WLED's own UDP
 * protocol (UDP 21324) and go to lighting_stream_put; the client's /json
 * requests are answered from here through http_server.c.
 *
 * Off unless settings_wled_on(), because WLED has no keys: while it is on,
 * anything on the network can switch the room and stream to it.
 *
 * Built only with CONFIG_ZBC_WLED (on by default); without it wled_start() and
 * wled_reload() are empty inlines and the rest is not declared. */
#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_ZBC_WLED

/* After Zigbee and Matter are up, never in the setup portal. Starts listening if
 * the setting is on. */
void   wled_start(void);
/* The setting changed: starts or stops listening, ends a stream to a room no
 * longer chosen, and adds or removes the mDNS service. */
void   wled_reload(void);

/* WLED's own answers: /json/info, /json/state, /json/cfg. */
cJSON *wled_info_json(void);
cJSON *wled_state_json(void);
cJSON *wled_cfg_json(void);
/* A WLED state request (`live`, `on`, `bri`). True when it asked for the state
 * back ("v": true). */
bool   wled_apply_state(const cJSON *body);

/* For /api/status: what the interface and its stream are doing. */
cJSON *wled_status_json(void);

#else

static inline void wled_start(void) {}
static inline void wled_reload(void) {}

#endif

#ifdef __cplusplus
}
#endif
