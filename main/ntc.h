/* An NTC thermistor on an ADC pin, and the overheat decision made from it.
 *
 * A leaf module in the sense sun.c is: math.h and nothing else, so
 * test/test_ntc.c compiles it whole with `cc`. thermal.c does the sampling and
 * the switching; everything worth getting exactly right - the divider, the
 * B-parameter equation, when a reading counts as broken, and when the lights
 * are held or let go - is here.
 *
 * The wiring is a voltage divider across 3V3 with a fixed resistor. A converted
 * Shelly Gen3/Gen4 has the NTC between the pin and GND and 10 kOhm to 3V3 -
 * ESPHome calls that `configuration: DOWNSTREAM`.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t b;            /* B constant, kelvin: 3350 on a Shelly */
    uint32_t r0_ohm;       /* resistance at 25 °C */
    uint32_t rs_ohm;       /* the fixed resistor of the divider */
    bool     ntc_to_3v3;   /* false: NTC between the pin and GND (ESPHome DOWNSTREAM) */
} zbc_ntc_cfg_t;

/* The plausible range. A reading outside it is a broken sensor, not a
 * temperature: an open NTC wired to GND reads well below -30 °C once the ADC
 * saturates short of 3V3, and a shorted one far above 150 °C. */
#define ZBC_NTC_MIN_DECI_C   (-300)
#define ZBC_NTC_MAX_DECI_C   1500

/* The pin's voltage as a temperature in tenths of a degree. False for a
 * reading at either rail or outside the plausible range; *deci_c is then left
 * alone. */
bool zbc_ntc_celsius(uint32_t mv, uint32_t vref_mv, const zbc_ntc_cfg_t *cfg, int16_t *deci_c);

/* The middle value of five, so one noisy sample does not move the reading. */
int32_t zbc_median5(const int32_t v[5]);

/* ------------------------------------------------------------ the decision */

typedef enum {
    ZBC_THERMAL_OK = 0,
    ZBC_THERMAL_HOT,        /* at or above the shutoff, or not yet cooled below the release */
    ZBC_THERMAL_FAULT,      /* the sensor reads as broken: treated as too hot */
} zbc_thermal_state_t;

/* Cooler than the shutoff by this much before the lights may be switched on again. */
#define ZBC_THERMAL_RELEASE_DECI_C 50
/* Consecutive broken readings before a fault, and good ones to clear it: one
 * ADC glitch must not cut the lights. The very first reading after start is
 * believed at once - a board that boots with a dead sensor never lights up. */
#define ZBC_THERMAL_FAULT_AFTER    3

typedef struct {
    uint8_t  state;        /* zbc_thermal_state_t */
    uint8_t  bad;          /* consecutive broken readings */
    uint8_t  good;         /* consecutive good readings while in FAULT */
    bool     have;         /* a good reading has been seen */
    int16_t  deci_c;       /* last good reading */
    int16_t  max_deci_c;   /* highest good reading */
    uint32_t trips;        /* times the lights were held off */
} zbc_thermal_t;

void zbc_thermal_init(zbc_thermal_t *st);

/* One reading. `valid` false is a broken one (deci_c ignored). Returns true
 * when the state changed. HOT holds until the reading is at or below
 * trip - ZBC_THERMAL_RELEASE_DECI_C; a FAULT cleared by good readings goes to
 * HOT rather than OK while it is still that warm. */
bool zbc_thermal_step(zbc_thermal_t *st, bool valid, int16_t deci_c, int16_t trip_deci_c);

#ifdef __cplusplus
}
#endif
