/* Host test for node/main/node_fallback.c: a wall switch through a hub outage.
 *
 * What goes wrong here shows on a wall as a light that toggles twice, or a switch
 * that does nothing, on the one day the hub is down. A leaf, so it is compiled whole.
 *
 *   make -C test
 */
#include <stdio.h>
#include <string.h>

#include "node_fallback.h"

static int failures, checks;

#define CHECK(cond)                                                     \
    do {                                                                \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            failures++;                                                 \
            printf("FAILED line %d: %s\n", __LINE__, #cond);            \
        }                                                               \
    } while (0)

#define S 1000000LL

int main(void)
{
    node_fb_t fb;
    uint8_t   key = 0xFF;

    /* The hub answers: the press goes out as it is and nothing happens here. */
    memset(&fb, 0, sizeof(fb));
    node_fb_press_t p = node_fb_press(&fb, 2, NODE_BTN_CLICK, true, 0);
    CHECK(!p.act_now);
    CHECK(p.send_ev == NODE_BTN_CLICK);
    CHECK(!node_fb_result(&fb, p.seq, true, &key));
    CHECK(!fb.hub_down);

    /* The first press of an outage: not before the stack gives up... */
    p = node_fb_press(&fb, 2, NODE_BTN_CLICK, true, 10 * S);
    CHECK(!p.act_now);
    uint8_t got = 0xFF;
    CHECK(!node_fb_tick(&fb, 10 * S + NODE_FB_GIVE_UP_US - 1, &got));
    /* ...and then once, on the right key. */
    key = 0xFF;
    CHECK(node_fb_result(&fb, p.seq, false, &key));
    CHECK(key == 2);
    CHECK(fb.hub_down);
    /* A second verdict on the same frame acts on nothing. */
    CHECK(!node_fb_result(&fb, p.seq, false, &key));
    CHECK(!node_fb_tick(&fb, 100 * S, &got));

    /* During the outage: acted on at once, and what goes out does not act at the hub. */
    p = node_fb_press(&fb, 3, NODE_BTN_SINGLE, true, 20 * S);
    CHECK(p.act_now);
    CHECK(p.send_ev == NODE_BTN_LOCAL);
    /* Its failure must not act a second time. */
    CHECK(!node_fb_result(&fb, p.seq, false, &key));
    CHECK(fb.hub_down);

    /* The hub is back: this press was acted on here, and its frame only says so. */
    p = node_fb_press(&fb, 3, NODE_BTN_SINGLE, true, 30 * S);
    CHECK(p.act_now);
    CHECK(p.send_ev == NODE_BTN_LOCAL);
    CHECK(!node_fb_result(&fb, p.seq, true, &key));
    CHECK(!fb.hub_down);
    /* The next one is the hub's again. */
    p = node_fb_press(&fb, 3, NODE_BTN_SINGLE, true, 31 * S);
    CHECK(!p.act_now);
    CHECK(p.send_ev == NODE_BTN_SINGLE);
    CHECK(!node_fb_result(&fb, p.seq, true, &key));

    /* A button with no local action does nothing here, and keeps asking the hub. */
    memset(&fb, 0, sizeof(fb));
    p = node_fb_press(&fb, 1, NODE_BTN_CLICK, false, 40 * S);
    CHECK(!node_fb_result(&fb, p.seq, false, &key));
    CHECK(fb.hub_down);
    p = node_fb_press(&fb, 1, NODE_BTN_CLICK, false, 41 * S);
    CHECK(!p.act_now);
    CHECK(p.send_ev == NODE_BTN_CLICK);

    /* A second press close behind a click is a press like any other out here: a quick
     * double during an outage toggles twice, as it would with no double rule. */
    memset(&fb, 0, sizeof(fb));
    fb.hub_down = true;
    p = node_fb_press(&fb, 1, NODE_BTN_SECOND, true, 45 * S);
    CHECK(p.act_now);
    CHECK(p.send_ev == NODE_BTN_LOCAL);
    CHECK(!node_fb_result(&fb, p.seq, false, &key));
    /* And before the outage is known, a lost one acts once the stack gives up. */
    memset(&fb, 0, sizeof(fb));
    p = node_fb_press(&fb, 1, NODE_BTN_SECOND, true, 46 * S);
    CHECK(!p.act_now);
    CHECK(p.send_ev == NODE_BTN_SECOND);
    key = 0xFF;
    CHECK(node_fb_result(&fb, p.seq, false, &key));
    CHECK(key == 1);

    /* A hold has no local action, even with the hub down and a toggle configured. */
    memset(&fb, 0, sizeof(fb));
    fb.hub_down = true;
    p = node_fb_press(&fb, 1, NODE_BTN_HOLD, true, 50 * S);
    CHECK(!p.act_now);
    CHECK(p.send_ev == NODE_BTN_HOLD);
    CHECK(!node_fb_result(&fb, p.seq, false, &key));
    p = node_fb_press(&fb, 1, NODE_BTN_RELEASE, true, 51 * S);
    CHECK(!p.act_now);
    CHECK(p.send_ev == NODE_BTN_RELEASE);

    /* A confirmation that never comes: the backstop, once. */
    memset(&fb, 0, sizeof(fb));
    p = node_fb_press(&fb, 5, NODE_BTN_CLICK, true, 60 * S);
    CHECK(!node_fb_tick(&fb, 61 * S, &got));
    got = 0xFF;
    CHECK(node_fb_tick(&fb, 60 * S + NODE_FB_GIVE_UP_US, &got));
    CHECK(got == 5);
    CHECK(fb.hub_down);
    CHECK(!node_fb_tick(&fb, 200 * S, &got));
    /* And the stack's verdict arriving after that changes nothing. */
    CHECK(!node_fb_result(&fb, p.seq, false, &key));

    /* More presses than slots: the newest are kept, and none acts twice. */
    memset(&fb, 0, sizeof(fb));
    node_fb_press_t many[NODE_FB_PENDING + 2];
    for (int i = 0; i < NODE_FB_PENDING + 2; i++) {
        many[i] = node_fb_press(&fb, (uint8_t)i, NODE_BTN_CLICK, true, (70 + i) * S);
    }
    int acted = 0;
    for (int i = 0; i < NODE_FB_PENDING + 2; i++) {
        if (node_fb_result(&fb, many[i].seq, false, &key)) {
            acted++;
            CHECK(key >= 2);
        }
    }
    CHECK(acted == NODE_FB_PENDING);

    /* The sequence number wraps without confusion. */
    memset(&fb, 0, sizeof(fb));
    fb.next_seq = 255;
    p = node_fb_press(&fb, 7, NODE_BTN_CLICK, true, 90 * S);
    CHECK(p.seq == 255);
    node_fb_press_t q = node_fb_press(&fb, 6, NODE_BTN_CLICK, true, 91 * S);
    CHECK(q.seq == 0);
    CHECK(node_fb_result(&fb, q.seq, false, &key));
    CHECK(key == 6);
    CHECK(node_fb_result(&fb, p.seq, false, &key));
    CHECK(key == 7);

    /* Everything above has had its verdict: nothing left for the backstop to watch. */
    CHECK(!node_fb_waiting(&fb));
    (void)node_fb_press(&fb, 1, NODE_BTN_CLICK, false, 0);
    CHECK(node_fb_waiting(&fb));

    if (failures) {
        printf("%d of %d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("all %d checks passed\n", checks);
    return 0;
}
