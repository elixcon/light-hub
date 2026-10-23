/* Host test for the remote decoder in main/remote.c.
 *
 * It is a leaf - no ESP-IDF, no state - so it is compiled whole rather than
 * lifted out of a bigger file, and every row of every table can be driven here.
 * The tables come from two hardware revisions of one Aqara switch that do not
 * resemble each other at all, and from the standard commands a rocker sends; a
 * mistake in any of them reads on hardware as "that button does nothing", which
 * is indistinguishable from a flat battery.
 *
 *   make -C test
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "remote.h"

static int s_checks;

static void ok(bool cond, const char *what)
{
    s_checks++;
    if (!cond) { printf("FAIL: %s\n", what); exit(1); }
}

/* Which remote the frames below come from; each test says. */
static zb_remote_model_t s_model = ZB_REMOTE_MODEL_OTHER;

static void decoded_is(zb_remote_ev_t got, uint8_t key, zb_remote_ev_t want, uint8_t want_key,
                       const char *what)
{
    s_checks++;
    /* Only a real press names a key; NONE and IGNORED leave it alone. */
    const bool carries_key = want != ZB_REMOTE_NONE && want != ZB_REMOTE_IGNORED;
    if (got != want || (carries_key && key != want_key)) {
        printf("FAIL: %s (got %s key %d, wanted %s key %d)\n", what, remote_event_name(got), key,
               remote_event_name(want), want_key);
        exit(1);
    }
}

static void attr_is(uint16_t cl, uint16_t at, uint32_t v, zb_remote_ev_t want, uint8_t want_key,
                    const char *what)
{
    uint8_t key = 0xEE;
    const zb_remote_ev_t got = remote_decode_attr(s_model, cl, at, v, &key);
    decoded_is(got, key, want, want_key, what);
}

static void cmd_is(uint16_t cl, uint8_t cmd, const uint8_t *p, uint16_t len, zb_remote_ev_t want,
                   uint8_t want_key, const char *what)
{
    uint8_t key = 0xEE;
    const zb_remote_ev_t got = remote_decode_cmd(s_model, cl, cmd, p, len, &key);
    decoded_is(got, key, want, want_key, what);
}

/* The 2018 revision, lumi.remote.b1acn01: Multistate Input PresentValue is the
 * whole of its protocol, and it decodes its own gestures. */
static void test_the_multistate_switch(void)
{
    s_model = ZB_REMOTE_MODEL_AQARA;
    /* It counts, so a 1 is final and must not wait for a second press. */
    attr_is(0x0012, 0x0055, 1,   ZB_REMOTE_SINGLE,  0, "present value 1 is a single press");
    attr_is(0x0012, 0x0055, 2,   ZB_REMOTE_DOUBLE,  0, "present value 2 is a double");
    attr_is(0x0012, 0x0055, 3,   ZB_REMOTE_TRIPLE,  0, "present value 3 is a triple");
    attr_is(0x0012, 0x0055, 0,   ZB_REMOTE_HOLD,    0, "present value 0 is a hold");
    attr_is(0x0012, 0x0055, 255, ZB_REMOTE_RELEASE, 0, "present value 255 is the release");
    attr_is(0x0012, 0x0055, 4,   ZB_REMOTE_NONE,    0, "present value 4 is not a gesture we have");
    attr_is(0x0012, 0x006f, 1,   ZB_REMOTE_NONE,    0, "status flags are not a press");
    /* The attribute says what it is whoever sends it. */
    s_model = ZB_REMOTE_MODEL_OTHER;
    attr_is(0x0012, 0x0055, 3,   ZB_REMOTE_TRIPLE,  0, "whatever the model string");
}

/* The 2016 revision, lumi.sensor_switch.aq2: On/Off as an *input* cluster, so
 * it looks like a lamp, plus a manufacturer attribute for the click count. */
static void test_the_on_off_switch(void)
{
    s_model = ZB_REMOTE_MODEL_AQARA;
    attr_is(0x0006, 0x8000, 2,   ZB_REMOTE_DOUBLE, 0, "click count 2 is a double");
    attr_is(0x0006, 0x8000, 3,   ZB_REMOTE_TRIPLE, 0, "click count 3 is a triple");
    attr_is(0x0006, 0x8000, 4,   ZB_REMOTE_SINGLE, 0, "a quadruple still switches the light");
    attr_is(0x0006, 0x8000, 128, ZB_REMOTE_SINGLE, 0, "and so does five or more");
    attr_is(0x0006, 0x8000, 1,   ZB_REMOTE_NONE,   0, "a count of 1 is not reported this way");

    /* One press is two reports - down and up - so only one edge may count, or
     * every click would fire twice. */
    attr_is(0x0006, 0x0000, 0, ZB_REMOTE_SINGLE, 0, "the attribute going false is the press");
    /* Ignored, not unrecognised: it is half of a press we have counted, and a
       caller that logs what it did not understand must not report it. */
    attr_is(0x0006, 0x0000, 1, ZB_REMOTE_IGNORED, 0, "and it coming back true is not a second one");
    /* From a device not known to count its own presses, the same report is a
       click that a second one may still make a double. */
    s_model = ZB_REMOTE_MODEL_OTHER;
    attr_is(0x0006, 0x0000, 0, ZB_REMOTE_CLICK, 0, "an unknown switch's press may pair up");
}

/* A standard remote reports nothing and sends the commands it would send to a
 * lamp. On and Off are one key, not two: a Hue Smart Button is a single physical
 * button that sends On to turn on and OffWithEffect to turn off, so splitting
 * them by command would put one button on two keys and a rule bound to either
 * would fire on every other press. */
static void test_on_off_commands(void)
{
    s_model = ZB_REMOTE_MODEL_OTHER;
    cmd_is(0x0006, 0x00, NULL, 0, ZB_REMOTE_CLICK, 0, "Off is the on/off press");
    cmd_is(0x0006, 0x01, NULL, 0, ZB_REMOTE_CLICK, 0, "and so is On");
    cmd_is(0x0006, 0x02, NULL, 0, ZB_REMOTE_CLICK, 0, "and Toggle");
    /* A ROM001 says "off" with OffWithEffect, so it must decode as a press. */
    cmd_is(0x0006, 0x40, NULL, 0, ZB_REMOTE_CLICK, 0, "and OffWithEffect, which a Hue button sends");
    cmd_is(0x0006, 0x41, NULL, 0, ZB_REMOTE_NONE,  0, "OnWithRecallGlobalScene is not a press");
}

/* Move and Step are not the same kind of thing, and treating them alike is the
 * mistake this test exists to pin. Move is continuous and only ends at its Stop;
 * Step is one discrete nudge, and a remote that dims by repeating it is running
 * its own sweep. */
static void test_level_commands(void)
{
    s_model = ZB_REMOTE_MODEL_OTHER;
    const uint8_t up[]   = { 0x00, 50 };
    const uint8_t down[] = { 0x01, 50 };

    cmd_is(0x0008, 0x01, up,   2, ZB_REMOTE_HOLD, 2, "Move up holds the up half");
    cmd_is(0x0008, 0x01, down, 2, ZB_REMOTE_HOLD, 1, "Move down holds the down half");
    cmd_is(0x0008, 0x05, up,   2, ZB_REMOTE_HOLD, 2, "MoveWithOnOff is the same hold");

    /* A ROM001 held for six seconds sends seven Steps ~800 ms apart, the last one
       back up - it reverses at the rail by itself. So a Step is a click, and a
       dim_* rule on the short press nudges once per Step; read as a hold, a
       short press would fire the long-press rule. */
    cmd_is(0x0008, 0x02, down, 2, ZB_REMOTE_CLICK, 1, "Step down is one nudge, not a hold");
    cmd_is(0x0008, 0x02, up,   2, ZB_REMOTE_CLICK, 2, "Step up likewise");
    cmd_is(0x0008, 0x06, up,   2, ZB_REMOTE_CLICK, 2, "and StepWithOnOff");

    /* Stop carries no direction, so it ends whichever key is holding. */
    cmd_is(0x0008, 0x03, NULL, 0, ZB_REMOTE_RELEASE, ZB_REMOTE_KEY_ANY, "Stop releases whatever is held");
    cmd_is(0x0008, 0x07, NULL, 0, ZB_REMOTE_RELEASE, ZB_REMOTE_KEY_ANY, "and so does StopWithOnOff");

    /* A truncated Move has no direction to read, and guessing one would send the
     * lights the wrong way. */
    cmd_is(0x0008, 0x01, NULL, 0, ZB_REMOTE_NONE, 0, "a Move with no payload is not acted on");
    cmd_is(0x0008, 0x02, NULL, 0, ZB_REMOTE_NONE, 0, "nor a Step with none");
    cmd_is(0x0008, 0x00, up,   2, ZB_REMOTE_NONE, 0, "MoveToLevel is not a button press");
}

/* The whole of one short press of a Hue Smart Button, in the order it arrives:
 * three frames 23 ms and 395 ms apart. Decoded as a hold, a release and nothing,
 * the press would run the dim rule for a fifth of a second and never the off
 * rule. */
static void test_a_hue_smart_button_short_press(void)
{
    s_model = ZB_REMOTE_MODEL_OTHER;
    const uint8_t step_down[] = { 0x01, 0x1e, 0x08, 0x00 };
    const uint8_t effect[]    = { 0x00, 0x00 };

    cmd_is(0x0008, 0x02, step_down, 4, ZB_REMOTE_CLICK, 1, "the Step is a nudge");
    cmd_is(0x0008, 0x03, NULL,      0, ZB_REMOTE_RELEASE, ZB_REMOTE_KEY_ANY,
           "the Stop 23 ms later releases nothing, because nothing is held");
    cmd_is(0x0006, 0x40, effect,    2, ZB_REMOTE_CLICK, 0, "and the OffWithEffect is the press itself");
}

/* IKEA SYMFONISK sound controller (E1744), from the zigbee2mqtt converter and
 * the ZHA quirk. Its double and triple press are Level Steps - the command a Hue
 * button sends while held - so they are told apart by model, and belong to the
 * press key, not to the turn keys. */
static void test_the_symfonisk_sound_controller(void)
{
    const uint8_t turn_right[] = { 0x00, 0xc3 };               /* Move up, rate 195 */
    const uint8_t turn_left[]  = { 0x01, 0xc3 };
    const uint8_t double_[]    = { 0x00, 0x01, 0x00, 0x00 };   /* Step up by 1 */
    const uint8_t triple[]     = { 0x01, 0x01, 0x00, 0x00 };

    s_model = ZB_REMOTE_MODEL_E1744;
    cmd_is(0x0006, 0x02, NULL,       0, ZB_REMOTE_SINGLE,  0, "E1744: a press is Toggle, and final");
    cmd_is(0x0008, 0x01, turn_right, 2, ZB_REMOTE_HOLD,    2, "E1744: turning right holds up");
    cmd_is(0x0008, 0x01, turn_left,  2, ZB_REMOTE_HOLD,    1, "E1744: turning left holds down");
    cmd_is(0x0008, 0x03, NULL,       0, ZB_REMOTE_RELEASE, ZB_REMOTE_KEY_ANY,
           "E1744: stopping is Stop");
    cmd_is(0x0008, 0x02, double_,    4, ZB_REMOTE_DOUBLE,  0, "E1744: a double press is Step up");
    cmd_is(0x0008, 0x02, triple,     4, ZB_REMOTE_TRIPLE,  0, "E1744: a triple press is Step down");
    cmd_is(0x0008, 0x06, double_,    4, ZB_REMOTE_DOUBLE,  0, "E1744: StepWithOnOff likewise");
    /* The same frames from any other remote decode by command. */
    s_model = ZB_REMOTE_MODEL_OTHER;
    cmd_is(0x0008, 0x02, double_,    4, ZB_REMOTE_CLICK,   2, "another remote's Step up is still a nudge");
    cmd_is(0x0006, 0x02, NULL,       0, ZB_REMOTE_CLICK,   0, "and its Toggle may still pair up");
    /* Nor is the Step-as-double rule the E1744's alone to lose to another known
       remote: a BILRESA's Step, should it ever send one, is still a nudge. */
    s_model = ZB_REMOTE_MODEL_E2490;
    cmd_is(0x0008, 0x02, double_,    4, ZB_REMOTE_CLICK,   2, "a BILRESA's Step up is a nudge");
}

/* IKEA BILRESA scroll wheel (E2490) in Zigbee mode: On and Off alternating for a
 * click, and IKEA's command 0x07 on Scenes for a double (anything but 257) and a
 * triple (257) - Zigbee2MQTT's on_double and off_double, both presses of the
 * wheel and so of the press key. */
static void test_the_bilresa_presses(void)
{
    const uint8_t dbl[]    = { 0x00, 0x01 };   /* 256, little-endian */
    const uint8_t triple[] = { 0x01, 0x01 };   /* 257 */

    s_model = ZB_REMOTE_MODEL_E2490;
    cmd_is(0x0006, 0x01, NULL,   0, ZB_REMOTE_SINGLE, 0, "BILRESA: a click that says On");
    cmd_is(0x0006, 0x00, NULL,   0, ZB_REMOTE_SINGLE, 0, "BILRESA: and the next that says Off");
    cmd_is(0x0005, 0x07, dbl,    2, ZB_REMOTE_DOUBLE, 0, "BILRESA: a double click");
    cmd_is(0x0005, 0x07, triple, 2, ZB_REMOTE_TRIPLE, 0, "BILRESA: a triple click");
    cmd_is(0x0005, 0x07, dbl,    1, ZB_REMOTE_NONE,   0, "a truncated one is not guessed at");
    /* An IKEA rocker's arrows send the same command, and are still two sides. */
    s_model = ZB_REMOTE_MODEL_OTHER;
    cmd_is(0x0005, 0x07, dbl,    2, ZB_REMOTE_CLICK,  2, "a rocker's right arrow is key 2");
    cmd_is(0x0005, 0x07, triple, 2, ZB_REMOTE_CLICK,  1, "and its left arrow key 1");

    uint8_t lvl = 0;
    const uint8_t malformed[] = { 120 };          /* no transition, no option fields */
    const uint8_t top[]       = { 255, 0x0a, 0x00 };
    ok(remote_wheel_frame(0x0008, 0x00, malformed, 1, &lvl) && lvl == 120,
       "a MoveToLevel with only its level byte is a wheel frame");
    ok(remote_wheel_frame(0x0008, 0x04, top, 3, &lvl) && lvl == 254,
       "and 255, which lamps do not have, reads as 254");
    ok(!remote_wheel_frame(0x0008, 0x01, malformed, 1, &lvl), "a Move is not a wheel frame");
    ok(!remote_wheel_frame(0x0008, 0x00, NULL, 0, &lvl), "nor an empty MoveToLevel");
}

/* Philips Hue dimmer switch gen 1 (RWL021). Its endpoint 1 sends what
 * Zigbee2MQTT's converter lists as ignored in favour of Philips' own cluster:
 * On from the On button, OffWithEffect from the Off button, Steps from up and
 * down (repeated while held, like a Hue Smart Button) and a Stop. Four buttons,
 * so On and Off are two keys here and nowhere else. */
static void test_the_hue_dimmer(void)
{
    const uint8_t step_up[]   = { 0x00, 0x1e, 0x09, 0x00 };
    const uint8_t step_down[] = { 0x01, 0x1e, 0x09, 0x00 };
    const uint8_t effect[]    = { 0x00, 0x00 };

    s_model = ZB_REMOTE_MODEL_HUE_DIMMER;
    cmd_is(0x0006, 0x01, NULL,      0, ZB_REMOTE_CLICK, 0, "RWL021: the On button is key 0");
    cmd_is(0x0006, 0x40, effect,    2, ZB_REMOTE_CLICK, 3, "RWL021: the Off button is key 3");
    cmd_is(0x0006, 0x00, NULL,      0, ZB_REMOTE_CLICK, 3, "RWL021: and a plain Off would be too");
    cmd_is(0x0008, 0x02, step_up,   4, ZB_REMOTE_CLICK, 2, "RWL021: up is a Step up");
    cmd_is(0x0008, 0x02, step_down, 4, ZB_REMOTE_CLICK, 1, "RWL021: down is a Step down");
    cmd_is(0x0008, 0x03, NULL,      0, ZB_REMOTE_RELEASE, ZB_REMOTE_KEY_ANY, "RWL021: then a Stop");
    /* It counts nothing itself, so its presses may still pair into doubles. */

    /* Its Philips cluster: button, three unknown bytes, type (Zigbee2MQTT's
       hueNotification). The press edge waits; the short release is the click. */
    const uint8_t on_press[]      = { 1, 0x00, 0x00, 0x30, 0, 0x00, 0x00, 0x00 };
    const uint8_t on_short[]      = { 1, 0x00, 0x00, 0x30, 2, 0x00, 0x01, 0x00 };
    const uint8_t on_hold[]       = { 1, 0x00, 0x00, 0x30, 1, 0x00, 0x08, 0x00 };
    const uint8_t on_long[]       = { 1, 0x00, 0x00, 0x30, 3, 0x00, 0x10, 0x00 };
    const uint8_t up_short[]      = { 2, 0x00, 0x00, 0x30, 2, 0x00, 0x01, 0x00 };
    const uint8_t down_hold[]     = { 3, 0x00, 0x00, 0x30, 1, 0x00, 0x08, 0x00 };
    const uint8_t off_short[]     = { 4, 0x00, 0x00, 0x30, 2, 0x00, 0x01, 0x00 };
    const uint8_t bad_button[]    = { 5, 0x00, 0x00, 0x30, 2, 0x00, 0x01, 0x00 };
    cmd_is(0xFC00, 0x00, on_press,  8, ZB_REMOTE_IGNORED, 0, "RWL021 0xFC00: the press edge is not yet a click");
    cmd_is(0xFC00, 0x00, on_short,  8, ZB_REMOTE_CLICK,   0, "RWL021 0xFC00: On, short release, is a click of key 0");
    cmd_is(0xFC00, 0x00, on_hold,   8, ZB_REMOTE_HOLD,    0, "RWL021 0xFC00: On held is a hold - its only way to say so");
    cmd_is(0xFC00, 0x00, on_long,   8, ZB_REMOTE_RELEASE, 0, "RWL021 0xFC00: and the long release ends it");
    cmd_is(0xFC00, 0x00, up_short,  8, ZB_REMOTE_CLICK,   2, "RWL021 0xFC00: Up is key 2, a click not a Step");
    cmd_is(0xFC00, 0x00, down_hold, 8, ZB_REMOTE_HOLD,    1, "RWL021 0xFC00: Down held is key 1 held");
    cmd_is(0xFC00, 0x00, off_short, 8, ZB_REMOTE_CLICK,   3, "RWL021 0xFC00: Off is key 3");
    cmd_is(0xFC00, 0x00, bad_button,8, ZB_REMOTE_NONE,    0, "a fifth button is not guessed at");
    cmd_is(0xFC00, 0x00, on_short,  4, ZB_REMOTE_NONE,    0, "nor a frame too short to carry the type");

    /* A Hue Smart Button is one button sending both: still one key. */
    s_model = ZB_REMOTE_MODEL_OTHER;
    cmd_is(0x0006, 0x40, effect,    2, ZB_REMOTE_CLICK, 0, "a one-button remote's Off stays key 0");
    /* Philips' cluster from anything not known to be a dimmer is left alone. */
    cmd_is(0xFC00, 0x00, on_short,  8, ZB_REMOTE_NONE,  0, "0xFC00 from another model is not decoded");
}

static void wheel_is(zb_wheel_t *w, uint8_t level, int64_t now, zb_remote_ev_t want,
                     uint8_t want_key, const char *what)
{
    uint8_t key = 0xEE;
    const zb_remote_ev_t got = remote_wheel_level(w, level, now, &key);
    s_checks++;
    if (got != want || (want == ZB_REMOTE_HOLD_TICK && key != want_key)) {
        printf("FAIL: %s (got %s key %d, wanted %s key %d)\n", what, remote_event_name(got), key,
               remote_event_name(want), want_key);
        exit(1);
    }
}

#define MS 1000LL

/* The direction of a wheel is the difference between two absolute levels. */
static void test_the_wheel_turns(void)
{
    zb_wheel_t w = { 0 };
    wheel_is(&w, 100, 0,        ZB_REMOTE_IGNORED,   0, "the first level after a boot has no direction");
    wheel_is(&w, 110, 101 * MS, ZB_REMOTE_HOLD_TICK, 2, "rising is up");
    wheel_is(&w, 120, 202 * MS, ZB_REMOTE_HOLD_TICK, 2, "and stays up");
    wheel_is(&w, 120, 303 * MS, ZB_REMOTE_HOLD_TICK, 2, "a repeated level keeps the turn alive");
    wheel_is(&w, 110, 404 * MS, ZB_REMOTE_HOLD_TICK, 1, "falling is down: the wheel turned back");
    wheel_is(&w, 254, 505 * MS, ZB_REMOTE_HOLD_TICK, 2, "the top is up");

    zb_wheel_t top = { 0 };
    wheel_is(&top, 254, 0, ZB_REMOTE_HOLD_TICK, 2,
             "pressing against the top is up even with nothing to compare");

    /* Level frames carry on across turns, since the level is the remote's own. */
    wheel_is(&w, 240, 5000 * MS, ZB_REMOTE_HOLD_TICK, 1, "a later turn compares with the last");
}

/* A real turn down to the bottom, with no click anywhere near it. */
static void test_the_wheel_reaches_the_bottom(void)
{
    zb_wheel_t w = { 0 };
    wheel_is(&w, 20, 0,        ZB_REMOTE_IGNORED,   0, "a starting level");
    wheel_is(&w, 10, 101 * MS, ZB_REMOTE_HOLD_TICK, 1, "going down");
    wheel_is(&w, 0,  202 * MS, ZB_REMOTE_IGNORED,   0, "a zero is held back, it might be a click");
    wheel_is(&w, 0,  303 * MS, ZB_REMOTE_HOLD_TICK, 1, "no click came, so it is still going down");
    wheel_is(&w, 0,  404 * MS, ZB_REMOTE_HOLD_TICK, 1, "and keeps going against the bottom");
    wheel_is(&w, 10, 505 * MS, ZB_REMOTE_HOLD_TICK, 2, "then back up from zero");

    /* A lone zero at the end of a turn is never acted on, but it is remembered,
     * or the next turn up from the bottom would compare with 10 and read as down. */
    zb_wheel_t end = { 0 };
    wheel_is(&end, 20, 0,         ZB_REMOTE_IGNORED,   0, "a turn");
    wheel_is(&end, 8,  101 * MS,  ZB_REMOTE_HOLD_TICK, 1, "down");
    wheel_is(&end, 0,  202 * MS,  ZB_REMOTE_IGNORED,   0, "ending at zero");
    wheel_is(&end, 5,  9000 * MS, ZB_REMOTE_HOLD_TICK, 2, "and much later up from zero is up");
}

/* The BILRESA's click: "slams to zero level, then the actual press occurs ~1ms
 * later, followed by level restoration ~100ms after". Read naively that is a
 * dim to the bottom and back, on every click - the lights would lurch. */
static void test_a_wheel_click_is_not_a_turn(void)
{
    /* The wheel has turned up before, so it has a direction - which is what makes
     * the restore dangerous: a frame at the same level keeps the old direction,
     * and without the quiet window it would start a sweep on every click. */
    zb_wheel_t w = { 0 };
    wheel_is(&w, 100, 0,        ZB_REMOTE_IGNORED,   0, "the wheel is somewhere");
    wheel_is(&w, 140, 101 * MS, ZB_REMOTE_HOLD_TICK, 2, "and was last turned up");
    wheel_is(&w, 0,   5000 * MS, ZB_REMOTE_IGNORED, 0, "the zero in front of a click");
    remote_wheel_note_onoff(&w, 5001 * MS);
    wheel_is(&w, 140, 5100 * MS, ZB_REMOTE_IGNORED, 0, "the restore behind it");
    ok(w.last == 140 && !w.zero_pending, "and the wheel is back where it was");
    wheel_is(&w, 150, 8000 * MS, ZB_REMOTE_HOLD_TICK, 2,
             "so the next turn up compares with 140, not with zero");

    /* An On/Off long after a zero is an ordinary click, not this one's. */
    zb_wheel_t late = { 0 };
    wheel_is(&late, 30, 0,        ZB_REMOTE_IGNORED, 0, "a turn");
    wheel_is(&late, 0,  101 * MS, ZB_REMOTE_IGNORED, 0, "a zero");
    remote_wheel_note_onoff(&late, 400 * MS);
    wheel_is(&late, 0,  202 * MS + 50 * MS, ZB_REMOTE_HOLD_TICK, 1,
             "a click too late to be the zero's leaves the zero a turn");
}

/* A ZG-101ZL. Each gesture is a tuyaAction saying what it was, then about 200 ms
 * later the command a bound lamp would get: Toggle, On or Off. Its hold has no
 * release, so it is a tick that silence ends. The trailing command is an echo
 * when it follows the action; alone, it is a whole gesture already counted. */
static void test_the_tuya_button(void)
{
    s_model = ZB_REMOTE_MODEL_TUYA_BUTTON;
    const uint8_t single[] = { 0 }, dbl[] = { 1 }, hold[] = { 2 }, other[] = { 3 };
    cmd_is(0x0006, 0xfd, single, 1, ZB_REMOTE_SINGLE,    0, "tuyaAction 0 is a single press");
    cmd_is(0x0006, 0xfd, dbl,    1, ZB_REMOTE_DOUBLE,    0, "1 is a double");
    cmd_is(0x0006, 0xfd, hold,   1, ZB_REMOTE_HOLD_TICK, 0, "2 is a hold with no release");
    cmd_is(0x0006, 0xfd, other,  1, ZB_REMOTE_NONE,      0, "3 is nothing it sends");
    cmd_is(0x0006, 0xfd, NULL,   0, ZB_REMOTE_NONE,      0, "nor is an empty one");
    cmd_is(0x0006, 0x02, NULL,   0, ZB_REMOTE_SINGLE,    0, "a Toggle on its own is final");
    cmd_is(0x0006, 0x00, NULL,   0, ZB_REMOTE_SINGLE,    0, "and so is an Off");

    /* The gaps measured on one: 190-200 ms. */
    ok(remote_is_echo(0x0006, 0x02, 195 * 1000), "Toggle after a single is its echo");
    ok(remote_is_echo(0x0006, 0x01, 195 * 1000), "On after a double");
    ok(remote_is_echo(0x0006, 0x00, 195 * 1000), "Off after a hold");
    ok(remote_is_echo(0x0006, 0x02, 0), "however soon");
    ok(!remote_is_echo(0x0006, 0x02, 2000 * 1000), "not seconds later: a press whose action was lost");
    ok(!remote_is_echo(0x0006, 0x02, -1), "nor from a remote that never sent one");
    ok(!remote_is_echo(0x0006, 0xfd, 195 * 1000), "an action is never an echo");
    ok(!remote_is_echo(0x0008, 0x02, 195 * 1000), "nor is a Level Step");

    /* Every Tuya scene switch sends it the same way, whatever its model. */
    s_model = ZB_REMOTE_MODEL_OTHER;
    cmd_is(0x0006, 0xfd, dbl, 1, ZB_REMOTE_DOUBLE, 0, "tuyaAction from any model");
}

/* Anything else has to decode to nothing, or a lamp answering a read would read
 * as somebody pressing something. */
static void test_everything_else_is_not_a_press(void)
{
    s_model = ZB_REMOTE_MODEL_OTHER;
    attr_is(0x0000, 0x0005, 0,  ZB_REMOTE_NONE, 0, "the model name is not a press");
    attr_is(0x0008, 0x0000, 42, ZB_REMOTE_NONE, 0, "a level report is not a press");
    attr_is(0x0300, 0x0007, 3,  ZB_REMOTE_NONE, 0, "a colour temperature is not a press");
    cmd_is(0x0300, 0x0a, NULL, 0, ZB_REMOTE_NONE, 0, "a colour command is not a press");
    cmd_is(0x0005, 0x05, NULL, 0, ZB_REMOTE_NONE, 0, "RecallScene is not one either, yet");
}

/* Every caller passes a key pointer, but the decoder must not depend on it. */
static void test_a_null_key_is_allowed(void)
{
    ok(remote_decode_attr(ZB_REMOTE_MODEL_OTHER, 0x0012, 0x0055, 1, NULL) == ZB_REMOTE_SINGLE,
       "a null key is harmless");
    ok(remote_decode_cmd(ZB_REMOTE_MODEL_OTHER, 0x0006, 0x02, NULL, 0, NULL) == ZB_REMOTE_CLICK,
       "for commands too");
}

/* The strings are the ModelIdentifiers the devices report. */
static void test_models(void)
{
    ok(remote_model("SYMFONISK Sound Controller") == ZB_REMOTE_MODEL_E1744, "the SYMFONISK");
    ok(remote_model("09BA") == ZB_REMOTE_MODEL_E2490, "the BILRESA");
    ok(remote_model("lumi.sensor_switch.aq2") == ZB_REMOTE_MODEL_AQARA, "the 2016 Aqara");
    ok(remote_model("lumi.remote.b1acn01") == ZB_REMOTE_MODEL_AQARA, "the 2018 Aqara");
    ok(remote_model("ROM001") == ZB_REMOTE_MODEL_OTHER, "a Hue button is decoded by command");
    ok(remote_model("RWL021") == ZB_REMOTE_MODEL_HUE_DIMMER, "the Hue dimmer");
    ok(remote_model("RWL020") == ZB_REMOTE_MODEL_HUE_DIMMER, "and its US twin");
    ok(remote_model("ZG-101ZL") == ZB_REMOTE_MODEL_TUYA_BUTTON, "the HOBEIAN button");
    ok(remote_model("") == ZB_REMOTE_MODEL_OTHER, "an empty model - not interrogated yet");
    ok(remote_model(NULL) == ZB_REMOTE_MODEL_OTHER, "and none at all");
}

int main(void)
{
    test_models();
    test_the_multistate_switch();
    test_the_on_off_switch();
    test_on_off_commands();
    test_level_commands();
    test_a_hue_smart_button_short_press();
    test_the_symfonisk_sound_controller();
    test_the_bilresa_presses();
    test_the_hue_dimmer();
    test_the_wheel_turns();
    test_the_wheel_reaches_the_bottom();
    test_a_wheel_click_is_not_a_turn();
    test_the_tuya_button();
    test_everything_else_is_not_a_press();
    test_a_null_key_is_allowed();
    printf("all %d checks passed\n", s_checks);
    return 0;
}
