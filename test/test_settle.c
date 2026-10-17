/* Host test for the settle scheduler in zb_coordinator.c.
 *
 * The thing under test is which slot gets read when. One timer serves many
 * slots, and getting that wrong ends a lamp's reads early, marks its state
 * unknown and sends it a duplicate of its own command - all of which read as a
 * flaky lamp, and none of which a log explains.
 *
 * gate-types.inc, settle-types.inc and settle-body.inc are extracted from
 * main/zb_coordinator.c
 * by the Makefile, never copied - see the note there.
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
#define ZBC_SETTLE_SLOTS  ZBC_COMMAND_SLOTS
#define ZBC_WANT_KEEP ((int8_t)-2)
#define ZBC_UNKNOWN (-1)
#define ZBC_ATTR_ONOFF 0

/* ---- the ESP-IDF surface, stubbed ---- */

static int64_t s_now = 1000000;
static int64_t esp_timer_get_time(void) { return s_now; }

static bool    s_armed;
static int64_t s_armed_delay;
typedef void *esp_timer_handle_t;
static esp_timer_handle_t s_settle_timer = (void *)1;
static void esp_timer_stop(esp_timer_handle_t t) { (void)t; s_armed = false; }
static void esp_timer_start_once(esp_timer_handle_t t, uint64_t us)
{ (void)t; s_armed = true; s_armed_delay = (int64_t)us; }

#define portMAX_DELAY 0xffffffffU
static int s_lock_depth, s_lock_max;
static bool esp_zigbee_lock_acquire(uint32_t t)
{ (void)t; if (++s_lock_depth > s_lock_max) s_lock_max = s_lock_depth; return true; }
static void esp_zigbee_lock_release(void) { s_lock_depth--; }

static const char *TAG = "test";
#define ESP_LOGW(tag, ...) ((void)(tag))

static struct { uint32_t went_quiet, corrections, slots_short; } s_radio;

/* Calls the code under test makes outwards, recorded so a test can assert. */
static int s_reads, s_resends, s_unknowns;
static uint16_t s_last_read_addr;
/* When true the lamp replies to every read, which is what marks the slot
 * answered. Defined below the includes because it needs s_settle. */
static bool s_lamp_answers;
static void read_light_state(uint16_t a, uint8_t ep, uint16_t f);
static void resend_intent_locked(uint16_t a, uint8_t ep, int8_t on, uint8_t lvl)
{ (void)a; (void)ep; (void)on; (void)lvl; s_resends++; }
static int device_db_set_attr_by_short(uint16_t a, uint8_t ep, int attr, int32_t v)
{ (void)a; (void)ep; (void)attr; (void)v; s_unknowns++; return 0; }
static bool gate_flush_stale(void) { return false; }

/* The settle slot now records the colour a command asked for, so it needs the
 * command types the gate is built on - the same region test_gate.c compiles. */
#include "gate-types.inc"
#include "settle-types.inc"
/* Whether a device has stopped answering is test_reach's subject; here it is a
 * switch the tests can throw. */
static bool s_gone;
static bool reach_is_gone(uint16_t addr) { (void)addr; return s_gone; }

#include "settle-body.inc"

static void read_light_state(uint16_t a, uint8_t ep, uint16_t f)
{
    (void)f;
    s_reads++;
    s_last_read_addr = a;
    if (!s_lamp_answers) {
        return;
    }
    for (int i = 0; i < ZBC_SETTLE_SLOTS; i++) {
        if (s_settle[i].short_addr == a && s_settle[i].ep == ep) {
            s_settle[i].answered = true;
            break;
        }
    }
}

/* ---- helpers ---- */

static int s_checks;
static void ok(bool cond, const char *what)
{
    s_checks++;
    if (!cond) { printf("FAIL: %s\n", what); exit(1); }
}

static void reset(void)
{
    memset(s_settle, 0, sizeof(s_settle));
    memset(&s_radio, 0, sizeof(s_radio));
    s_now = 1000000;
    s_armed = false; s_armed_delay = 0;
    s_reads = s_resends = s_unknowns = 0;
    s_lamp_answers = false;
    s_lock_depth = s_lock_max = 0;
    s_gone = false;
}

static int slot_of(uint16_t addr, uint8_t ep)
{
    for (int i = 0; i < ZBC_SETTLE_SLOTS; i++) {
        if (s_settle[i].short_addr == addr && s_settle[i].ep == ep) return i;
    }
    return -1;
}

/* Run the clock forward, firing the timer whenever it is due. */
static void advance(int64_t us)
{
    const int64_t end = s_now + us;
    while (s_armed && s_now + s_armed_delay <= end) {
        s_now += s_armed_delay;
        s_armed = false;
        settle_work();
    }
    s_now = end;
}

/* ---- the tests ---- */

/* One lamp's command must not restart the shared timer and cut short the
 * window of another lamp still settling. */
static void test_a_second_command_does_not_cut_the_first_short(void)
{
    reset();
    schedule_settle_read(0x3045, 11, 0, 8, 1, 200);         /* due at +1.2 s */
    const int64_t due_a = s_settle[slot_of(0x3045, 11)].due_us;

    s_now += 100000;                                         /* 0.1 s later... */
    schedule_settle_read(0x3d33, 3, 0, 0, 1, 200);           /* ...a short one */

    ok(s_settle[slot_of(0x3045, 11)].due_us == due_a, "the first lamp's deadline is untouched");
    ok(s_armed, "the timer is armed");
    /* The second lamp is due sooner, so the timer must serve it first. */
    ok(s_now + s_armed_delay <= due_a, "the timer is armed for the nearer of the two");
}

/* Each slot is read at its own deadline, not at whichever was set last. */
static void test_each_slot_is_read_at_its_own_deadline(void)
{
    reset();
    schedule_settle_read(0x3045, 11, 0, 100, 1, 200);        /* +10.4 s */
    schedule_settle_read(0x3d33, 3, 0, 0, 1, 200);           /* +0.4 s */

    advance(1000000);
    ok(s_settle[slot_of(0x3d33, 3)].reads_left == 1, "the near lamp has been read");
    ok(s_settle[slot_of(0x3045, 11)].reads_left == 2, "the far lamp has not been touched");
    ok(s_reads == 1, "exactly one read went out");
}

/* A lamp that answers every read runs two reads, then an audit that finds
 * nothing wrong, and stops - without ever calling its state unknown. */
static void test_a_healthy_lamp_settles_and_stops(void)
{
    reset();
    s_lamp_answers = true;
    schedule_settle_read(0x3045, 11, 0, 8, 1, 200);
    s_settle[slot_of(0x3045, 11)].delivered = true;

    advance(20000000);
    ok(s_settle[slot_of(0x3045, 11)].reads_left == 0, "the slot is finished");
    ok(s_unknowns == 0, "its state was never called unknown");
    ok(s_resends == 0, "and it was never sent the command again");
    ok(!s_armed, "the timer stops when there is nothing outstanding");
}

/* A lamp that never answers, but whose command *was* confirmed delivered, is a
 * lost read - so it is re-read and its state called unknown, never re-commanded. */
static void test_a_delivered_command_is_not_resent_on_silence(void)
{
    reset();
    schedule_settle_read(0x3045, 11, 0, 8, 1, 200);
    s_settle[slot_of(0x3045, 11)].delivered = true;

    advance(20000000);
    ok(s_resends == 0, "a delivered command is never repeated just because the lamp is quiet");
    ok(s_unknowns == 1, "its state is called unknown instead");
    ok(s_radio.went_quiet == 1, "and counted");
}

/* ...whereas one that was never confirmed delivered is worth saying again. */
static void test_an_undelivered_command_is_resent_on_silence(void)
{
    reset();
    schedule_settle_read(0x3045, 11, 0, 8, 1, 200);          /* delivered stays false */

    advance(20000000);
    ok(s_resends >= 1, "an unconfirmed command is sent again when the lamp is silent");
    ok(s_resends <= 2, "and is bounded by fixes_left");
}

/* A re-send starts the reads over: the read that judges the lamp has to follow
 * the re-sent command, not go out beside it and be answered with the state the
 * lamp was in before the command arrived. */
static void test_a_resend_is_not_judged_by_a_read_sent_beside_it(void)
{
    reset();
    schedule_settle_read(0x3045, 11, 0, 8, 1, 200);          /* delivered stays false */
    advance(1200000);                                        /* first read, unanswered */
    ok(s_reads == 1 && s_resends == 0, "one read, no re-send yet");
    advance(2500000);                                        /* silence: re-send */
    ok(s_resends == 1, "the command was sent again");
    ok(s_reads == 1, "and no read went out beside it");
    ok(!s_settle[slot_of(0x3045, 11)].judge, "nothing is being judged yet");
    ok(s_settle[slot_of(0x3045, 11)].reads_left == 2, "the two reads start over");
}

/* The first pass must never re-send: no read has gone out yet to be unanswered. */
static void test_the_first_pass_never_resends(void)
{
    reset();
    schedule_settle_read(0x3045, 11, 0, 8, 1, 200);
    advance(1300000);                                        /* just the first pass */
    ok(s_reads == 1, "one read has gone out");
    ok(s_resends == 0, "and nothing was re-commanded on the strength of it");
}

/* A group fan-out gives every member the same deadline; all of them get read -
 * but spaced out, one per tick, rather than as one burst of frames. */
static void test_a_group_fanout_reads_every_member(void)
{
    reset();
    for (int m = 0; m < CONFIG_ZBC_MAX_GROUP_MEMBERS; m++) {
        schedule_settle_read((uint16_t)(0x3000 + m), 11, 0, 8, 1, 200);
    }
    advance(1200000);                                        /* exactly the deadline */
    ok(s_reads == 1, "the first member is read the moment it is due");
    ok(s_armed && s_armed_delay <= ZBC_READ_STAGGER_US, "and the next is only a stagger away");
    advance(CONFIG_ZBC_MAX_GROUP_MEMBERS * ZBC_READ_STAGGER_US);
    ok(s_reads == CONFIG_ZBC_MAX_GROUP_MEMBERS, "every member was read within the stagger");
    for (int m = 0; m < CONFIG_ZBC_MAX_GROUP_MEMBERS; m++) {
        ok(s_settle[slot_of((uint16_t)(0x3000 + m), 11)].reads_left == 1, "each exactly once");
    }
}

/* A lamp that reported after the command does not need the unjudged first
 * read; the judged one still follows. */
static void test_a_report_after_the_command_skips_the_first_read(void)
{
    reset();
    schedule_settle_read(0x3045, 11, 0, 8, 1, 200);
    s_settle[slot_of(0x3045, 11)].delivered = true;
    s_settle[slot_of(0x3045, 11)].answered  = true;         /* its own report came in */

    advance(1300000);                                        /* the first pass */
    ok(s_reads == 0, "the unjudged read was skipped");
    ok(s_settle[slot_of(0x3045, 11)].reads_left == 1, "but the schedule moved on");
    advance(2500000);                                        /* the second */
    ok(s_reads == 1, "the judged read still goes out");
    ok(s_settle[slot_of(0x3045, 11)].judge, "and is the one that judges");
    ok(s_resends == 0, "nothing was re-sent: a lamp that reported got the command");
}

/* ZBC_WANT_KEEP: a colour command on a slot that is still settling keeps the
 * level command's intent and its remaining budget rather than wiping them. */
static void test_want_keep_preserves_a_live_intent(void)
{
    reset();
    schedule_settle_read(0x3045, 11, 0, 8, 1, 200);
    int s = slot_of(0x3045, 11);
    s_settle[s].fixes_left = 1;                              /* one already spent */

    schedule_settle_read(0x3045, 11, 0, 8, ZBC_WANT_KEEP, 0);
    s = slot_of(0x3045, 11);
    ok(s_settle[s].want_on == 1, "the intent survived the colour command");
    ok(s_settle[s].want_level == 200, "and so did the level it asked for");
    ok(s_settle[s].fixes_left == 1, "the budget carried over rather than resetting");
}

/* ...and on a slot that has finished settling, it does not revive a stale one. */
static void test_want_keep_does_not_revive_a_finished_intent(void)
{
    reset();
    schedule_settle_read(0x3045, 11, 0, 8, 1, 200);
    s_settle[slot_of(0x3045, 11)].delivered = true;
    advance(20000000);                                       /* let it finish */

    schedule_settle_read(0x3045, 11, 0, 8, ZBC_WANT_KEEP, 0);
    const int s = slot_of(0x3045, 11);
    ok(s_settle[s].want_on == -1, "a finished intent is not brought back");
    ok(s_settle[s].fixes_left == 0, "and it gets no correction budget");
}

/* schedule_settle_read takes the lock; it must not be nested or leaked. */
static void test_locking_is_balanced(void)
{
    reset();
    schedule_settle_read(0x3045, 11, 0, 8, 1, 200);
    ok(s_lock_depth == 0, "schedule_settle_read gives the lock back");
    ok(s_lock_max == 1, "and never nests it");
    advance(4000000);
    ok(s_lock_depth == 0, "settle_work gives the lock back");
    ok(s_lock_max == 1, "and never nests it either");
}

/* Slots are recycled once finished. */
static void test_slots_are_recycled(void)
{
    reset();
    for (int round = 0; round < 8; round++) {
        for (int m = 0; m < CONFIG_ZBC_MAX_GROUP_MEMBERS; m++) {
            const uint16_t addr = (uint16_t)(0x3000 + round * 32 + m);
            schedule_settle_read(addr, 11, 0, 0, 1, 200);
            s_settle[slot_of(addr, 11)].delivered = true;
        }
        advance(20000000);
    }
    ok(s_reads >= 8 * CONFIG_ZBC_MAX_GROUP_MEMBERS, "128 endpoints through 16 slots");
}

/* A lost read must not cost five minutes of "unknown". The audit marks the
 * state unknown - honestly - and then asks again, because nothing else re-reads
 * a lamp and the alternative is waiting for its own report. */
static void test_a_quiet_lamp_is_asked_again(void)
{
    reset();
    schedule_settle_read(0x3045, 11, 0, 8, 1, 200);
    s_settle[slot_of(0x3045, 11)].delivered = true;

    /* Two reads and the audit, all unanswered: the state goes unknown. */
    advance(9000000);
    ok(s_unknowns == 1, "the state was called unknown");
    const int reads_then = s_reads;

    /* ...and it is asked again rather than left there. */
    advance(7000000);
    ok(s_reads > reads_then, "the lamp was read again after going quiet");
    ok(s_resends == 0, "but never re-commanded - a silent lamp is not told things");
}

/* Nor its colour: the recovery read is judged like any last read, and a colour
 * intent left standing would have the lamp that answers it re-sent a colour. */
static void test_recovery_drops_the_colour_intent(void)
{
    reset();
    const zbc_cmd_t ct = { .kind = ZBC_CMD_CT, .a = 362 };
    schedule_settle_read_ex(0x3045, 11, 0, 8, ZBC_WANT_KEEP, 0, &ct);
    const int s = slot_of(0x3045, 11);
    s_settle[s].delivered = true;
    ok(s_settle[s].color_fixes_left == 2, "the colour command has its budget");

    advance(9000000);                       /* two reads and the audit, unanswered */
    ok(s_unknowns == 1, "the lamp went quiet");
    ok(s_settle[s].want_color == ZBC_CMD_NONE && s_settle[s].color_fixes_left == 0,
       "and its recovery reads carry no colour to correct");
}

/* The lamp answers the recovery read: it settles and stops being chased. */
static void test_recovery_stops_once_the_lamp_answers(void)
{
    reset();
    schedule_settle_read(0x3045, 11, 0, 8, 1, 200);
    s_settle[slot_of(0x3045, 11)].delivered = true;
    advance(9000000);                       /* goes quiet */
    ok(s_unknowns == 1, "its state went unknown");

    s_lamp_answers = true;                  /* now it starts replying again */
    advance(30000000);
    ok(s_settle[slot_of(0x3045, 11)].reads_left == 0, "it stops once the lamp answers");
    ok(!s_armed, "and the timer goes idle");
}

/* A lamp that is genuinely gone stops being asked, rather than being read for ever. */
static void test_recovery_is_bounded(void)
{
    reset();
    schedule_settle_read(0x3045, 11, 0, 8, 1, 200);
    s_settle[slot_of(0x3045, 11)].delivered = true;

    advance(120000000);                     /* two minutes of silence */
    ok(s_settle[slot_of(0x3045, 11)].reads_left == 0, "it gives up in the end");
    ok(!s_armed, "and stops waking up for it");
    ok(s_reads <= 12, "having asked a bounded number of times, not continuously");
    ok(s_resends == 0, "and never sent it a command");
}

/* A command queued behind another goes out later than it was asked for, and the
 * reads armed with it have already come and gone: the lamp answers them with the
 * state it was in before it was told anything, the slot counts that as settled,
 * and for a colour - which is not reported - that answer is what the registry
 * keeps. The reads have to follow the frame. */
static void test_reads_start_again_when_a_held_command_goes_out(void)
{
    reset();
    s_lamp_answers = true;
    schedule_settle_read(0x3045, 11, 0xff, 7, -1, 0);   /* 0.7 s transition */

    /* The gate is holding it. Both reads fire and are answered - by a lamp that
     * has not had the command yet. */
    advance(4000000);
    ok(s_reads == 2, "the reads went out while the command was still queued");
    ok(slot_of(0x3045, 11) >= 0 && s_settle[slot_of(0x3045, 11)].reads_left == 1,
       "and the slot is down to its audit pass");

    /* Two seconds later the frame finally leaves. */
    settle_sent_locked(0x3045, 11, 7);
    const int after = s_reads;
    advance(4000000);
    ok(s_reads == after + 2, "the lamp is read twice more, once it has been told");
}

/* The window is the transition of the frame that went out, counted from when it
 * went out - not from whenever the request happened to be made. Checked on the
 * deadline itself, which is exact, rather than on the stubbed timer. */
static void test_the_restarted_window_is_the_transition(void)
{
    reset();
    s_lamp_answers = true;
    schedule_settle_read(0x3045, 11, 0xff, 0, -1, 0);
    const int i = slot_of(0x3045, 11);
    advance(4000000);

    s_now += 3000000;                          /* the frame waited three seconds */
    settle_sent_locked(0x3045, 11, 20);        /* and carries a two-second ramp */
    ok(s_settle[i].due_us == s_now + 2400000, "the window starts where the frame did");
    ok(s_settle[i].reads_left == 2, "with both of its reads back");
}

/* An endpoint nobody is watching must not be given a slot by this: the gate
 * announces every frame it sends, including ones whose reads are long done. */
static void test_a_finished_slot_is_not_revived(void)
{
    reset();
    s_lamp_answers = true;
    schedule_settle_read(0x3045, 11, 0xff, 0, -1, 0);
    advance(10000000);
    const int before = s_reads;
    ok(s_settle[slot_of(0x3045, 11)].reads_left == 0, "the slot is finished");

    settle_sent_locked(0x3045, 11, 0);
    settle_sent_locked(0x9999, 3, 0);          /* an endpoint with no slot at all */
    advance(10000000);
    ok(s_reads == before, "and stays finished");
    ok(slot_of(0x9999, 3) < 0, "no slot is invented for an unwatched endpoint");
}

/* The frame that has just gone out has not been confirmed yet, so the previous
 * one's confirmation must not stand in for it - that is what tells settle_work
 * a silent lamp heard the command and stops it being sent again. */
static void test_the_restart_clears_the_confirmation(void)
{
    reset();
    s_lamp_answers = false;
    schedule_settle_read(0x3045, 11, 0xff, 0, 1, 200);
    const int i = slot_of(0x3045, 11);
    s_settle[i].delivered = true;

    settle_sent_locked(0x3045, 11, 0);
    ok(!s_settle[i].delivered, "the new frame carries no confirmation of its own");
    advance(4000000);
    ok(s_resends == 1, "so a lamp that stays silent is told again");
}

/* A command to a device that has stopped answering was its probe, and the probe's
 * own confirmation is what says whether it is back. Re-sends and recovery reads
 * after it would be more of what it has stopped taking - but its two reads are
 * what find out what a lamp that is back did with it (the reads themselves are
 * skipped by read_attributes while it is still gone), so those are armed. */
static void test_a_probe_is_read_but_never_corrected(void)
{
    reset();
    s_gone = true;
    schedule_settle_read(0x3045, 11, 0, 8, 1, 200);
    const int s = slot_of(0x3045, 11);
    ok(s >= 0 && s_settle[s].reads_left == 2, "the probe's reads are armed");
    ok(s_armed, "and the timer");
    ok(s_settle[s].want_on == -1 && s_settle[s].fixes_left == 0, "with no intent to correct against");
    ok(s_settle[s].recover_left == 0, "and no recovery reads");
    ok(s_lock_depth == 0, "and the lock is released");

    /* Nothing answers - the device is still gone - and nothing follows. */
    advance(30 * 1000000LL);
    ok(s_resends == 0, "no command is re-sent");
    ok(s_unknowns == 0, "and a lamp still gone is not news: its state is left alone");
    ok(s_radio.went_quiet == 0, "nor counted as going quiet");
    ok(s_reads == 2, "two reads were due, and no more");
}

/* ---- the colour a lamp did not take ---- */

/* Colour is never reported and is re-read only every five minutes, so a colour
 * command a lamp ignored would otherwise stand for the evening. The read that
 * follows the command judges it - generously, because the point is to catch a
 * command ignored outright, not a lamp's own rounding. */
static void test_a_colour_within_tolerance_is_left_alone(void)
{
    ok(!color_off_target(ZBC_CMD_CT, 362, 0, ZBC_COLOR_PART_CT, 362), "the colour it was told");
    ok(!color_off_target(ZBC_CMD_CT, 362, 0, ZBC_COLOR_PART_CT, 380), "a lamp's own rounding");
    ok(!color_off_target(ZBC_CMD_CT, 362, 0, ZBC_COLOR_PART_CT, 344), "and the other way");
    ok(!color_off_target(ZBC_CMD_HS, 100, 200, ZBC_COLOR_PART_HUE, 106), "a hue near enough");
    ok(!color_off_target(ZBC_CMD_HS, 100, 200, ZBC_COLOR_PART_SAT, 195), "a saturation near enough");
}

/* The case this was built for: a warm-off parks the lamp at its warmest (500
 * mireds), the switch-on tells it the curve's colour, and a Hue still dark when
 * that frame lands ignores it. */
static void test_the_warm_off_colour_left_behind_is_caught(void)
{
    ok(color_off_target(ZBC_CMD_CT, 362, 0, ZBC_COLOR_PART_CT, 500), "still at its warmest");
    ok(color_off_target(ZBC_CMD_HS, 100, 200, ZBC_COLOR_PART_HUE, 140), "a hue it never took");
    ok(color_off_target(ZBC_CMD_HS, 100, 200, ZBC_COLOR_PART_SAT, 40), "a saturation it never took");
}

/* Hue is a circle of 254: 2 and 252 are four apart, not two hundred and fifty. */
static void test_hue_is_judged_the_short_way_round(void)
{
    ok(!color_off_target(ZBC_CMD_HS, 2, 200, ZBC_COLOR_PART_HUE, 252), "across zero, near enough");
    ok(!color_off_target(ZBC_CMD_HS, 252, 200, ZBC_COLOR_PART_HUE, 2), "and from the other side");
    ok(color_off_target(ZBC_CMD_HS, 2, 200, ZBC_COLOR_PART_HUE, 127), "the far side of the circle");
}

/* Only the part matching the command is judged. After MoveToColorTemperature a
 * lamp is in colour-temperature mode and its CurrentHue is whatever it last
 * was; judging that would correct a colour nobody asked for. XY has nothing to
 * judge at all - CurrentX and CurrentY are not in the settle read. */
static void test_only_the_part_the_command_set_is_judged(void)
{
    ok(!color_off_target(ZBC_CMD_CT, 362, 0, ZBC_COLOR_PART_HUE, 250), "a stale hue after a CT command");
    ok(!color_off_target(ZBC_CMD_CT, 362, 0, ZBC_COLOR_PART_SAT, 250), "a stale saturation too");
    ok(!color_off_target(ZBC_CMD_HS, 100, 200, ZBC_COLOR_PART_CT, 500), "stale mireds after a hue command");
    ok(!color_off_target(ZBC_CMD_XY, 30000, 30000, ZBC_COLOR_PART_CT, 500), "an xy command judges nothing");
    ok(!color_off_target(ZBC_CMD_NONE, 0, 0, ZBC_COLOR_PART_CT, 500), "and neither does no command");
}

/* The colour intent lives on the level command's slot, and the two budgets are
 * separate: a curve turns one request into both, and one must not spend the
 * other's tries. A command that is not a colour command clears it, so a read is
 * never judged against the colour before last. */
static void test_a_colour_intent_is_kept_apart_from_the_level_one(void)
{
    reset();
    const zbc_cmd_t ct = { .kind = ZBC_CMD_CT, .a = 362 };
    schedule_settle_read(0x3045, 11, 0, 8, 1, 200);          /* the level command */
    schedule_settle_read_ex(0x3045, 11, 0, 8, ZBC_WANT_KEEP, 0, &ct);
    int s = slot_of(0x3045, 11);
    ok(s_settle[s].want_on == 1 && s_settle[s].want_level == 200, "the level intent survives");
    ok(s_settle[s].want_color == ZBC_CMD_CT && s_settle[s].want_color_a == 362,
       "and the colour is recorded beside it");
    ok(s_settle[s].color_fixes_left == 2 && s_settle[s].fixes_left == 2,
       "each with its own budget");

    schedule_settle_read(0x3045, 11, 0, 8, 1, 120);          /* a plain level command */
    s = slot_of(0x3045, 11);
    ok(s_settle[s].want_color == ZBC_CMD_NONE && s_settle[s].color_fixes_left == 0,
       "a command that is not a colour clears the colour intent");
}

/* A device that has stopped answering gets a probe, never a correction - the
 * same rule the level intent follows. */
static void test_a_probe_records_no_colour_to_correct(void)
{
    reset();
    s_gone = true;
    const zbc_cmd_t ct = { .kind = ZBC_CMD_CT, .a = 362 };
    schedule_settle_read_ex(0x3045, 11, 0, 8, ZBC_WANT_KEEP, 0, &ct);
    const int s = slot_of(0x3045, 11);
    ok(s_settle[s].color_fixes_left == 0, "a probe gets no colour correction budget");
    s_gone = false;
}

int main(void)
{
    test_a_probe_is_read_but_never_corrected();
    test_a_second_command_does_not_cut_the_first_short();
    test_each_slot_is_read_at_its_own_deadline();
    test_a_healthy_lamp_settles_and_stops();
    test_a_delivered_command_is_not_resent_on_silence();
    test_an_undelivered_command_is_resent_on_silence();
    test_the_first_pass_never_resends();
    test_a_resend_is_not_judged_by_a_read_sent_beside_it();
    test_a_group_fanout_reads_every_member();
    test_a_report_after_the_command_skips_the_first_read();
    test_want_keep_preserves_a_live_intent();
    test_want_keep_does_not_revive_a_finished_intent();
    test_locking_is_balanced();
    test_slots_are_recycled();
    test_a_quiet_lamp_is_asked_again();
    test_recovery_drops_the_colour_intent();
    test_recovery_stops_once_the_lamp_answers();
    test_recovery_is_bounded();
    test_reads_start_again_when_a_held_command_goes_out();
    test_the_restarted_window_is_the_transition();
    test_a_finished_slot_is_not_revived();
    test_the_restart_clears_the_confirmation();
    test_a_colour_within_tolerance_is_left_alone();
    test_the_warm_off_colour_left_behind_is_caught();
    test_hue_is_judged_the_short_way_round();
    test_only_the_part_the_command_set_is_judged();
    test_a_colour_intent_is_kept_apart_from_the_level_one();
    test_a_probe_records_no_colour_to_correct();
    printf("all %d checks passed\n", s_checks);
    (void)TAG;
    return 0;
}
