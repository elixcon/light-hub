/* Host test for main/ntc.c: the NTC divider, the B-parameter equation, and the
 * overheat decision that holds the wired lights off.
 *
 * ntc.c is a leaf - math.h and nothing else - so it is compiled whole, like
 * sun.c. Faults here are the kind nobody sees until the day it matters: a
 * divider read the wrong way round reports a cold board as it heats up, and a
 * hysteresis off by one lets a relay chatter at the shutoff.
 *
 *   make -C test
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "ntc.h"

static int s_checks;
static void ok(bool cond, const char *what)
{
    s_checks++;
    if (!cond) { printf("FAIL: %s\n", what); exit(1); }
}

/* A converted Shelly Gen3/Gen4: B 3350, 10 kOhm at 25 °C, 10 kOhm to 3V3. */
static const zbc_ntc_cfg_t SHELLY = { .b = 3350, .r0_ohm = 10000, .rs_ohm = 10000, .ntc_to_3v3 = false };

/* The pin voltage an NTC at `c` gives, worked out independently of ntc.c. */
static uint32_t mv_at(double c, const zbc_ntc_cfg_t *cfg)
{
    const double r = cfg->r0_ohm * exp(cfg->b * (1.0 / (c + 273.15) - 1.0 / 298.15));
    const double v = cfg->ntc_to_3v3 ? 3300.0 * cfg->rs_ohm / (cfg->rs_ohm + r)
                                     : 3300.0 * r / (cfg->rs_ohm + r);
    return (uint32_t)lround(v);
}

static void test_conversion(void)
{
    int16_t d = 0;
    ok(zbc_ntc_celsius(1650, 3300, &SHELLY, &d) && d == 250, "R = R0 reads 25.0 °C");

    /* 75 °C is 1991 Ohm, 548 mV with the NTC to GND: hotter reads lower. */
    ok(mv_at(75, &SHELLY) == 548, "reference: 75 °C is 548 mV");
    ok(zbc_ntc_celsius(548, 3300, &SHELLY, &d) && d >= 748 && d <= 752, "548 mV reads 75 °C");
    int16_t hot = 0, cold = 0;
    zbc_ntc_celsius(500, 3300, &SHELLY, &hot);
    zbc_ntc_celsius(2000, 3300, &SHELLY, &cold);
    ok(hot > cold, "to GND: a lower voltage is hotter");

    zbc_ntc_cfg_t up = SHELLY;
    up.ntc_to_3v3 = true;
    ok(zbc_ntc_celsius(mv_at(75, &up), 3300, &up, &d) && d >= 748 && d <= 752, "to 3V3: 75 °C");
    zbc_ntc_celsius(500, 3300, &up, &hot);
    zbc_ntc_celsius(2000, 3300, &up, &cold);
    ok(hot < cold, "to 3V3: a higher voltage is hotter");

    for (int c = -20; c <= 140; c += 5) {
        ok(zbc_ntc_celsius(mv_at(c, &SHELLY), 3300, &SHELLY, &d) && abs(d - c * 10) <= 5, "round trip, to GND");
        ok(zbc_ntc_celsius(mv_at(c, &up), 3300, &up, &d) && abs(d - c * 10) <= 5, "round trip, to 3V3");
    }

    d = 1234;
    ok(!zbc_ntc_celsius(0, 3300, &SHELLY, &d), "0 mV is a shorted NTC");
    ok(!zbc_ntc_celsius(3300, 3300, &SHELLY, &d), "vref is an open NTC");
    ok(!zbc_ntc_celsius(3100, 3300, &SHELLY, &d), "an ADC saturated at 3.1 V is not a temperature");
    ok(!zbc_ntc_celsius(20, 3300, &SHELLY, &d), "20 mV is far past 150 °C");
    ok(d == 1234, "a broken reading leaves the value alone");

    const zbc_ntc_cfg_t none = { 0 };
    ok(!zbc_ntc_celsius(1650, 3300, &none, &d), "a zero configuration converts nothing");
}

static void test_median(void)
{
    ok(zbc_median5((int32_t[]){ 5, 1, 4, 2, 3 }) == 3, "median of a shuffle");
    ok(zbc_median5((int32_t[]){ 1650, 1651, 4095, 1649, 1650 }) == 1650, "one spike does not move it");
    ok(zbc_median5((int32_t[]){ 0, 0, 1650, 1650, 1650 }) == 1650, "two dropouts do not either");
}

static bool step(zbc_thermal_t *st, int deci)
{
    return zbc_thermal_step(st, true, (int16_t)deci, 750);
}

static void test_decision(void)
{
    zbc_thermal_t st;
    zbc_thermal_init(&st);
    ok(!step(&st, 400) && st.state == ZBC_THERMAL_OK, "40 °C is fine");
    ok(!step(&st, 749) && st.state == ZBC_THERMAL_OK, "74.9 °C is fine");
    ok(step(&st, 750) && st.state == ZBC_THERMAL_HOT, "75.0 °C trips");
    ok(st.trips == 1, "one trip counted");
    ok(!step(&st, 740) && st.state == ZBC_THERMAL_HOT, "74.0 °C is still held");
    ok(!step(&st, 710) && st.state == ZBC_THERMAL_HOT, "71.0 °C (4 °C cooler) is still held");
    ok(!step(&st, 701) && st.state == ZBC_THERMAL_HOT, "70.1 °C is still held");
    ok(step(&st, 700) && st.state == ZBC_THERMAL_OK, "70.0 °C (5 °C cooler) lets go");
    ok(!step(&st, 740) && st.state == ZBC_THERMAL_OK, "warming again below the shutoff does not hold");
    ok(step(&st, 800) && st.trips == 2 && st.max_deci_c == 800, "a second trip, and the highest reading");

    /* A broken first reading is believed at once. */
    zbc_thermal_init(&st);
    ok(zbc_thermal_step(&st, false, 0, 750) && st.state == ZBC_THERMAL_FAULT, "broken at start: fault at once");
    ok(st.trips == 1, "a fault counts as a trip");

    /* Later, one glitch is not a fault; three are. */
    zbc_thermal_init(&st);
    step(&st, 400);
    ok(!zbc_thermal_step(&st, false, 0, 750) && st.state == ZBC_THERMAL_OK, "one broken reading: still fine");
    ok(!zbc_thermal_step(&st, false, 0, 750) && st.state == ZBC_THERMAL_OK, "two: still fine");
    step(&st, 400);
    ok(!zbc_thermal_step(&st, false, 0, 750) && !zbc_thermal_step(&st, false, 0, 750),
       "a good reading in between starts the count again");
    ok(zbc_thermal_step(&st, false, 0, 750) && st.state == ZBC_THERMAL_FAULT, "three in a row: fault");
    ok(st.deci_c == 400, "the last good reading is kept");

    /* Clearing a fault takes three good readings, and a warm board stays held. */
    ok(!step(&st, 720) && !step(&st, 720) && st.state == ZBC_THERMAL_FAULT, "two good readings: still a fault");
    ok(!zbc_thermal_step(&st, false, 0, 750) && !step(&st, 720) && !step(&st, 720) && st.state == ZBC_THERMAL_FAULT,
       "a broken reading in between starts the clearing count again");
    ok(step(&st, 720) && st.state == ZBC_THERMAL_HOT, "cleared while warm: held as hot");
    ok(step(&st, 690) && st.state == ZBC_THERMAL_OK, "then released once cool");

    zbc_thermal_init(&st);
    zbc_thermal_step(&st, false, 0, 750);
    step(&st, 300); step(&st, 300);
    ok(step(&st, 300) && st.state == ZBC_THERMAL_OK, "cleared while cool: fine at once");
}

int main(void)
{
    test_conversion();
    test_median();
    test_decision();
    printf("all %d checks passed\n", s_checks);
    return 0;
}
