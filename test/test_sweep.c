/* Host test for the dim sweep model in buttons.c.
 *
 * A held button is driven by the clock: each tick works out where the sweep
 * should be, aims one interval beyond that, and the release reckons where it
 * got to. The failure mode is not a crash - it is a light that lurches when you
 * let go, or dims at the wrong speed, and neither is visible in a log. A hold
 * can sweep several rooms and lights at once, one sweep per dimming rule of its
 * case, and those must neither disturb each other nor lose step on a turn.
 *
 * sweep.inc is extracted from main/buttons.c by the Makefile, never copied.
 *
 *   make -C test
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_ZBC_MAX_BUTTONS 8
#define ZBC_UNKNOWN (-1)
#define ZBC_BTN_MAX_SWEEPS 4
#define ZBC_RULE_AND 0x01

/* The timings the model is written against, from buttons.h. */
#define BTN_STEP_US    (250 * 1000)
#define BTN_REPEAT_US (1000 * 1000)

typedef enum {
    ZBC_ACT_NONE = 0, ZBC_ACT_ON, ZBC_ACT_OFF, ZBC_ACT_TOGGLE,
    ZBC_ACT_SET, ZBC_ACT_DIM_UP, ZBC_ACT_DIM_DOWN, ZBC_ACT_DIM_CYCLE,
} zbc_btn_action_t;

typedef struct {
    uint16_t group_id;
    uint8_t  cond, cond_arg, action, action_arg, flags, ep;
    uint8_t  ieee[8];
} zbc_btn_rule_t;

static bool button_db_rule_assigned(const zbc_btn_rule_t *r) { return r->ep != 0 || r->group_id != 0; }

typedef struct {
    uint16_t group_id;
    uint8_t  ieee[8];
    uint8_t  ep;
} lighting_target_t;

typedef struct {
    lighting_target_t target;
    int16_t from, rate;
    uint8_t action;
    bool    active;
} btn_sweep_t;

/* Only the fields the sweep model touches. */
typedef struct {
    int8_t      dim_dir;
    int64_t     ramp_since;
    btn_sweep_t sweep[ZBC_BTN_MAX_SWEEPS];
    int32_t     hold_iv_us;
} btn_live_t;

static btn_live_t s_live[CONFIG_ZBC_MAX_BUTTONS];

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
static portMUX_TYPE s_live_mux = portMUX_INITIALIZER_UNLOCKED;
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m)  ((void)(m))

/* ---- the lamps, stubbed: record what was commanded ----
 *
 * Targets are rooms 1-4 here (a light's target reaches lighting.c the same way,
 * and what lighting.c does with it is not this model's business). */

#define ROOMS 5
static bool s_on[ROOMS];
static bool s_known_off[ROOMS];
static int  s_pct[ROOMS];

static struct { int room; int pct; int transition; } s_cmd[256];
static int s_cmd_n;
static int s_stop[ROOMS];

static bool lighting_target_state(const lighting_target_t *t, bool *on, int *pct)
{
    if (t->group_id >= ROOMS) return false;
    if (on)  *on = s_on[t->group_id];
    if (pct) *pct = s_pct[t->group_id];
    return true;
}
static int lighting_target_ramp_to(const lighting_target_t *t, int pct, uint16_t transition)
{
    s_cmd[s_cmd_n].room = t->group_id;
    s_cmd[s_cmd_n].pct = pct;
    s_cmd[s_cmd_n].transition = transition;
    s_cmd_n++;
    return 0;
}
static int lighting_target_stop_ramp(const lighting_target_t *t, int pct) { s_stop[t->group_id] = pct; return 0; }
static bool lighting_target_known_off(const lighting_target_t *t) { return s_known_off[t->group_id]; }

/* Room 1, the one every single-rule test uses. */
#define s_group_on        s_on[1]
#define s_group_known_off s_known_off[1]
#define s_stop_pct        s_stop[1]

/* One rule for the single-rule tests, as the old one-rule model took it. */
#define run_dim(slot, r, first, now) run_hold((slot), (r), 1, (first), (now))

#include "sweep.inc"

/* ---- helpers ---- */

static int s_checks;
static void ok(bool cond, const char *what)
{
    s_checks++;
    if (!cond) { printf("FAIL: %s\n", what); exit(1); }
}

static void reset(int start_pct)
{
    memset(s_live, 0, sizeof(s_live));
    s_live[0].dim_dir = -1;      /* the first hold flips this, so it dims up */
    s_cmd_n = 0;
    for (int g = 0; g < ROOMS; g++) {
        s_stop[g] = -1;
        s_on[g] = true;
        s_known_off[g] = false;
        s_pct[g] = start_pct;
    }
}

/* The last command sent to a room, or -1. */
static int last_cmd(int room)
{
    for (int i = s_cmd_n - 1; i >= 0; i--) {
        if (s_cmd[i].room == room) return s_cmd[i].pct;
    }
    return -1;
}

static int cmds_to(int room)
{
    int n = 0;
    for (int i = 0; i < s_cmd_n; i++) n += s_cmd[i].room == room;
    return n;
}

static zbc_btn_rule_t cycle(int step)
{
    return (zbc_btn_rule_t){ .group_id = 1, .action = ZBC_ACT_DIM_CYCLE,
                             .action_arg = (uint8_t)step };
}

/* Drive a hold: the LONG tick, then a LONG_REPEAT every BTN_REPEAT_US, then the
 * release - exactly the sequence btn_step emits. Returns where the release
 * reckoned the sweep had got to. */
static int hold(const zbc_btn_rule_t *r, int64_t t0, int64_t hold_us)
{
    run_dim(0, r, true, t0);
    for (int64_t t = t0 + BTN_REPEAT_US; t <= t0 + hold_us; t += BTN_REPEAT_US) {
        run_dim(0, r, false, t);
    }
    run_release(0, t0 + hold_us);
    return s_stop_pct;
}

/* ---- the tests ---- */

/* The step in the editor is per BTN_STEP_US; the sweep must run at that speed
 * however often commands actually go out. */
static void test_the_sweep_runs_at_the_editor_s_rate(void)
{
    reset(20);
    const zbc_btn_rule_t r = cycle(10);          /* 10 % per 250 ms = 40 %/s */
    const int64_t t0 = 1000000;

    run_dim(0, &r, true, t0);
    ok(s_cmd_n == 1, "the first tick commands once");
    ok(s_cmd[0].pct == 60, "and aims one interval ahead: 20 + 40 = 60");
    ok(s_cmd[0].transition == BTN_REPEAT_US / 100000,
       "with a transition equal to the gap to the next command");

    run_dim(0, &r, false, t0 + BTN_REPEAT_US);
    ok(s_cmd[1].pct == 100, "the next aims another interval on");
}

/* The rail pause: the tick that turns round sends nothing, so the light rests
 * at full long enough to let go there. */
static void test_the_rail_tick_sends_nothing(void)
{
    reset(20);
    const zbc_btn_rule_t r = cycle(10);
    const int64_t t0 = 1000000;

    run_dim(0, &r, true, t0);                     /* 20 -> aims 60 */
    run_dim(0, &r, false, t0 + BTN_REPEAT_US);    /* 60 -> aims 100 */
    const int before = s_cmd_n;
    run_dim(0, &r, false, t0 + 2 * BTN_REPEAT_US);  /* at the rail: turn round */
    ok(s_cmd_n == before, "the tick at the rail commands nothing");
    ok(s_live[0].dim_dir == -1, "and the sweep has turned round");
}

/* The fault this test exists for. After the pause the light is still at the
 * rail, so the next command must resume from there - not from a position the
 * model had run on to while the lamp stood still. */
static void test_the_sweep_resumes_from_the_rail(void)
{
    reset(20);
    const zbc_btn_rule_t r = cycle(10);
    const int64_t t0 = 1000000;

    run_dim(0, &r, true, t0);
    run_dim(0, &r, false, t0 + BTN_REPEAT_US);
    run_dim(0, &r, false, t0 + 2 * BTN_REPEAT_US);   /* rail, turn round, silent */

    const int before = s_cmd_n;
    run_dim(0, &r, false, t0 + 3 * BTN_REPEAT_US);
    ok(s_cmd_n == before + 1, "the tick after the pause commands");
    /* One interval down from the rail, not two: two would ask the lamp to
     * travel 80 % in the second it has, at double the sweep rate. */
    ok(s_cmd[s_cmd_n - 1].pct == 60,
       "it aims one interval below the rail, so the lamp travels at the sweep's rate");
}

/* ...and the release must reckon from the same place, or letting go during the
 * pause commands a jump of up to a whole interval's travel. */
static void test_releasing_during_the_pause_does_not_lurch(void)
{
    const zbc_btn_rule_t r = cycle(10);
    const int64_t t0 = 1000000;

    /* Let go at five points across the pause; the light is at the rail for all
     * of them, so every one must reckon the sweep is still at the rail. */
    for (int i = 0; i <= 4; i++) {
        reset(20);
        run_dim(0, &r, true, t0);
        run_dim(0, &r, false, t0 + BTN_REPEAT_US);
        run_dim(0, &r, false, t0 + 2 * BTN_REPEAT_US);      /* rail */
        run_release(0, t0 + 2 * BTN_REPEAT_US + i * (BTN_REPEAT_US / 4));
        ok(s_stop_pct == 100, "letting go during the rail pause lands at the rail");
    }
}

/* Once the sweep really has resumed, the release tracks it down again. */
static void test_the_release_tracks_the_sweep_after_the_pause(void)
{
    reset(20);
    const zbc_btn_rule_t r = cycle(10);
    const int64_t t0 = 1000000;

    run_dim(0, &r, true, t0);
    run_dim(0, &r, false, t0 + BTN_REPEAT_US);
    run_dim(0, &r, false, t0 + 2 * BTN_REPEAT_US);          /* rail */
    run_dim(0, &r, false, t0 + 3 * BTN_REPEAT_US);          /* resumes at 100 */
    run_release(0, t0 + 3 * BTN_REPEAT_US + BTN_REPEAT_US / 2);
    ok(s_stop_pct == 80, "half an interval past the rail is 20 % down from it");
}

/* The rail must not be re-triggered every tick once the sweep is resting on it,
 * or the direction flips for ever and no command is ever sent again. */
static void test_the_rail_does_not_latch(void)
{
    reset(20);
    const zbc_btn_rule_t r = cycle(10);
    const int64_t t0 = 1000000;

    run_dim(0, &r, true, t0);
    int commands = 0;
    for (int i = 1; i <= 12; i++) {
        const int before = s_cmd_n;
        run_dim(0, &r, false, t0 + (int64_t)i * BTN_REPEAT_US);
        commands += (s_cmd_n > before);
    }
    ok(commands >= 8, "a long hold keeps commanding rather than sticking at a rail");
    ok(s_live[0].dim_dir == 1 || s_live[0].dim_dir == -1, "and the direction stays sane");
}

/* A plain dim up stops at the top instead of turning round. */
static void test_a_plain_dim_stops_at_the_rail(void)
{
    reset(20);
    zbc_btn_rule_t r = cycle(10);
    r.action = ZBC_ACT_DIM_UP;
    const int64_t t0 = 1000000;

    run_dim(0, &r, true, t0);
    run_dim(0, &r, false, t0 + BTN_REPEAT_US);
    const int before = s_cmd_n;
    for (int i = 2; i <= 5; i++) {
        run_dim(0, &r, false, t0 + (int64_t)i * BTN_REPEAT_US);
    }
    ok(s_cmd_n == before, "a plain dim up sends nothing more once it is at the top");
    ok(s_live[0].dim_dir == -1 && s_live[0].ramp_since == t0, "and does not turn round");
}

/* A new hold reverses the direction the last one ended on, so coming back up
 * from 40 % does not mean riding all the way down first. */
static void test_a_new_hold_reverses(void)
{
    reset(50);
    const zbc_btn_rule_t r = cycle(10);
    run_dim(0, &r, true, 1000000);
    const int8_t first = s_live[0].dim_dir;
    run_dim(0, &r, true, 9000000);
    ok(s_live[0].dim_dir == -first, "the second hold goes the other way");
}

/* Holding a dark room dims up from the bottom rather than restoring a level. */
static void test_a_dark_room_sweeps_up_from_the_bottom(void)
{
    reset(0);
    s_group_on = false;
    const zbc_btn_rule_t r = cycle(10);
    run_dim(0, &r, true, 1000000);
    ok(s_cmd_n == 1 && s_cmd[0].pct == 40, "it starts from nothing and climbs");
}

/* ...and asking a dark room to go darker does nothing at all. */
static void test_a_dark_room_asked_to_dim_down_does_nothing(void)
{
    reset(0);
    s_group_on = false;
    zbc_btn_rule_t r = cycle(10);
    r.action = ZBC_ACT_DIM_DOWN;
    run_dim(0, &r, true, 1000000);
    ok(s_cmd_n == 0, "nothing is commanded");
    run_release(0, 2000000);
    ok(s_stop_pct == -1, "and the release has nothing to stop");
}

/* A wheel's hold aims only its own interval ahead, and takes that long over each
 * step - a second of look-ahead would be a second the light coasts on after the
 * wheel stops. A pin keeps the second (hold_iv_us 0), which every other test
 * here is written against. */
static void test_a_wheel_aims_only_its_own_interval_ahead(void)
{
    reset(50);
    s_live[0].hold_iv_us = 500000;
    const zbc_btn_rule_t r = cycle(10);           /* 40 %/s, and the first hold goes up */
    run_dim(0, &r, true, 1000000);
    ok(s_cmd_n == 1 && s_cmd[0].pct == 70, "half a second ahead of 50 at 40 %/s is 70");
    ok(s_cmd[0].transition == 5, "over half a second");
}

/* Members that have stopped answering are neither on nor off, and "not on" must
 * not be taken for "dark", or a turn down does nothing. The last level is still
 * known, and a sweep starts from it. */
static void test_unknown_members_still_dim_down_from_the_last_level(void)
{
    reset(60);
    s_group_on = false;             /* nobody reports on ... */
    s_group_known_off = false;      /* ... and nobody reports off */
    zbc_btn_rule_t r = cycle(10);
    r.action = ZBC_ACT_DIM_DOWN;
    run_dim(0, &r, true, 1000000);
    ok(s_cmd_n == 1 && s_cmd[0].pct < 60 && s_cmd[0].pct > 0, "it dims down from the level last commanded");
}

/* ...but a room a member says is dark stays a dark room. */
static void test_a_known_dark_room_ignores_a_stale_level(void)
{
    reset(60);
    s_group_on = false;
    s_group_known_off = true;
    zbc_btn_rule_t r = cycle(10);
    r.action = ZBC_ACT_DIM_DOWN;
    run_dim(0, &r, true, 1000000);
    ok(s_cmd_n == 0, "a group that is off is not dimmed down from an old level");
}

/* A release with no sweep running must not command anything. */
static void test_a_release_without_a_sweep_is_silent(void)
{
    reset(50);
    run_release(0, 1000000);
    ok(s_stop_pct == -1, "no sweep, no stop");
}

/* The reckoning stays inside the range whatever the hold length. */
static void test_the_reckoning_stays_in_range(void)
{
    const zbc_btn_rule_t r = cycle(10);
    for (int ms = 0; ms <= 20000; ms += 137) {
        reset(50);
        const int landed = hold(&r, 1000000, (int64_t)ms * 1000);
        ok(landed >= 1 && landed <= 100, "the sweep lands inside 1-100");
    }
}

/* ------------------------------------------- the direction of a nudge stream */

/* A remote that dims by repeating a Step decides the direction itself, and for a
 * one-button remote that decision is not the user's: a Hue Smart Button always
 * starts a hold going down. dim_cycle is what makes each new hold reverse, and
 * these two helpers are that decision. */
static void test_a_plain_dim_nudge_keeps_its_direction(void)
{
    int8_t d = -1;
    ok(nudge_dir(&d, ZBC_ACT_DIM_DOWN, true)  == -1, "dim_down nudges down");
    ok(nudge_dir(&d, ZBC_ACT_DIM_DOWN, false) == -1, "and goes on doing so");
    ok(nudge_dir(&d, ZBC_ACT_DIM_UP, true)    ==  1, "dim_up nudges up");
    ok(nudge_dir(&d, ZBC_ACT_DIM_UP, false)   ==  1, "and goes on doing so");
}

/* DIM_CYCLE cycles. Taking the direction as `action == DIM_DOWN ? -1 : 1` would
 * make it - not being DIM_DOWN - always nudge up. */
static void test_a_cycling_nudge_reverses_on_each_new_stream(void)
{
    int8_t d = -1;
    const int8_t first = nudge_dir(&d, ZBC_ACT_DIM_CYCLE, true);
    ok(first == 1, "the first stream flips the resting direction, as a hold does");
    ok(nudge_dir(&d, ZBC_ACT_DIM_CYCLE, false) == first, "and it holds within the stream");
    ok(nudge_dir(&d, ZBC_ACT_DIM_CYCLE, false) == first, "however many steps it runs to");

    const int8_t second = nudge_dir(&d, ZBC_ACT_DIM_CYCLE, true);
    ok(second == -first, "a new stream goes the other way");
    ok(nudge_dir(&d, ZBC_ACT_DIM_CYCLE, true) == first, "and the one after that turns back");
}

/* And a stream long enough to reach a rail turns round there rather than sitting
 * on it - the same rule run_hold applies to a swept hold. */
static void test_a_cycling_nudge_turns_round_at_a_rail(void)
{
    int8_t d = 1;
    nudge_note_landing(&d, ZBC_ACT_DIM_CYCLE, 1, 100);
    ok(d == -1, "arriving at the top turns it round");
    nudge_note_landing(&d, ZBC_ACT_DIM_CYCLE, -1, 1);
    ok(d == 1, "and arriving at the bottom turns it back");

    d = 1;
    nudge_note_landing(&d, ZBC_ACT_DIM_CYCLE, 1, 99);
    ok(d == 1, "short of the rail it carries on");

    /* A plain dim_up/dim_down means what it says and must not be turned round. */
    d = 1;
    nudge_note_landing(&d, ZBC_ACT_DIM_UP, 1, 100);
    ok(d == 1, "a plain dim is left alone at the rail");
}

/* ------------------------------------------- several targets in one hold */

static zbc_btn_rule_t dim(int room, uint8_t action, int step, bool joined)
{
    return (zbc_btn_rule_t){ .group_id = (uint16_t)room, .action = action,
                             .action_arg = (uint8_t)step, .flags = joined ? ZBC_RULE_AND : 0 };
}

/* Each target starts from its own level: one sweep started from the other's
 * would snap it there on the first command. */
static void test_two_targets_sweep_from_their_own_levels(void)
{
    reset(0);
    s_pct[1] = 20;
    s_pct[2] = 60;
    const zbc_btn_rule_t c[] = { dim(1, ZBC_ACT_DIM_CYCLE, 10, false), dim(2, ZBC_ACT_DIM_CYCLE, 10, true) };
    run_hold(0, c, 2, true, 1000000);
    ok(cmds_to(1) == 1 && last_cmd(1) == 60, "the first aims an interval ahead of its own 20");
    ok(cmds_to(2) == 1 && last_cmd(2) == 100, "the second of its own 60");
}

/* Each at its own step size: a rule's number is its own speed. */
static void test_each_target_keeps_its_own_speed(void)
{
    reset(0);
    s_pct[1] = 20;
    s_pct[2] = 30;
    const zbc_btn_rule_t c[] = { dim(1, ZBC_ACT_DIM_UP, 10, false), dim(2, ZBC_ACT_DIM_UP, 5, true) };
    const int64_t t0 = 1000000;
    run_hold(0, c, 2, true, t0);
    ok(last_cmd(1) == 60 && last_cmd(2) == 50, "40 %/s and 20 %/s, a second ahead");
    run_release(0, t0 + BTN_REPEAT_US + BTN_REPEAT_US / 2);
    ok(s_stop[1] == 80, "released at 1.5 s the first is reckoned at 20 + 60");
    ok(s_stop[2] == 60, "and the second, separately, at 30 + 30");
}

/* The first reversing light at a rail turns them all, and the tick that turns
 * sends nothing to any of them. A command sent to the other before the turn was
 * noticed would travel on to where the model no longer says it is. */
static void test_a_reversing_rail_turns_every_reversing_light(void)
{
    reset(0);
    s_pct[1] = 20;
    s_pct[2] = 60;
    const zbc_btn_rule_t c[] = { dim(1, ZBC_ACT_DIM_CYCLE, 10, false), dim(2, ZBC_ACT_DIM_CYCLE, 10, true) };
    const int64_t t0 = 1000000;
    run_hold(0, c, 2, true, t0);                        /* 1 -> 60, 2 -> 100 */
    const int before = s_cmd_n;
    run_hold(0, c, 2, false, t0 + BTN_REPEAT_US);       /* 2 is at the top */
    ok(s_cmd_n == before, "the turning tick commands neither light");
    ok(s_live[0].dim_dir == -1, "and both are now going down");

    run_hold(0, c, 2, false, t0 + 2 * BTN_REPEAT_US);
    ok(last_cmd(1) == 20, "the first resumes from where it was, 60, down");
    ok(last_cmd(2) == 60, "the second from the top, down");
}

/* ...and a release in that pause lands each where it stands. */
static void test_releasing_in_a_shared_pause_lands_each_where_it_is(void)
{
    reset(0);
    s_pct[1] = 20;
    s_pct[2] = 60;
    const zbc_btn_rule_t c[] = { dim(1, ZBC_ACT_DIM_CYCLE, 10, false), dim(2, ZBC_ACT_DIM_CYCLE, 10, true) };
    const int64_t t0 = 1000000;
    run_hold(0, c, 2, true, t0);
    run_hold(0, c, 2, false, t0 + BTN_REPEAT_US);       /* turn */
    run_release(0, t0 + BTN_REPEAT_US + BTN_REPEAT_US / 2);
    ok(s_stop[1] == 60, "the first stops at the 60 it was sent to");
    ok(s_stop[2] == 100, "the second at the top");
}

/* A plain dim in a hold that turns rests with it for the interval, and the
 * release in that interval still reckons it where it is. */
static void test_a_plain_dim_rests_through_the_turn(void)
{
    reset(0);
    s_pct[1] = 70;
    s_pct[2] = 10;
    const zbc_btn_rule_t c[] = { dim(1, ZBC_ACT_DIM_CYCLE, 10, false), dim(2, ZBC_ACT_DIM_UP, 10, true) };
    const int64_t t0 = 1000000;
    run_hold(0, c, 2, true, t0);                        /* 1 -> 100, 2 -> 50 */
    run_hold(0, c, 2, false, t0 + BTN_REPEAT_US);       /* 1 at the top: turn */
    run_release(0, t0 + BTN_REPEAT_US + BTN_REPEAT_US / 2);
    ok(s_stop[2] == 50, "the plain dim is where its last command took it");

    reset(0);
    s_pct[1] = 70;
    s_pct[2] = 10;
    run_hold(0, c, 2, true, t0);
    run_hold(0, c, 2, false, t0 + BTN_REPEAT_US);
    run_hold(0, c, 2, false, t0 + 2 * BTN_REPEAT_US);
    ok(last_cmd(1) == 60, "the reversing one comes down from the top");
    ok(last_cmd(2) == 90, "the plain one carries on up from 50");
}

/* A plain dim that reaches its rail stops; the other goes on. */
static void test_one_plain_dim_at_its_rail_leaves_the_other(void)
{
    reset(0);
    s_pct[1] = 90;
    s_pct[2] = 20;
    const zbc_btn_rule_t c[] = { dim(1, ZBC_ACT_DIM_UP, 10, false), dim(2, ZBC_ACT_DIM_UP, 10, true) };
    const int64_t t0 = 1000000;
    run_hold(0, c, 2, true, t0);
    run_hold(0, c, 2, false, t0 + BTN_REPEAT_US);
    ok(cmds_to(1) == 1, "the one at the top is left there");
    ok(cmds_to(2) == 2 && last_cmd(2) == 100, "the other climbs on");
}

/* Up and down in one case: a crossfade. */
static void test_up_and_down_in_one_case(void)
{
    reset(0);
    s_pct[1] = 20;
    s_pct[2] = 80;
    const zbc_btn_rule_t c[] = { dim(1, ZBC_ACT_DIM_UP, 10, false), dim(2, ZBC_ACT_DIM_DOWN, 10, true) };
    run_hold(0, c, 2, true, 1000000);
    ok(last_cmd(1) == 60 && last_cmd(2) == 40, "one goes up while the other goes down");
}

/* A dark target asked to go darker drops out; the rest of the hold carries on,
 * and the release stops only what moved. */
static void test_a_dark_target_drops_out_of_the_hold(void)
{
    reset(0);
    s_on[1] = false;
    s_known_off[1] = true;
    s_pct[2] = 60;
    const zbc_btn_rule_t c[] = { dim(1, ZBC_ACT_DIM_DOWN, 10, false), dim(2, ZBC_ACT_DIM_DOWN, 10, true) };
    const int64_t t0 = 1000000;
    run_hold(0, c, 2, true, t0);
    ok(cmds_to(1) == 0, "the dark one is not dimmed");
    ok(last_cmd(2) == 20, "the lit one is");
    run_release(0, t0 + BTN_REPEAT_US / 2);
    ok(s_stop[1] == -1, "the release leaves the dark one alone");
    ok(s_stop[2] == 40, "and stops the other half a second down");
}

/* Only the dimming rules of a case sweep: an on, a toggle or an unassigned rule
 * beside them is not a sweep, and a target that no longer exists is dropped. */
static void test_only_assigned_dimming_rules_sweep(void)
{
    reset(50);
    zbc_btn_rule_t c[] = { { .group_id = 1, .action = ZBC_ACT_TOGGLE },
                           dim(2, ZBC_ACT_DIM_UP, 10, true),
                           dim(0, ZBC_ACT_DIM_UP, 10, true),       /* its room was deleted */
                           dim(9, ZBC_ACT_DIM_UP, 10, true) };     /* not a room lighting.c knows */
    run_hold(0, c, 4, true, 1000000);
    ok(s_cmd_n == 1 && s_cmd[0].room == 2, "only the one assigned dim is swept");
    run_release(0, 1500000);
    ok(s_stop[1] == -1 && s_stop[2] > 0, "and only it is stopped");
}

/* More dimming rules than there are sweeps: the rest are ignored rather than
 * written past the end. button_db refuses such a button; this is the backstop. */
static void test_no_more_sweeps_than_there_is_room_for(void)
{
    reset(50);
    zbc_btn_rule_t c[ZBC_BTN_MAX_SWEEPS + 1];
    for (int i = 0; i <= ZBC_BTN_MAX_SWEEPS; i++) {
        c[i] = dim(i < ROOMS - 1 ? i + 1 : 1, ZBC_ACT_DIM_UP, 10, i > 0);
    }
    run_hold(0, c, ZBC_BTN_MAX_SWEEPS + 1, true, 1000000);
    ok(s_cmd_n == ZBC_BTN_MAX_SWEEPS, "one command per sweep there is room for");
}

/* After the first tick the rules are not read again: the sweeps carry their
 * targets, so the lamps still move - and are still stopped - if the button is
 * edited during the hold. */
static void test_the_sweeps_outlive_an_edit(void)
{
    reset(0);
    s_pct[1] = 20;
    s_pct[2] = 30;
    const zbc_btn_rule_t c[] = { dim(1, ZBC_ACT_DIM_UP, 10, false), dim(2, ZBC_ACT_DIM_UP, 10, true) };
    const int64_t t0 = 1000000;
    run_hold(0, c, 2, true, t0);
    run_hold(0, NULL, 0, false, t0 + BTN_REPEAT_US);
    ok(cmds_to(1) == 2 && cmds_to(2) == 2, "the repeat still moves both");
    run_release(0, t0 + BTN_REPEAT_US);
    ok(s_stop[1] == 60 && s_stop[2] == 70, "and the release still stops both");
}

int main(void)
{
    test_a_plain_dim_nudge_keeps_its_direction();
    test_a_cycling_nudge_reverses_on_each_new_stream();
    test_a_cycling_nudge_turns_round_at_a_rail();
    test_the_sweep_runs_at_the_editor_s_rate();
    test_the_rail_tick_sends_nothing();
    test_the_sweep_resumes_from_the_rail();
    test_releasing_during_the_pause_does_not_lurch();
    test_the_release_tracks_the_sweep_after_the_pause();
    test_the_rail_does_not_latch();
    test_a_plain_dim_stops_at_the_rail();
    test_a_new_hold_reverses();
    test_a_dark_room_sweeps_up_from_the_bottom();
    test_a_dark_room_asked_to_dim_down_does_nothing();
    test_a_wheel_aims_only_its_own_interval_ahead();
    test_unknown_members_still_dim_down_from_the_last_level();
    test_a_known_dark_room_ignores_a_stale_level();
    test_a_release_without_a_sweep_is_silent();
    test_the_reckoning_stays_in_range();
    test_two_targets_sweep_from_their_own_levels();
    test_each_target_keeps_its_own_speed();
    test_a_reversing_rail_turns_every_reversing_light();
    test_releasing_in_a_shared_pause_lands_each_where_it_is();
    test_a_plain_dim_rests_through_the_turn();
    test_one_plain_dim_at_its_rail_leaves_the_other();
    test_up_and_down_in_one_case();
    test_a_dark_target_drops_out_of_the_hold();
    test_only_assigned_dimming_rules_sweep();
    test_no_more_sweeps_than_there_is_room_for();
    test_the_sweeps_outlive_an_edit();
    printf("all %d checks passed\n", s_checks);
    return 0;
}
