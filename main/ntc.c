/* See ntc.h. */
#include "ntc.h"

#include <math.h>

#define KELVIN_25C 298.15
#define ZERO_C_K   273.15

bool zbc_ntc_celsius(uint32_t mv, uint32_t vref_mv, const zbc_ntc_cfg_t *cfg, int16_t *deci_c)
{
    if (!cfg || cfg->b == 0 || cfg->r0_ohm == 0 || cfg->rs_ohm == 0 || mv == 0 || mv >= vref_mv) {
        return false;
    }
    /* Divider: the fixed resistor and the NTC share vref, and the pin sits
     * between them. With the NTC to GND the pin reads the NTC's share. */
    const double v  = (double)mv;
    const double vr = (double)vref_mv;
    const double r  = cfg->ntc_to_3v3 ? cfg->rs_ohm * (vr - v) / v
                                      : cfg->rs_ohm * v / (vr - v);
    /* The B-parameter equation: 1/T = 1/T0 + ln(R/R0)/B. */
    const double inv_t = 1.0 / KELVIN_25C + log(r / (double)cfg->r0_ohm) / (double)cfg->b;
    if (inv_t <= 0) {
        return false;
    }
    const double c = 1.0 / inv_t - ZERO_C_K;
    const long   d = lround(c * 10.0);
    if (d < ZBC_NTC_MIN_DECI_C || d > ZBC_NTC_MAX_DECI_C) {
        return false;
    }
    *deci_c = (int16_t)d;
    return true;
}

int32_t zbc_median5(const int32_t v[5])
{
    int32_t s[5];
    for (int i = 0; i < 5; i++) {
        s[i] = v[i];
    }
    for (int i = 1; i < 5; i++) {
        for (int j = i; j > 0 && s[j - 1] > s[j]; j--) {
            const int32_t t = s[j];
            s[j]     = s[j - 1];
            s[j - 1] = t;
        }
    }
    return s[2];
}

void zbc_thermal_init(zbc_thermal_t *st)
{
    *st = (zbc_thermal_t){
        .state      = ZBC_THERMAL_OK,
        /* One short of a fault, so a broken first reading is believed at once. */
        .bad        = ZBC_THERMAL_FAULT_AFTER - 1,
        .max_deci_c = INT16_MIN,
    };
}

bool zbc_thermal_step(zbc_thermal_t *st, bool valid, int16_t deci_c, int16_t trip_deci_c)
{
    const uint8_t was = st->state;

    if (!valid) {
        st->good = 0;
        if (st->bad < ZBC_THERMAL_FAULT_AFTER) {
            st->bad++;
        }
        if (st->bad >= ZBC_THERMAL_FAULT_AFTER) {
            st->state = ZBC_THERMAL_FAULT;
        }
    } else {
        st->bad    = 0;
        st->have   = true;
        st->deci_c = deci_c;
        if (deci_c > st->max_deci_c) {
            st->max_deci_c = deci_c;
        }
        const bool cool = deci_c <= trip_deci_c - ZBC_THERMAL_RELEASE_DECI_C;
        if (st->state == ZBC_THERMAL_FAULT) {
            if (++st->good >= ZBC_THERMAL_FAULT_AFTER) {
                st->good  = 0;
                st->state = cool ? ZBC_THERMAL_OK : ZBC_THERMAL_HOT;
            }
        } else if (deci_c >= trip_deci_c) {
            st->state = ZBC_THERMAL_HOT;
        } else if (st->state == ZBC_THERMAL_HOT && cool) {
            st->state = ZBC_THERMAL_OK;
        }
    }

    if (was == ZBC_THERMAL_OK && st->state != ZBC_THERMAL_OK) {
        st->trips++;
    }
    return st->state != was;
}
