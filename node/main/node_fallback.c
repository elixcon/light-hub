#include "node_fallback.h"

#include <stddef.h>

static bool is_press(uint8_t ev)
{
    return ev == NODE_BTN_CLICK || ev == NODE_BTN_SECOND || ev == NODE_BTN_SINGLE;
}

node_fb_press_t node_fb_press(node_fb_t *fb, uint8_t key, uint8_t ev, bool has_local, int64_t now)
{
    node_fb_press_t out = { .seq = fb->next_seq++, .send_ev = ev, .act_now = false };
    const bool local = has_local && is_press(ev);
    if (local && fb->hub_down) {
        out.act_now = true;
        out.send_ev = NODE_BTN_LOCAL;
    }

    /* The oldest entry makes way when all are waiting. Its verdict is then ignored,
     * which costs a local action only if six presses are outstanding. */
    int slot = 0;
    for (int i = 0; i < NODE_FB_PENDING; i++) {
        if (!fb->pending[i].used) {
            slot = i;
            break;
        }
        if (fb->pending[i].at < fb->pending[slot].at) {
            slot = i;
        }
    }
    fb->pending[slot] = (node_fb_entry_t){
        .at = now, .seq = out.seq, .key = key, .used = true,
        .act_if_lost = local && !out.act_now,
    };
    return out;
}

static bool settle(node_fb_t *fb, node_fb_entry_t *e, bool delivered, uint8_t *key)
{
    const bool act = !delivered && e->act_if_lost;
    if (act && key) {
        *key = e->key;
    }
    fb->hub_down = !delivered;
    e->used = false;
    return act;
}

bool node_fb_result(node_fb_t *fb, uint8_t seq, bool delivered, uint8_t *key)
{
    for (int i = 0; i < NODE_FB_PENDING; i++) {
        if (fb->pending[i].used && fb->pending[i].seq == seq) {
            return settle(fb, &fb->pending[i], delivered, key);
        }
    }
    return false;
}

bool node_fb_tick(node_fb_t *fb, int64_t now, uint8_t *key)
{
    for (int i = 0; i < NODE_FB_PENDING; i++) {
        if (fb->pending[i].used && now - fb->pending[i].at >= NODE_FB_GIVE_UP_US) {
            if (settle(fb, &fb->pending[i], false, key)) {
                return true;
            }
        }
    }
    return false;
}
