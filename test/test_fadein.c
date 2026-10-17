/* Whether a lamp being given a brightness has to be preset to the bottom of
 * its range first, from main/lighting.c.
 *
 * Lifted rather than copied: see test/Makefile. This decision is invisible from
 * the coordinator and visible from the sofa. Get it wrong one way and a lamp
 * told to go to 10 % after an evening at full flares to full and slides back
 * down over the fade - the light appears to overshoot and be corrected. Get it
 * wrong the other way and a slider drag dips the room to nothing on every step,
 * or a lamp that is already lit blinks down and back up. Neither leaves a trace
 * in any log. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ZBC_UNKNOWN    (-1)

#include "fadein.inc"

static int failures;
static int checks;

#define S 1000000LL   /* one second of esp_timer_get_time() */

static void expect(const char *what, bool fade, int reported_on, int level,
                   bool sent_off, int64_t sent_us, int64_t now_us, bool want)
{
    const bool got = zbc_should_fade_in(fade, reported_on, level, sent_off, sent_us, now_us);
    checks++;
    if (got != want) {
        failures++;
        printf("\n  %-38s expected %s, got %s", what, want ? "preset" : "plain",
               got ? "preset" : "plain");
    }
}

static void believe(const char *what, int reported_on, bool sent_off, int64_t sent_us,
                    int64_t now_us, int want)
{
    const int got = zbc_believed_on(reported_on, sent_off, sent_us, now_us);
    checks++;
    if (got != want) {
        failures++;
        printf("\n  %-38s expected %d, got %d", what, want, got);
    }
}

int main(void)
{
    const int64_t now = 100 * S;

    /* The group is switched off at full, and a button asks for 10 % a while
     * later. The lamp still holds level 254 - OffWithEffect keeps CurrentLevel -
     * so without the preset it lights at full and ramps down. */
    expect("off at full, asked for 10 %", true, 0, 254, true, 90 * S, now, true);

    /* And the same the other way round: no distance to cover at all, so the
     * lamp would snap on. */
    expect("off at full, asked for full", true, 0, 254, true, 90 * S, now, true);

    /* A lit lamp is never preset: that is a dip in full view. It holds even
     * when we were the last to send an off - either that off was lost or
     * something else has switched the lamp on since, and either way the lamp's
     * own word is the one to go by. */
    expect("lit, dimming down",          true, 1, 254, false, 90 * S,      now, false);
    expect("lit although we sent an off", true, 1, 254, true, 90 * S, now, false);

    /* Instant means instant: there is no ramp to give somewhere to start, and
     * the preset would be a frame for nothing. */
    expect("no fade set", false, 0, 254, true, 90 * S, now, false);

    /* Nothing to preset from. A lamp already at the bottom costs a command for
     * no change, and one that has never reported a level might be anywhere. */
    expect("already at the floor",  true, 0, 1,           true, 90 * S, now, false);
    expect("below the floor",       true, 0, 0,           true, 90 * S, now, false);
    expect("level never reported",  true, 0, ZBC_UNKNOWN, true, 90 * S, now, false);

    /* Parked by a warm off, which dims to PARKED_LEVEL before the colour and
     * the off: every warm-offed lamp is here, and the move alone lights it at
     * the bottom and ramps - the preset would be a round trip of nothing on
     * every switch-on. One step above it is a level somebody chose. */
    expect("parked by a warm off",  true, 0, PARKED_LEVEL,     true, 90 * S, now, false);
    expect("between floor and park", true, 0, PARKED_LEVEL - 1, true, 90 * S, now, false);
    expect("just above parked",     true, 0, PARKED_LEVEL + 1, true, 90 * S, now, true);

    /* Our own off is more recent news than a report that has not caught up: a
     * Hue reports off only once the 0.8 s effect has run, so an off followed
     * straight away by a brightness would otherwise be judged on a report still
     * saying "on". */
    expect("off 100 ms ago, report lagging", true, 1, 254, true, now - S / 10, now, false);
    expect("off 100 ms ago, no report yet",  true, ZBC_UNKNOWN, 254, true, now - S / 10, now, true);

    /* A slider drag over a lamp that is off: the first step presets, and every
     * step after it is plain, because the report of the first has not arrived
     * and the lag window says so. Without this the drag dips on every step. */
    expect("drag, first step",          true, 0, 254, true, 90 * S, now, true);
    expect("drag, 200 ms later",        true, 0, 254, false, now - S / 5,      now, false);
    expect("drag, 1.9 s later",         true, 0, 254, false, now - 19 * S / 10,  now, false);

    /* Past the window the lamp's own word is believed again: it says it is off
     * and nothing we sent explains that, so it really is dark - switched off at
     * the wall, or by another controller. */
    expect("off elsewhere, long after ours", true, 0, 254, false, now - 30 * S, now, true);

    /* Nothing has ever been sent to this endpoint and it reports off: after a
     * restart that is the whole of what is known, and it is enough. */
    expect("fresh boot, lamp reports off",     true, 0, 254, false, 0, now, true);
    expect("fresh boot, lamp reports on",      true, 1, 254, false, 0, now, false);
    expect("fresh boot, lamp has never said",  true, ZBC_UNKNOWN, 254, false, 0, now, false);

    /* What a press reads a lamp as. The case that brought it in: a room switched
     * off from Adaptive Lighting, and the same remote pressed 3 s later to bring
     * it up to 10 %. One lamp's report still said "on" - a Hue reports off only
     * after its effect, and one whose first settle read was skipped is not read
     * again until 3.7 s after the frame - so the press read the room as lit and
     * switched it off a second time. */
    believe("off 3 s ago, report still on",   1, true,  now - 3 * S,  now, 0);
    believe("off 3 s ago, report caught up",  0, true,  now - 3 * S,  now, 0);
    believe("off 5.9 s ago, report still on", 1, true,  now - 59 * S / 10, now, 0);

    /* And the other way: a room switched on a moment ago is on, though a lamp
     * that does not report has not been read yet. */
    believe("on 1 s ago, not read yet",       0, false, now - S,      now, 1);
    believe("on 1 s ago, never known",        ZBC_UNKNOWN, false, now - S, now, 1);

    /* Past the window the lamp's word stands, whatever we last told it: an off
     * that was lost and a lamp switched on at the wall both say "on", and both
     * are on. Not knowing stays not knowing. */
    believe("off long ago, lamp says on",     1, true,  now - 30 * S, now, 1);
    believe("on long ago, lamp says off",     0, false, now - 30 * S, now, 0);
    believe("on long ago, lamp gone quiet",   ZBC_UNKNOWN, false, now - 30 * S, now, ZBC_UNKNOWN);

    /* Nothing sent since the restart is no word of ours at all - including in
     * the first seconds after boot, when `now` is itself under the window. */
    believe("nothing sent, lamp says on",     1, false, 0, now, 1);
    believe("nothing sent, lamp says off",    0, false, 0, now, 0);
    believe("nothing sent, 2 s after boot",   0, false, 0, 2 * S, 0);

    if (failures) {
        printf("\n%d of %d checks failed\n", failures, checks);
    } else {
        printf("all %d checks passed\n", checks);
    }
    return failures ? 1 : 0;
}
