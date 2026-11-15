/* A Hue gradient lamp that is not on the desk, simulated, against what
 * main/zb_coordinator.c decides about one (region `grad-check`).
 *
 * The lamp here is built from Bifrost's description of the real one: it takes
 * command 0 on cluster 0xFC03, ignores a frame it cannot parse to the end,
 * shows the colours it was sent - pulled into its gamut - and answers a read of
 * its state attribute with what it shows. An ordinary Hue bulb lists the same
 * cluster and takes no gradient, and is simulated as that. The radio between is
 * a function that can lose a frame.
 *
 * What runs on the device is the same: zb_set_gradient begins a request and
 * sends it, data_confirm_cb hands the confirmation to grad_confirmed, the work
 * task asks grad_tick whether to read, and the answer comes back through
 * grad_attr_frame and grad_reported. Here those are called in that order by
 * hand.
 *
 *   make -C test
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gradient.h"

#define ZBC_GRAD_SLOTS 3
#include "grad-check.inc"

static int s_checks;
static void ok(bool cond, const char *what)
{
    s_checks++;
    if (!cond) { printf("FAIL: %s\n", what); exit(1); }
}

/* ------------------------------------------------------------- the lamp */

typedef struct {
    bool           takes_gradients;   /* a strip, not a bulb */
    bool           clips;             /* pulls x into a narrower gamut */
    bool           deaf;              /* answers no reads */
    bool           has;
    zbc_gradient_t shown;
    uint16_t       x, y;
    int            frames, rejected, reads;
} lamp_t;

static void lamp_command(lamp_t *l, const uint8_t *body, size_t len)
{
    zbc_hue_state_t st;
    l->frames++;
    if (!zbc_hue_state_decode(body, len, &st)) {
        l->rejected++;
        return;
    }
    if (st.flags & ZBC_HUE_F_XY) {
        l->has = false;
        l->x = st.x, l->y = st.y;
    }
    if ((st.flags & ZBC_HUE_F_COLORS) && l->takes_gradients) {
        l->has   = true;
        l->shown = st.gradient;
        for (int i = 0; l->clips && i < l->shown.n; i++) {
            if (l->shown.x[i] > 40000) {
                l->shown.x[i] = 40000;
            }
        }
    }
}

/* Its answer to a read of the state attribute, as the frame arrives at APS. */
static size_t lamp_answer(lamp_t *l, uint8_t *asdu, size_t cap)
{
    l->reads++;
    if (l->deaf) {
        return 0;
    }
    zbc_hue_state_t st = { .flags = ZBC_HUE_F_ON | ZBC_HUE_F_BRIGHTNESS | (l->has ? ZBC_HUE_F_COLORS : ZBC_HUE_F_XY),
                           .on = true, .brightness = 200, .x = l->x, .y = l->y, .gradient = l->shown };
    uint8_t      val[ZBC_HUE_STATE_LEN_MAX];
    const size_t n = zbc_hue_state_encode(&st, val, sizeof(val));
    const uint8_t hdr[] = { 0x1C, 0x0B, 0x10, 0x42, 0x01, 0x02, 0x00, 0x00, 0x41, (uint8_t)n };
    if (cap < sizeof(hdr) + n) {
        return 0;
    }
    memcpy(asdu, hdr, sizeof(hdr));
    memcpy(asdu + sizeof(hdr), val, n);
    return sizeof(hdr) + n;
}

/* ---------------------------------------------- the coordinator's side */

#define ADDR 0x1234
#define EP   11

static int64_t s_now = 1000000;
static int     s_lose;          /* this many of the next frames never arrive */
static int     s_sent, s_corrections;

/* send_cmd_now and the confirmation that follows it. Returns what the
 * confirmation asked for. */
static zbc_grad_do_t radio_send(zbc_grad_t *g, lamp_t *l)
{
    uint8_t      body[ZBC_HUE_GRADIENT_LEN_MAX];
    const size_t len = zbc_hue_gradient_encode(&g->want, g->transition_ds, body, sizeof(body));
    ok(len > 0, "a wanted gradient encodes");
    s_sent++;
    const bool delivered = s_lose == 0;
    if (delivered) {
        lamp_command(l, body, len);
    } else {
        s_lose--;
    }
    return grad_confirmed(g, g->gen, delivered, s_now);
}

/* Runs a request to its end: re-sends, reads and answers, in simulated time. */
static void run(zbc_grad_t *g, lamp_t *l)
{
    zbc_grad_do_t act = radio_send(g, l);
    for (int guard = 0; guard < 40; guard++) {
        if (act == ZBC_GRAD_DO_SEND) {
            act = radio_send(g, l);
            continue;
        }
        const int64_t due = grad_next_due();
        if (!due) {
            return;
        }
        s_now = due;
        if (grad_tick(g, s_now) != ZBC_GRAD_DO_READ) {
            continue;
        }
        uint8_t        asdu[80];
        const size_t   n = lamp_answer(l, asdu, sizeof(asdu));
        const uint8_t *val;
        size_t         vlen;
        act = ZBC_GRAD_DO_NOTHING;
        if (n && grad_attr_frame(asdu, n, &val, &vlen)) {
            zbc_hue_state_t st;
            bool            corrected = false;
            ok(zbc_hue_state_decode(val, vlen, &st), "the lamp's answer parses");
            act = grad_reported(g, &st, &corrected);
            s_corrections += corrected;
        }
    }
    ok(false, "a request comes to an end");
}

static zbc_gradient_t sunset(void)
{
    return (zbc_gradient_t){ .n = 3, .x = { 38000, 30000, 12000 }, .y = { 24000, 20000, 6000 } };
}

static void reset(void)
{
    memset(s_grad, 0, sizeof(s_grad));
    s_lose = s_sent = s_corrections = 0;
}

static void test_taken(void)
{
    reset();
    lamp_t l = { .takes_gradients = true };
    const zbc_gradient_t want = sunset();
    zbc_grad_t *g = grad_claim(ADDR, EP);
    grad_begin(g, &want, 4);
    ok(g->state == ZBC_GRAD_PENDING, "a gradient asked for is pending");
    const int64_t t0 = s_now;
    run(g, &l);
    ok(g->state == ZBC_GRAD_SHOWN, "a strip that takes it shows it");
    ok(s_sent == 1 && l.reads == 1 && s_corrections == 0, "with one frame and one read");
    ok(s_now - t0 == 400000 + ZBC_GRAD_READ_MARGIN_US, "the read waits out the fade and a second");
    ok(zbc_gradient_near(&g->reported, &want, 16), "what it reports is what was sent, to twelve bits");
    ok(l.has && l.shown.n == 3, "and the lamp has three colours");
    ok(grad_next_due() == 0, "nothing more is waiting");
}

static void test_lost(void)
{
    reset();
    lamp_t l = { .takes_gradients = true };
    const zbc_gradient_t want = sunset();
    zbc_grad_t *g = grad_claim(ADDR, EP);
    grad_begin(g, &want, 0);
    s_lose = 1;
    run(g, &l);
    ok(g->state == ZBC_GRAD_SHOWN && s_sent == 2 && l.frames == 1, "a frame lost on the way is sent again");
    ok(s_corrections == 0, "which is no correction: the lamp was never wrong");

    reset();
    l = (lamp_t){ .takes_gradients = true };
    g = grad_claim(ADDR, EP);
    grad_begin(g, &want, 0);
    s_lose = 99;
    run(g, &l);
    ok(s_sent == 1 + ZBC_GRAD_RESENDS && l.frames == 0, "a lamp that is not there is tried a bounded number of times");
    ok(g->state == ZBC_GRAD_PENDING && grad_next_due() == 0 && l.reads == 0, "and stays wanted, unread");
}

static void test_bulb(void)
{
    reset();
    lamp_t l = { .takes_gradients = false, .x = 20000, .y = 21000 };
    const zbc_gradient_t want = sunset();
    zbc_grad_t *g = grad_claim(ADDR, EP);
    grad_begin(g, &want, 0);
    run(g, &l);
    ok(g->state == ZBC_GRAD_NOT_TAKEN, "an ordinary bulb ends as not taken");
    ok(s_sent == 1 + ZBC_GRAD_FIXES && s_corrections == ZBC_GRAD_FIXES, "after being told a bounded number of times");
    ok(l.rejected == 0, "every frame it was sent was one a lamp parses");
    ok(grad_next_due() == 0, "and is then left alone");
}

static void test_other_colours(void)
{
    /* A lamp that shows a gradient, but not these colours: its gamut ends
     * before the first one. */
    reset();
    lamp_t l = { .takes_gradients = true, .clips = true };
    zbc_gradient_t want = sunset();
    want.x[0] = 46000;
    zbc_grad_t *g = grad_claim(ADDR, EP);
    grad_begin(g, &want, 0);
    run(g, &l);
    ok(g->state == ZBC_GRAD_SHOWN, "a lamp that shows other colours is showing a gradient");
    ok(s_corrections == ZBC_GRAD_FIXES, "after the bounded corrections");
    ok(g->reported.x[0] > 39900 && g->reported.x[0] < 40100, "and what is known is what it reports, not what was sent");

    /* Inside the tolerance is the same colour. */
    reset();
    l = (lamp_t){ .takes_gradients = true, .clips = true };
    want.x[0] = 40000 + ZBC_GRAD_TOLERANCE - 20;
    g = grad_claim(ADDR, EP);
    grad_begin(g, &want, 0);
    run(g, &l);
    ok(g->state == ZBC_GRAD_SHOWN && s_corrections == 0 && s_sent == 1, "a colour within the tolerance needs no correction");
}

static void test_silent(void)
{
    reset();
    lamp_t l = { .takes_gradients = true, .deaf = true };
    const zbc_gradient_t want = sunset();
    zbc_grad_t *g = grad_claim(ADDR, EP);
    grad_begin(g, &want, 0);
    run(g, &l);
    ok(l.reads == ZBC_GRAD_READS && s_sent == 1, "a lamp that answers nothing is read a bounded number of times");
    ok(g->state == ZBC_GRAD_PENDING, "and stays pending: nobody knows");

    /* Its answer arriving later still settles it. */
    l.deaf = false;
    uint8_t        asdu[80];
    const size_t   n = lamp_answer(&l, asdu, sizeof(asdu));
    const uint8_t *val;
    size_t         vlen;
    zbc_hue_state_t st;
    bool            corrected;
    ok(grad_attr_frame(asdu, n, &val, &vlen) && zbc_hue_state_decode(val, vlen, &st), "a late answer parses");
    ok(grad_reported(g, &st, &corrected) == ZBC_GRAD_DO_NOTHING && g->state == ZBC_GRAD_SHOWN, "and is believed");
}

static void test_newer_request(void)
{
    reset();
    lamp_t l = { .takes_gradients = true };
    zbc_gradient_t first = sunset(), second = sunset();
    second.x[1] = 9000;
    zbc_grad_t *g = grad_claim(ADDR, EP);
    grad_begin(g, &first, 0);
    const uint16_t old = g->gen;
    grad_begin(g, &second, 0);
    ok(g->gen != old, "a second request is another one");
    ok(grad_confirmed(g, old, true, s_now) == ZBC_GRAD_DO_NOTHING && !g->delivered,
       "the first request's confirmation does not count for the second");
    ok(grad_confirmed(g, old, false, s_now) == ZBC_GRAD_DO_NOTHING, "nor is its lost frame sent again");
    /* An answer from before the second frame got there says nothing about it. */
    zbc_hue_state_t st = { .flags = ZBC_HUE_F_COLORS, .gradient = first };
    bool            corrected;
    ok(grad_reported(g, &st, &corrected) == ZBC_GRAD_DO_NOTHING && g->state == ZBC_GRAD_PENDING && !corrected,
       "a report from before the frame was delivered is not judged");
    run(g, &l);
    ok(g->state == ZBC_GRAD_SHOWN && zbc_gradient_near(&g->reported, &second, 16), "the second is what ends up shown");
}

static void test_found(void)
{
    /* After a restart nothing is known; the lamp is asked and says. */
    reset();
    lamp_t l = { .takes_gradients = true, .has = true, .shown = sunset() };
    uint8_t        asdu[80];
    size_t         n = lamp_answer(&l, asdu, sizeof(asdu));
    const uint8_t *val;
    size_t         vlen;
    zbc_hue_state_t st;
    bool            corrected;
    ok(grad_attr_frame(asdu, n, &val, &vlen) && zbc_hue_state_decode(val, vlen, &st), "a found lamp's answer parses");
    zbc_grad_t *g = grad_claim(ADDR, EP);
    ok(grad_reported(g, &st, &corrected) == ZBC_GRAD_DO_NOTHING && g->state == ZBC_GRAD_SHOWN && !corrected,
       "a lamp found showing a gradient is known to show it");
    ok(zbc_gradient_near(&g->reported, &l.shown, 16), "with its colours");

    /* Something else gave it a plain colour. */
    l.has = false;
    n = lamp_answer(&l, asdu, sizeof(asdu));
    ok(grad_attr_frame(asdu, n, &val, &vlen) && zbc_hue_state_decode(val, vlen, &st), "its next answer parses");
    ok(grad_reported(g, &st, &corrected) == ZBC_GRAD_DO_NOTHING && g->state == ZBC_GRAD_NONE,
       "a lamp that no longer reports one has none");

    /* A bulb that took none keeps saying so, and that is not news. */
    grad_free(g);
    g = grad_claim(ADDR, EP);
    g->state = ZBC_GRAD_NOT_TAKEN;
    ok(grad_reported(g, &st, &corrected) == ZBC_GRAD_DO_NOTHING && g->state == ZBC_GRAD_NOT_TAKEN, "not taken stays not taken");
}

static void test_frames(void)
{
    const uint8_t *val;
    size_t         vlen;
    /* A read response: manufacturer-specific, to the client. */
    const uint8_t rsp[] = { 0x1C, 0x0B, 0x10, 0x07, 0x01, 0x02, 0x00, 0x00, 0x41, 0x04, 0x03, 0x00, 0x01, 0x64 };
    ok(grad_attr_frame(rsp, sizeof(rsp), &val, &vlen) && vlen == 4 && val == &rsp[10], "a read response's value is found");
    /* A report has no status byte. */
    const uint8_t rep[] = { 0x1C, 0x0B, 0x10, 0x07, 0x0A, 0x02, 0x00, 0x41, 0x04, 0x03, 0x00, 0x01, 0x64 };
    ok(grad_attr_frame(rep, sizeof(rep), &val, &vlen) && vlen == 4 && val == &rep[9], "a report's value is found");
    /* Without the manufacturer code the header is two bytes shorter. */
    const uint8_t plain[] = { 0x18, 0x07, 0x01, 0x02, 0x00, 0x00, 0x41, 0x02, 0x01, 0x00 };
    ok(grad_attr_frame(plain, sizeof(plain), &val, &vlen) && vlen == 2, "a frame without a manufacturer code");

    uint8_t bad[sizeof(rsp)];
    memcpy(bad, rsp, sizeof(rsp));
    bad[7] = 0x86;
    ok(!grad_attr_frame(bad, sizeof(bad), &val, &vlen), "an attribute the lamp does not have");
    memcpy(bad, rsp, sizeof(rsp));
    bad[5] = 0x03;
    ok(!grad_attr_frame(bad, sizeof(bad), &val, &vlen), "another attribute");
    memcpy(bad, rsp, sizeof(rsp));
    bad[8] = 0x42;
    ok(!grad_attr_frame(bad, sizeof(bad), &val, &vlen), "another type");
    memcpy(bad, rsp, sizeof(rsp));
    bad[0] = 0x1D;
    ok(!grad_attr_frame(bad, sizeof(bad), &val, &vlen), "a cluster command is not an attribute");
    memcpy(bad, rsp, sizeof(rsp));
    bad[4] = 0x0B;
    ok(!grad_attr_frame(bad, sizeof(bad), &val, &vlen), "a default response");
    memcpy(bad, rsp, sizeof(rsp));
    bad[9] = 0x05;
    ok(!grad_attr_frame(bad, sizeof(bad), &val, &vlen), "a string longer than the frame");
    bad[9] = 0xFF;
    ok(!grad_attr_frame(bad, sizeof(bad), &val, &vlen), "an invalid string");
    for (size_t n = 0; n < 10; n++) {
        ok(!grad_attr_frame(rsp, n, &val, &vlen), "a frame cut short");
    }
}

static void test_slots(void)
{
    reset();
    const zbc_gradient_t want = sunset();
    zbc_grad_t *a = grad_claim(1, 1), *b, *c;
    grad_begin(a, &want, 0);
    ok(grad_claim(1, 1) == a && grad_find(1, 1) == a, "an endpoint keeps its slot");
    ok(grad_find(1, 2) == NULL && grad_find(0, 0) == NULL, "another endpoint has none, and no address is no lamp");
    b = grad_claim(2, 1);
    grad_begin(b, &want, 0);
    c = grad_claim(3, 1);
    grad_begin(c, &want, 0);
    ok(a != b && b != c && a != c, "three lamps, three slots");
    a->state = ZBC_GRAD_SHOWN;
    b->state = ZBC_GRAD_NOT_TAKEN;
    zbc_grad_t *d = grad_claim(4, 1);
    ok(d == b && d->short_addr == 4 && d->state == ZBC_GRAD_NONE, "a fourth takes the slot of the lamp that took none");
    grad_begin(d, &want, 0);
    zbc_grad_t *e = grad_claim(5, 1);
    ok(e == a, "then of one already showing its gradient");
    grad_begin(e, &want, 0);
    zbc_grad_t *f = grad_claim(6, 1);
    ok(f == c, "and with all still waiting, of the one asked for longest ago");
    grad_free(f);
    ok(grad_find(6, 1) == NULL && grad_claim(7, 1) == f, "a freed slot is the first taken");
}

int main(void)
{
    test_taken();
    test_lost();
    test_bulb();
    test_other_colours();
    test_silent();
    test_newer_request();
    test_found();
    test_frames();
    test_slots();
    printf("ok (%d checks)\n", s_checks);
    return 0;
}
