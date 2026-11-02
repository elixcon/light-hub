/* What a wired button does when the hub cannot be reached.
 *
 * Presses go to the hub, whose rules decide what they mean. A button may also name a
 * light of this node to toggle when the press does not get there, so a wall switch
 * goes on working through an outage.
 *
 * The trap is a double toggle, in two places:
 *
 * - A press is acted on here only once the stack has given up on delivering it. A
 *   timer shorter than that would act, and then the retry would arrive and the hub's
 *   rule would act too. The first press of an outage therefore takes as long as the
 *   stack's retries, some seconds.
 * - While the hub is known to be down a press is acted on at once, and what goes out
 *   is NODE_BTN_LOCAL, which the hub does not act on. A delivered one ends the outage
 *   for the presses after it. Were it the press itself, the first press after the hub
 *   came back would toggle here and there.
 *
 * Only a press or a switch's flip has a local action. A leaf: no ESP-IDF, so
 * test_fallback drives it. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "node_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A backstop for a confirmation the stack never gives, well past its own retries:
 * only then can a late delivery no longer toggle twice. */
#define NODE_FB_GIVE_UP_US (8 * 1000000LL)
#define NODE_FB_PENDING    6

typedef struct {
    int64_t at;
    uint8_t seq;
    uint8_t key;
    bool    used;
    bool    act_if_lost;   /* a press with a local action, not yet acted on */
} node_fb_entry_t;

typedef struct {
    node_fb_entry_t pending[NODE_FB_PENDING];
    uint8_t next_seq;
    bool    hub_down;
} node_fb_t;

typedef struct {
    uint8_t seq;       /* hand back to node_fb_result with the send's verdict */
    uint8_t send_ev;   /* what to put on the air */
    bool    act_now;   /* run the local action now */
} node_fb_press_t;

/* A gesture to send. `has_local` is whether this button has a local action. */
node_fb_press_t node_fb_press(node_fb_t *fb, uint8_t key, uint8_t ev, bool has_local, int64_t now);

/* The stack's verdict on a frame. True, with the key, when its local action is due. */
bool node_fb_result(node_fb_t *fb, uint8_t seq, bool delivered, uint8_t *key);

/* Run now and then. True, with the key, when a confirmation has waited too long; call
 * again until false. */
bool node_fb_tick(node_fb_t *fb, int64_t now, uint8_t *key);

#ifdef __cplusplus
}
#endif
