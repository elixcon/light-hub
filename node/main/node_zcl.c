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
        if (cmd_id == 0x06 && len >= 4) {
            out->kind          = NODE_ZCL_HS;
            out->hue           = payload[0] > 254 ? 254 : payload[0];
            out->sat           = payload[1] > 254 ? 254 : payload[1];
            out->transition_ds = le16(&payload[2]);
            return true;
        }
        if (cmd_id == 0x07 && len >= 6) {
            out->kind          = NODE_ZCL_XY;
            out->x             = le16(&payload[0]);
            out->y             = le16(&payload[2]);
            out->transition_ds = le16(&payload[4]);
            return true;
        }
        return false;

    case NODE_ZCL_CLUSTER_HUE: {
        /* Philips' command 0: the fields its flags name. Of those, a strip acts
         * on the gradient's colours, or else on a plain colour, and on the fade
         * time; a frame with neither is not for it. */
        zbc_hue_state_t st;
        if (cmd_id != ZBC_HUE_CMD_STATE || !zbc_hue_state_decode(payload, len, &st)) {
            return false;
        }
        if (st.flags & ZBC_HUE_F_FADE) {
            out->transition_ds = st.fade;
        }
        if ((st.flags & ZBC_HUE_F_COLORS) && st.gradient.n >= 2) {
            out->kind     = NODE_ZCL_GRADIENT;
            out->gradient = st.gradient;
            return true;
        }
        if ((st.flags & ZBC_HUE_F_COLORS) && st.gradient.n == 1) {
            out->kind = NODE_ZCL_XY;
            out->x    = st.gradient.x[0];
            out->y    = st.gradient.y[0];
            return true;
        }
        if (st.flags & ZBC_HUE_F_XY) {
            out->kind = NODE_ZCL_XY;
            out->x    = st.x;
            out->y    = st.y;
            return true;
        }
        return false;
    }

    default:
        return false;
    }
}

bool node_zcl_reads_state(const uint8_t *payload, uint16_t len)
{
    for (uint16_t at = 0; payload && at + 2 <= len; at += 2) {
        if (le16(&payload[at]) == ZBC_HUE_ATTR_STATE) {
            return true;
        }
    }
    return false;
}

size_t node_zcl_state_rsp(uint8_t tsn, const zbc_hue_state_t *st, uint8_t *out, size_t cap)
{
    uint8_t      val[ZBC_HUE_STATE_LEN_MAX];
    const size_t n = zbc_hue_state_encode(st, val, sizeof(val));
    if (n == 0 || !out || cap < 10 + n) {
        return 0;
    }
    size_t at = 0;
    out[at++] = 0x1C;                           /* global, manufacturer-specific, to the client, no default response */
    out[at++] = (uint8_t)ZBC_HUE_MANUF;
    out[at++] = (uint8_t)(ZBC_HUE_MANUF >> 8);
    out[at++] = tsn;
    out[at++] = 0x01;                           /* Read Attributes Response */
    out[at++] = (uint8_t)ZBC_HUE_ATTR_STATE;
    out[at++] = (uint8_t)(ZBC_HUE_ATTR_STATE >> 8);
    out[at++] = 0x00;                           /* success */
    out[at++] = 0x41;                           /* octet string */
    out[at++] = (uint8_t)n;
    for (size_t i = 0; i < n; i++) {
        out[at++] = val[i];
    }
    return at;
}

zbc_hue_state_t node_zcl_strip_state(bool on, uint8_t level, const zbc_gradient_t *gradient)
{
    zbc_hue_state_t st = { .flags = ZBC_HUE_F_ON | ZBC_HUE_F_BRIGHTNESS, .on = on,
                           .brightness = level < 1 ? 1 : (level > 254 ? 254 : level) };
    if (gradient && gradient->n >= 2 && zbc_gradient_valid(gradient)) {
        st.flags   |= ZBC_HUE_F_COLORS;
        st.gradient = *gradient;
    } else if (gradient && gradient->n == 1) {
        st.flags |= ZBC_HUE_F_XY;
        st.x      = gradient->x[0];
        st.y      = gradient->y[0];
    }
    return st;
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
