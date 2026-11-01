/* The private cluster between a hub and a Light Node. Both firmwares compile this,
 * and nothing else about each other. A leaf: no ESP-IDF and no Zigbee SDK.
 *
 * A node's lights are ordinary ZCL lamps. This is only for what no standard cluster
 * says: which wired button did what, and "open Wi-Fi so you can be configured".
 *
 * On the control endpoint, NODE_CONTROL_EP, which is how a hub knows a node from its
 * simple descriptor rather than from a model list. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NODE_PROTO_CLUSTER 0xFC80u
/* Espressif's manufacturer code: the SDK's EZB_ZCL_ESP_MANUF_CODE. */
#define NODE_PROTO_MANUF   0x131Bu

#define NODE_CONTROL_EP    200
/* The hub's own endpoint (ZBC_GATEWAY_EP_ID), which a node's frames go to. */
#define NODE_HUB_EP        1

/* Hub to node: open Wi-Fi for this many minutes without a request, then go back to
 * Zigbee alone. {uint8 minutes}; 0 means the node's default. */
#define NODE_CMD_ENTER_WIFI 0x00u

/* Node to hub: {uint8 key, uint8 event}. One press of a wired button, already told
 * from a hold and debounced. Rules are the hub's. */
#define NODE_CMD_BUTTON     0x01u
/* Node to hub: {uint8 state, uint8 ipv4[4]}, whenever its Wi-Fi changes. */
#define NODE_CMD_WIFI_STATE 0x02u

typedef enum {
    NODE_BTN_CLICK = 1,   /* a press that a second may yet make a double, on the hub's clock */
    NODE_BTN_HOLD,
    NODE_BTN_RELEASE,
    NODE_BTN_SINGLE,      /* a switch's flip: nothing follows it to wait for */
    /* Pressed, and already acted on here because the hub could not be reached. Says
     * only that this node is alive and can reach it again: acted on as well, a press
     * made while the hub was coming back would toggle the light twice. */
    NODE_BTN_LOCAL,
} node_btn_event_t;

#define NODE_MAX_KEYS 8

typedef enum {
    NODE_WIFI_OFF = 0,
    NODE_WIFI_JOINING,    /* looking for its stored network */
    NODE_WIFI_STATION,    /* on it, at ipv4 */
    NODE_WIFI_HOTSPOT,    /* its setup hotspot is up: Zigbee goes quiet until it closes */
} node_wifi_state_t;

#ifdef __cplusplus
}
#endif
