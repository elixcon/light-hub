/* Host test for the pin gesture machines in buttons.c: btn_step (a push button)
 * and btn_switch_step (a switch that stays where it is put).
 *
 * Extracted from the source at build time (region `btnstep`). What matters here
 * shows up on a wall as "the light toggles by itself": a switch left on must not
 * toggle at boot, contact bounce must not count as flips, and a push button must
 * keep its short/long behaviour.
 *
 *   make -C test
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* From buttons.h - what the machines are written against. */
#define BTN_DEBOUNCE_US  (30 * 1000)
#define BTN_LONG_US     (600 * 1000)
#define BTN_DOUBLE_US   (300 * 1000)
#define BTN_REPEAT_US  (1000 * 1000)

typedef enum {
    ZBC_EMIT_NONE = 0,
    ZBC_EMIT_SHORT,
    ZBC_EMIT_LONG,
    ZBC_EMIT_LONG_REPEAT,
    ZBC_EMIT_LONG_RELEASE,
    ZBC_EMIT_DOUBLE,
    ZBC_EMIT_TRIPLE,
} zbc_btn_emit_t;

typedef struct {
    uint8_t raw;
    uint8_t stable;
    uint8_t swallow;
    uint8_t pending_short;
    int64_t raw_since;
    int64_t pressed_at;
    int64_t released_at;
    uint8_t long_fired;
    int64_t next_repeat;
    uint8_t primed;
} zbc_btn_fsm_t;

#include "btnstep.inc"

static int s_checks;
static void ok(bool cond, const char *what)
{
    s_checks++;
    if (!cond) { printf("FAIL: %s\n", what); exit(1); }
}

#define TICK (10 * 1000)

/* Holds a level for `ms`, counting what the switch machine emits. */
static int sw_run(zbc_btn_fsm_t *st, int level, bool inverted, int ms, int64_t *now)
{
    int shorts = 0;
    for (int t = 0; t < ms; t += 10) {
        *now += TICK;
        zbc_btn_emit_t e = btn_switch_step(st, level, inverted, *now);
        ok(e == ZBC_EMIT_NONE || e == ZBC_EMIT_SHORT, "a switch emits only short presses");
        shorts += e == ZBC_EMIT_SHORT;
    }
    return shorts;
}

static void test_switch(void)
{
    zbc_btn_fsm_t st = { 0 };
    int64_t now = 1000000;

    /* Found on at boot: taken as it stands. */
    ok(sw_run(&st, 1, false, 500, &now) == 0, "a switch already on at boot does not toggle");
    ok(sw_run(&st, 0, false, 100, &now) == 1, "flipped off: one press");
    ok(sw_run(&st, 0, false, 2000, &now) == 0, "left off: nothing more, no long press");
    ok(sw_run(&st, 1, false, 100, &now) == 1, "flipped on: one press");
    ok(sw_run(&st, 1, false, 2000, &now) == 0, "left on: nothing more");

    /* Found off at boot. */
    zbc_btn_fsm_t off = { 0 };
    ok(sw_run(&off, 0, false, 500, &now) == 0, "a switch off at boot does not toggle");
    ok(sw_run(&off, 1, false, 100, &now) == 1, "then on: one press");

    /* Bounce shorter than the debounce is not a flip. */
    zbc_btn_fsm_t b = { 0 };
    sw_run(&b, 0, false, 200, &now);
    ok(sw_run(&b, 1, false, 10, &now) + sw_run(&b, 0, false, 10, &now) +
       sw_run(&b, 1, false, 20, &now) + sw_run(&b, 0, false, 200, &now) == 0, "bounce is not a flip");
    ok(sw_run(&b, 1, false, 20, &now) + sw_run(&b, 0, false, 20, &now) +
       sw_run(&b, 1, false, 200, &now) == 1, "bouncing into a flip is one press");

    /* Wired to GND with a pull-up: low is on. */
    zbc_btn_fsm_t inv = { 0 };
    ok(sw_run(&inv, 1, true, 300, &now) == 0, "inverted, off at boot");
    ok(sw_run(&inv, 0, true, 100, &now) == 1, "inverted: pulled low is a flip");
    ok(inv.stable == 1, "and reads as on");

    /* A bounce during the first reading does not become a flip either. */
    zbc_btn_fsm_t first = { 0 };
    ok(sw_run(&first, 1, false, 20, &now) + sw_run(&first, 0, false, 20, &now) +
       sw_run(&first, 1, false, 300, &now) == 0, "bouncing while first read: no press");
}

static zbc_btn_emit_t bt_run(zbc_btn_fsm_t *st, int level, int ms, int64_t *now, bool want_double)
{
    zbc_btn_emit_t last = ZBC_EMIT_NONE;
    for (int t = 0; t < ms; t += 10) {
        *now += TICK;
        zbc_btn_emit_t e = btn_step(st, level, false, want_double, *now);
        if (e != ZBC_EMIT_NONE) last = e;
    }
    return last;
}

static void test_button(void)
{
    zbc_btn_fsm_t st = { 0 };
    int64_t now = 1000000;
    bt_run(&st, 0, 200, &now, false);
    ok(bt_run(&st, 1, 200, &now, false) == ZBC_EMIT_NONE, "push button: nothing while briefly down");
    ok(bt_run(&st, 0, 100, &now, false) == ZBC_EMIT_SHORT, "push button: short on release");
    ok(bt_run(&st, 1, 700, &now, false) == ZBC_EMIT_LONG, "push button: long while held");
    ok(bt_run(&st, 0, 100, &now, false) == ZBC_EMIT_LONG_RELEASE, "push button: release ends the hold");
}

int main(void)
{
    test_switch();
    test_button();
    printf("all %d checks passed\n", s_checks);
    return 0;
}
