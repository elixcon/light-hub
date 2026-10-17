/* When a switch-only group member gets an On/Off, from main/lighting.c.
 *
 * Lifted rather than copied: see test/Makefile. The fault this guards against
 * is invisible from outside - a relay that is sent an On on every step of a
 * slider drag still ends up on, and only the airtime says otherwise; a relay
 * switched by hand that is never put right looks like a lost command. */
#include <stdio.h>
#include <stdbool.h>

#define ZBC_UNKNOWN (-1)

#include "switchpoint.inc"

static int failures;
static int checks;

static void expect(const char *what, bool want_on, int sent_off, int reported, bool want)
{
    checks++;
    bool got = switch_should_send(want_on, sent_off, reported);
    if (got != want) {
        printf("\n  %s: want_on %d, sent_off %d, reported %d -> %s, expected %s",
               what, want_on, sent_off, reported, got ? "send" : "hold", want ? "send" : "hold");
        failures++;
    }
}

/* A drag across the switch point, as a train of requests against a latch that
 * is updated the way set_switch_point updates it. The relay's report arrives a
 * few steps after each command, which is what happens on air. */
static void drag(const char *what, const int *pcts, int n, int threshold, int want_sends)
{
    int sent_off = ZBC_UNKNOWN, reported = 0, sends = 0, pending = -1, age = 0;
    for (int i = 0; i < n; i++) {
        bool want_on = pcts[i] >= threshold;
        if (switch_should_send(want_on, sent_off, reported)) {
            sends++;
            if (pending != (int)want_on) {       /* a repeat does not delay the report */
                pending = want_on;
                age = 0;
            }
        }
        sent_off = want_on ? 0 : 1;
        if (pending >= 0 && ++age >= 3) {        /* the report lands three steps later */
            reported = pending;
            pending = -1;
        }
    }
    checks++;
    if (sends > want_sends) {
        printf("\n  %s: %d commands, expected at most %d", what, sends, want_sends);
        failures++;
    }
}

int main(void)
{
    /* Nothing sent yet: always send, whichever way. */
    expect("unknown latch, on",  true,  ZBC_UNKNOWN, ZBC_UNKNOWN, true);
    expect("unknown latch, off", false, ZBC_UNKNOWN, ZBC_UNKNOWN, true);

    /* Settled and agreeing: nothing to say. */
    expect("on and reported on",   true,  0, 1, false);
    expect("off and reported off", false, 1, 0, false);
    expect("on, report unknown",   true,  0, ZBC_UNKNOWN, false);

    /* Crossing the point. The report must not be what carries these: before
     * any report has arrived, and while the last one still shows what the latch
     * already agrees with, it is the latch alone that says a command is due. */
    expect("crossing up",   true,  1, 0, true);
    expect("crossing down", false, 0, 1, true);
    expect("crossing up, no report yet",   true,  1, ZBC_UNKNOWN, true);
    expect("crossing down, no report yet", false, 0, ZBC_UNKNOWN, true);
    expect("crossing up, report already on",    true,  1, 1, true);
    expect("crossing down, report already off", false, 0, 0, true);

    /* Switched by hand behind our back. */
    expect("relay turned off by hand", true,  0, 0, true);
    expect("relay turned on by hand",  false, 1, 1, true);

    /* A slow drag up from 20 % to 60 % and back, point at 40 %: two crossings.
     * While the report is on its way each step repeats the command - three steps
     * of lag, so three per crossing, plus the first send against an unknown
     * latch - and never one per step for the length of the drag. */
    int up_down[82];
    int n = 0;
    for (int p = 20; p <= 60; p++) up_down[n++] = p;
    for (int p = 60; p >= 20; p--) up_down[n++] = p;
    drag("drag across 40 %", up_down, n, 40, 1 + 2 * 3);

    /* Starting against a relay reported off: the same lag, and then silence. */
    int above[40];
    for (int i = 0; i < 40; i++) above[i] = 60 + i;
    drag("drag above the point", above, 40, 40, 3);

    if (failures) {
        printf("\n%d of %d checks failed\n", failures, checks);
    } else {
        printf("all %d checks passed\n", checks);
    }
    return failures ? 1 : 0;
}
