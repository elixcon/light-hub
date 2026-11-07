/* Host test for the Matter bridge's arithmetic in matter_bridge.cpp.
 *
 * Two things are tested here, both of which fail on hardware in a way nothing
 * in a log explains:
 *
 * - The level <-> percent round trip. Matter counts 1-254, a group counts
 *   percent. If the bridge writes back a level that maps to the percentage it
 *   already shows, a controller's slider moves by one every time it is used.
 *   Both directions have to be stable: a level set from Matter must not be
 *   rewritten, and a percentage set from the web UI must be written once.
 *
 * - The Move sweep. A Matter Move is sent to the lamps as a series of
 *   move-to-levels, each aimed one interval ahead of where the sweep is, so that
 *   a lost command is repaired by the next one. It must run at the rate asked for,
 *   end at a rail by itself, and reckon a Stop where the sweep actually was.
 *
 * - Alexa. It sends every brightness with a transition of 0, which must get the
 *   fade setting, not a snap. And its "set to 100 %" on a dark light is On and
 *   then a plain MoveToLevel, which must not be taken for one on a dark light.
 *   Nor may the On be carried out by itself first: it would light the group at
 *   its last brightness on the way, and flash a lamp whose curve is dark at the
 *   one asked for. An On waits for a brightness to replace it.
 *
 * - A device shown on its own. A lamp is the Matter light its clusters make it, a
 *   remote's button a Generic Switch whose events say what the hub's gesture was -
 *   a controller's automation runs on them, and a wrong count runs the wrong one -
 *   and every endpoint has a UniqueID that fits the attribute.
 *
 * levels.inc and matter.inc are extracted from main/light_math.h and
 * main/matter_bridge.cpp by the Makefile, never copied.
 *
 *   make -C test
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/btn_gesture.h"
#include "levels.inc"
#include "matter.inc"

static int s_checks;
static void ok(bool cond, const char *what)
{
    s_checks++;
    if (!cond) { printf("FAIL: %s\n", what); exit(1); }
}

/* A level a controller set comes back from the group as zbc_level_to_pct() of
 * it. The bridge must see that as already shown, or it rewrites the attribute to
 * a neighbouring level and the controller's slider jumps. */
static void test_a_level_from_matter_is_never_written_back(void)
{
    for (int level = 1; level <= 254; level++) {
        ok(mb_level_shows(level, zbc_level_to_pct(level)),
           "a Matter level is shown by the percentage it maps to");
    }
}

/* A percentage from the web UI is written once as zbc_pct_to_level(), and the
 * next pass must then leave it alone - otherwise it is rewritten every second. */
static void test_a_percentage_is_written_once(void)
{
    for (int pct = 1; pct <= 100; pct++) {
        ok(mb_level_shows(zbc_pct_to_level(pct), pct),
           "the level written for a percentage shows that percentage");
        ok(!mb_level_shows(zbc_pct_to_level(pct), pct == 100 ? 99 : pct + 1),
           "a different percentage is not mistaken for this one");
    }
}

static void test_a_null_level_shows_nothing(void)
{
    ok(!mb_level_shows(0, 1), "level 0 is not a level");
    ok(!mb_level_shows(255, 100), "255 is Matter's null, not full");
}

static void test_a_step_is_never_zero(void)
{
    ok(mb_step_pct(1) == 1, "the smallest step still moves one percent");
    ok(mb_step_pct(254) == 100, "a full-range step is 100 percent");
    ok(mb_step_pct(25) == 10, "25 level units is 10 percent");
}

static mb_move_t move(int from, int dir, int rate, int64_t t0)
{
    mb_move_t m = { true, (int8_t)dir, (uint8_t)from, (uint8_t)rate, t0, t0 };
    return m;
}

/* Driven every 100 ms, as bridge_task does, for `span_us`. */
static int run(mb_move_t *m, int64_t t0, int64_t span_us, int *targets, uint16_t *ds)
{
    int n = 0;
    for (int64_t t = t0; t <= t0 + span_us; t += 100 * 1000) {
        uint16_t d = 0;
        int target = mb_move_tick(m, t, &d);
        if (target) {
            targets[n] = target;
            ds[n]      = d;
            n++;
        }
    }
    return n;
}

static void test_a_move_sends_once_a_second_aimed_one_interval_ahead(void)
{
    const int64_t t0 = 5000000;
    mb_move_t m = move(20, +1, 50, t0);
    int targets[64];
    uint16_t ds[64];
    int n = run(&m, t0, 2500 * 1000, targets, ds);
    ok(n == 3, "three commands in two and a half seconds");
    ok(targets[0] == 70 && targets[1] == 120 && targets[2] == 170,
       "each aims one second of travel beyond where the sweep is");
    ok(ds[0] == 10 && ds[1] == 10 && ds[2] == 10,
       "each takes exactly the interval to get there");
}

static void test_a_move_ends_at_the_rail_by_itself(void)
{
    const int64_t t0 = 0;
    mb_move_t m = move(200, +1, 80, t0);
    int targets[64];
    uint16_t ds[64];
    int n = run(&m, t0, 10 * 1000 * 1000, targets, ds);
    ok(n == 1, "one command reaches the rail from 200 at 80 a second");
    ok(targets[0] == 254, "and it aims at the rail");
    ok(ds[0] == 7, "in the time the remaining 54 units take, not a whole interval");
    ok(!m.active, "after which the sweep is over, with no Stop needed");

    m = move(40, -1, 80, t0);
    n = run(&m, t0, 10 * 1000 * 1000, targets, ds);
    ok(n == 1 && targets[0] == 1, "down to the bottom rail the same way");
    ok(!m.active, "and over");
}

static void test_a_stop_reckons_where_the_sweep_was(void)
{
    const int64_t t0 = 1000000;
    mb_move_t m = move(100, +1, 80, t0);
    ok(mb_move_stop(&m, t0 + 1500 * 1000) == 220, "1.5 s at 80 a second from 100 is 220");
    ok(!m.active, "a stop ends the sweep");

    m = move(100, -1, 80, t0);
    ok(mb_move_stop(&m, t0 + 10 * 1000 * 1000) == 1, "a stop past the rail is at the rail");
}

static void test_a_zero_rate_sends_nothing(void)
{
    mb_move_t m = move(100, +1, 0, 0);
    uint16_t d = 0;
    ok(mb_move_tick(&m, 0, &d) == 0, "a rate of 0 has no effect");
}

/* Adaptive Lighting from Matter, on either exposure. A Mode Select mode that
 * decoded the wrong way round would start the mode when a controller asked for
 * Normal; a toggle on the second endpoint must follow the mode, not the tile. */
static void test_adaptive_lighting_from_matter(void)
{
    ok(mb_al_intent_for_mode(MB_MODE_ADAPTIVE) == 1, "the Adaptive mode starts Adaptive Lighting");
    ok(mb_al_intent_for_mode(MB_MODE_NORMAL) == 0, "the Normal mode stops it");
    ok(mb_al_intent_for_mode(2) == -1 && mb_al_intent_for_mode(255) == -1, "an unknown mode does nothing");
    ok(mb_al_toggle_starts(false), "a toggle starts a mode that is not running");
    ok(!mb_al_toggle_starts(true), "and stops one that is");
}

/* The variants without OnOff leave a dark group dark. Lit is the controller's
 * view or the lamps' - Alexa's "set to 100 %" on a dark light is On and then a
 * plain MoveToLevel, which arrives before any lamp has said it is lit. */
static void test_a_dark_group_stays_dark(void)
{
    ok(mb_stays_dark(true, false, false), "a plain MoveToLevel leaves a dark group dark");
    ok(!mb_stays_dark(true, true, false), "not once the controller has switched the light on");
    ok(!mb_stays_dark(true, false, true), "nor with a lamp lit that Matter has not heard of yet");
    ok(!mb_stays_dark(false, false, false), "the variants with OnOff light it");
}

static mb_intent_t intent(uint8_t kind, bool if_on)
{
    mb_intent_t in = {0};
    in.kind        = kind;
    in.if_on       = if_on;
    in.endpoint_id = 1;
    in.level       = 76;
    return in;
}

/* Alexa's "set to 30 %" on a dark light is On and then a plain MoveToLevel a few
 * hundred ms later. Carried out as two, the On lit every lamp at the last
 * brightness first - and a lamp whose curve is dark at 30 % flashed. */
static void test_an_on_followed_by_a_brightness_is_one_request(void)
{
    mb_on_hold_t h = {0};
    mb_intent_t  out[2];
    mb_intent_t  due;
    const int64_t t0 = 5000000;

    ok(mb_on_hold_take(&h, &(mb_intent_t){.kind = MB_ON, .endpoint_id = 1}, t0, out) == 0,
       "an On is not carried out at once");
    ok(!mb_on_hold_due(&h, t0 + 400 * 1000, &due), "nor 400 ms later, the slowest gap measured");

    mb_intent_t level = intent(MB_LEVEL, true);
    ok(mb_on_hold_take(&h, &level, t0 + 270 * 1000, out) == 1, "the brightness replaces it");
    ok(out[0].kind == MB_LEVEL && out[0].level == 76, "and is carried out as it came");
    ok(!out[0].if_on, "lighting the group, as the On it replaces said to");
    /* A relay in the room has no brightness to follow: without the On's
     * switch-on it would stay dark while the lamps came up. */
    ok(out[0].switch_on, "and switching on what can only switch, as the On would have");
    ok(!mb_on_hold_due(&h, t0 + 10 * 1000 * 1000, &due), "the On is never carried out after all");

    h = (mb_on_hold_t){0};
    mb_intent_t alone = intent(MB_LEVEL, true);
    ok(mb_on_hold_take(&h, &alone, t0, out) == 1 && !out[0].switch_on,
       "a brightness that replaced nothing is not a switch-on");

    h = (mb_on_hold_t){0};
    mb_on_hold_take(&h, &(mb_intent_t){.kind = MB_ON}, t0, out);
    mb_intent_t off = intent(MB_OFF, false);
    ok(mb_on_hold_take(&h, &off, t0 + 100 * 1000, out) == 1 && out[0].kind == MB_OFF,
       "an Off replaces it as well");
    ok(!out[0].switch_on, "and switches nothing on");
    ok(!mb_on_hold_due(&h, t0 + 10 * 1000 * 1000, &due), "leaving nothing to light the group");
}

/* "Turn on" is an On alone, which must still arrive - late by the wait, no more. */
static void test_an_on_alone_is_carried_out_once_its_wait_is_over(void)
{
    mb_on_hold_t h = {0};
    mb_intent_t  out[2];
    mb_intent_t  due;
    const int64_t t0 = 5000000;

    mb_on_hold_take(&h, &(mb_intent_t){.kind = MB_ON, .transition_ds = 7}, t0, out);
    ok(!mb_on_hold_due(&h, t0 + MB_ON_HOLD_US - 1, &due), "not before its wait is over");
    ok(mb_on_hold_due(&h, t0 + MB_ON_HOLD_US, &due), "then carried out");
    ok(due.kind == MB_ON && due.transition_ds == 7, "as it came");
    ok(!mb_on_hold_due(&h, t0 + 2 * MB_ON_HOLD_US, &due), "once");

    mb_intent_t level = intent(MB_LEVEL, true);
    ok(mb_on_hold_take(&h, &level, t0 + 2 * MB_ON_HOLD_US, out) == 1 && out[0].if_on,
       "a brightness after that is on its own again, and a plain one is judged as one");

    /* Alexa sends "turn on" twice at times; the second is the same request. */
    h = (mb_on_hold_t){0};
    mb_on_hold_take(&h, &(mb_intent_t){.kind = MB_ON}, t0, out);
    ok(mb_on_hold_take(&h, &(mb_intent_t){.kind = MB_ON}, t0 + 300 * 1000, out) == 0,
       "a second On is held with the first");
    ok(mb_on_hold_due(&h, t0 + MB_ON_HOLD_US, &due), "which keeps its time");
    ok(!mb_on_hold_due(&h, t0 + 300 * 1000 + MB_ON_HOLD_US, &due), "and is carried out once");
}

/* Siri's "set to 30 %" is the other way round from Alexa's: MoveToLevelWithOnOff
 * and then On, about 250 ms later. Carried out as two, the On was the room's
 * turn-on: 30 %, and then Adaptive Lighting or full. */
static void test_an_on_after_a_brightness_is_part_of_it(void)
{
    mb_on_hold_t h = {0};
    mb_intent_t  out[2];
    mb_intent_t  due;
    const int64_t t0 = 5000000;

    mb_intent_t with_onoff = intent(MB_LEVEL, false);
    with_onoff.switch_on   = true;
    with_onoff.at_us       = t0;
    ok(mb_on_hold_take(&h, &with_onoff, t0, out) == 1 && out[0].kind == MB_LEVEL,
       "the brightness is carried out at once");

    mb_intent_t on = {.kind = MB_ON, .endpoint_id = 1, .at_us = t0 + 248 * 1000};
    ok(mb_on_hold_take(&h, &on, on.at_us, out) == 0, "the On after it adds nothing");
    ok(!h.held, "and does not wait to be carried out");
    ok(!mb_on_hold_due(&h, t0 + 10 * 1000 * 1000, &due), "not then, and not later");

    on.at_us = t0 + 500 * 1000;
    ok(mb_on_hold_take(&h, &on, on.at_us, out) == 0 && !h.held, "a second On is the same request");

    /* The room's turn-on is what must not run, however long the brightness kept
     * the On waiting in the queue: a room of lamps takes a while to send to. */
    h = (mb_on_hold_t){0};
    mb_on_hold_take(&h, &with_onoff, t0, out);
    on.at_us = t0 + 248 * 1000;
    ok(mb_on_hold_take(&h, &on, t0 + 5 * 1000 * 1000, out) == 0 && !h.held,
       "the gap is between their arrivals, not when each got its turn");

    /* A plain MoveToLevel lights nothing, and a dark group ignored it. With the
     * On after it the two ask for what WithOnOff does. */
    h = (mb_on_hold_t){0};
    mb_intent_t plain   = intent(MB_LEVEL, true);
    plain.at_us         = t0;
    plain.transition_ds = 7;
    mb_on_hold_take(&h, &plain, t0, out);
    on.at_us = t0 + 248 * 1000;
    ok(mb_on_hold_take(&h, &on, on.at_us, out) == 1, "after a plain one the On lights the group");
    ok(out[0].kind == MB_LEVEL && out[0].level == 76 && out[0].transition_ds == 7, "at that brightness");
    ok(!out[0].if_on, "dark or not");
    ok(out[0].switch_on, "and what can only switch as well");
    ok(!h.held, "with nothing left to carry out");
    on.at_us = t0 + 300 * 1000;
    ok(mb_on_hold_take(&h, &on, on.at_us, out) == 0, "and only the once");

    /* Alexa's pair, with its habit of sending On twice. */
    h = (mb_on_hold_t){0};
    on.at_us = t0;
    mb_on_hold_take(&h, &on, t0, out);
    plain.at_us = t0 + 270 * 1000;
    ok(mb_on_hold_take(&h, &plain, plain.at_us, out) == 1 && out[0].switch_on, "an On then a brightness");
    on.at_us = t0 + 300 * 1000;
    ok(mb_on_hold_take(&h, &on, on.at_us, out) == 0 && !h.held, "and an On after those");
}

/* "Turn on" said after a brightness is a request of its own. */
static void test_an_on_later_than_a_brightness_is_a_turn_on(void)
{
    mb_on_hold_t h = {0};
    mb_intent_t  out[2];
    mb_intent_t  due;
    const int64_t t0 = 5000000;

    mb_intent_t level = intent(MB_LEVEL, false);
    level.switch_on   = true;
    level.at_us       = t0;
    mb_on_hold_take(&h, &level, t0, out);
    mb_intent_t on = {.kind = MB_ON, .endpoint_id = 1, .at_us = t0 + MB_ON_AFTER_LEVEL_US};
    ok(mb_on_hold_take(&h, &on, on.at_us, out) == 0 && h.held, "once the window is over the On is held");
    ok(mb_on_hold_due(&h, on.at_us + MB_ON_HOLD_US, &due) && due.kind == MB_ON, "and carried out");

    const uint8_t kinds[] = {MB_OFF, MB_TOGGLE, MB_STEP, MB_MOVE, MB_STOP, MB_AL_START, MB_AL_STOP, MB_AL_TOGGLE};
    for (size_t i = 0; i < sizeof(kinds); i++) {
        h = (mb_on_hold_t){0};
        mb_on_hold_take(&h, &level, t0, out);
        mb_intent_t next = intent(kinds[i], false);
        next.at_us       = t0 + 100 * 1000;
        mb_on_hold_take(&h, &next, next.at_us, out);
        on.at_us = t0 + 200 * 1000;
        ok(mb_on_hold_take(&h, &on, on.at_us, out) == 0 && h.held,
           "an On after another command belongs to no brightness");
        ok(mb_on_hold_due(&h, on.at_us + MB_ON_HOLD_US, &due) && due.kind == MB_ON, "and is carried out");
    }
}

/* A toggle, a step, a sweep start from what the On leaves; the On goes first. */
static void test_anything_else_follows_the_on(void)
{
    const uint8_t kinds[] = {MB_TOGGLE, MB_STEP, MB_MOVE, MB_STOP, MB_AL_START, MB_AL_STOP, MB_AL_TOGGLE};
    for (size_t i = 0; i < sizeof(kinds); i++) {
        mb_on_hold_t h = {0};
        mb_intent_t  out[2];
        mb_intent_t  due;
        mb_on_hold_take(&h, &(mb_intent_t){.kind = MB_ON}, 0, out);
        mb_intent_t next = intent(kinds[i], true);
        ok(mb_on_hold_take(&h, &next, 100 * 1000, out) == 2, "the On and the command, both");
        ok(out[0].kind == MB_ON && out[1].kind == kinds[i], "the On first");
        ok(out[1].if_on, "the command as it came");
        ok(!mb_on_hold_due(&h, MB_ON_HOLD_US, &due), "and the On only the once");
    }

    mb_on_hold_t h = {0};
    mb_intent_t  out[2];
    mb_intent_t  level = intent(MB_LEVEL, true);
    ok(mb_on_hold_take(&h, &level, 0, out) == 1 && out[0].if_on,
       "with no On held a command passes straight through");
}

static void test_a_transition_of_0_gets_the_fade_setting(void)
{
    ok(mb_transition_ds(true, 0, 7) == 7, "Alexa's 0 gets the fade setting");
    ok(mb_transition_ds(false, 0, 7) == 7, "a transition left out gets the fade setting");
    ok(mb_transition_ds(false, 25, 7) == 7, "whatever the field held");
    ok(mb_transition_ds(true, 25, 7) == 25, "a transition the controller named is kept");
    ok(mb_transition_ds(true, 1, 7) == 1, "however short");
    ok(mb_transition_ds(true, 0, 0) == 0, "with the fade set to Instant, 0 is a snap");
}

/* "Alexa, turn on the basement": what it does is the room's setting - back as
 * it was, Adaptive Lighting, or full - whether the room is dark or dimmed. The
 * stored values are the enum's, so 0 has to stay "as it was": every record
 * written before the setting existed holds 0. */
static void test_turn_on_follows_the_room_setting(void)
{
    ok(MB_ON_AS_WAS == 0, "a record from before the setting reads as the old On");
    ok(mb_turn_on(MB_ON_AS_WAS, true) == MB_ON_AS_WAS, "as it was: the last brightness");
    ok(mb_turn_on(MB_ON_ADAPTIVE, true) == MB_ON_ADAPTIVE, "adaptive: starts the mode");
    ok(mb_turn_on(MB_ON_FULL, true) == MB_ON_FULL, "full: full brightness");
    ok(mb_turn_on(MB_ON_FULL, false) == MB_ON_FULL, "full does not need the room to offer Adaptive Lighting");

    /* Adaptive Lighting where the room does not offer it - switched off after
     * the choice was made - is the plain On, never nothing. */
    ok(mb_turn_on(MB_ON_ADAPTIVE, false) == MB_ON_AS_WAS,
       "adaptive in a room that does not offer it switches on as it was");

    /* A value this build does not know, from a newer one, is the old On. */
    ok(mb_turn_on(7, true) == MB_ON_AS_WAS, "an unknown setting is the old On");
}

/* A lamp is shown as the most it can do. Colour wins over colour temperature,
 * which wins over dimming: Matter's lights nest that way. */
static void test_a_lamp_is_the_light_its_clusters_make_it(void)
{
    ok(mb_lamp_type(false, false, false) == MB_LAMP_ONOFF, "a relay is an On/Off Light");
    ok(mb_lamp_type(true, false, false) == MB_LAMP_DIMMABLE, "a dimmer is a Dimmable Light");
    ok(mb_lamp_type(true, true, false) == MB_LAMP_CT, "a tunable white is a Color Temperature Light");
    ok(mb_lamp_type(true, true, true) == MB_LAMP_COLOR, "a colour lamp is an Extended Color Light");
    ok(mb_lamp_type(true, false, true) == MB_LAMP_COLOR, "colour without colour temperature is still colour");
    ok(mb_lamp_type(false, true, false) == MB_LAMP_CT, "colour temperature wins over a missing level");

    /* The shape decides a rebuild: hue and saturation count only on a colour
     * light, where they are a feature of their own. */
    ok(mb_lamp_shape(true, true, true, true) != mb_lamp_shape(true, true, true, false),
       "a colour lamp gaining hue and saturation is rebuilt");
    ok(mb_lamp_shape(true, true, false, true) == mb_lamp_shape(true, true, false, false),
       "hue and saturation mean nothing to a tunable white");
    ok(mb_shape_type(mb_lamp_shape(true, true, true, true)) == MB_LAMP_COLOR, "the shape keeps the type");
    ok(mb_shape_type(mb_lamp_shape(true, false, false, false)) == MB_LAMP_DIMMABLE, "a dimmer's too");
    ok(mb_lamp_shape(false, false, false, false) != mb_lamp_shape(true, false, false, false),
       "a relay that turns out to dim is rebuilt");
}

static int count_of(const mb_sw_step_t *s, int n, uint8_t event)
{
    int c = 0;
    for (int i = 0; i < n; i++) {
        c += s[i].event == event;
    }
    return c;
}

/* Every press starts with InitialPress, and the switch is back at rest once the
 * gesture is over - or held, after a LongPress, until the LongRelease. */
static void check_positions(const mb_sw_step_t *s, int n, uint8_t at_end, const char *what)
{
    ok(n > 0 && (s[0].event == MB_SW_INITIAL_PRESS || s[0].event == MB_SW_LONG_RELEASE), what);
    ok(mb_sw_position(s[n - 1].event) == at_end, what);
}

static void test_a_button_reports_what_the_hub_decided(void)
{
    mb_sw_step_t s[MB_SW_MAX_STEPS];

    int n = mb_switch_events(ZBC_EMIT_SHORT, s);
    ok(n == 3, "a short press is three events");
    ok(s[0].event == MB_SW_INITIAL_PRESS && s[1].event == MB_SW_SHORT_RELEASE && s[2].event == MB_SW_MULTI_COMPLETE,
       "pressed, released, then the count");
    ok(s[2].count == 1, "a single press counts one");
    check_positions(s, n, 0, "a short press ends at rest");

    n = mb_switch_events(ZBC_EMIT_DOUBLE, s);
    ok(n == 5, "a double press is five events");
    ok(count_of(s, n, MB_SW_SHORT_RELEASE) == 2, "both presses are released");
    ok(count_of(s, n, MB_SW_INITIAL_PRESS) == 1, "only the first press is an InitialPress");
    ok(s[2].event == MB_SW_MULTI_ONGOING && s[2].count == 2, "the second press says it is the second");
    ok(s[n - 1].event == MB_SW_MULTI_COMPLETE && s[n - 1].count == 2, "and the sequence ends at two");
    ok(s[n - 1].count <= MB_SW_MULTI_PRESS_MAX, "never more than the switch says it counts");
    check_positions(s, n, 0, "a double press ends at rest");

    n = mb_switch_events(ZBC_EMIT_LONG, s);
    ok(n == 2 && s[0].event == MB_SW_INITIAL_PRESS && s[1].event == MB_SW_LONG_PRESS, "a hold is pressed, then long");
    ok(count_of(s, n, MB_SW_MULTI_COMPLETE) == 0, "a hold is no multi-press");
    check_positions(s, n, 1, "a hold leaves the switch pressed");

    n = mb_switch_events(ZBC_EMIT_LONG_RELEASE, s);
    ok(n == 1 && s[0].event == MB_SW_LONG_RELEASE, "letting go of a hold is a LongRelease");
    ok(mb_sw_position(s[0].event) == 0, "and the switch is at rest again");

    ok(mb_switch_events(ZBC_EMIT_LONG_REPEAT, s) == 0, "a hold's repeats say nothing");
    ok(mb_switch_events(ZBC_EMIT_TRIPLE, s) == 0, "a triple is more than the switch counts");
    ok(mb_switch_events(ZBC_EMIT_MOTION, s) == 0, "a sensor's motion is no press");
    ok(mb_switch_events(ZBC_EMIT_CLEAR, s) == 0, "nor is its clear");
    ok(mb_switch_events(ZBC_EMIT_NONE, s) == 0, "nothing is nothing");
}

/* The UniqueID is at most 32 characters, whatever the address and endpoint; and
 * two endpoints of one device never share one. */
static void test_a_unique_id_fits_and_is_unique(void)
{
    const uint8_t ff[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    char a[33], b[33];
    ok(mb_device_uid(a, sizeof(a), ff, 240) <= 32, "the longest an endpoint can have fits");
    ok(strlen(a) == 28, "and is not cut short");

    const uint8_t ieee[8] = { 0x00, 0x17, 0x88, 0x01, 0x02, 0x6e, 0xf2, 0xa8 };
    mb_device_uid(a, sizeof(a), ieee, 11);
    ok(strcmp(a, "elixcon-00178801026ef2a8-11") == 0, "a lamp's names its address and endpoint");
    mb_device_uid(b, sizeof(b), ieee, 1);
    ok(strcmp(a, b) != 0, "two endpoints of one device differ");
}

int main(void)
{
    test_turn_on_follows_the_room_setting();
    test_a_transition_of_0_gets_the_fade_setting();
    test_a_dark_group_stays_dark();
    test_an_on_followed_by_a_brightness_is_one_request();
    test_an_on_alone_is_carried_out_once_its_wait_is_over();
    test_an_on_after_a_brightness_is_part_of_it();
    test_an_on_later_than_a_brightness_is_a_turn_on();
    test_anything_else_follows_the_on();
    test_adaptive_lighting_from_matter();
    test_a_level_from_matter_is_never_written_back();
    test_a_percentage_is_written_once();
    test_a_null_level_shows_nothing();
    test_a_step_is_never_zero();
    test_a_move_sends_once_a_second_aimed_one_interval_ahead();
    test_a_move_ends_at_the_rail_by_itself();
    test_a_stop_reckons_where_the_sweep_was();
    test_a_zero_rate_sends_nothing();
    test_a_lamp_is_the_light_its_clusters_make_it();
    test_a_button_reports_what_the_hub_decided();
    test_a_unique_id_fits_and_is_unique();
    printf("all %d checks passed\n", s_checks);
    return 0;
}
