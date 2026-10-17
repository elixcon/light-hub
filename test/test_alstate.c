/* Host test for Adaptive Lighting's decisions in main/lighting.c: whether a
 * group's mode is doing anything, what it does to a lamp's colour, and whether a
 * minute's drift is worth a command.
 *
 * Every fault here looks like a matter of taste on hardware: a room whose lights
 * jump to the sun's brightness while nobody has a clock to go by; a lamp whose hue
 * swings every minute because an offset was added to a colour that is not a
 * temperature; a colour command a minute for a change nobody can see.
 *
 * alstate.inc is extracted from main/lighting.c by the Makefile, never copied.
 *
 *   make -C test
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "light_math.h"
#include "sun.h"

#define ZBC_AL_SHAPE_POINTS 4   /* al_db.h, which needs ESP-IDF */

#include "pchip.inc"
#include "alstate.inc"

static int s_checks;
static void ok(bool cond, const char *what)
{
    s_checks++;
    if (!cond) { printf("FAIL: %s\n", what); exit(1); }
}

static void test_when_the_mode_does_anything(void)
{
    ok(al_reason(false, true, true) == AL_OFF, "a mode nobody started is off");
    ok(al_reason(false, false, false) == AL_OFF, "whatever the clock and the location say");
    ok(al_reason(true, true, true) == AL_ON, "a started mode with a clock and a place runs");
    /* The two gates. Skipping either sends the lights to a sun worked out from
     * 1970, or from a place of 0 N 0 E. */
    ok(al_reason(true, false, true) == AL_NO_CLOCK, "without a clock it waits");
    ok(al_reason(true, true, false) == AL_NO_LOCATION, "without a location it waits");
    ok(al_reason(true, false, false) == AL_NO_CLOCK, "the clock is reported first");
    ok(!al_reason_active(AL_NO_CLOCK) && !al_reason_active(AL_NO_LOCATION) && !al_reason_active(AL_OFF),
       "waiting sends nothing");
    ok(al_reason_active(AL_ON), "running sends");
}

static void test_the_offset(void)
{
    /* A warmth curve's own kelvin is shifted. */
    ok(al_shifted_kelvin(true, false, 3000, true, 4000, -1200) == 1800, "a colour temperature output is shifted");
    ok(al_shifted_kelvin(true, false, 3000, true, 4000, 500) == 3500, "and shifted cooler at noon");
    /* No curve: the neutral base stands in for one. */
    ok(al_shifted_kelvin(false, true, 0, true, 4000, -1500) == 2500, "no colour output: shifted from the base");
    ok(al_shifted_kelvin(false, true, 0, false, 4000, -1500) == 0, "a lamp without colour temperature is left alone");
    ok(al_shifted_kelvin(true, false, 3000, false, 4000, -1500) == 0, "even when its curve names a temperature");
    /* A hue, or an xy blend, is neither of those. */
    ok(al_shifted_kelvin(false, false, 0, true, 4000, -1500) == 0, "a colour output is never shifted");
    ok(al_shifted_kelvin(true, false, 3000, true, 4000, AL_NO_OFFSET) == 0, "no offset: nothing to shift");
    ok(al_shifted_kelvin(true, false, 1800, true, 4000, -3000) == 1000, "a large offset is floored, not negative");
    ok(al_shifted_kelvin(true, false, 19000, true, 4000, 3000) == 20000, "and capped at the other end");
}

static void test_ticks(void)
{
    ok(al_needs_send(60, -1, -500, AL_NO_OFFSET), "the first tick always sends");
    ok(!al_needs_send(60, 60, -500, -500), "nothing moved: nothing sent");
    ok(!al_needs_send(60, 60, -539, -500), "39 K is under the deadband");
    ok(al_needs_send(60, 60, -540, -500), "40 K is not");
    ok(al_needs_send(60, 60, -460, -500), "the deadband works both ways");
    ok(al_needs_send(61, 60, -500, -500), "any brightness step sends");
}

/* The x a phase lands on: 1 at midnight, 50.5 at sunrise and sunset, 100 at noon. */
static double phase_of(double x)
{
    return (x - 1.0) / 99.0 * 2.0 - 1.0;
}

/* The day's shape. Without points it has to be the two-value profile to the
 * unit, at every phase: a box updated with nobody watching must do what it did. */
static void test_the_shape(void)
{
    bool same = true;
    for (int i = -1000; i <= 1000; i++) {
        const double ph = i / 1000.0;
        same = same && al_shape(ph, 20, 100, 0, NULL, NULL) == zbc_al_mix(ph, 20, 100) &&
               al_shape(ph, -1500, 500, 0, NULL, NULL) == zbc_al_mix(ph, -1500, 500);
    }
    ok(same, "no points is zbc_al_mix exactly, brightness and colour alike");
    /* Every profile a page can set, not just the defaults: the straight line
     * must round as the old formula did wherever it lands on a half. */
    for (int mid = 1; mid <= 100 && same; mid += 3) {
        for (int noon = 1; noon <= 100 && same; noon += 1) {
            for (int i = -2000; i <= 2000 && same; i++) {
                same = al_shape(i / 2000.0, mid, noon, 0, NULL, NULL) == zbc_al_mix(i / 2000.0, mid, noon);
            }
        }
    }
    for (int mid = -3000; mid <= 3000 && same; mid += 50) {
        for (int noon = -3000; noon <= 3000 && same; noon += 250) {
            for (int i = -2000; i <= 2000 && same; i++) {
                same = al_shape(i / 2000.0, mid, noon, 0, NULL, NULL) == zbc_al_mix(i / 2000.0, mid, noon);
            }
        }
    }
    ok(same, "and for every brightness pair and a grid of colour shifts");

    /* "Bright until sunset": 90 % at sunset, full from mid-afternoon. */
    const uint8_t xs[] = { 50, 75 };
    const int16_t ys[] = { 90, 100 };
    ok(al_shape(-1.0, 20, 100, 2, xs, ys) == 20, "midnight is the midnight value");
    ok(al_shape(1.0, 20, 100, 2, xs, ys) == 100, "noon is the noon value");
    ok(al_shape(phase_of(50), 20, 100, 2, xs, ys) == 90, "the curve passes through a point");
    ok(al_shape(phase_of(75), 20, 100, 2, xs, ys) == 100, "and through the next");
    ok(al_shape(phase_of(90), 20, 100, 2, xs, ys) == 100, "a flat top stays flat - the monotone cubic does not overshoot");
    ok(al_shape(-2.0, 20, 100, 2, xs, ys) == 20 && al_shape(2.0, 20, 100, 2, xs, ys) == 100,
       "a phase out of range is clamped");
    ok(al_shape(0.0, 20, 100, 2, xs, ys) > al_shape(0.0, 20, 100, 0, NULL, NULL) + 25,
       "sunset is well brighter than the straight line's 60 %");

    int  prev = -1;
    bool rising = true, inside = true;
    for (int i = -1000; i <= 1000; i++) {
        const int v = al_shape(i / 1000.0, 20, 100, 2, xs, ys);
        rising = rising && v >= prev;
        inside = inside && v >= 20 && v <= 100;
        prev   = v;
    }
    ok(rising, "rising points make a value that only rises from midnight to noon");
    ok(inside, "and it never leaves the range of its points");

    /* "Warm evenings": most of the night's warmth already at sunset, where the
     * brightness curve can still be high - which no two-value profile can do. */
    const uint8_t kx[] = { 50 };
    const int16_t ky[] = { -1300 };
    ok(al_shape(phase_of(50), -1500, 500, 1, kx, ky) == -1300, "a colour shift passes through its point");
    ok(al_shape(-1.0, -1500, 500, 1, kx, ky) == -1500 && al_shape(1.0, -1500, 500, 1, kx, ky) == 500,
       "and keeps its ends");

    const uint8_t many_x[] = { 10, 20, 30, 40, 50, 60 };
    const int16_t many_y[] = { 30, 40, 50, 60, 70, 80 };
    ok(al_shape(phase_of(40), 20, 100, 6, many_x, many_y) == 60, "more points than allowed are cut to the limit");
}

int main(void)
{
    test_when_the_mode_does_anything();
    test_the_offset();
    test_ticks();
    test_the_shape();
    printf("ok (%d checks)\n", s_checks);
    return 0;
}
