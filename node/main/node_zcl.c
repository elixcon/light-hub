#include "node_zcl.h"

#include <stddef.h>

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* MoveToLevel's level. 255 is not a level, and a BILRESA sends it for the top. */
static uint8_t level_of(uint8_t raw)
{
    return raw > 254 ? 254 : raw;
}

bool node_zcl_decode(uint16_t cluster_id, uint8_t cmd_id, const uint8_t *payload, uint16_t len,
                     node_zcl_cmd_t *out)
{
    if (!out || (len > 0 && !payload)) {
        return false;
    }
    *out = (node_zcl_cmd_t){ .kind = NODE_ZCL_NONE, .transition_ds = NODE_ZCL_TT_DEFAULT };

    switch (cluster_id) {
    case NODE_ZCL_CLUSTER_ON_OFF:
        switch (cmd_id) {
        case 0x00: out->kind = NODE_ZCL_OFF; return true;
        case 0x01: out->kind = NODE_ZCL_ON; return true;
        case 0x02: out->kind = NODE_ZCL_TOGGLE; return true;
        case 0x40: out->kind = NODE_ZCL_OFF_EFFECT; return true;
        default:   return false;
        }

    case NODE_ZCL_CLUSTER_LEVEL:
        switch (cmd_id) {
        case 0x00:
        case 0x04:
            if (len < 3) {
                return false;
            }
            out->kind          = cmd_id == 0x04 ? NODE_ZCL_LEVEL_ONOFF : NODE_ZCL_LEVEL;
            out->level         = level_of(payload[0]);
            out->transition_ds = le16(&payload[1]);
            return true;
        case 0x03:
        case 0x07:
            out->kind = NODE_ZCL_STOP;
            return true;
        default:
            return false;
        }

    case NODE_ZCL_CLUSTER_COLOR:
        if (cmd_id == 0x0A && len >= 4) {
            out->kind          = NODE_ZCL_CT;
            out->mireds        = le16(&payload[0]);
            out->transition_ds = le16(&payload[2]);
            return true;
        }
        return false;

    default:
        return false;
    }
}

node_zcl_report_t node_zcl_report(int on, int level, int comes_back_to, bool fading_out)
{
    node_zcl_report_t r = { .on = on > 0 && !fading_out };
    const int shown = fading_out && comes_back_to > 0 ? comes_back_to : level;
    if (shown >= 0) {
        r.has_level = true;
        r.level     = (uint8_t)shown;
    }
    return r;
}
