/* Host test for node/main/node_zcl.c: the lamp commands a Light Node takes from the
 * stack. A frame misread here shows as a lamp that goes to the wrong level, and one
 * wrongly taken is one the stack would have answered properly. A leaf, so it is
 * compiled whole.
 *
 *   make -C test
 */
#include <stdio.h>
#include <string.h>

#include "node_zcl.h"

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
    CHECK(!node_zcl_decode(NODE_ZCL_CLUSTER_COLOR, 0x06, ct, 4, &c));
    CHECK(!node_zcl_decode(NODE_ZCL_CLUSTER_COLOR, 0x07, ct, 4, &c));

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
