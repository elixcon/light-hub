/* The node's Zigbee side: an end device whose receiver stays on, one endpoint per
 * wired light, each an ordinary lamp to whatever coordinator it joined. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Endpoint 200 is the control endpoint; a light's endpoint is its light_db id. */
#define NODE_CONTROL_EP 200
#define NODE_LIGHT_EP_MAX 199

esp_err_t node_zb_start(void);

/* Any task. A light may have changed: the attributes follow what it is doing. */
void node_zb_state_changed(void);

/* Erases the stack's stored network from NVS. Only before node_zb_start. */
esp_err_t node_zb_forget_network(void);

/* For the diagnostics page. */
typedef struct {
    bool     started;
    bool     joined;
    bool     rx_on;
    uint16_t short_addr;
    uint16_t pan_id;
    uint8_t  channel;
    uint32_t steer_attempts;
    uint32_t frames;
    uint32_t handled;
    uint32_t duplicates;
    uint32_t refused;
    uint32_t attr_syncs;
    /* The parent, as the neighbour table has it; 0xFFFF until there is one. */
    uint16_t parent_short;
    uint8_t  parent_lqi;
    int8_t   parent_rssi;
    uint8_t  parent_type;
    uint8_t  neighbours;
    uint8_t  parent_ieee[8];
} node_zb_stats_t;
void node_zb_stats(node_zb_stats_t *out);

/* The last signals and frames, newest last, for the diagnostics page. */
typedef struct {
    uint32_t at_ms;
    uint16_t src;
    uint16_t cluster;
    uint8_t  dst_ep;
    uint8_t  cmd;
    uint8_t  fc;
    uint8_t  handled;
} node_zb_frame_t;
int  node_zb_frames(node_zb_frame_t *out, int max);

typedef struct {
    uint32_t at_ms;
    char     name[28];
    int      status;
} node_zb_signal_t;
int  node_zb_signals(node_zb_signal_t *out, int max);

#ifdef __cplusplus
}
#endif
