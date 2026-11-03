/* Host test for node/main/node_mode.c: which mode a Light Node boots in, and when
 * Wi-Fi closes again.
 *
 * Both failures are quiet and expensive. A node that opens Wi-Fi when it should not
 * spends the power this firmware exists to save, for ever, and one that stays
 * unreachable when it should have opened it needs a cable to get back. A leaf, so it
 * is compiled whole.
 *
 *   make -C test
 */
#include <stdio.h>

#include "node_mode.h"

static int failures, checks;

#define CHECK(cond)                                                     \
    do {                                                                \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            failures++;                                                 \
            printf("FAILED line %d: %s\n", __LINE__, #cond);            \
        }                                                               \
    } while (0)

#define MIN (60 * 1000000LL)

static void test_parent_floor(void)
{
    /* Strongest first, each lower than the last, ending with no demand at all, or a
     * node with one distant router would never get back on. */
    unsigned i = 0;
    while (node_parent_floor(i + 1) > 0) {
        CHECK(node_parent_floor(i) > node_parent_floor(i + 1));
        i++;
    }
    CHECK(i < 20);
    CHECK(node_parent_floor(0) > 0);
    CHECK(node_parent_floor(i + 1) == 0);
    CHECK(node_parent_floor(100) == 0);
}

int main(void)
{
    test_parent_floor();
    /* Normal: paired, nobody asked. Wi-Fi never starts, with or without a network. */
    CHECK(node_mode_decide(true, NODE_ASK_NONE, true) == NODE_MODE_ZIGBEE);
    CHECK(node_mode_decide(true, NODE_ASK_NONE, false) == NODE_MODE_ZIGBEE);

    /* The hub asked: its stored network if it has one, otherwise its hotspot. */
    CHECK(node_mode_decide(true, NODE_ASK_WIFI, true) == NODE_MODE_STATION);
    CHECK(node_mode_decide(true, NODE_ASK_WIFI, false) == NODE_MODE_HOTSPOT);

    /* Ten presses: the hotspot, whatever is stored. The way back when the stored
     * network is the thing that is broken. */
    CHECK(node_mode_decide(true, NODE_ASK_HOTSPOT, true) == NODE_MODE_HOTSPOT);
    CHECK(node_mode_decide(true, NODE_ASK_HOTSPOT, false) == NODE_MODE_HOTSPOT);

    /* Unpaired beats everything: there is no Zigbee network to be quiet on. */
    for (int ask = NODE_ASK_NONE; ask <= NODE_ASK_HOTSPOT; ask++) {
        CHECK(node_mode_decide(false, (node_ask_t)ask, true) == NODE_MODE_UNPAIRED);
        CHECK(node_mode_decide(false, (node_ask_t)ask, false) == NODE_MODE_UNPAIRED);
    }

#define node_window_over(mode, now, last_use, joined_at, minutes) \
    ((now) >= node_window_end((mode), (last_use), (joined_at), (minutes)))

    /* Zigbee alone has no window to close. */
    CHECK(node_window_end(NODE_MODE_ZIGBEE, 0, 0, 0) == NODE_WINDOW_NEVER);
    CHECK(!node_window_over(NODE_MODE_ZIGBEE, 1000 * MIN, 0, 0, 0));

    /* Asked for Wi-Fi: until that many minutes without a request. */
    CHECK(!node_window_over(NODE_MODE_STATION, 14 * MIN, 0, 1, 0));
    CHECK(node_window_over(NODE_MODE_STATION, NODE_WIFI_MINUTES_DEFAULT * MIN, 0, 1, 0));
    CHECK(!node_window_over(NODE_MODE_HOTSPOT, 29 * MIN, 0, 0, 30));
    CHECK(node_window_over(NODE_MODE_HOTSPOT, 30 * MIN, 0, 0, 30));
    /* Using the page pushes it back. */
    CHECK(!node_window_over(NODE_MODE_STATION, 20 * MIN, 10 * MIN, 1, 15));
    CHECK(node_window_over(NODE_MODE_STATION, 25 * MIN, 10 * MIN, 1, 15));

    /* Unpaired and unjoined: Wi-Fi is the only way in, however long it has been. */
    CHECK(!node_window_over(NODE_MODE_UNPAIRED, 100000 * MIN, 0, 0, 0));
    /* Joined: two minutes after the page was last used. */
    CHECK(!node_window_over(NODE_MODE_UNPAIRED, 11 * MIN, 0, 10 * MIN, 0));
    CHECK(node_window_over(NODE_MODE_UNPAIRED, 12 * MIN, 0, 10 * MIN, 0));
    /* Used after joining: from then. */
    CHECK(!node_window_over(NODE_MODE_UNPAIRED, 12 * MIN, 11 * MIN, 10 * MIN, 0));
    CHECK(node_window_over(NODE_MODE_UNPAIRED, 13 * MIN, 11 * MIN, 10 * MIN, 0));
    /* Used long before joining: from the join, or somebody mid-edit loses the page. */
    CHECK(!node_window_over(NODE_MODE_UNPAIRED, 11 * MIN, 1 * MIN, 10 * MIN, 0));

    if (failures) {
        printf("%d of %d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("all %d checks passed\n", checks);
    return 0;
}
