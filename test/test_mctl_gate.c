/* Host test for the Matter lamps' command gate in main/matter_ctl.cpp, and for
 * when a device counts as gone.
 *
 * The gate decides which command goes to a lamp and which waits; its faults show
 * on hardware only as a light that sometimes does the wrong thing - comes back on
 * after a fade off, skips the warm-off's colour, lags a slider by seconds. The
 * region is plain C inside a C++ file (see test/Makefile), extracted, never
 * copied.
 *
 *   make -C test
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mctl-gate.inc"

static int failures;
static int checks;

#define EXPECT(cond, ...) do {                     \
        checks++;                                  \
        if (!(cond)) {                             \
            printf("\n  line %d: ", __LINE__);     \
            printf(__VA_ARGS__);                   \
            failures++;                            \
        }                                          \
    } while (0)

#define S 1000000LL

static mctl_cmd_t mk(uint8_t kind, uint16_t a, uint16_t b)
{
    mctl_cmd_t c = { 0 };
    c.kind = kind;
    c.a    = a;
    c.b    = b;
    return c;
}

/* The next command the gate lets out, with the counters discarded. */
static bool next(mctl_gate_t *g, int64_t now, bool can_send, mctl_cmd_t *out)
{
    int dropped = 0, timed_out = 0;
    return mctl_gate_next(g, now, can_send, out, &dropped, &timed_out);
}

static void test_first_goes_second_waits(void)
{
    mctl_gate_t g = { 0 };
    mctl_cmd_t  c;
    const mctl_cmd_t on = mk(MCTL_CMD_ONOFF, 1, 0);
    mctl_gate_admit(&g, &on, 0);
    EXPECT(next(&g, 0, true, &c) && c.kind == MCTL_CMD_ONOFF, "an idle lamp's command did not go");
    EXPECT(!next(&g, 1000, true, &c), "a second command went while the first was unanswered");

    /* Two levels while the On is out: the newer replaces the older. */
    const mctl_cmd_t l1 = mk(MCTL_CMD_LEVEL, 50, 0), l2 = mk(MCTL_CMD_LEVEL, 200, 0);
    mctl_gate_admit(&g, &l1, 1000);
    mctl_gate_admit(&g, &l2, 2000);
    EXPECT(!next(&g, 3000, true, &c), "a queued command overtook the one on its way");
    mctl_gate_done(&g, 4000);
    EXPECT(next(&g, 4000, true, &c) && c.kind == MCTL_CMD_LEVEL && c.a == 200,
           "the newer level did not replace the older (got kind %u a %u)", c.kind, c.a);
    mctl_gate_done(&g, 5000);
    EXPECT(!g.busy && !next(&g, 5000, true, &c), "the gate did not end idle");
}

static void test_level_before_colour(void)
{
    mctl_gate_t g = { 0 };
    mctl_cmd_t  c;
    const mctl_cmd_t first = mk(MCTL_CMD_LEVEL, 10, 0);
    mctl_gate_admit(&g, &first, 0);
    next(&g, 0, true, &c);
    /* A curve's colour, then its level: both wait; the level goes first. */
    const mctl_cmd_t ct = mk(MCTL_CMD_CT, 370, 0), lvl = mk(MCTL_CMD_LEVEL, 100, 0);
    mctl_gate_admit(&g, &ct, 10);
    mctl_gate_admit(&g, &lvl, 20);
    mctl_gate_done(&g, 30);
    EXPECT(next(&g, 30, true, &c) && c.kind == MCTL_CMD_LEVEL, "the colour went before its level");
    mctl_gate_done(&g, 40);
    EXPECT(next(&g, 40, true, &c) && c.kind == MCTL_CMD_CT && c.a == 370,
           "the colour was lost behind the level");

    /* Hue and xy are colour too: one replaces the other, neither replaces a level. */
    const mctl_cmd_t hs = mk(MCTL_CMD_HS, 1, 2), xy = mk(MCTL_CMD_XY, 3, 4), l = mk(MCTL_CMD_LEVEL, 5, 0);
    mctl_gate_admit(&g, &hs, 50);
    mctl_gate_admit(&g, &l, 51);
    mctl_gate_admit(&g, &xy, 52);
    mctl_gate_done(&g, 60);
    EXPECT(next(&g, 60, true, &c) && c.kind == MCTL_CMD_LEVEL, "a colour replaced a level");
    mctl_gate_done(&g, 70);
    EXPECT(next(&g, 70, true, &c) && c.kind == MCTL_CMD_XY && c.a == 3, "xy did not replace hue");
}

/* The warm-off is two calls: a dim to the bottom, then the colour-and-off. The
 * second must queue behind the first - were the first to wait for the kick, the
 * second would replace it and the lamp would never be parked. */
static void test_warm_off_keeps_its_dim(void)
{
    mctl_gate_t g = { 0 };
    mctl_cmd_t  c;
    const mctl_cmd_t dim = mk(MCTL_CMD_LEVEL, 3, 0), warm = mk(MCTL_CMD_WARM_OFF, 454, 0);
    mctl_gate_admit(&g, &dim, 0);          /* both before anything is sent */
    mctl_gate_admit(&g, &warm, 0);
    EXPECT(next(&g, 1000, true, &c) && c.kind == MCTL_CMD_LEVEL && c.a == 3, "the dim was replaced");
    mctl_gate_done(&g, 2000);
    EXPECT(next(&g, 2000, true, &c) && c.kind == MCTL_CMD_WARM_OFF && c.step == 0,
           "the warm-off's colour did not follow the dim");
    EXPECT(!next(&g, 2001, true, &c), "the off overtook the colour");
    mctl_gate_done(&g, 3000);
    EXPECT(next(&g, 3000, true, &c) && c.kind == MCTL_CMD_WARM_OFF && c.step == 1 && c.a == 454,
           "the off did not follow its colour");
    /* A level asked for meanwhile waits for the off, not the other way round. */
    const mctl_cmd_t lvl = mk(MCTL_CMD_LEVEL, 100, 0);
    mctl_gate_admit(&g, &lvl, 3100);
    EXPECT(!next(&g, 3200, true, &c), "a level overtook the off");
    mctl_gate_done(&g, 4000);
    EXPECT(next(&g, 4000, true, &c) && c.kind == MCTL_CMD_LEVEL, "the level after the off was lost");
}

static void test_fade_in_is_two_steps(void)
{
    mctl_gate_t g = { 0 };
    mctl_cmd_t  c;
    const mctl_cmd_t fi = mk(MCTL_CMD_FADE_IN, 120, 0);
    mctl_gate_admit(&g, &fi, 0);
    EXPECT(next(&g, 0, true, &c) && c.kind == MCTL_CMD_FADE_IN && c.step == 0, "no preset first");
    /* A ramp asked for before the preset is answered does not replace the ramp
     * half of the fade in: it waits. */
    const mctl_cmd_t lvl = mk(MCTL_CMD_LEVEL, 80, 0);
    mctl_gate_admit(&g, &lvl, 10);
    mctl_gate_done(&g, 20);
    EXPECT(next(&g, 20, true, &c) && c.kind == MCTL_CMD_FADE_IN && c.step == 1 && c.a == 120,
           "the ramp did not follow the preset");
    mctl_gate_done(&g, 30);
    EXPECT(next(&g, 30, true, &c) && c.kind == MCTL_CMD_LEVEL && c.a == 80, "the later level was lost");
    /* A failed preset still lets the ramp out: done is done, answered or not. */
    mctl_gate_done(&g, 40);
    mctl_gate_admit(&g, &fi, 50);
    next(&g, 50, true, &c);
    mctl_gate_done(&g, 60);
    EXPECT(next(&g, 60, true, &c) && c.step == 1, "the ramp was skipped after the preset");
}

static void test_waiting_for_a_session(void)
{
    mctl_gate_t g = { 0 };
    mctl_cmd_t  c;
    int dropped = 0, timed_out = 0;
    const mctl_cmd_t on = mk(MCTL_CMD_ONOFF, 1, 0);
    mctl_gate_admit(&g, &on, 0);
    EXPECT(!mctl_gate_next(&g, 5 * S, false, &c, &dropped, &timed_out), "a command went with no session");
    EXPECT(mctl_gate_next(&g, 10 * S, true, &c, &dropped, &timed_out) && c.kind == MCTL_CMD_ONOFF,
           "a command held for the session did not go once it was up");
    EXPECT(dropped == 0, "a command held for 10 s was dropped");

    /* Held past the stale limit, it is dropped - whether or not the session came -
     * and whatever waited behind it gets its own chance. */
    mctl_gate_t h = { 0 };
    const mctl_cmd_t lvl = mk(MCTL_CMD_LEVEL, 7, 0);
    mctl_gate_admit(&h, &on, 0);
    EXPECT(!mctl_gate_next(&h, MCTL_STALE_US + 1, false, &c, &dropped, &timed_out), "a stale command went");
    EXPECT(dropped == 1 && !h.busy, "a stale command was not dropped (%d)", dropped);
    mctl_gate_admit(&h, &on, 100 * S);
    EXPECT(mctl_gate_next(&h, 100 * S, true, &c, &dropped, &timed_out), "the On did not go");
    /* A level asked for a second before the unanswered On is given up on. */
    mctl_gate_admit(&h, &lvl, 100 * S + MCTL_GATE_TIMEOUT_US - 1 * S);
    dropped = 0;
    EXPECT(mctl_gate_next(&h, 100 * S + MCTL_GATE_TIMEOUT_US, true, &c, &dropped, &timed_out) &&
               c.kind == MCTL_CMD_LEVEL,
           "an answer that never came held the gate shut");
    EXPECT(timed_out == 1 && dropped == 0, "timed out %d, dropped %d", timed_out, dropped);
    /* Behind a command that hangs until the timeout, anything asked for early is
     * stale by the time the gate opens, and goes nowhere. */
    mctl_gate_done(&h, 200 * S);
    mctl_gate_admit(&h, &on, 200 * S);
    mctl_gate_next(&h, 200 * S, true, &c, &dropped, &timed_out);
    mctl_gate_admit(&h, &lvl, 201 * S);
    dropped = timed_out = 0;
    EXPECT(!mctl_gate_next(&h, 200 * S + MCTL_GATE_TIMEOUT_US, true, &c, &dropped, &timed_out),
           "a command 19 s old went out");
    EXPECT(timed_out == 1 && dropped == 1 && !h.busy, "timed out %d, dropped %d", timed_out, dropped);
}

static void test_unreachable(void)
{
    EXPECT(!mctl_refuses(true, 0, 1000 * S), "a device that is up was refused");
    EXPECT(!mctl_refuses(false, 0, 1 * S), "a device still connecting at boot was refused");
    EXPECT(!mctl_refuses(false, 100 * S, 100 * S + MCTL_UNREACH_US), "refused at exactly the limit");
    EXPECT(mctl_refuses(false, 100 * S, 100 * S + MCTL_UNREACH_US + 1), "a device down past the limit was not refused");
}

int main(void)
{
    test_first_goes_second_waits();
    test_level_before_colour();
    test_warm_off_keeps_its_dim();
    test_fade_in_is_two_steps();
    test_waiting_for_a_session();
    test_unreachable();
    if (failures) {
        printf("\n%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("all %d checks passed\n", checks);
    return 0;
}
