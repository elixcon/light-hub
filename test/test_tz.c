/* Host test for main/tz.c, the POSIX TZ rules Adaptive Lighting's sunrise and
 * sunset limits are read in.
 *
 * tz.c is a leaf - the C library and nothing else - so it is compiled whole,
 * the way sun.c is.
 *
 * The rules below are exactly what web/src/lib/tz.ts writes for these zones
 * (web/test/tz.test.ts asserts the same strings), so the two ends are checked
 * against each other through them. What a fault looks like on the board: the
 * evening limit an hour out for half the year, starting the night the clocks
 * change - which reads as "it got dim early again" rather than as a bug.
 *
 *   make -C test
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "tz.h"

static int s_checks;
static void ok(bool cond, const char *what)
{
    s_checks++;
    if (!cond) { printf("FAIL: %s\n", what); exit(1); }
}

static int64_t utc(int y, int m, int d, int hh, int mm)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + doe - 719468) * 86400 + hh * 3600 + mm * 60;
}

/* The offset a second before and at a transition. */
static void around(const char *tz, int64_t at, int32_t before, int32_t after, const char *what)
{
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: %d before", what, (int)before);
    ok(zbc_tz_offset(tz, at - 1) == before, msg);
    snprintf(msg, sizeof(msg), "%s: %d from", what, (int)after);
    ok(zbc_tz_offset(tz, at) == after, msg);
}

#define BERLIN  "STD-1DST-2,M3.5.0/2,M10.5.0/3"
#define NEWYORK "STD5DST4,M3.2.0/2,M11.1.0/2"
#define SYDNEY  "STD-10DST-11,M10.1.0/2,M4.1.0/3"
#define KOLKATA "STD-5:30"
#define UTC0    "STD0"

static void test_zones(void)
{
    /* Last Sunday of March and of October, 01:00 UTC both. */
    around(BERLIN, utc(2026, 3, 29, 1, 0), 3600, 7200, "Berlin into summer time 2026");
    around(BERLIN, utc(2026, 10, 25, 1, 0), 7200, 3600, "Berlin back 2026");
    around(BERLIN, utc(2027, 3, 28, 1, 0), 3600, 7200, "Berlin into summer time 2027");
    around(BERLIN, utc(2027, 10, 31, 1, 0), 7200, 3600, "Berlin back 2027 (a fifth Sunday)");
    ok(zbc_tz_offset(BERLIN, utc(2026, 1, 1, 0, 0)) == 3600, "Berlin on new year's day is CET");
    ok(zbc_tz_offset(BERLIN, utc(2026, 7, 1, 12, 0)) == 7200, "Berlin in July is CEST");

    /* Second Sunday of March and first of November, 02:00 local. */
    around(NEWYORK, utc(2026, 3, 8, 7, 0), -18000, -14400, "New York into daylight time 2026");
    around(NEWYORK, utc(2026, 11, 1, 6, 0), -14400, -18000, "New York back 2026");

    /* The southern hemisphere: daylight time spans the new year. */
    around(SYDNEY, utc(2026, 4, 4, 16, 0), 39600, 36000, "Sydney back to standard time 2026");
    around(SYDNEY, utc(2026, 10, 3, 16, 0), 36000, 39600, "Sydney into daylight time 2026");
    ok(zbc_tz_offset(SYDNEY, utc(2026, 1, 1, 0, 0)) == 39600, "Sydney on new year's day is AEDT");
    ok(zbc_tz_offset(SYDNEY, utc(2026, 12, 31, 23, 0)) == 39600, "Sydney on new year's eve is AEDT");
    ok(zbc_tz_offset(SYDNEY, utc(2026, 7, 1, 0, 0)) == 36000, "Sydney in July is AEST");

    ok(zbc_tz_offset(KOLKATA, utc(2026, 7, 1, 0, 0)) == 19800, "Kolkata is +5:30 all year");
    ok(zbc_tz_offset(UTC0, utc(2026, 7, 1, 0, 0)) == 0, "UTC is 0");
}

static void test_forms(void)
{
    ok(zbc_tz_offset("CET-1CEST,M3.5.0,M10.5.0/3", utc(2026, 3, 29, 1, 0)) == 7200,
       "the default rule time is 02:00 and the default DST offset one hour more");
    ok(zbc_tz_offset("<+0330>-3:30", 0) == 12600, "a quoted name and minutes");
    ok(zbc_tz_offset("<-03>3", 0) == -10800, "a quoted name with a sign in it");
    ok(zbc_tz_valid("STD-1DST-2,M3.5.0/-1,M10.5.0/26"), "rule times outside the day are allowed");

    const char *bad[] = {
        NULL, "", "ST-1", "STD", "STD-1DST", "STD-1DST,M3.5.0", "STD-1DST,J60,J300",
        "STD-1DST,60,300", "STD-1DST,M13.5.0,M10.5.0", "STD-1DST,M3.6.0,M10.5.0",
        "STD-1DST,M3.5.7,M10.5.0", "STD-1DST,M3.5.0,M10.5.0x", "STD-25", "STD-1:60", "<>-1",
        "STD-1DST-2,M3.5.0/2,M10.5.0/3 ", "STD-1DST-2,M3.5.0/2,M10.5.0/3STD-1DST-2,M3.5.0/2,M10.5.0/3XYZW",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char msg[120];
        snprintf(msg, sizeof(msg), "refused: \"%s\"", bad[i] ? bad[i] : "(null)");
        ok(!zbc_tz_valid(bad[i]), msg);
        ok(zbc_tz_offset(bad[i], 0) == 0, "a refused rule reads as UTC");
    }
}

int main(void)
{
    test_zones();
    test_forms();
    printf("ok (%d checks)\n", s_checks);
    return 0;
}
