#include "node_mode.h"

node_mode_t node_mode_decide(bool paired, node_ask_t ask, bool has_network)
{
    /* Checked first: with no network to be on, none of the rest means anything. */
    if (!paired) {
        return NODE_MODE_UNPAIRED;
    }
    if (ask == NODE_ASK_HOTSPOT || (ask == NODE_ASK_WIFI && !has_network)) {
        return NODE_MODE_HOTSPOT;
    }
    return ask == NODE_ASK_WIFI ? NODE_MODE_STATION : NODE_MODE_ZIGBEE;
}

int64_t node_window_end(node_mode_t mode, int64_t last_use, int64_t joined_at, uint8_t minutes)
{
    switch (mode) {
    case NODE_MODE_STATION:
    case NODE_MODE_HOTSPOT: {
        const int64_t m = minutes ? minutes : NODE_WIFI_MINUTES_DEFAULT;
        return last_use + m * 60 * 1000000LL;
    }
    case NODE_MODE_UNPAIRED:
        /* Unjoined, Wi-Fi is the only way to reach it: it stays. */
        if (joined_at == 0) {
            return NODE_WINDOW_NEVER;
        }
        /* From the later of the two, so joining mid-edit gives the page the full time. */
        return (last_use > joined_at ? last_use : joined_at) + NODE_JOINED_IDLE_US;
    case NODE_MODE_ZIGBEE:
    default:
        return NODE_WINDOW_NEVER;
    }
}

uint8_t node_parent_floor(unsigned failed_rejoins)
{
    /* Every 6 dB or so, from -37 dBm down to -67. */
    static const uint8_t floors[] = { 150, 135, 120, 105, 90, 75 };
    return failed_rejoins < sizeof(floors) ? floors[failed_rejoins] : 0;
}
