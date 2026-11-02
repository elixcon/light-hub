/* The node's Zigbee side: an end device whose receiver stays on, one endpoint per
 * wired light, each an ordinary lamp to whatever coordinator it joined. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Endpoint 200 (node_proto.h) is the control endpoint; a light's endpoint is its
 * light_db id. */
#define NODE_LIGHT_EP_MAX 199

/* `with_wifi` asks for the antenna to be shared. Not asked otherwise: the arbiter has
 * nothing to do with no Wi-Fi to arbitrate against. */
esp_err_t node_zb_start(bool with_wifi);

/* One gesture of a wired button, to the hub. The verdict comes to
 * node_buttons_delivered, with `seq`. ESP_FAIL when it could not even be queued. */
esp_err_t node_zb_send_button(uint8_t key, uint8_t event, uint8_t seq);

/* Tells the hub what Wi-Fi is doing, when there is a network to tell it on. */
void node_zb_send_wifi_state(uint8_t state, const uint8_t ipv4[4]);

/* Since when this boot has been on a Zigbee network, 0 while it has not. */
int64_t node_zb_joined_at(void);

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
    /* The weakest parent this boot's rejoin would take, and how many steps down that was. */
    uint8_t  join_floor;
    uint8_t  rejoins_failed;
    uint32_t joined_after_ms;
} node_zb_stats_t;
void node_zb_stats(node_zb_stats_t *out);

#ifdef __cplusplus
}
#endif
