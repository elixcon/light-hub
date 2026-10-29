/* Host test for the motion sensor machine in buttons.c.
 *
 * A sensor's reports become two events for its rules: motion, once, when the
 * room was quiet, and no motion, once, when its no-motion time has run out. The
 * time is counted from different reports for sensors that say when the room is
 * clear (Hue) and those that never do (Xiaomi). Wrong here, a room goes dark
 * with somebody in it, or never goes dark at all.
 *
 * motion.inc is extracted from main/buttons.c by the Makefile, never copied.
 *
 *   make -C test
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* From buttons.h. */
typedef enum {
    ZBC_EMIT_NONE = 0,
    ZBC_EMIT_SHORT,
    ZBC_EMIT_LONG,
    ZBC_EMIT_LONG_REPEAT,
    ZBC_EMIT_LONG_RELEASE,
    ZBC_EMIT_DOUBLE,
    ZBC_EMIT_TRIPLE,
    ZBC_EMIT_MOTION,
    ZBC_EMIT_CLEAR,
} zbc_btn_emit_t;

#include "motion.inc"

#define S(x)    ((int64_t)(x) * 1000000)
#define HOLD    S(120)
#define TICK    (-1)

static int s_checks;
static void ok(bool cond, const char *what)
{
    s_checks++;
    if (!cond) { printf("FAIL: %s\n", what); exit(1); }
}

/* Ticks the sampler every 10 ms from `from` to `to`, and counts what it says. */
static int ticks(zbc_motion_fsm_t *st, int64_t from, int64_t to, int64_t *cleared_at)
{
    int clears = 0;
    for (int64_t t = from; t <= to; t += 10000) {
        if (motion_step(st, TICK, false, HOLD, t) == ZBC_EMIT_CLEAR) {
            clears++;
            if (cleared_at) *cleared_at = t;
        }
    }
    return clears;
}

/* A Xiaomi: "occupied" at most once a minute while somebody moves, never
 * "unoccupied". The clear is a hold after the last motion. */
static void test_a_sensor_that_never_says_clear(void)
{
    zbc_motion_fsm_t st = { 0 };
    ok(motion_step(&st, 1, false, HOLD, S(10)) == ZBC_EMIT_MOTION, "the first motion runs the motion rules");
    ok(motion_step(&st, 1, false, HOLD, S(70)) == ZBC_EMIT_NONE, "a minute later: only pushes the clear back");
    ok(motion_step(&st, 1, false, HOLD, S(130)) == ZBC_EMIT_NONE, "and again");
    int64_t at = 0;
    ok(ticks(&st, S(131), S(400), &at) == 1, "one clear");
    ok(at >= S(250) && at < S(250) + 10000, "a hold after the last motion");
    ok(motion_step(&st, 1, false, HOLD, S(500)) == ZBC_EMIT_MOTION, "motion after the clear is motion again");
}

/* A Hue: "occupied" once, "unoccupied" when its own timer ends. The hold
 * starts at the "unoccupied". */
static void test_a_sensor_that_says_clear(void)
{
    zbc_motion_fsm_t st = { 0 };
    ok(motion_step(&st, 1, true, HOLD, S(10)) == ZBC_EMIT_MOTION, "motion");
    ok(ticks(&st, S(10), S(600), NULL) == 0, "still occupied ten minutes on: it has not said otherwise");
    ok(motion_step(&st, 0, true, HOLD, S(600)) == ZBC_EMIT_NONE, "unoccupied starts the hold");
    int64_t at = 0;
    ok(ticks(&st, S(600), S(900), &at) == 1, "one clear");
    ok(at >= S(720) && at < S(720) + 10000, "a hold after its unoccupied");
}

/* A sensor configured to report repeats its "unoccupied" every five minutes.
 * Restarting the hold from each repeat would put the clear off for ever once
 * the hold is longer than that. */
static void test_a_repeated_unoccupied_does_not_put_the_clear_off(void)
{
    const int64_t hold = S(600);
    zbc_motion_fsm_t st = { 0 };
    motion_step(&st, 1, true, hold, S(0));
    motion_step(&st, 0, true, hold, S(30));
    motion_step(&st, 0, true, hold, S(330));
    int clears = 0;
    for (int64_t t = S(330); t <= S(700); t += 10000) {
        clears += motion_step(&st, TICK, false, hold, t) == ZBC_EMIT_CLEAR;
    }
    ok(clears == 1, "cleared a hold after the first unoccupied, not after the last");
}

/* The "unoccupied" got lost. A sensor that says it otherwise is taken as clear
 * after the backstop, not left lit for ever. */
static void test_a_lost_unoccupied_runs_into_the_backstop(void)
{
    zbc_motion_fsm_t st = { 0 };
    motion_step(&st, 1, true, HOLD, S(0));
    ok(ticks(&st, S(0), S(1900), NULL) == 0, "not before the backstop and the hold");
    int64_t at = 0;
    ok(ticks(&st, S(1900), S(2000), &at) == 1, "cleared then");
    ok(at >= HOLD + ZBC_MOTION_BACKSTOP_US, "at the hold plus the backstop");
}

/* A sensor still saying "occupied" every five minutes pushes the backstop on:
 * somebody is in the room. */
static void test_a_repeated_occupied_holds_the_room(void)
{
    zbc_motion_fsm_t st = { 0 };
    ok(motion_step(&st, 1, true, HOLD, S(0)) == ZBC_EMIT_MOTION, "motion");
    for (int m = 5; m <= 120; m += 5) {
        ok(motion_step(&st, 1, true, HOLD, S(m * 60)) == ZBC_EMIT_NONE, "a repeat is not motion again");
    }
    ok(ticks(&st, S(0), S(120 * 60), NULL) == 0, "two hours in, still occupied");
}

/* An "unoccupied" while nothing is occupied changes nothing. */
static void test_unoccupied_while_quiet_is_nothing(void)
{
    zbc_motion_fsm_t st = { 0 };
    ok(motion_step(&st, 0, true, HOLD, S(5)) == ZBC_EMIT_NONE, "nothing");
    ok(st.clear_at == 0, "and nothing is due");
    ok(ticks(&st, S(5), S(500), NULL) == 0, "never a clear");
}

/* At boot: the no-motion rules run once, a hold after the start, unless there
 * is motion first - and then it is motion, not a repeat. */
static void test_the_boot_clear(void)
{
    zbc_motion_fsm_t st = { 0 };
    motion_start(&st, HOLD, S(3));
    int64_t at = 0;
    ok(ticks(&st, S(3), S(400), &at) == 1, "one clear after a quiet boot");
    ok(at >= S(123) && at < S(123) + 10000, "a hold after it");

    zbc_motion_fsm_t moved = { 0 };
    motion_start(&moved, HOLD, S(3));
    ok(motion_step(&moved, 1, false, HOLD, S(20)) == ZBC_EMIT_MOTION, "motion soon after the boot runs the motion rules");
    ok(ticks(&moved, S(20), S(139), NULL) == 0, "and moves the clear");
    ok(ticks(&moved, S(139), S(200), NULL) == 1, "to a hold after it");

    zbc_motion_fsm_t told = { 0 };
    motion_start(&told, HOLD, S(3));
    motion_step(&told, 0, true, HOLD, S(10));
    ok(ticks(&told, S(10), S(400), &at) == 1 && at >= S(123), "an unoccupied before it leaves the boot clear where it was");
}

/* A clear is said once. */
static void test_a_clear_is_said_once(void)
{
    zbc_motion_fsm_t st = { 0 };
    motion_step(&st, 1, false, HOLD, 0);
    ok(ticks(&st, 0, S(3600), NULL) == 1, "an hour of ticks, one clear");
}

int main(void)
{
    test_a_sensor_that_never_says_clear();
    test_a_sensor_that_says_clear();
    test_a_repeated_unoccupied_does_not_put_the_clear_off();
    test_a_lost_unoccupied_runs_into_the_backstop();
    test_a_repeated_occupied_holds_the_room();
    test_unoccupied_while_quiet_is_nothing();
    test_the_boot_clear();
    test_a_clear_is_said_once();
    printf("all %d checks passed\n", s_checks);
    return 0;
}
