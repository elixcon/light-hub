/* A counter that moves whenever anything the web page shows has changed.
 *
 * The page polls `GET /api/state` every two seconds. Rebuilding the device,
 * group, button and remote tables as JSON for every poll would cost a cJSON tree
 * of tens of kilobytes on the heap and the airtime to carry it, on the antenna
 * the Zigbee mesh shares. With `?rev=` the server answers a poll that has nothing
 * new with the status and the access state alone.
 *
 * Every registry bumps this when it saves, and the runtime state that the
 * page shows without a save - a lamp's report, a button's pin, a remote's
 * press, a wired light switching - bumps it too. Anything shown by the page
 * that changes without passing through here is shown stale until something
 * else moves; the composite in http_server.c covers the one such thing, a
 * device going stale after ten minutes of silence.
 *
 * A leaf: nothing but an atomic, callable from any task and from a spinlock
 * or an esp_timer callback. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void     zbc_state_bump(void);
uint32_t zbc_state_rev(void);

#ifdef __cplusplus
}
#endif
