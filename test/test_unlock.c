/* Host test for the press-5-times failsafe in main/buttons.c.
 *
 * The failsafe is the one way back into a coordinator whose keys are lost, so
 * both of its failures are expensive: a rhythm it refuses to recognise leaves
 * somebody locked out of their own lights, and a pattern it recognises too
 * easily - a hold, a bouncing contact, a Hue button's stream of Steps - opens
 * the web UI to the whole network. Neither shows in a serial log.
 *
 * unlock.inc is extracted from main/buttons.c by the Makefile, never copied.
 *
 *   make -C test
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* From buttons.h - the only thing the region is written against. */
#define BTN_DEBOUNCE_US (30 * 1000)

#include "unlock.inc"

static int failures;
static int checks;
/* How many hotspot restarts the last run_pin() asked for. Its return value is
 * the count of plain unlocks. */
static int hotspots;

#define MS 1000LL

/* A pin wired to GND (pressed = low), sampled every 10 ms as poll_cb does.
 * starts_ms[i] is when press i begins; each is held for hold_ms. With `bounce`
 * the contact chatters for the first 20 ms of every press and every release. */
static int run_pin(const int64_t *starts_ms, int n, int hold_ms, bool bounce)
{
    zbc_unlock_fsm_t st;
    memset(&st, 0, sizeof(st));
    int unlocks = 0;
    hotspots = 0;
    const int64_t end = starts_ms[n - 1] + hold_ms + 3000;
    for (int64_t t = 0; t <= end; t += 10) {
        bool pressed = false;
        for (int i = 0; i < n; i++) {
            const int64_t a = starts_ms[i], b = starts_ms[i] + hold_ms;
            if (t >= a && t < b) {
                pressed = true;
            }
            if (bounce) {
                if (t >= a && t < a + 20) {
                    pressed = ((t - a) / 10) % 2 == 0;
                }
                if (t >= b && t < b + 20) {
                    pressed = ((t - b) / 10) % 2 == 1;
                }
            }
        }
        const zbc_unlock_t u = unlock_level(&st, pressed ? 0 : 1, true, t * MS);
        if (u == UNLOCK_OPEN) {
            unlocks++;
        } else if (u == UNLOCK_HOTSPOT) {
            hotspots++;
        }
    }
    return unlocks;
}

/* Presses every `gap_ms`, starting a second in. */
static int even(int count, int gap_ms, int hold_ms, bool bounce)
{
    int64_t starts[32];
    for (int i = 0; i < count; i++) {
        starts[i] = 1000 + (int64_t)i * gap_ms;
    }
    return run_pin(starts, count, hold_ms, bounce);
}

static void expect(const char *what, int got, int want)
{
    checks++;
    if (got != want) {
        printf("\n  %s: %d unlock(s), expected %d", what, got, want);
        failures++;
    }
}

int main(void)
{
    /* The gesture itself, at a comfortable pace. */
    expect("five presses a second apart", even(5, 1000, 150, false), 1);
    expect("four presses are not enough", even(4, 1000, 150, false), 0);
    expect("a slow finger held most of each gap", even(5, 1400, 1200, false), 1);

    /* The rhythm's edges. Debounce delays every edge by the same 30 ms, so the
     * gap between them is the gap between the presses. */
    expect("gaps of exactly 500 ms", even(5, 500, 150, false), 1);
    expect("gaps of exactly 2500 ms", even(5, 2500, 150, false), 1);
    expect("gaps of 400 ms (too quick)", even(5, 400, 150, false), 0);
    expect("gaps of 490 ms (just too quick)", even(5, 490, 150, false), 0);
    expect("gaps of 3000 ms (too slow)", even(5, 3000, 150, false), 0);
    expect("gaps of 2510 ms (just too slow)", even(5, 2510, 150, false), 0);
    /* What a person pressing "about once a second" actually does: the reason
     * the limit is 2.5 s rather than 1.5 s. */
    {
        int64_t starts[] = { 1000, 3050, 5000, 7060, 9020, 10940 };
        expect("the measured human rhythm (1.92-2.06 s)", run_pin(starts, 6, 150, false), 1);
    }
    expect("a fast double click, five times", even(10, 250, 80, false), 0);

    /* Things that must never count as presses. */
    expect("one long hold", even(1, 1000, 8000, false), 0);
    expect("a bouncing contact counts each press once", even(5, 1000, 150, true), 1);
    {
        /* 10 ms spikes, a second apart: shorter than the debounce. */
        int64_t starts[] = { 1000, 2000, 3000, 4000, 5000 };
        expect("spikes shorter than the debounce", run_pin(starts, 5, 10, false), 0);
    }

    /* A stumble starts a new series rather than spoiling the next five. */
    {
        int64_t starts[] = { 1000, 2000, 3000, 4000, 7000, 8000, 9000, 10000, 11000 };
        expect("four, a pause, then five", run_pin(starts, 9, 150, false), 1);
    }
    {
        int64_t starts[] = { 1000, 2000, 3000, 4000, 7000, 8000, 9000, 10000 };
        expect("four, a pause, then only four", run_pin(starts, 8, 150, false), 0);
    }
    {
        /* 2200 is too soon after 2000, so it is the first of a new five. */
        int64_t starts[] = { 1000, 2000, 2200, 3200, 4200, 5200, 6200 };
        expect("a bump on the second press, then five in rhythm", run_pin(starts, 7, 100, false), 1);
    }

    /* The series runs on past five to the hotspot at ten, and starts over after
     * that - so the sixth to ninth press complete nothing. */
    expect("nine presses: the unlock", even(9, 1000, 150, false), 1);
    expect("nine presses: no hotspot", hotspots, 0);
    expect("ten presses: one unlock on the way", even(10, 1000, 150, false), 1);
    expect("ten presses: the hotspot", hotspots, 1);
    expect("twenty presses: two unlocks", even(20, 1000, 150, false), 2);
    expect("twenty presses: two hotspots", hotspots, 2);
    expect("ten at the measured human pace: unlock", even(10, 2000, 150, false), 1);
    expect("ten at the measured human pace: hotspot", hotspots, 1);
    {
        /* A stumble after the unlock is a new series: the web UI is open, and
         * getting to the hotspot takes ten more. */
        int64_t starts[] = { 1000, 2000, 3000, 4000, 5000, 6000, 7000, 11000, 12000, 13000, 14000 };
        expect("five, two more, a pause, four: one unlock", run_pin(starts, 11, 150, false), 1);
        expect("five, two more, a pause, four: no hotspot", hotspots, 0);
    }
    expect("ten presses too slowly", even(10, 2600, 150, false), 0);
    expect("ten presses too slowly: no hotspot", hotspots, 0);
    {
        /* The tenth press has to keep the rhythm too - a pause before it is a
         * stumble like any other, and the hotspot takes a full ten again. */
        int64_t starts[] = { 1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000 };
        int64_t more[10];
        memcpy(more, starts, sizeof(starts));
        more[9] = 9000 + 4000;
        run_pin(more, 10, 150, false);
        expect("nine in rhythm, the tenth four seconds late", hotspots, 0);
    }

    /* A remote's clicks, and exactly which one completes what. */
    {
        zbc_unlock_fsm_t st;
        memset(&st, 0, sizeof(st));
        int open_at = -1, hotspot_at = -1, events = 0;
        for (int i = 0; i < 10; i++) {
            const zbc_unlock_t u = unlock_tap(&st, 5000 * MS + (int64_t)i * 1000 * MS);
            if (u == UNLOCK_OPEN) { open_at = i + 1; events++; }
            if (u == UNLOCK_HOTSPOT) { hotspot_at = i + 1; events++; }
        }
        expect("remote: the unlock fires on press", open_at, 5);
        expect("remote: the hotspot fires on press", hotspot_at, 10);
        expect("remote: exactly two events in ten presses", events, 2);
    }

    /* A remote's clicks, which arrive already decoded. */
    {
        zbc_unlock_fsm_t st;
        memset(&st, 0, sizeof(st));
        int hits = 0, at = -1;
        for (int i = 0; i < 5; i++) {
            if (unlock_tap(&st, 5000 * MS + (int64_t)i * 800 * MS)) {
                hits++;
                at = i;
            }
        }
        expect("five remote clicks 800 ms apart", hits, 1);
        checks++;
        if (at != 4) {
            printf("\n  the remote unlock fired on click %d, expected the fifth", at + 1);
            failures++;
        }
    }
    {
        zbc_unlock_fsm_t st;
        memset(&st, 0, sizeof(st));
        int hits = 0;
        for (int i = 0; i < 12; i++) {
            hits += unlock_tap(&st, 5000 * MS + (int64_t)i * 300 * MS) ? 1 : 0;
        }
        expect("twelve remote clicks 300 ms apart", hits, 0);
    }
    {
        /* The very first click ever must not be measured against time zero: a
         * press 0.5-2.5 s after boot is still only the first press. */
        zbc_unlock_fsm_t st;
        memset(&st, 0, sizeof(st));
        int hits = 0;
        for (int i = 0; i < 4; i++) {
            hits += unlock_tap(&st, (int64_t)(800 + i * 1000) * MS) ? 1 : 0;
        }
        expect("four clicks starting just after boot", hits, 0);
    }

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("all %d checks passed\n", checks);
    return 0;
}
