/* The node's modes, from outside node_main.c. */
#pragma once

#include <stdint.h>

#include "node_mode.h"

#ifdef __cplusplus
extern "C" {
#endif

node_mode_t node_mode(void);

/* The page was used: Wi-Fi's time starts again. */
void node_touch(void);

/* Seconds until Wi-Fi closes and this boot restarts into Zigbee alone; -1 when it does
 * not. */
int  node_window_left_s(void);

/* Restarts into the mode `ask` leads to, with every light coming back as it is now.
 * Takes NVS and blocks: never from an esp_timer callback or a Zigbee callback. */
void node_restart_into(node_ask_t ask, uint8_t minutes);

#ifdef __cplusplus
}
#endif
