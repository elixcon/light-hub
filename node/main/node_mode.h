/* Which mode a boot runs in, and when Wi-Fi closes again. Modes change by restarting
 * into another boot, so there is no path from one to another to get wrong.
 *
 * A leaf: no ESP-IDF, so test_nodemode drives every row. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NODE_MODE_ZIGBEE = 0,   /* paired, the normal case: Wi-Fi never starts */
    NODE_MODE_STATION,      /* asked for Wi-Fi, with a network to join: Zigbee stays on */
    NODE_MODE_HOTSPOT,      /* the setup hotspot: Zigbee off, for the antenna's sake */
    NODE_MODE_UNPAIRED,     /* Wi-Fi as it can be, and looking for a network to join */
} node_mode_t;

/* The one-shot request a boot finds, erased as it is read. */
typedef enum {
    NODE_ASK_NONE = 0,
    NODE_ASK_WIFI,          /* the hub asked, or an update wants its page back */
    NODE_ASK_HOTSPOT,       /* ten presses, whatever network is stored */
} node_ask_t;

node_mode_t node_mode_decide(bool paired, node_ask_t ask, bool has_network);

/* A node with no network stored opens its hotspot for this long at a time, as a
 * paired one does when asked. */
#define NODE_WIFI_MINUTES_DEFAULT 15
/* Once joined from the unpaired state, Wi-Fi stays for whoever is using the page. */
#define NODE_JOINED_IDLE_US (2 * 60 * 1000000LL)

/* Whether Wi-Fi's time is up, so this boot should restart into the next mode. All
 * times are in microseconds since boot; last_use is the last HTTP request or the
 * start of the mode, and joined_at is 0 while unjoined. */
bool node_window_over(node_mode_t mode, int64_t now, int64_t last_use, int64_t joined_at,
                      uint8_t minutes);

#ifdef __cplusplus
}
#endif
