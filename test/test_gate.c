/* Host test for the per-endpoint command gate in zb_coordinator.c.
 *
 * The gate decides which command goes on the wire and which one waits. Its
 * failure mode on hardware is a lamp that occasionally stops responding, which
 * nothing in a serial log explains - and the serial port reboots the board, so
 * looking destroys the run. Hence testing it here instead, over the orderings
 * that actually occur but are impractical to produce deliberately on a mesh.
 *
 * gate-types.inc and gate-body.inc are extracted from main/zb_coordinator.c by
 * the Makefile, never copied - see the note there.
 *
 *   make -C test
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_ZBC_MAX_GROUP_MEMBERS 16
/* The mechanism is tested at sixteen slots; the shipped size is in
 * zb_coordinator.c, outside the region, and is at least this. */
#define ZBC_COMMAND_SLOTS CONFIG_ZBC_MAX_GROUP_MEMBERS

/* ---- the ESP-IDF surface the gate touches, stubbed ---- */

typedef int ezb_err_t;
#define EZB_ERR_NONE 0
#define EZB_ERR_FAIL -1
typedef int esp_err_t;
#define ESP_OK 0

static int64_t s_now = 1000000;
static int64_t esp_timer_get_time(void) { return s_now; }

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m)  ((void)(m))

static struct { uint32_t gate_timeouts, slots_short; } s_radio;
static const char *TAG = "test";
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ZBC_SEND_RETRY(err_var, call) do { (err_var) = (call); } while (0)
static esp_err_t esp_zigbee_err_to_esp(ezb_err_t e) { return e == EZB_ERR_NONE ? ESP_OK : -1; }

#include "gate-types.inc"

/* Every frame the gate puts on the wire, in order, so a test can assert on the
 * sequence rather than on the gate's private state. */
static struct { uint16_t addr; uint8_t ep; zbc_cmd_t cmd; } s_wire[256];
static int  s_wire_n;
static bool s_refuse;          /* make the next send fail, as the stack does */

static ezb_err_t send_cmd_now(uint16_t addr, uint8_t ep, const zbc_cmd_t *c)
{
    if (s_refuse) {
        return EZB_ERR_FAIL;
    }
    assert(s_wire_n < (int)(sizeof(s_wire) / sizeof(s_wire[0])));
    s_wire[s_wire_n].addr = addr;
    s_wire[s_wire_n].ep   = ep;
    s_wire[s_wire_n].cmd  = *c;
    s_wire_n++;
    return EZB_ERR_NONE;
}

/* The settle scheduler proper is test_settle's subject; here it only has to
 * exist, and to record that the gate told it when a held frame went out. */
static struct { uint16_t addr; uint8_t ep; uint16_t transition_ds; } s_armed[64];
static int s_armed_n;

static void settle_sent_locked(uint16_t addr, uint8_t ep, uint16_t transition_ds)
{
    assert(s_armed_n < (int)(sizeof(s_armed) / sizeof(s_armed[0])));
    s_armed[s_armed_n].addr = addr;
    s_armed[s_armed_n].ep = ep;
    s_armed[s_armed_n].transition_ds = transition_ds;
    s_armed_n++;
}

/* Whether a device is still answering is test_reach's subject; to the gate it
 * is only a yes or no in front of everything else. */
#define ZB_ERR_UNREACHABLE -2
static bool s_unreachable;
static bool reach_admits(uint16_t addr, bool command)
{
    (void)addr; (void)command;
    return !s_unreachable;
}

#include "gate-body.inc"

/* ---- helpers ---- */

static int    s_checks;
static void ok(bool cond, const char *what)
{
    s_checks++;
    if (!cond) {
        printf("FAIL: %s\n", what);
        exit(1);
    }
}

static void reset(void)
{
    s_armed_n = 0;
    memset(s_gate, 0, sizeof(s_gate));
    s_wire_n = 0;
    s_refuse = false;
    s_unreachable = false;
    s_now    = 1000000;
}

static zbc_cmd_t level(uint16_t l) { return (zbc_cmd_t){ .kind = ZBC_CMD_LEVEL, .a = l }; }
static zbc_cmd_t fade_off(void)    { return (zbc_cmd_t){ .kind = ZBC_CMD_FADE_OFF }; }
static zbc_cmd_t ct(uint16_t m)    { return (zbc_cmd_t){ .kind = ZBC_CMD_CT, .a = m }; }
static zbc_cmd_t fade_in(uint16_t l) { return (zbc_cmd_t){ .kind = ZBC_CMD_FADE_IN, .a = l }; }

/* What the public zb_* functions do: admit, then send if admitted. */
static void issue(uint16_t addr, uint8_t ep, zbc_cmd_t c, int ch)
{
    if (gate_admit(addr, ep, &c, ch)) {
        if (send_cmd_now(addr, ep, &c) != EZB_ERR_NONE) {
            gate_abandon(addr, ep);
        }
    }
}

#define WIRE_KIND(i) (s_wire[(i)].cmd.kind)
#define WIRE_A(i)    (s_wire[(i)].cmd.a)

/* ---- the tests ---- */

/* The failure this whole mechanism exists for: an off issued while a level is
 * still in flight must not be able to reach the lamp before that level does. */
static void test_off_never_overtakes_level(void)
{
    reset();
    issue(0x3045, 11, level(200), ZBC_CH_LEVEL);
    ok(s_wire_n == 1 && WIRE_KIND(0) == ZBC_CMD_LEVEL, "level goes out at once");

    issue(0x3045, 11, fade_off(), ZBC_CH_LEVEL);
    ok(s_wire_n == 1, "the off waits while the level is unconfirmed");

    gate_advance(0x3045, 11);       /* the level is confirmed */
    ok(s_wire_n == 2 && WIRE_KIND(1) == ZBC_CMD_FADE_OFF, "then the off goes out");

    gate_advance(0x3045, 11);
    ok(s_wire_n == 2, "and nothing follows it");
}

/* A drag produces a stream where only the last value matters. Everything
 * superseded before it was sent should be dropped, not queued up and replayed. */
static void test_newest_wins_within_a_channel(void)
{
    reset();
    issue(0x3045, 11, level(10), ZBC_CH_LEVEL);
    for (int v = 20; v <= 90; v += 10) {
        issue(0x3045, 11, level((uint16_t)v), ZBC_CH_LEVEL);
    }
    ok(s_wire_n == 1, "one command on the wire through the whole drag");

    gate_advance(0x3045, 11);
    ok(s_wire_n == 2 && WIRE_A(1) == 90, "the value the finger stopped on is the one sent");

    gate_advance(0x3045, 11);
    ok(s_wire_n == 2, "the eight superseded steps never go on air");
}

/* A curve turns one request into a level command and a colour command. Both are
 * wanted; collapsing them would leave curve_db's deadband asserting a colour the
 * lamp never received. */
static void test_colour_is_not_collapsed_into_level(void)
{
    reset();
    issue(0x3045, 11, level(120), ZBC_CH_LEVEL);
    issue(0x3045, 11, ct(370), ZBC_CH_COLOR);
    ok(s_wire_n == 1, "the colour waits behind the level");

    gate_advance(0x3045, 11);
    ok(s_wire_n == 2 && WIRE_KIND(1) == ZBC_CMD_CT, "and then goes out");

    gate_advance(0x3045, 11);
    ok(s_wire_n == 2, "both halves sent, exactly once each");
}

/* ...and a newer level must not evict a queued colour, nor the other way round. */
static void test_channels_do_not_evict_each_other(void)
{
    reset();
    issue(0x3045, 11, level(120), ZBC_CH_LEVEL);
    issue(0x3045, 11, ct(370), ZBC_CH_COLOR);
    issue(0x3045, 11, level(200), ZBC_CH_LEVEL);   /* supersedes the queued level only */

    gate_advance(0x3045, 11);
    gate_advance(0x3045, 11);
    ok(s_wire_n == 3, "three frames: the first level, then the new level, then the colour");

    bool saw_ct = false, saw_200 = false;
    for (int i = 1; i < s_wire_n; i++) {
        saw_ct  |= WIRE_KIND(i) == ZBC_CMD_CT;
        saw_200 |= WIRE_KIND(i) == ZBC_CMD_LEVEL && WIRE_A(i) == 200;
    }
    ok(saw_ct && saw_200, "the colour survived a newer level");
}

/* The fade in is two frames of one command. The ramp must follow the preset,
 * and must not be treated as a newer intent that supersedes it. */
static void test_fade_in_sends_both_halves_in_order(void)
{
    reset();
    issue(0x3045, 11, fade_in(200), ZBC_CH_LEVEL);
    ok(s_wire_n == 1 && WIRE_KIND(0) == ZBC_CMD_FADE_IN && s_wire[0].cmd.step == 0,
       "the preset goes first");

    gate_advance(0x3045, 11);
    ok(s_wire_n == 2 && WIRE_KIND(1) == ZBC_CMD_FADE_IN && s_wire[1].cmd.step == 1,
       "the ramp follows once the preset is confirmed");
    ok(WIRE_A(1) == 200, "and it carries the target level");

    gate_advance(0x3045, 11);
    ok(s_wire_n == 2, "the endpoint is then free");
}

/* An off arriving mid-fade-in still has to wait for both halves, or it lands
 * between them and the ramp turns the lamp back on. */
static void test_fade_in_is_not_split_by_a_later_command(void)
{
    reset();
    issue(0x3045, 11, fade_in(200), ZBC_CH_LEVEL);
    issue(0x3045, 11, fade_off(), ZBC_CH_LEVEL);
    ok(s_wire_n == 1, "the off waits");

    gate_advance(0x3045, 11);
    ok(s_wire_n == 2 && s_wire[1].cmd.step == 1, "the ramp is still the next frame");

    gate_advance(0x3045, 11);
    ok(s_wire_n == 3 && WIRE_KIND(2) == ZBC_CMD_FADE_OFF, "the off comes last of the three");
}

/* Endpoints are independent: a group command fans out to every member at once
 * and none of them may be held up by another. */
static void test_endpoints_are_independent(void)
{
    reset();
    for (int m = 0; m < CONFIG_ZBC_MAX_GROUP_MEMBERS; m++) {
        issue((uint16_t)(0x3000 + m), 11, level(200), ZBC_CH_LEVEL);
    }
    ok(s_wire_n == CONFIG_ZBC_MAX_GROUP_MEMBERS, "every member's command goes out at once");

    /* The same endpoint number on a different device must not share a slot. */
    reset();
    issue(0x3045, 11, level(200), ZBC_CH_LEVEL);
    issue(0x3d33, 11, level(200), ZBC_CH_LEVEL);
    ok(s_wire_n == 2, "same endpoint number, different device, both sent");

    reset();
    issue(0x3045, 11, level(200), ZBC_CH_LEVEL);
    issue(0x3045, 12, level(200), ZBC_CH_LEVEL);
    ok(s_wire_n == 2, "same device, different endpoint, both sent");
}

/* More endpoints than there are slots: a command that cannot be sequenced is
 * still sent, because unordered beats never. */
static void test_no_slot_still_sends(void)
{
    reset();
    for (int m = 0; m < CONFIG_ZBC_MAX_GROUP_MEMBERS; m++) {
        issue((uint16_t)(0x3000 + m), 11, level(200), ZBC_CH_LEVEL);
    }
    issue(0x9999, 11, level(200), ZBC_CH_LEVEL);
    ok(s_wire_n == CONFIG_ZBC_MAX_GROUP_MEMBERS + 1, "the unslotted command goes out anyway");
}

/* A confirmation that never arrives must not shut an endpoint for good. */
static void test_timeout_reopens_the_gate(void)
{
    reset();
    issue(0x3045, 11, level(200), ZBC_CH_LEVEL);
    issue(0x3045, 11, fade_off(), ZBC_CH_LEVEL);
    ok(s_wire_n == 1, "the off is waiting on a confirmation that will not come");

    s_now += ZBC_GATE_TIMEOUT_US - 1;
    ok(gate_flush_stale() == false, "not yet");
    ok(s_wire_n == 1, "still waiting inside the timeout");

    s_now += 2;
    ok(gate_flush_stale() == true, "the flush finds it");
    ok(s_wire_n == 2 && WIRE_KIND(1) == ZBC_CMD_FADE_OFF, "and the queued off goes out");

    /* And the endpoint is usable again afterwards. */
    gate_advance(0x3045, 11);
    issue(0x3045, 11, level(50), ZBC_CH_LEVEL);
    ok(s_wire_n == 3, "the endpoint still works after a timeout");
}

/* A send the stack refuses produces no confirmation, so the gate has to be
 * reopened by hand or everything behind it is stranded. */
static void test_refused_send_does_not_wedge(void)
{
    reset();
    s_refuse = true;
    issue(0x3045, 11, level(200), ZBC_CH_LEVEL);
    ok(s_wire_n == 0, "nothing reached the wire");

    s_refuse = false;
    issue(0x3045, 11, level(150), ZBC_CH_LEVEL);
    ok(s_wire_n == 1 && WIRE_A(0) == 150, "the next command is not blocked by the refused one");
}

/* A command queued behind one that was then refused still has to go out. */
static void test_queued_behind_a_refusal_is_flushed(void)
{
    reset();
    issue(0x3045, 11, level(200), ZBC_CH_LEVEL);   /* in flight */
    issue(0x3045, 11, ct(370), ZBC_CH_COLOR);      /* queued */
    ok(s_wire_n == 1, "the colour is queued");

    /* The confirmation arrives, the gate tries to send the colour, the stack
     * refuses it - so it is abandoned rather than left half-sent. */
    s_refuse = true;
    gate_advance(0x3045, 11);
    s_refuse = false;
    ok(s_wire_n == 1, "the refused colour did not reach the wire");

    issue(0x3045, 11, level(50), ZBC_CH_LEVEL);
    ok(s_wire_n == 2, "and the endpoint is not wedged behind it");
}

/* gate_advance reports whether a newer intent went out in place of the one that
 * just finished - which is what stops data_confirm_cb re-sending a stale one. */
static void test_advance_reports_supersession(void)
{
    reset();
    issue(0x3045, 11, level(200), ZBC_CH_LEVEL);
    ok(gate_advance(0x3045, 11) == false, "nothing queued: not superseded");

    issue(0x3045, 11, level(200), ZBC_CH_LEVEL);
    issue(0x3045, 11, fade_off(), ZBC_CH_LEVEL);
    ok(gate_advance(0x3045, 11) == true, "something queued: superseded");
}

/* A stray confirmation for an idle endpoint must do nothing at all. */
static void test_stray_confirmation_is_harmless(void)
{
    reset();
    ok(gate_advance(0x3045, 11) == false, "unknown endpoint");
    ok(s_wire_n == 0, "and nothing sent");

    issue(0x3045, 11, level(200), ZBC_CH_LEVEL);
    gate_advance(0x3045, 11);
    gate_advance(0x3045, 11);
    gate_advance(0x3045, 11);
    ok(s_wire_n == 1, "duplicate confirmations do not conjure commands");
}

/* Slots are recycled once idle, or sixteen endpoints would be the lifetime
 * limit rather than the concurrent one. */
static void test_slots_are_recycled(void)
{
    reset();
    for (int round = 0; round < 8; round++) {
        for (int m = 0; m < CONFIG_ZBC_MAX_GROUP_MEMBERS; m++) {
            const uint16_t addr = (uint16_t)(0x3000 + round * 32 + m);
            issue(addr, 11, level(200), ZBC_CH_LEVEL);
            gate_advance(addr, 11);
        }
    }
    ok(s_wire_n == 8 * CONFIG_ZBC_MAX_GROUP_MEMBERS, "128 endpoints through 16 slots");
}

/* The reads that follow a command are armed when it is asked for, and a held
 * command goes out later than that - sometimes much later. Whoever finally puts
 * it on the wire has to say so, or the reads land before the lamp has been told
 * anything and the answer they get is the state before the command. */
static void test_a_held_frame_says_when_it_goes_out(void)
{
    reset();
    issue(0x3045, 11, level(120), ZBC_CH_LEVEL);
    ok(s_armed_n == 0, "a frame the gate let straight through arms nothing here");

    issue(0x3045, 11, (zbc_cmd_t){ .kind = ZBC_CMD_CT, .a = 370, .transition_ds = 7 },
          ZBC_CH_COLOR);
    ok(s_armed_n == 0, "and neither does one that is only queued");

    gate_advance(0x3045, 11);
    ok(s_wire_n == 2 && s_armed_n == 1, "the colour going out is announced");
    ok(s_armed[0].addr == 0x3045 && s_armed[0].ep == 11, "for its own endpoint");
    ok(s_armed[0].transition_ds == 7, "with the transition the lamp will take");

    gate_advance(0x3045, 11);
    ok(s_armed_n == 1, "an advance with nothing queued announces nothing");
}

/* The ramp half of a fade in is a frame of its own, sent a confirmation after
 * the preset - so it is the one the reads should be waiting on. */
static void test_the_ramp_half_announces_itself(void)
{
    reset();
    issue(0x3045, 11, (zbc_cmd_t){ .kind = ZBC_CMD_FADE_IN, .a = 200, .transition_ds = 7 },
          ZBC_CH_LEVEL);
    ok(s_wire_n == 1 && s_armed_n == 0, "the preset is the request itself");

    gate_advance(0x3045, 11);
    ok(s_wire_n == 2 && WIRE_A(1) == 200, "the ramp follows");
    ok(s_armed_n == 1 && s_armed[0].transition_ds == 7, "and it is the one announced");
}

/* A refused frame never reached the lamp, so nothing should be waiting on it. */
static void test_a_refused_frame_announces_nothing(void)
{
    reset();
    issue(0x3045, 11, level(120), ZBC_CH_LEVEL);
    issue(0x3045, 11, ct(370), ZBC_CH_COLOR);
    s_refuse = true;
    gate_advance(0x3045, 11);
    ok(s_armed_n == 0, "the reads are not started for a frame the stack refused");
}

/* A command to a device that has stopped answering is not sent, says so, and
 * does not claim the gate: had it, the next command after the device came back
 * would wait out ZBC_GATE_TIMEOUT_US behind a frame that never went out. */
static void test_an_unreachable_device_is_not_sent_to(void)
{
    reset();
    s_unreachable = true;
    const zbc_cmd_t c = level(120);
    ok(gate_send(0x3045, 11, &c, ZBC_CH_LEVEL) == ZB_ERR_UNREACHABLE, "the send says the device is gone");
    ok(s_wire_n == 0, "and nothing went on air");
    s_unreachable = false;
    const zbc_cmd_t d = level(200);
    ok(gate_send(0x3045, 11, &d, ZBC_CH_LEVEL) == ESP_OK && s_wire_n == 1 && s_wire[0].cmd.a == 200,
       "back, the next command goes straight out - the gate was never held");
}

int main(void)
{
    test_off_never_overtakes_level();
    test_newest_wins_within_a_channel();
    test_colour_is_not_collapsed_into_level();
    test_channels_do_not_evict_each_other();
    test_fade_in_sends_both_halves_in_order();
    test_fade_in_is_not_split_by_a_later_command();
    test_endpoints_are_independent();
    test_no_slot_still_sends();
    test_timeout_reopens_the_gate();
    test_refused_send_does_not_wedge();
    test_queued_behind_a_refusal_is_flushed();
    test_advance_reports_supersession();
    test_stray_confirmation_is_harmless();
    test_slots_are_recycled();
    test_a_held_frame_says_when_it_goes_out();
    test_the_ramp_half_announces_itself();
    test_a_refused_frame_announces_nothing();
    test_an_unreachable_device_is_not_sent_to();
    printf("all %d checks passed\n", s_checks);
    (void)TAG;
    return 0;
}
