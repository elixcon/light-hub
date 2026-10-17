/* Host test for the remote gesture machine in buttons.c.
 *
 * A press that arrives over the mesh is already decoded - the remote itself has
 * decided it was a click, or a hold, or the end of one - so zbtn_step has none
 * of btn_step's debouncing and none of its thresholds. What it does have is a
 * clock: the repeats that make a hold sweep, the double-press window, and the
 * cap that ends a hold whose release was lost. All three fail on hardware as
 * "the switch feels wrong sometimes", which nothing in a serial log explains.
 *
 * zremote.inc is extracted from main/buttons.c by the Makefile, never copied.
 *
 *   make -C test
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* From buttons.h and remote.h - the two things the machine is written against. */
#define BTN_DOUBLE_US   (300 * 1000)
#define BTN_REPEAT_US  (1000 * 1000)
#define ZBTN_HOLD_MAX_US (30 * 1000 * 1000)
#define ZBTN_TICK_QUIET_US      (800 * 1000)
#define ZBTN_TICK_QUIET_MIN_US  (350 * 1000)
#define ZBTN_TICK_REPEAT_MIN_US (500 * 1000)
#define ZBTN_TICK_QUIET_MAX_US  (1500 * 1000)
#define ZBTN_TICK_GAP_MAX_US    (1500 * 1000)
#define ZBTN_STOP_GRACE_US      (150 * 1000)

typedef enum {
    ZBC_EMIT_NONE = 0,
    ZBC_EMIT_SHORT,
    ZBC_EMIT_LONG,
    ZBC_EMIT_LONG_REPEAT,
    ZBC_EMIT_LONG_RELEASE,
    ZBC_EMIT_DOUBLE,
    ZBC_EMIT_TRIPLE,
} zbc_btn_emit_t;

typedef enum {
    ZB_REMOTE_NONE = 0, ZB_REMOTE_CLICK, ZB_REMOTE_DOUBLE,
    ZB_REMOTE_HOLD, ZB_REMOTE_RELEASE, ZB_REMOTE_IGNORED, ZB_REMOTE_HOLD_TICK,
    ZB_REMOTE_SINGLE, ZB_REMOTE_TRIPLE,
} zb_remote_ev_t;

typedef struct {
    uint8_t pending_click;
    uint8_t holding;
    uint8_t tick_hold;
    int64_t clicked_at;
    int64_t held_since;
    int64_t next_repeat;
    int64_t last_tick;
    int64_t tick_gap;
    int64_t stop_at;
} zbc_zbtn_fsm_t;

#include "zremote.inc"

/* ------------------------------------------------------------------ harness */

static int s_checks;

static void ok(bool cond, const char *what)
{
    s_checks++;
    if (!cond) { printf("FAIL: %s\n", what); exit(1); }
}

static const char *name(zbc_btn_emit_t e)
{
    switch (e) {
    case ZBC_EMIT_SHORT:        return "short";
    case ZBC_EMIT_LONG:         return "long";
    case ZBC_EMIT_LONG_REPEAT:  return "repeat";
    case ZBC_EMIT_LONG_RELEASE: return "release";
    case ZBC_EMIT_DOUBLE:       return "double";
    case ZBC_EMIT_TRIPLE:       return "triple";
    default:                    return "none";
    }
}

static void expect(zbc_btn_emit_t got, zbc_btn_emit_t want, const char *what)
{
    s_checks++;
    if (got != want) {
        printf("FAIL: %s (got %s, wanted %s)\n", what, name(got), name(want));
        exit(1);
    }
}

/* Runs the sampler forward to `until`, collecting what it emits. The real one
 * ticks every 10 ms, so this does too. */
static int tick_until(zbc_zbtn_fsm_t *st, bool want_double, int64_t *now, int64_t until,
                      zbc_btn_emit_t *out, int max)
{
    int n = 0;
    while (*now < until) {
        *now += 10 * 1000;
        zbc_btn_emit_t e = zbtn_step(st, ZB_REMOTE_NONE, want_double, *now);
        if (e != ZBC_EMIT_NONE && n < max) {
            out[n++] = e;
        }
    }
    return n;
}

/* -------------------------------------------------------------------- tests */

/* A button with nothing on its double press must not make every single click
 * feel 300 ms late - the same bargain btn_step strikes for a finger on a pin. */
static void test_a_click_without_double_rules_is_immediate(void)
{
    zbc_zbtn_fsm_t st = { 0 };
    expect(zbtn_step(&st, ZB_REMOTE_CLICK, false, 1000), ZBC_EMIT_SHORT,
           "a click with no double rules fires at once");
}

/* With double rules it waits, and the sampler is what lets it go. */
static void test_a_click_with_double_rules_waits_out_the_window(void)
{
    zbc_zbtn_fsm_t st = { 0 };
    int64_t now = 1000000;
    expect(zbtn_step(&st, ZB_REMOTE_CLICK, true, now), ZBC_EMIT_NONE,
           "a click with double rules is held back");

    zbc_btn_emit_t got[4];
    int n = tick_until(&st, true, &now, 1000000 + BTN_DOUBLE_US - 20000, got, 4);
    ok(n == 0, "and stays held back inside the window");

    n = tick_until(&st, true, &now, 1000000 + BTN_DOUBLE_US + 30000, got, 4);
    ok(n == 1 && got[0] == ZBC_EMIT_SHORT, "then comes out as a short press");
}

/* Two clicks inside the window are a double, and only one event comes out. */
static void test_two_clicks_inside_the_window_are_a_double(void)
{
    zbc_zbtn_fsm_t st = { 0 };
    int64_t now = 1000000;
    expect(zbtn_step(&st, ZB_REMOTE_CLICK, true, now), ZBC_EMIT_NONE, "first click waits");
    now += BTN_DOUBLE_US / 2;
    expect(zbtn_step(&st, ZB_REMOTE_CLICK, true, now), ZBC_EMIT_DOUBLE, "second click doubles");

    zbc_btn_emit_t got[4];
    int n = tick_until(&st, true, &now, now + 2 * BTN_DOUBLE_US, got, 4);
    ok(n == 0, "and no short press follows it");
}

/* Outside the window they are two separate short presses. */
static void test_two_clicks_outside_the_window_are_two_shorts(void)
{
    zbc_zbtn_fsm_t st = { 0 };
    int64_t now = 1000000;
    zbc_btn_emit_t got[4];

    zbtn_step(&st, ZB_REMOTE_CLICK, true, now);
    int n = tick_until(&st, true, &now, now + BTN_DOUBLE_US + 30000, got, 4);
    ok(n == 1 && got[0] == ZBC_EMIT_SHORT, "the first click comes out alone");

    expect(zbtn_step(&st, ZB_REMOTE_CLICK, true, now), ZBC_EMIT_NONE, "the second waits its turn");
    n = tick_until(&st, true, &now, now + BTN_DOUBLE_US + 30000, got, 4);
    ok(n == 1 && got[0] == ZBC_EMIT_SHORT, "and comes out as its own short press");
}

/* A remote that decodes its own doubles is believed without waiting. */
static void test_a_device_decoded_double_is_immediate(void)
{
    zbc_zbtn_fsm_t st = { 0 };
    expect(zbtn_step(&st, ZB_REMOTE_DOUBLE, true, 1000), ZBC_EMIT_DOUBLE,
           "a double the device decoded fires at once");
}

/* A remote that counts its own presses (an E1744, a BILRESA, an Aqara) has
 * already waited for a second press before sending a single one. Its double
 * rules live on the same key as its single press, so a single that waited out
 * the window again would make every toggle 300 ms later for nothing. */
static void test_a_counted_single_is_immediate_despite_double_rules(void)
{
    zbc_zbtn_fsm_t st = { 0 };
    int64_t now = 1000000;
    expect(zbtn_step(&st, ZB_REMOTE_SINGLE, true, now), ZBC_EMIT_SHORT,
           "a single the device counted fires at once, double rules or not");

    /* And it leaves nothing behind to pair with: the device's own double comes
     * next, as a double, and a second single is a second short press. */
    zbc_btn_emit_t got[4];
    ok(tick_until(&st, true, &now, now + BTN_DOUBLE_US * 2, got, 4) == 0,
       "nothing comes out of the window afterwards");
    now += 1000;
    expect(zbtn_step(&st, ZB_REMOTE_SINGLE, true, now), ZBC_EMIT_SHORT,
           "a second counted single is another short press, not a double");
}

/* A counted single clears a click an unknown remote left waiting, rather than
 * being paired with it. Both are presses of one key; the counted one knows. */
static void test_a_counted_single_does_not_pair_with_a_click(void)
{
    zbc_zbtn_fsm_t st = { 0 };
    int64_t now = 1000000;
    expect(zbtn_step(&st, ZB_REMOTE_CLICK, true, now), ZBC_EMIT_NONE, "a click waits");
    expect(zbtn_step(&st, ZB_REMOTE_SINGLE, true, now + 50000), ZBC_EMIT_SHORT,
           "a counted single is a short press, not the second half of a double");
    zbc_btn_emit_t got[4];
    ok(tick_until(&st, true, &now, now + BTN_DOUBLE_US * 2, got, 4) == 0,
       "and the waiting click is gone");
}

static void test_a_device_decoded_triple_is_immediate(void)
{
    zbc_zbtn_fsm_t st = { 0 };
    int64_t now = 1000000;
    expect(zbtn_step(&st, ZB_REMOTE_CLICK, true, now), ZBC_EMIT_NONE, "a click waits");
    expect(zbtn_step(&st, ZB_REMOTE_TRIPLE, true, now + 50000), ZBC_EMIT_TRIPLE,
           "a triple the device decoded fires at once");
    zbc_btn_emit_t got[4];
    ok(tick_until(&st, true, &now, now + BTN_DOUBLE_US * 2, got, 4) == 0,
       "and takes the waiting click with it");
}

/* The hold is what drives the sweep: one LONG, then a repeat every
 * BTN_REPEAT_US, and nothing in between. */
static void test_a_hold_repeats_at_the_command_rate(void)
{
    zbc_zbtn_fsm_t st = { 0 };
    int64_t now = 1000000;
    expect(zbtn_step(&st, ZB_REMOTE_HOLD, false, now), ZBC_EMIT_LONG,
           "a hold starts sweeping at once");

    zbc_btn_emit_t got[16];
    const int n = tick_until(&st, false, &now, now + 5 * BTN_REPEAT_US + 5000, got, 16);
    ok(n == 5, "five seconds of hold is five commands");
    for (int i = 0; i < n; i++) {
        ok(got[i] == ZBC_EMIT_LONG_REPEAT, "each one a repeat");
    }
}

/* A remote that re-sends Move while the button is down must not restart the
 * hold: a new one flips a reversing dimmer round, mid-sweep. */
static void test_a_second_hold_while_held_is_ignored(void)
{
    zbc_zbtn_fsm_t st = { 0 };
    int64_t now = 1000000;
    zbtn_step(&st, ZB_REMOTE_HOLD, false, now);
    now += BTN_REPEAT_US / 2;
    expect(zbtn_step(&st, ZB_REMOTE_HOLD, false, now), ZBC_EMIT_NONE,
           "a repeated hold changes nothing");

    zbc_btn_emit_t got[4];
    const int n = tick_until(&st, false, &now, 1000000 + BTN_REPEAT_US + 5000, got, 4);
    ok(n == 1 && got[0] == ZBC_EMIT_LONG_REPEAT,
       "and the repeat still lands one interval after the hold began");
}

/* The release is what stops the lamps where the finger left them. */
static void test_a_release_ends_the_hold(void)
{
    zbc_zbtn_fsm_t st = { 0 };
    int64_t now = 1000000;
    zbtn_step(&st, ZB_REMOTE_HOLD, false, now);
    now += 1500000;
    expect(zbtn_step(&st, ZB_REMOTE_RELEASE, false, now), ZBC_EMIT_NONE,
           "the release waits out the grace for a Move that carries on");

    zbc_btn_emit_t got[8];
    int n = tick_until(&st, false, &now, now + ZBTN_STOP_GRACE_US - 20000, got, 8);
    ok(n == 0, "nothing inside the grace");
    n = tick_until(&st, false, &now, now + 40000, got, 8);
    ok(n == 1 && got[0] == ZBC_EMIT_LONG_RELEASE, "then the release ends the sweep");
    n = tick_until(&st, false, &now, now + 4 * BTN_REPEAT_US, got, 8);
    ok(n == 0, "and nothing goes out afterwards");
}

/* A release with nothing held is what a remote sends when its hold never
 * reached us. Stopping lamps that are not ramping is pointless. */
static void test_a_release_with_nothing_held_is_silent(void)
{
    zbc_zbtn_fsm_t st = { 0 };
    expect(zbtn_step(&st, ZB_REMOTE_RELEASE, false, 1000), ZBC_EMIT_NONE,
           "a release out of nowhere does nothing");
}

/* The release is one frame on a link that drops them. Without the cap a lost
 * one sweeps the lights for ever, a command a second. */
static void test_a_lost_release_times_out(void)
{
    zbc_zbtn_fsm_t st = { 0 };
    int64_t now = 1000000;
    zbtn_step(&st, ZB_REMOTE_HOLD, false, now);

    zbc_btn_emit_t got[64];
    int n = tick_until(&st, false, &now, 1000000 + ZBTN_HOLD_MAX_US - BTN_REPEAT_US, got, 64);
    ok(n > 0, "it sweeps while it believes the button is down");
    for (int i = 0; i < n; i++) {
        ok(got[i] == ZBC_EMIT_LONG_REPEAT, "all repeats until the cap");
    }

    n = tick_until(&st, false, &now, 1000000 + ZBTN_HOLD_MAX_US + BTN_REPEAT_US, got, 64);
    ok(n >= 1 && got[n - 1] == ZBC_EMIT_LONG_RELEASE, "the cap ends it with a release");

    n = tick_until(&st, false, &now, now + 5 * BTN_REPEAT_US, got, 64);
    ok(n == 0, "and it stays ended");
}

/* A click arriving mid-hold must not queue a short press behind the sweep.
 *
 * The leak is not during the hold - the repeats are checked first, so nothing
 * else can come out while the button is down - it is the moment after the
 * release, when the waiting click is suddenly well past its window and fires
 * immediately. So the light would dim, stop where the finger left it, and then
 * be switched by a press nobody made. The hold clearing pending_click is what
 * prevents that, and this test is only a test of it if it ticks past the
 * release. */
static void test_a_click_during_a_hold_does_not_leak_out(void)
{
    zbc_zbtn_fsm_t st = { 0 };
    int64_t now = 1000000;
    zbtn_step(&st, ZB_REMOTE_CLICK, true, now);      /* held back for the window */
    now += 50000;
    expect(zbtn_step(&st, ZB_REMOTE_HOLD, true, now), ZBC_EMIT_LONG, "the hold wins");

    zbc_btn_emit_t got[8];
    int n = tick_until(&st, true, &now, now + 2 * BTN_REPEAT_US, got, 8);
    for (int i = 0; i < n; i++) {
        ok(got[i] != ZBC_EMIT_SHORT, "no short press escapes from under the hold");
    }

    expect(zbtn_step(&st, ZB_REMOTE_RELEASE, true, now), ZBC_EMIT_NONE,
           "the hold's release waits out the grace");
    n = tick_until(&st, true, &now, now + 2 * BTN_DOUBLE_US, got, 8);
    ok(n == 1 && got[0] == ZBC_EMIT_LONG_RELEASE,
       "then ends normally, and the click it interrupted does not fire after it");
}

/* Runs a turn: `frames` more ticks `gap` apart, with the sampler going in
 * between, and returns how many releases came out along the way. */
static int turn(zbc_zbtn_fsm_t *st, int64_t *now, int64_t gap, int frames)
{
    zbc_btn_emit_t got[16];
    int releases = 0;
    for (int i = 0; i < frames; i++) {
        const int n = tick_until(st, false, now, *now + gap, got, 16);
        for (int j = 0; j < n; j++) {
            releases += got[j] == ZBC_EMIT_LONG_RELEASE;
        }
        zbtn_step(st, ZB_REMOTE_HOLD_TICK, false, *now);
    }
    return releases;
}

/* Silence after the last frame: nothing may end the hold before `not_before`
 * and a release must have come by `by`. */
static void ends_between(zbc_zbtn_fsm_t *st, int64_t *now, int64_t not_before, int64_t by,
                         const char *what)
{
    zbc_btn_emit_t got[16];
    const int64_t last = *now;
    int n = tick_until(st, false, now, last + not_before - 20000, got, 16);
    for (int j = 0; j < n; j++) {
        ok(got[j] != ZBC_EMIT_LONG_RELEASE, what);
    }
    n = tick_until(st, false, now, last + by + 20000, got, 16);
    ok(n >= 1 && got[n - 1] == ZBC_EMIT_LONG_RELEASE, what);
    n = tick_until(st, false, now, *now + 3 * BTN_REPEAT_US, got, 16);
    ok(n == 0, "and it stays ended");
}

/* A BILRESA wheel never says it has stopped: it sends a frame every 310-330 ms
 * while it turns, then nothing. The hold has to end on silence, but not in the
 * gap between two frames, or the light goes down and back up on every frame of
 * a turn. */
static void test_a_tick_hold_ends_on_silence(void)
{
    zbc_zbtn_fsm_t st = { 0 };
    int64_t now = 1000000;
    expect(zbtn_step(&st, ZB_REMOTE_HOLD_TICK, false, now), ZBC_EMIT_LONG,
           "the first frame of a turn starts the sweep");
    ok(turn(&st, &now, 320000, 10) == 0, "frames 320 ms apart are one sweep, not ten");
    ends_between(&st, &now, 320000 * 3 / 2, 320000 * 3 / 2, "and silence of one and a half gaps ends it");
}

/* The sweep's commands come at the wheel's own pace once it is known - the
 * look-ahead is the same interval, so a second between them is a second the
 * light coasts on after the wheel stops. Not faster than the floor, though. */
static void test_a_wheel_repeats_at_its_own_pace(void)
{
    zbc_zbtn_fsm_t st = { 0 };
    int64_t now = 1000000;
    zbtn_step(&st, ZB_REMOTE_HOLD_TICK, false, now);
    /* Stepped here rather than with tick_until, because the interval between two
     * repeats is the thing being measured and tick_until only says which slice of
     * time an event fell in. */
    int repeats = 0;
    int64_t last_repeat = 0, shortest = BTN_REPEAT_US;
    const int64_t end = now + 10 * 320000;           /* 3.2 s of turning */
    int64_t next_frame = now + 320000;
    while (now < end) {
        now += 10 * 1000;
        const zbc_btn_emit_t e = now >= next_frame
            ? (next_frame += 320000, zbtn_step(&st, ZB_REMOTE_HOLD_TICK, false, now))
            : zbtn_step(&st, ZB_REMOTE_NONE, false, now);
        if (e == ZBC_EMIT_LONG_REPEAT) {
            if (last_repeat && now - last_repeat < shortest) {
                shortest = now - last_repeat;
            }
            last_repeat = now;
            repeats++;
        }
    }
    ok(repeats >= 5, "a wheel turning for 3.2 s repeats at least five times, not three");
    ok(shortest >= ZBTN_TICK_REPEAT_MIN_US - 20000, "and never faster than the floor");
}

/* Every turn after the first already knows the wheel's pace, so its first
 * repeat has to come at that pace too. The first command aims only that far
 * ahead; a second's wait for the next would let the lamp arrive and sit still,
 * mid-turn, every time. */
static void test_a_second_turn_starts_at_the_learned_pace(void)
{
    zbc_zbtn_fsm_t st = { 0 };
    int64_t now = 1000000;
    zbtn_step(&st, ZB_REMOTE_HOLD_TICK, false, now);
    turn(&st, &now, 320000, 5);                        /* learns 320 ms */
    zbc_btn_emit_t got[16];
    tick_until(&st, false, &now, now + ZBTN_TICK_QUIET_MAX_US + 20000, got, 16);
    ok(!st.holding, "the first turn has ended");

    now += 1000000;                                    /* a pause, then turn again */
    const int64_t start = now;
    expect(zbtn_step(&st, ZB_REMOTE_HOLD_TICK, false, now), ZBC_EMIT_LONG, "a second turn starts");
    /* Still turning: the wheel's next frame arrives 320 ms in, as it would. */
    int n = tick_until(&st, false, &now, start + 320000, got, 16);
    ok(n == 0, "nothing before the next frame");
    zbtn_step(&st, ZB_REMOTE_HOLD_TICK, false, now);
    n = tick_until(&st, false, &now, start + ZBTN_TICK_REPEAT_MIN_US + 20000, got, 16);
    ok(n == 1 && got[0] == ZBC_EMIT_LONG_REPEAT, "and repeats after half a second, not a whole one");
}

/* Before a single gap has been measured there is nothing to learn from. */
static void test_an_unmeasured_remote_waits_the_default(void)
{
    zbc_zbtn_fsm_t st = { 0 };
    int64_t now = 1000000;
    expect(zbtn_step(&st, ZB_REMOTE_HOLD_TICK, false, now), ZBC_EMIT_LONG, "one frame starts it");
    ends_between(&st, &now, ZBTN_TICK_QUIET_US, ZBTN_TICK_QUIET_US, "a lone frame waits the default");
}

/* A remote sending every 101 ms must still not keep the lights moving for long
 * after it stops, nor be cut off by one late frame. */
static void test_a_fast_cadence_has_a_floor(void)
{
    zbc_zbtn_fsm_t st = { 0 };
    int64_t now = 1000000;
    zbtn_step(&st, ZB_REMOTE_HOLD_TICK, false, now);
    ok(turn(&st, &now, 100000, 20) == 0, "frames 100 ms apart are one sweep");
    ends_between(&st, &now, ZBTN_TICK_QUIET_MIN_US, ZBTN_TICK_QUIET_MIN_US, "and the floor ends it");
}

/* Against the top of its range the wheel slows to about a frame a second. The
 * gap is learned across the one release that change costs, and after that the
 * hold carries on at the new cadence. */
static void test_a_slower_cadence_is_learned(void)
{
    zbc_zbtn_fsm_t st = { 0 };
    int64_t now = 1000000;
    zbtn_step(&st, ZB_REMOTE_HOLD_TICK, false, now);
    ok(turn(&st, &now, 320000, 5) == 0, "turning at 320 ms");
    ok(turn(&st, &now, 1000000, 1) <= 1, "the first slow gap may cost one release");
    ok(st.holding, "and the next frame starts it again");
    ok(turn(&st, &now, 1000000, 4) == 0, "after which frames a second apart are one sweep");
    ends_between(&st, &now, ZBTN_TICK_QUIET_MAX_US, ZBTN_TICK_QUIET_MAX_US, "capped at the ceiling");
}

/* The quiet release belongs to tick holds alone. An Aqara sends nothing at all
 * between its hold and its release, and cutting that short would stop the lights
 * a third of a second into every hold. */
static void test_a_move_hold_ignores_silence(void)
{
    zbc_zbtn_fsm_t st = { 0 };
    int64_t now = 1000000;
    zbtn_step(&st, ZB_REMOTE_HOLD, false, now);

    zbc_btn_emit_t got[16];
    const int n = tick_until(&st, false, &now, now + 3 * BTN_REPEAT_US + 5000, got, 16);
    ok(n == 3, "three silent seconds are three repeats");
    for (int i = 0; i < n; i++) {
        ok(got[i] == ZBC_EMIT_LONG_REPEAT, "and no release among them");
    }
    ok(st.holding, "the hold is still there for its release");
    expect(zbtn_step(&st, ZB_REMOTE_RELEASE, false, now), ZBC_EMIT_NONE,
           "which starts the grace");
}

/* A Stop followed by a Move of the same turn is not the end of the turn. An
 * E1744 turned steadily sends a Stop every 0.2-1.7 s and the next Move 21-67 ms
 * after it; ending the hold on each one would make a smooth turn stop and start. */
static void test_a_stop_followed_by_a_move_is_one_turn(void)
{
    zbc_zbtn_fsm_t st = { 0 };
    int64_t now = 1000000;
    zbc_btn_emit_t got[16];
    expect(zbtn_step(&st, ZB_REMOTE_HOLD, false, now), ZBC_EMIT_LONG, "the turn starts");
    int n = tick_until(&st, false, &now, now + 300000, got, 16);
    ok(n == 0, "nothing in the first 300 ms");
    expect(zbtn_step(&st, ZB_REMOTE_RELEASE, false, now), ZBC_EMIT_NONE, "a Stop waits");
    n = tick_until(&st, false, &now, now + 50000, got, 16);
    ok(n == 0, "for the next Move");
    expect(zbtn_step(&st, ZB_REMOTE_HOLD, false, now), ZBC_EMIT_NONE, "which carries the turn on");

    n = tick_until(&st, false, &now, 1000000 + 2 * BTN_REPEAT_US + 5000, got, 16);
    ok(n == 2 && got[0] == ZBC_EMIT_LONG_REPEAT && got[1] == ZBC_EMIT_LONG_REPEAT,
       "and the sweep repeats as if the Stop had never come");
}

/* A pending Stop sends no sweep command: that would aim a whole interval ahead
 * of a turn that has most likely ended. */
static void test_no_repeat_while_a_stop_is_pending(void)
{
    zbc_zbtn_fsm_t st = { 0 };
    int64_t now = 1000000;
    zbc_btn_emit_t got[16];
    zbtn_step(&st, ZB_REMOTE_HOLD, false, now);
    now = 1000000 + BTN_REPEAT_US - 50000;
    expect(zbtn_step(&st, ZB_REMOTE_RELEASE, false, now), ZBC_EMIT_NONE, "a Stop just before a repeat");
    const int n = tick_until(&st, false, &now, now + ZBTN_STOP_GRACE_US + 20000, got, 16);
    ok(n == 1 && got[0] == ZBC_EMIT_LONG_RELEASE, "gives the release and no repeat");
}

/* zbtn_end_now is what a reversal uses: the first Move after a Stop is the true
 * direction, so the old hold must not linger through the grace. */
static void test_end_now_ends_at_once(void)
{
    zbc_zbtn_fsm_t st = { 0 };
    int64_t now = 1000000;
    zbc_btn_emit_t got[16];
    zbtn_step(&st, ZB_REMOTE_HOLD, false, now);
    zbtn_step(&st, ZB_REMOTE_RELEASE, false, now + 100000);
    expect(zbtn_end_now(&st), ZBC_EMIT_LONG_RELEASE, "a pending Stop ends at once");
    const int n = tick_until(&st, false, &now, now + ZBTN_STOP_GRACE_US * 3, got, 16);
    ok(n == 0, "and does not release a second time when the grace runs out");
    expect(zbtn_end_now(&st), ZBC_EMIT_NONE, "nothing to end twice");
}

/* A recorded left turn of an E1744, as the down key sees it: its Moves and every
 * Stop, at their times (ms after the first frame). The up Moves in between are
 * latched away before they reach this machine. Must be one sweep and one
 * release. */
static void test_a_traced_symfonisk_turn_is_one_sweep(void)
{
    static const struct { int ms; zb_remote_ev_t ev; } trace[] = {
        {    0, ZB_REMOTE_HOLD },    {  195, ZB_REMOTE_RELEASE }, {  216, ZB_REMOTE_HOLD },
        { 1083, ZB_REMOTE_RELEASE }, { 1130, ZB_REMOTE_HOLD },    { 1414, ZB_REMOTE_HOLD },
        { 1794, ZB_REMOTE_HOLD },    { 1982, ZB_REMOTE_HOLD },    { 2510, ZB_REMOTE_HOLD },
        { 2858, ZB_REMOTE_RELEASE }, { 2907, ZB_REMOTE_HOLD },    { 3432, ZB_REMOTE_RELEASE },
    };
    zbc_zbtn_fsm_t st = { 0 };
    const int64_t t0 = 1000000;
    int64_t now = t0;
    int longs = 0, releases = 0;
    zbc_btn_emit_t got[16];
    for (size_t i = 0; i < sizeof(trace) / sizeof(trace[0]); i++) {
        const int n = tick_until(&st, false, &now, t0 + trace[i].ms * 1000LL - 5000, got, 16);
        for (int j = 0; j < n; j++) {
            releases += got[j] == ZBC_EMIT_LONG_RELEASE;
        }
        now = t0 + trace[i].ms * 1000LL;
        const zbc_btn_emit_t e = zbtn_step(&st, trace[i].ev, false, now);
        longs    += e == ZBC_EMIT_LONG;
        releases += e == ZBC_EMIT_LONG_RELEASE;
    }
    const int n = tick_until(&st, false, &now, now + ZBTN_STOP_GRACE_US + 20000, got, 16);
    for (int j = 0; j < n; j++) {
        releases += got[j] == ZBC_EMIT_LONG_RELEASE;
    }
    ok(longs == 1, "the traced turn starts one sweep");
    ok(releases == 1, "and ends it once, at the last Stop");
}

/* A hold that was released must not remember being a tick hold: an Aqara hold
 * that follows a wheel turn on the same record would otherwise time out. */
static void test_a_released_tick_hold_leaves_no_trace(void)
{
    zbc_zbtn_fsm_t st = { 0 };
    int64_t now = 1000000;
    zbc_btn_emit_t got[16];
    zbtn_step(&st, ZB_REMOTE_HOLD_TICK, false, now);
    tick_until(&st, false, &now, now + ZBTN_TICK_QUIET_US + 20000, got, 16);

    expect(zbtn_step(&st, ZB_REMOTE_HOLD, false, now), ZBC_EMIT_LONG, "a plain hold starts");
    const int n = tick_until(&st, false, &now, now + 2 * BTN_REPEAT_US + 5000, got, 16);
    ok(n == 2 && got[0] == ZBC_EMIT_LONG_REPEAT && got[1] == ZBC_EMIT_LONG_REPEAT,
       "and is not ended by silence");
}

int main(void)
{
    test_a_tick_hold_ends_on_silence();
    test_a_wheel_repeats_at_its_own_pace();
    test_a_second_turn_starts_at_the_learned_pace();
    test_an_unmeasured_remote_waits_the_default();
    test_a_fast_cadence_has_a_floor();
    test_a_slower_cadence_is_learned();
    test_a_move_hold_ignores_silence();
    test_a_released_tick_hold_leaves_no_trace();
    test_a_click_without_double_rules_is_immediate();
    test_a_click_with_double_rules_waits_out_the_window();
    test_two_clicks_inside_the_window_are_a_double();
    test_two_clicks_outside_the_window_are_two_shorts();
    test_a_device_decoded_double_is_immediate();
    test_a_counted_single_is_immediate_despite_double_rules();
    test_a_counted_single_does_not_pair_with_a_click();
    test_a_device_decoded_triple_is_immediate();
    test_a_hold_repeats_at_the_command_rate();
    test_a_second_hold_while_held_is_ignored();
    test_a_release_ends_the_hold();
    test_a_stop_followed_by_a_move_is_one_turn();
    test_no_repeat_while_a_stop_is_pending();
    test_end_now_ends_at_once();
    test_a_traced_symfonisk_turn_is_one_sweep();
    test_a_release_with_nothing_held_is_silent();
    test_a_lost_release_times_out();
    test_a_click_during_a_hold_does_not_leak_out();
    printf("all %d checks passed\n", s_checks);
    return 0;
}
