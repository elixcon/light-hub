/* Which lamp is left at its warmest when it is switched off, from
 * main/lighting.c.
 *
 * Lifted rather than copied: see test/Makefile. Every fault here is invisible
 * from the coordinator - nothing reads the result back from a dark lamp.
 * Warmest and coolest are the two ends of the same axis and mireds run the
 * opposite way to kelvin, so taking the wrong limit leaves every lamp at its
 * coldest, the opposite of what the feature is for. A command sent to a lamp
 * that never reported its limits is refused by the lamp, if it is lucky, and
 * obeyed with a guessed value if it is not. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ZBC_UNKNOWN        (-1)
#define ZBC_FEAT_ONOFF      0x01
#define ZBC_FEAT_LEVEL      0x02
#define ZBC_FEAT_COLOR_TEMP 0x04
#define ZBC_FEAT_COLOR_HS   0x08
#define CT_DEADBAND_K       40

/* Only the fields the region reads; it takes them by name, so a mutation that
 * reached for ct_min_mireds - the coolest - would not compile against this. */
typedef struct {
    uint16_t features;
    int32_t  ct_mireds;       /* where the lamp is now */
    int32_t  ct_min_mireds;
    int32_t  ct_max_mireds;
} zbc_endpoint_t;

#include "prewarm.inc"

static int failures;
static int checks;

/* ct_min is the coolest end, and is always the smaller of the two: a lamp that
 * runs 2700-6500 K reports 153 and 370. Given both, the warm end must come
 * back, whatever the setting or the features say. */
/* `now` is where the lamp is sitting; ZBC_UNKNOWN for a lamp that has never
 * said. 153 mireds (6535 K) stands for "cold", so every case that is not about
 * the skip has something worth warming. */
static void expect3(const char *what, bool enabled, bool lit, uint16_t features, int32_t ct_max,
                    int32_t now, uint16_t want)
{
    checks++;
    const zbc_endpoint_t e = { .features = features, .ct_mireds = now,
                               .ct_min_mireds = 153, .ct_max_mireds = ct_max };
    const uint16_t got = prewarm_mireds(enabled, lit, &e);
    if (got != want) {
        printf("\n  %s: -> %u mireds, expected %u", what, got, want);
        failures++;
    }
}

static void expect2(const char *what, bool enabled, uint16_t features, int32_t ct_max,
                    int32_t now, uint16_t want)
{
    expect3(what, enabled, true, features, ct_max, now, want);
}

static void expect(const char *what, bool enabled, uint16_t features, int32_t ct_max,
                   uint16_t want)
{
    expect2(what, enabled, features, ct_max, 153, want);
}

int main(void)
{
    const uint16_t lamp   = ZBC_FEAT_ONOFF | ZBC_FEAT_LEVEL | ZBC_FEAT_COLOR_TEMP;
    const uint16_t dimmer = ZBC_FEAT_ONOFF | ZBC_FEAT_LEVEL;
    const uint16_t relay  = ZBC_FEAT_ONOFF;
    const uint16_t rgb    = ZBC_FEAT_ONOFF | ZBC_FEAT_LEVEL | ZBC_FEAT_COLOR_HS;

    /* The setting is what turns this on at all. */
    expect("setting off", false, lamp, 370, 0);

    /* The warmest the lamp says it can go, not the coolest, and not a guess.
     * 370 mireds is 2700 K, 500 is 2000 K - the OSRAM PAR16 and a lamp that
     * goes warmer keep their own ends of the range. */
    expect("tunable white, 2700 K floor", true, lamp, 370, 370);
    expect("tunable white, 2000 K floor", true, lamp, 500, 500);

    /* Nothing to send: no colour temperature to set. A relay and a plain dimmer
     * would answer UNSUPPORTED_ATTRIBUTE, an RGB lamp without the temperature
     * bit would take a colour command it was never meant to have. */
    expect("plain dimmer", true, dimmer, 370, 0);
    expect("relay",        true, relay,  370, 0);
    expect("colour only",  true, rgb,    370, 0);

    /* The cool end is never the answer, however plausible it looks beside it. */
    expect("both ends known", true, lamp, 370, 370);

    /* The limits never arrived - a lamp that has not answered
     * ColorTempPhysicalMaxMireds has no warmest to be put at. */
    expect("limits unknown", true, lamp, ZBC_UNKNOWN, 0);
    expect("limits zero",    true, lamp, 0,           0);

    /* A garbled limit must not wrap on the way into a uint16_t. */
    expect("limit past 16 bits", true, lamp, 70000, 0);
    expect("limit at the edge",  true, lamp, 65535, 65535);

    /* Already warm: a curve has taken the lamp to the bottom of its path as it
     * dimmed, so there is nothing to do and nothing to see. The deadband is the
     * same 40 K the colour path uses everywhere else. */
    expect2("sitting at its warmest",   true, lamp, 500, 500, 0);
    expect2("a hair off warmest",       true, lamp, 500, 495, 0);       /* 2020 K vs 2000 K */
    expect2("well short of warmest",    true, lamp, 500, 370, 500);     /* 2700 K vs 2000 K */
    expect2("stone cold",               true, lamp, 500, 153, 500);
    expect2("never said where it is",   true, lamp, 500, ZBC_UNKNOWN, 500);

    /* Only a lit lamp. The dim in front of the warmth is a move-to-level with
     * on/off, so a lamp that is already dark - or on its way there after an off
     * a moment ago - is lit at 1 % and switched off again: the flash a second
     * room off three seconds after the first gave the one lamp in the room that
     * reports by itself, whose colour had not been re-read since and still said
     * 3000 K. Cold or not, a dark lamp gets nothing. */
    expect3("dark, and cold",             true, false, lamp, 500, 331, 0);
    expect3("dark, colour never read",    true, false, lamp, 500, ZBC_UNKNOWN, 0);
    expect3("lit, and cold",              true, true,  lamp, 500, 331, 500);

    if (failures) {
        printf("\n%d of %d checks failed\n", failures, checks);
    } else {
        printf("all %d checks passed\n", checks);
    }
    return failures ? 1 : 0;
}
