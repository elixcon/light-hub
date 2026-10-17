/* Host test for main/sun.c, the sun model Adaptive Lighting runs on.
 *
 * sun.c is a leaf - math.h and nothing else - so it is compiled whole, the way
 * remote.c is, rather than lifted out of a bigger file.
 *
 * What is worth checking is what nobody would notice on hardware for months:
 * a sign error in the equation of time moves noon by up to half an hour at the
 * two times of year it peaks, and a night phase with the wrong sign makes the
 * room brighten after sunset. Both look like a matter of taste rather than a
 * fault.
 *
 *   make -C test
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "sun.h"

static int s_checks;
static void ok(bool cond, const char *what)
{
    s_checks++;
    if (!cond) { printf("FAIL: %s\n", what); exit(1); }
}

/* 00:00 UTC on a date, without leaning on the host's timegm. */
static int64_t utc(int y, int m, int d, int hh, int mm)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + doe - 719468) * 86400 + hh * 3600 + mm * 60;
}

static bool near_min(int64_t got, int64_t want, int tol_min)
{
    return llabs(got - want) <= tol_min * 60;
}

static void report(const char *what, int64_t got, int64_t want)
{
    if (!near_min(got, want, 3)) {
        printf("  %s: got %+.1f min from the reference\n", what, (got - want) / 60.0);
    }
}

/* Published sunrise and sunset for two cities, one either side of the equator,
 * each at both solstices. Three minutes of tolerance: the references round to
 * the minute and the simplified algorithm is good to about one. */
static void test_reference_cities(void)
{
    zbc_sun_day_t d;
    /* Munich 48.137 N, 11.575 E. */
    ok(zbc_sun_day(utc(2025, 6, 21, 12, 0), 48.137, 11.575, &d), "Munich June evaluates");
    report("Munich June sunrise", d.rise, utc(2025, 6, 21, 3, 12));
    report("Munich June sunset",  d.set,  utc(2025, 6, 21, 19, 17));
    ok(d.polar == 0 && near_min(d.rise, utc(2025, 6, 21, 3, 12), 3), "Munich June sunrise 03:12 UTC");
    ok(near_min(d.set, utc(2025, 6, 21, 19, 17), 3), "Munich June sunset 19:17 UTC");

    ok(zbc_sun_day(utc(2025, 12, 21, 12, 0), 48.137, 11.575, &d), "Munich December evaluates");
    report("Munich December sunrise", d.rise, utc(2025, 12, 21, 7, 2));
    report("Munich December sunset",  d.set,  utc(2025, 12, 21, 15, 22));
    ok(near_min(d.rise, utc(2025, 12, 21, 7, 2), 3), "Munich December sunrise 07:02 UTC");
    ok(near_min(d.set, utc(2025, 12, 21, 15, 22), 3), "Munich December sunset 15:22 UTC");

    /* Sydney 33.869 S, 151.209 E: its June day straddles midnight UTC, which is
     * exactly what picking the nearest solar noon has to get right. */
    ok(zbc_sun_day(utc(2025, 6, 21, 2, 0), -33.869, 151.209, &d), "Sydney June evaluates");
    report("Sydney June sunrise", d.rise, utc(2025, 6, 20, 21, 0));
    report("Sydney June sunset",  d.set,  utc(2025, 6, 21, 6, 54));
    ok(near_min(d.rise, utc(2025, 6, 20, 21, 0), 3), "Sydney June sunrise 21:00 UTC the day before");
    ok(near_min(d.set, utc(2025, 6, 21, 6, 54), 3), "Sydney June sunset 06:54 UTC");
}

/* The equation of time peaks at about +16.4 min in early November and bottoms
 * out at about -14.2 min in mid February, so solar noon on the prime meridian
 * is about 11:44 and 12:14 UTC. Flip the sign and both land half an hour out. */
static void test_equation_of_time(void)
{
    zbc_sun_day_t d;
    zbc_sun_day(utc(2025, 11, 3, 12, 0), 0.0, 0.0, &d);
    ok(near_min(d.noon, utc(2025, 11, 3, 11, 44), 2), "noon on the meridian is 11:44 UTC on 3 November");
    zbc_sun_day(utc(2025, 2, 11, 12, 0), 0.0, 0.0, &d);
    ok(near_min(d.noon, utc(2025, 2, 11, 12, 14), 2), "noon on the meridian is 12:14 UTC on 11 February");

    /* Longitude alone moves noon, four minutes a degree. */
    zbc_sun_day_t east;
    zbc_sun_day(utc(2025, 2, 11, 12, 0), 0.0, 15.0, &east);
    ok(near_min(d.noon - east.noon, 3600, 1), "fifteen degrees east is an hour earlier");
}

static void test_polar(void)
{
    zbc_sun_day_t d;
    zbc_sun_day(utc(2025, 6, 21, 12, 0), 78.0, 15.0, &d);
    ok(d.polar == 1, "78 N in June is a polar day");
    ok(zbc_sun_phase(utc(2025, 6, 21, 23, 0), &d) == 1.0, "a polar day is noon all day");
    zbc_sun_day(utc(2025, 12, 21, 12, 0), 78.0, 15.0, &d);
    ok(d.polar == -1, "78 N in December is a polar night");
    ok(zbc_sun_phase(utc(2025, 12, 21, 11, 0), &d) == -1.0, "a polar night is midnight all day");
    zbc_sun_day(utc(2025, 12, 21, 12, 0), -78.0, 15.0, &d);
    ok(d.polar == 1, "78 S in December is a polar day");

    ok(!zbc_sun_day(0, 91.0, 0.0, &d), "a latitude past the pole is refused");
    ok(!zbc_sun_day(0, 0.0, 181.0, &d), "a longitude past the antimeridian is refused");
}

/* Walk a whole day a minute at a time. The phase has to hit its marks exactly,
 * stay in range, never jump, and rise only between sunrise and noon - a night
 * phase with the wrong sign is the room brightening after dark. */
static void test_phase_over_a_day(double lat, double lon, int64_t start)
{
    zbc_sun_day_t d;
    zbc_sun_day(start + 43200, lat, lon, &d);
    ok(d.polar == 0, "the walked day has a sunrise and a sunset");
    ok(fabs(zbc_sun_phase(d.noon, &d) - 1.0) < 1e-9, "phase is exactly 1 at solar noon");
    ok(fabs(zbc_sun_phase(d.rise, &d)) < 1e-9, "phase is 0 at sunrise");
    ok(fabs(zbc_sun_phase(d.set, &d)) < 1e-9, "phase is 0 at sunset");
    ok(fabs(zbc_sun_phase(d.noon + 43200, &d) + 1.0) < 1e-9, "phase is exactly -1 at the next solar midnight");
    ok(fabs(zbc_sun_phase(d.noon - 43200, &d) + 1.0) < 1e-9, "phase is exactly -1 at the previous solar midnight");
    ok(zbc_sun_phase(d.set + 3600, &d) < 0.0, "an hour after sunset is night");
    ok(zbc_sun_phase(d.rise - 3600, &d) < 0.0, "an hour before sunrise is night");

    /* Seeded from the day `start` belongs to, not the one walked above. */
    zbc_sun_day_t first;
    zbc_sun_day(start, lat, lon, &first);
    double prev = zbc_sun_phase(start, &first);
    for (int64_t t = start + 60; t < start + 2 * 86400; t += 60) {
        zbc_sun_day_t now;
        zbc_sun_day(t, lat, lon, &now);
        const double p = zbc_sun_phase(t, &now);
        ok(p >= -1.0 && p <= 1.0, "phase stays within -1..1");
        /* The fastest change is a half cosine over the shortest stretch; a
         * minute of it is well under 0.02. The day handing over at solar
         * midnight must not add a jump either. */
        ok(fabs(p - prev) < 0.02, "phase never jumps from one minute to the next");
        /* Both samples inside the stretch: a pair straddling noon compares the
         * two sides of the peak, which are a rounded second apart. */
        if (t - 60 > now.rise && t < now.noon) {
            ok(p >= prev - 1e-12, "phase rises from sunrise to noon");
        }
        if (t - 60 > now.noon && t < now.set) {
            ok(p <= prev + 1e-12, "phase falls from noon to sunset");
        }
        prev = p;
    }
}

static void test_mix(void)
{
    ok(zbc_al_mix(1.0, -1500, 500) == 500, "noon gives the noon value");
    ok(zbc_al_mix(-1.0, -1500, 500) == -1500, "midnight gives the midnight value");
    ok(zbc_al_mix(0.0, -1500, 500) == -500, "sunrise and sunset land halfway");
    ok(zbc_al_mix(0.0, 20, 100) == 60, "brightness mixes the same way");
    ok(zbc_al_mix(2.0, 20, 100) == 100 && zbc_al_mix(-2.0, 20, 100) == 20, "a phase out of range is clamped");
}

static const int16_t NONE[2] = { -1, -1 };

/* The sunrise and sunset limits: "the day starts no later than 06:30 and ends
 * no earlier than 21:00". Munich on 10 October 2026, still on summer time:
 * sunrise about 07:25 CEST, sunset about 18:40, noon about 13:03. */
static void test_stretch(void)
{
    const double  lat = 48.137, lon = 11.575;
    const int32_t cest = 7200, cet = 3600;
    zbc_sun_day_t real, d;
    zbc_sun_day(utc(2026, 10, 10, 12, 0), lat, lon, &real);
    ok(real.polar == 0 && near_min(real.rise, utc(2026, 10, 10, 5, 25), 5), "Munich in October rises about 05:25 UTC");

    d = real;
    zbc_sun_stretch(&d, cest, NONE, NONE);
    ok(d.rise == real.rise && d.set == real.set && d.noon == real.noon, "no limits leave the day alone");

    const int16_t rise_latest[2]  = { -1, 6 * 60 + 30 };
    const int16_t set_earliest[2] = { 21 * 60, -1 };
    d = real;
    zbc_sun_stretch(&d, cest, rise_latest, set_earliest);
    ok(d.rise == utc(2026, 10, 10, 4, 30), "sunrise no later than 06:30 CEST is 04:30 UTC");
    ok(d.set == utc(2026, 10, 10, 19, 0), "sunset no earlier than 21:00 CEST is 19:00 UTC");
    ok(d.noon == real.noon, "noon does not move");

    d = real;
    zbc_sun_stretch(&d, cet, rise_latest, set_earliest);
    ok(d.set == utc(2026, 10, 10, 20, 0), "the same evening limit an hour later in UTC on winter time");
    ok(d.rise == real.rise, "and 06:30 CET is after the sun's 05:25 UTC, so the morning is the sun's");

    /* The phase the room gets: brighter in the evening and at 06:00 local with
     * the limits than without - the whole point. The half cosine is flat around
     * sunset, so the gain shows most a couple of hours on. */
    zbc_sun_day_t lim = real;
    zbc_sun_stretch(&lim, cest, rise_latest, set_earliest);
    ok(zbc_sun_phase(utc(2026, 10, 10, 18, 0), &lim) > zbc_sun_phase(utc(2026, 10, 10, 18, 0), &real) + 0.1,
       "20:00 is brighter with the evening stretched");
    ok(zbc_sun_phase(utc(2026, 10, 10, 20, 0), &lim) > zbc_sun_phase(utc(2026, 10, 10, 20, 0), &real) + 0.3,
       "22:00 is well brighter with the evening stretched");
    ok(zbc_sun_phase(utc(2026, 10, 10, 18, 0), &lim) > 0.0, "20:00 is still day before a 21:00 sunset");
    ok(zbc_sun_phase(utc(2026, 10, 10, 4, 0), &lim) > zbc_sun_phase(utc(2026, 10, 10, 4, 0), &real),
       "06:00 is brighter with the morning stretched");
    ok(fabs(zbc_sun_phase(lim.set, &lim)) < 1e-9 && fabs(zbc_sun_phase(lim.rise, &lim)) < 1e-9,
       "the moved sunrise and sunset are where the phase crosses 0");

    /* Windows the sun already keeps to change nothing. */
    const int16_t rise_wide[2] = { 4 * 60, 9 * 60 };
    const int16_t set_wide[2]  = { 17 * 60, 22 * 60 };
    d = real;
    zbc_sun_stretch(&d, cest, rise_wide, set_wide);
    ok(d.rise == real.rise && d.set == real.set, "windows around the sun's own times change nothing");

    /* The other bound of each: a later sunrise, an earlier sunset. */
    const int16_t rise_earliest[2] = { 8 * 60, -1 };
    const int16_t set_latest[2]    = { -1, 18 * 60 };
    d = real;
    zbc_sun_stretch(&d, cest, rise_earliest, set_latest);
    ok(d.rise == utc(2026, 10, 10, 6, 0) && d.set == utc(2026, 10, 10, 16, 0), "earliest sunrise, latest sunset");

    /* June: the winter limit is already kept, a summer one is not. */
    zbc_sun_day_t june;
    zbc_sun_day(utc(2026, 6, 21, 12, 0), lat, lon, &june);
    d = june;
    zbc_sun_stretch(&d, cest, rise_latest, set_earliest);
    ok(d.rise == june.rise, "in June the sun is up before 06:30 anyway");
    ok(d.set == june.set, "and sets after 21:00 anyway");
    const int16_t rise_not_before[2] = { 6 * 60, 6 * 60 + 30 };
    d = june;
    zbc_sun_stretch(&d, cest, rise_not_before, NONE);
    ok(d.rise == utc(2026, 6, 21, 4, 0), "a summer morning held back to 06:00");

    /* Limits that would reach noon or a solar midnight stop half an hour short. */
    const int16_t rise_midnight[2] = { -1, 0 };
    const int16_t rise_evening[2]  = { 23 * 60, -1 };
    const int16_t set_noon[2]      = { -1, 12 * 60 };
    d = real;
    zbc_sun_stretch(&d, cest, rise_midnight, set_noon);
    ok(d.rise == real.noon - 43200 + 1800, "a sunrise at midnight stops half an hour after solar midnight");
    ok(d.set == real.noon + 1800, "a sunset before noon stops half an hour after it");
    d = real;
    zbc_sun_stretch(&d, cest, rise_evening, NONE);
    ok(d.rise == real.noon - 1800, "a sunrise after noon stops half an hour before it");

    /* A day shorter than the margins - near the polar circle - is the sun's own
     * while no limit moves it: the margins are for a moved sunrise or sunset. */
    const zbc_sun_day_t brief = { .rise = real.noon - 600, .noon = real.noon, .set = real.noon + 600, .polar = 0 };
    d = brief;
    zbc_sun_stretch(&d, cest, NONE, NONE);
    ok(d.rise == brief.rise && d.set == brief.set, "a twenty-minute day nobody limited is left alone");
    const int16_t rise_noonish[2] = { 12 * 60, 13 * 60 };   /* its sunrise is 12:53 CEST */
    d = brief;
    zbc_sun_stretch(&d, cest, rise_noonish, NONE);
    ok(d.rise == brief.rise, "and so is one whose limits it already keeps");

    zbc_sun_day_t polar;
    zbc_sun_day(utc(2026, 12, 21, 12, 0), 78.0, 15.0, &polar);
    d = polar;
    zbc_sun_stretch(&d, cet, rise_latest, set_earliest);
    ok(d.polar == -1 && d.rise == polar.rise && d.set == polar.set, "a polar night is not stretched");

    /* Two days a minute at a time with the limits on: the phase must not jump
     * where a stretched evening hands over to the next morning. */
    const int64_t start = utc(2026, 10, 9, 0, 0);
    double        prev  = 0.0;
    for (int64_t t = start; t < start + 2 * 86400; t += 60) {
        zbc_sun_day_t now;
        zbc_sun_day(t, lat, lon, &now);
        zbc_sun_stretch(&now, cest, rise_latest, set_earliest);
        const double p = zbc_sun_phase(t, &now);
        ok(p >= -1.0 && p <= 1.0, "a stretched phase stays within -1..1");
        if (t > start) {
            ok(fabs(p - prev) < 0.02, "a stretched phase never jumps from one minute to the next");
        }
        prev = p;
    }
}

int main(void)
{
    test_reference_cities();
    test_equation_of_time();
    test_polar();
    test_phase_over_a_day(48.137, 11.575, utc(2025, 3, 20, 0, 0));
    test_phase_over_a_day(-33.869, 151.209, utc(2025, 6, 20, 0, 0));
    test_phase_over_a_day(64.1, -21.9, utc(2025, 6, 1, 0, 0));
    test_mix();
    test_stretch();
    printf("ok (%d checks)\n", s_checks);
    return 0;
}
