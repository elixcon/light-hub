/* Host test for node/main/node_zcl.c: the lamp commands a Light Node takes from the
 * stack. A frame misread here shows as a lamp that goes to the wrong level, and one
 * wrongly taken is one the stack would have answered properly. A leaf, so it is
 * compiled whole.
 *
 *   make -C test
 */
#include <stdio.h>
#include <string.h>

#include <stdbool.h>
#include <stdint.h>

#include "node_zcl.h"

#define ZBC_GRAD_SLOTS 2
#include "grad-check.inc"

static int failures, checks;

#define CHECK(cond)                                                     \
    do {                                                                \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            failures++;                                                 \
            printf("FAILED line %d: %s\n", __LINE__, #cond);            \
        }                                                               \
    } while (0)

int main(void)
{
    node_zcl_cmd_t c;

    CHECK(node_zcl_decode(NODE_ZCL_CLUSTER_ON_OFF, 0x00, NULL, 0, &c) && c.kind == NODE_ZCL_OFF);
    CHECK(node_zcl_decode(NODE_ZCL_CLUSTER_ON_OFF, 0x01, NULL, 0, &c) && c.kind == NODE_ZCL_ON);
    CHECK(c.transition_ds == NODE_ZCL_TT_DEFAULT);
    CHECK(node_zcl_decode(NODE_ZCL_CLUSTER_ON_OFF, 0x02, NULL, 0, &c) && c.kind == NODE_ZCL_TOGGLE);
    const uint8_t effect[] = { 0x00, 0x00 };
    CHECK(node_zcl_decode(NODE_ZCL_CLUSTER_ON_OFF, 0x40, effect, 2, &c) && c.kind == NODE_ZCL_OFF_EFFECT);
    /* OnWithTimedOff and the rest of the cluster are the stack's. */
    CHECK(!node_zcl_decode(NODE_ZCL_CLUSTER_ON_OFF, 0x42, NULL, 0, &c));

    /* MoveToLevelWithOnOff: level, then a little-endian transition. */
    const uint8_t lvl[] = { 100, 0x34, 0x12 };
    CHECK(node_zcl_decode(NODE_ZCL_CLUSTER_LEVEL, 0x04, lvl, 3, &c));
    CHECK(c.kind == NODE_ZCL_LEVEL_ONOFF);
    CHECK(c.level == 100);
    CHECK(c.transition_ds == 0x1234);
    /* MoveToLevel is its own kind: a lamp that is off stays off. */
    CHECK(node_zcl_decode(NODE_ZCL_CLUSTER_LEVEL, 0x00, lvl, 3, &c) && c.kind == NODE_ZCL_LEVEL);
    /* With the option fields after it, as most senders write it. */
    const uint8_t opt[] = { 50, 10, 0, 0, 0 };
    CHECK(node_zcl_decode(NODE_ZCL_CLUSTER_LEVEL, 0x04, opt, 5, &c) && c.level == 50 && c.transition_ds == 10);
    /* 255 is not a level. */
    const uint8_t top[] = { 255, 0, 0 };
    CHECK(node_zcl_decode(NODE_ZCL_CLUSTER_LEVEL, 0x04, top, 3, &c) && c.level == 254);
    /* The attribute's transition passes through for the caller to resolve. */
    const uint8_t dflt[] = { 10, 0xFF, 0xFF };
    CHECK(node_zcl_decode(NODE_ZCL_CLUSTER_LEVEL, 0x04, dflt, 3, &c) && c.transition_ds == NODE_ZCL_TT_DEFAULT);
    /* Too short: left to the stack, which answers it properly. */
    CHECK(!node_zcl_decode(NODE_ZCL_CLUSTER_LEVEL, 0x04, lvl, 2, &c));
    CHECK(!node_zcl_decode(NODE_ZCL_CLUSTER_LEVEL, 0x04, NULL, 0, &c));

    /* Both Stops. */
    CHECK(node_zcl_decode(NODE_ZCL_CLUSTER_LEVEL, 0x03, NULL, 0, &c) && c.kind == NODE_ZCL_STOP);
    CHECK(node_zcl_decode(NODE_ZCL_CLUSTER_LEVEL, 0x07, NULL, 0, &c) && c.kind == NODE_ZCL_STOP);
    /* Move and Step are not taken: a lost Stop would run the lamp to a rail. */
    for (uint8_t cmd = 0x01; cmd <= 0x02; cmd++) {
        CHECK(!node_zcl_decode(NODE_ZCL_CLUSTER_LEVEL, cmd, lvl, 3, &c));
    }
    for (uint8_t cmd = 0x05; cmd <= 0x06; cmd++) {
        CHECK(!node_zcl_decode(NODE_ZCL_CLUSTER_LEVEL, cmd, lvl, 3, &c));
    }

    /* MoveToColorTemperature. */
    const uint8_t ct[] = { 0x90, 0x01, 0x0A, 0x00 };
    CHECK(node_zcl_decode(NODE_ZCL_CLUSTER_COLOR, 0x0A, ct, 4, &c));
    CHECK(c.kind == NODE_ZCL_CT);
    CHECK(c.mireds == 400);
    CHECK(c.transition_ds == 10);
    CHECK(!node_zcl_decode(NODE_ZCL_CLUSTER_COLOR, 0x0A, ct, 3, &c));
    /* Hue and xy: this is a tunable white. */
    /* MoveToHueAndSaturation and MoveToColor: an LED strip's colour. */
    const uint8_t hs[] = { 200, 100, 0x05, 0x00 };
    CHECK(node_zcl_decode(NODE_ZCL_CLUSTER_COLOR, 0x06, hs, 4, &c) && c.kind == NODE_ZCL_HS);
    CHECK(c.hue == 200 && c.sat == 100 && c.transition_ds == 5);
    const uint8_t hs_top[] = { 255, 255, 0, 0 };
    CHECK(node_zcl_decode(NODE_ZCL_CLUSTER_COLOR, 0x06, hs_top, 4, &c) && c.hue == 254 && c.sat == 254);
    CHECK(!node_zcl_decode(NODE_ZCL_CLUSTER_COLOR, 0x06, hs, 3, &c));
    const uint8_t xy[] = { 0x34, 0x12, 0x78, 0x56, 0x0A, 0x00 };
    CHECK(node_zcl_decode(NODE_ZCL_CLUSTER_COLOR, 0x07, xy, 6, &c) && c.kind == NODE_ZCL_XY);
    CHECK(c.x == 0x1234 && c.y == 0x5678 && c.transition_ds == 10);
    CHECK(!node_zcl_decode(NODE_ZCL_CLUSTER_COLOR, 0x07, xy, 5, &c));
    /* Move and Step are still not taken: see node_zcl.h. */
    CHECK(!node_zcl_decode(NODE_ZCL_CLUSTER_COLOR, 0x01, xy, 6, &c));
    CHECK(!node_zcl_decode(NODE_ZCL_CLUSTER_COLOR, 0x08, xy, 6, &c));

    /* The hub's gradient command, as the hub builds it, read here. */
    const zbc_gradient_t want = { .n = 3, .x = { 38000, 30000, 12000 }, .y = { 24000, 20000, 6000 } };
    uint8_t      body[ZBC_HUE_GRADIENT_LEN_MAX];
    const size_t blen = zbc_hue_gradient_encode(&want, 7, body, sizeof(body));
    CHECK(blen > 0);
    CHECK(node_zcl_decode(NODE_ZCL_CLUSTER_HUE, ZBC_HUE_CMD_STATE, body, (uint16_t)blen, &c));
    CHECK(c.kind == NODE_ZCL_GRADIENT && c.transition_ds == 7);
    CHECK(zbc_gradient_near(&c.gradient, &want, 16));
    /* Cut short, it is no command; nor is another command of the cluster. */
    CHECK(!node_zcl_decode(NODE_ZCL_CLUSTER_HUE, ZBC_HUE_CMD_STATE, body, (uint16_t)(blen - 1), &c));
    CHECK(!node_zcl_decode(NODE_ZCL_CLUSTER_HUE, 0x01, body, (uint16_t)blen, &c));
    /* A plain colour on that cluster, without a fade: left to the attribute. */
    const uint8_t hue_xy[] = { 0x08, 0x00, 0x34, 0x12, 0x78, 0x56 };
    CHECK(node_zcl_decode(NODE_ZCL_CLUSTER_HUE, ZBC_HUE_CMD_STATE, hue_xy, 6, &c) && c.kind == NODE_ZCL_XY);
    CHECK(c.x == 0x1234 && c.y == 0x5678 && c.transition_ds == NODE_ZCL_TT_DEFAULT);
    /* On and brightness alone are not for a strip's colour. */
    const uint8_t hue_on[] = { 0x03, 0x00, 0x01, 0x64 };
    CHECK(!node_zcl_decode(NODE_ZCL_CLUSTER_HUE, ZBC_HUE_CMD_STATE, hue_on, 4, &c));

    /* The hub's read of the state attribute, and the answer, through the hub's
     * own parser of it (zb_coordinator.c, region grad-check). */
    const uint8_t read_state[] = { 0x02, 0x00 }, read_other[] = { 0x01, 0x00, 0x03, 0x00 };
    const uint8_t read_both[] = { 0x01, 0x00, 0x02, 0x00 };
    CHECK(node_zcl_reads_state(read_state, 2) && node_zcl_reads_state(read_both, 4));
    CHECK(!node_zcl_reads_state(read_other, 4) && !node_zcl_reads_state(read_state, 1) && !node_zcl_reads_state(NULL, 0));

    uint8_t         asdu[NODE_ZCL_STATE_RSP_MAX];
    const uint8_t  *val  = NULL;
    size_t          vlen = 0;
    zbc_hue_state_t got;
    zbc_hue_state_t st = node_zcl_strip_state(true, 180, &want);
    size_t          n  = node_zcl_state_rsp(0x5A, &st, asdu, sizeof(asdu));
    CHECK(n > 0 && asdu[3] == 0x5A);
    CHECK(grad_attr_frame(asdu, n, &val, &vlen) && zbc_hue_state_decode(val, vlen, &got));
    CHECK(got.on && got.brightness == 180 && (got.flags & ZBC_HUE_F_COLORS));
    CHECK(zbc_gradient_near(&got.gradient, &want, 16));
    /* The whole loop: the hub asks for a gradient, the node takes it, the hub
     * reads it back and finds it shown without a correction. */
    zbc_grad_t *slot = grad_claim(0x4321, 3);
    grad_begin(slot, &want, 7);
    CHECK(grad_confirmed(slot, slot->gen, true, 1000) == ZBC_GRAD_DO_NOTHING);
    st = node_zcl_strip_state(true, 180, &c.gradient);     /* c: what the node decoded above */
    CHECK(node_zcl_decode(NODE_ZCL_CLUSTER_HUE, ZBC_HUE_CMD_STATE, body, (uint16_t)blen, &c));
    st = node_zcl_strip_state(true, 180, &c.gradient);
    n  = node_zcl_state_rsp(1, &st, asdu, sizeof(asdu));
    bool corrected = true;
    CHECK(grad_attr_frame(asdu, n, &val, &vlen) && zbc_hue_state_decode(val, vlen, &got));
    CHECK(grad_reported(slot, &got, &corrected) == ZBC_GRAD_DO_NOTHING && !corrected);
    CHECK(slot->state == ZBC_GRAD_SHOWN);
    /* One colour is reported as a colour, and a white as neither. */
    const zbc_gradient_t one = { .n = 1, .x = { 20000 }, .y = { 21000 } };
    st = node_zcl_strip_state(false, 0, &one);
    n  = node_zcl_state_rsp(2, &st, asdu, sizeof(asdu));
    CHECK(grad_attr_frame(asdu, n, &val, &vlen) && zbc_hue_state_decode(val, vlen, &got));
    CHECK(!got.on && got.brightness == 1 && !(got.flags & ZBC_HUE_F_COLORS) && got.x == 20000 && got.y == 21000);
    st = node_zcl_strip_state(true, 254, NULL);
    n  = node_zcl_state_rsp(3, &st, asdu, sizeof(asdu));
    CHECK(grad_attr_frame(asdu, n, &val, &vlen) && zbc_hue_state_decode(val, vlen, &got));
    CHECK(got.flags == (ZBC_HUE_F_ON | ZBC_HUE_F_BRIGHTNESS));
    /* A strip the hub knew to show a gradient, now white: the hub lets go of it. */
    CHECK(grad_reported(slot, &got, &corrected) == ZBC_GRAD_DO_NOTHING && slot->state == ZBC_GRAD_NONE);
    CHECK(node_zcl_state_rsp(4, &st, asdu, 5) == 0);

    /* What a light reports. Lit or dark, it is what it is. */
    node_zcl_report_t r = node_zcl_report(1, 200, 200, false);
    CHECK(r.on && r.has_level && r.level == 200);
    r = node_zcl_report(0, 150, 150, false);
    CHECK(!r.on && r.has_level && r.level == 150);
    /* Fading up, it says how far it has got. */
    r = node_zcl_report(1, 30, 200, false);
    CHECK(r.on && r.level == 30);
    /* Fading out, it is already the off it will be, at the level it comes back to.
     * Reported as "on, at 13", the hub's next toggle went the wrong way. */
    r = node_zcl_report(1, 33, 200, true);
    CHECK(!r.on && r.level == 200);
    /* An on/off light has no level to report. */
    r = node_zcl_report(1, -1, -1, false);
    CHECK(r.on && !r.has_level);

    /* Another cluster altogether. */
    CHECK(!node_zcl_decode(0x0005, 0x05, NULL, 0, &c));
    CHECK(!node_zcl_decode(NODE_ZCL_CLUSTER_ON_OFF, 0x00, NULL, 0, NULL));

    if (failures) {
        printf("%d of %d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("all %d checks passed\n", checks);
    return 0;
}
