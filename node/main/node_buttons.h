/* The wired buttons of a node: the hub's gesture machines (btn_gesture.c) on a 10 ms
 * sampler, and each gesture sent to the hub as one key of the node's control endpoint.
 * The rules are the hub's. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "button_db.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t node_buttons_start(void);

/* Re-reads button_db and reconfigures the pins, for the page. */
void      node_buttons_reload(void);

/* The debounced state and raw level of a button's pin, for the page. */
bool      node_buttons_pin_state(uint16_t id, bool *pressed, int *level);

/* The light a button toggles when the hub is out of reach: its first short-press rule
 * that toggles a wired light of this node, or 0. An ordinary rule, so nothing new is
 * stored and a board's other rules are simply not looked at. */
uint16_t  node_buttons_fallback(const zbc_button_t *b);

/* The frame's verdict, from the Zigbee task: spinlock only, no blocking. */
void      node_buttons_delivered(uint8_t seq, bool delivered);

/* Whether the hub could not be reached the last time a press tried. */
bool      node_buttons_hub_down(void);

#ifdef __cplusplus
}
#endif
