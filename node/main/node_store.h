/* What a node keeps across restarts about itself, in NVS namespace "node". The lights,
 * the buttons and the Wi-Fi network are the shared modules' own, elsewhere. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "node_mode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Whether this flash has joined a Zigbee network as a node. */
bool     node_store_paired(void);
void     node_store_set_paired(bool paired);

/* True until this flash has been claimed as a node. A hub's stored network is not one
 * an end device can use; node_store_claim records that it has been dealt with. */
bool     node_store_foreign(void);
void     node_store_claim(void);

/* The one-shot request for the next boot, erased as it is read, so a power cut in that
 * boot lands in plain Zigbee rather than in Wi-Fi for ever. */
void     node_store_set_ask(node_ask_t ask, uint8_t minutes);
node_ask_t node_store_take_ask(uint8_t *minutes);

/* Bring the lights back as they are, whatever their power-on setting: this restart is
 * not a power cut. One-shot in the same way. */
void     node_store_set_resume(void);
bool     node_store_take_resume(void);

/* A digest of the endpoints the last boot registered, to tell the hub when they differ. */
uint32_t node_store_endpoints(void);
void     node_store_set_endpoints(uint32_t digest);

#ifdef __cplusplus
}
#endif
