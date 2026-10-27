#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/mcpwm_prelude.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "soc/ledc_periph.h"
#include "soc/soc_caps.h"

#include "buttons.h"
#include "light_db.h"
#include "local_lights.h"
#include "settings.h"
#include "state_rev.h"

static const char *TAG = "local_lights";

/* light_math.h plans against these numbers without knowing the chip; this is
 * where the chip is known. */
_Static_assert(ZBC_PWM_LEDC_CHANNELS <= SOC_LEDC_CHANNEL_NUM, "more LEDC channels planned than the chip has");
_Static_assert(ZBC_PWM_LEDC_TIMERS <= LEDC_TIMER_MAX, "more LEDC timers planned than the chip has");
_Static_assert(ZBC_PWM_RES_MAX <= SOC_LEDC_TIMER_BIT_WIDTH, "PWM resolution beyond the timer width");
_Static_assert(ZBC_PWM_MCPWM_UNITS <= SOC_MCPWM_TIMERS_PER_GROUP && ZBC_PWM_MCPWM_UNITS <= SOC_MCPWM_OPERATORS_PER_GROUP,
               "more MCPWM units planned than the chip has");
_Static_assert(SOC_MCPWM_GENERATORS_PER_OPERATOR == 2 && SOC_MCPWM_COMPARATORS_PER_OPERATOR >= 2,
               "an MCPWM unit is planned as two outputs with a comparator each");

#define LEDC_MODE        LEDC_LOW_SPEED_MODE
#define TICK_MS          10                     /* 100 Hz while something is fading */
#define FADE_LATE_US     (2 * TICK_MS * 1000)   /* a tick this far behind skipped one */
#define FADE_OFF_DS      8                      /* what OffWithEffect 0x00/0x00 does */
#define SAVE_AFTER_US    (10 * 1000000LL)       /* a slider drag is one flash write */

/* >>> host-test region: cwww <<< */
uint8_t zbc_pwm_resolution(uint32_t hz)
{
    if (hz == 0) {
        return 0;
    }
    const uint32_t ratio = ZBC_PWM_CLK_HZ / hz;
    uint8_t        res   = 0;
    while (res < ZBC_PWM_RES_MAX && (1u << (res + 1)) <= ratio) {
        res++;
    }
    return res;
}

uint32_t zbc_mcpwm_peak(uint32_t hz, uint32_t *divider)
{
    uint32_t div = 0, peak = 0;
    if (hz != 0) {
        /* The fewest ticks a count that fit, rounded: the frequency is then the
         * nearest the counts allow, within half a count (0.012 % at 4096). The
         * tick is the clock divided as the driver divides it. */
        do {
            div++;
            peak = (ZBC_PWM_MCPWM_CLK_HZ / div + hz) / (2 * hz);
        } while (peak > ZBC_PWM_MCPWM_PEAK_MAX && div < 256);
    }
    if (divider) {
        *divider = div;
    }
    return peak;
}

zbc_mcpwm_out_t zbc_mcpwm_out(uint32_t d, uint32_t peak, bool at_peak)
{
    if (d == 0) {
        return (zbc_mcpwm_out_t){ .force = 0, .cmp = 0 };
    }
    if (d >= peak) {
        return (zbc_mcpwm_out_t){ .force = 1, .cmp = 0 };
    }
    /* High for 2d ticks of the 2 * peak in a period: d either side of the zero,
     * or of the peak. */
    return (zbc_mcpwm_out_t){ .force = -1, .cmp = at_peak ? peak - d : d };
}

/* The light a level gives, 0-1. CIE 1931: level is lightness L* (0-100), and
 * the light output it takes is the inverse of the L* curve. A linear duty would
 * spend the whole bottom third of the range looking nearly full. */
static double level_light(int64_t level_milli)
{
    if (level_milli <= 0) {
        return 0.0;
    }
    const double l = (double)level_milli / 2540.0;
    return l <= 8.0 ? l / 903.2963 : pow((l + 16.0) / 116.0, 3.0);
}

uint32_t zbc_level_to_duty(uint32_t level_milli, uint32_t max)
{
    if (level_milli == 0) {
        return 0;
    }
    if (level_milli >= 254000u) {
        return max;
    }
    const double y = level_light(level_milli);
    uint32_t     d = (uint32_t)(y * (double)max + 0.5);
    if (d == 0) {
        d = 1;                    /* lit is lit, however dim */
    }
    return d > max ? max : d;
}

void zbc_cwww_mix(uint32_t duty, uint32_t mireds, uint32_t cold_m, uint32_t warm_m,
                  uint8_t mix, uint32_t *dc, uint32_t *dw)
{
    /* How cold, as the fraction num/den: 1 at the cold white's own temperature,
     * 0 at the warm white's. Integer, so the ends are exact. */
    uint64_t num = 1, den = 2;
    if (warm_m > cold_m) {
        const uint32_t m = mireds < cold_m ? cold_m : (mireds > warm_m ? warm_m : mireds);
        num = warm_m - m;
        den = warm_m - cold_m;
    }
    if (mix == ZBC_MIX_FULL) {
        const uint64_t c = (2 * (uint64_t)duty * num + den / 2) / den;
        const uint64_t w = (2 * (uint64_t)duty * (den - num) + den / 2) / den;
        *dc = (uint32_t)(c > duty ? duty : c);
        *dw = (uint32_t)(w > duty ? duty : w);
    } else {
        *dc = (uint32_t)(((uint64_t)duty * num + den / 2) / den);
        *dw = duty - *dc;         /* exactly the brightness, not a rounding either way */
    }
}

void zbc_mix3(uint32_t duty, uint32_t mireds, uint32_t cold_m, uint32_t neutral_m, uint32_t warm_m,
              uint8_t mix, uint32_t out[3])
{
    const uint8_t  how = mix == ZBC_MIX_FULL ? ZBC_MIX_FULL : ZBC_MIX_CONSTANT;
    const uint32_t m   = mireds < cold_m ? cold_m : (mireds > warm_m ? warm_m : mireds);
    /* Between the two LEDs either side, as a two-channel light between its own:
     * at the neutral LED's own temperature both sides give it alone, so the
     * light does not jump as the colour temperature crosses it. */
    if (m <= neutral_m) {
        zbc_cwww_mix(duty, m, cold_m, neutral_m, how, &out[ZBC_CH_COLD], &out[ZBC_CH_NEUTRAL]);
        out[ZBC_CH_WARM] = 0;
    } else {
        zbc_cwww_mix(duty, m, neutral_m, warm_m, how, &out[ZBC_CH_NEUTRAL], &out[ZBC_CH_WARM]);
        out[ZBC_CH_COLD] = 0;
    }
}

bool zbc_mix_curve_valid(const zbc_mix_curve_t *c)
{
    if (c->n == 0) {
        return true;
    }
    if (c->n < 2 || c->n > ZBC_MIX_MAX_POINTS || c->x[0] != 1 || c->x[c->n - 1] != 100) {
        return false;
    }
    for (int i = 0; i < c->n; i++) {
        if (c->y[i] > 100 || (i > 0 && c->x[i] <= c->x[i - 1])) {
            return false;
        }
    }
    return true;
}

float zbc_mix_position(uint32_t mireds, uint32_t cold_m, uint32_t warm_m)
{
    if (warm_m <= cold_m) {
        return 50.5f;
    }
    const uint32_t m = mireds < cold_m ? cold_m : (mireds > warm_m ? warm_m : mireds);
    return 1.0f + 99.0f * (float)(warm_m - m) / (float)(warm_m - cold_m);
}

void zbc_mix_custom(uint32_t duty, uint32_t mireds, uint32_t cold_m, uint32_t warm_m,
                    const zbc_mix_curve_t curves[], int nch, uint32_t out[])
{
    const float at = zbc_mix_position(mireds, cold_m, warm_m);
    for (int i = 0; i < nch; i++) {
        const zbc_mix_curve_t *c = &curves[i];
        if (c->n < 2) {
            out[i] = 0;               /* no curve: this channel is not part of the mix */
            continue;
        }
        float ys[ZBC_MIX_MAX_POINTS];
        for (int k = 0; k < c->n; k++) {
            ys[k] = (float)c->y[k];
        }
        float pct = zbc_pchip(c->x, ys, c->n, at);
        pct       = pct < 0.0f ? 0.0f : (pct > 100.0f ? 100.0f : pct);
        const uint32_t d = (uint32_t)((float)duty * pct / 100.0f + 0.5f);
        out[i] = d > duty ? duty : d;
    }
}

/* A pulse of d counts centred on c2/2 (twice the centre, so a centre between two
 * counts is exact), on the circle of one period. */
static zbc_pwm_out_t place_pulse(uint32_t d, uint32_t c2, uint32_t max)
{
    if (d == 0) {
        return (zbc_pwm_out_t){ .hpoint = 0, .duty = 0, .invert = false };
    }
    if (d >= max) {
        /* Flat, in the form the pulses just below it take: a change of form costs
         * one wrong period (apply_pwm_locked), and must not come at full. Dark
         * needs no such care - a one-count pulse always fits. */
        const zbc_pwm_out_t below = place_pulse(max - 1, c2, max);
        return below.invert ? (zbc_pwm_out_t){ .hpoint = below.hpoint, .duty = 0, .invert = true }
                            : (zbc_pwm_out_t){ .hpoint = 0, .duty = max, .invert = false };
    }
    const uint32_t start = (uint32_t)((((uint64_t)c2 + 4 * (uint64_t)max - d) / 2) % max);
    if (start + d <= max) {
        return (zbc_pwm_out_t){ .hpoint = start, .duty = d, .invert = false };
    }
    /* It would run past the end of the period. A high interval that wraps is the
     * inverse of the low interval between its two halves, and that one does not
     * wrap - so the channel is inverted and given the gap instead. LEDC is never
     * asked for hpoint + duty > max. */
    return (zbc_pwm_out_t){ .hpoint = start + d - max, .duty = max - d, .invert = true };
}

/* a / b rounded towards minus infinity, b > 0. */
static int64_t floor_div(int64_t a, int64_t b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

void zbc_pwm_place_n(const uint32_t d_in[], int n, uint32_t max, uint32_t shift, zbc_pwm_out_t out[])
{
    uint32_t d[ZBC_PWM_MAX_LIGHT_CH];
    int64_t  total = 0;
    int      lit   = 0;
    n     = n > ZBC_PWM_MAX_LIGHT_CH ? ZBC_PWM_MAX_LIGHT_CH : n;
    shift = shift % max;
    for (int i = 0; i < n; i++) {
        d[i] = d_in[i] > max ? max : d_in[i];
        total += d[i];
        lit += d[i] > 0;
    }

    /* The lit pulses one after another from `shift`, each a lit-th of the dark
     * time after the last, which may be negative when they cannot all fit. Two
     * lit pulses therefore have their centres exactly half a period apart, and
     * at shift 0 a pair whose duties fit never wraps - every constant mix. The
     * centre is worked out doubled and times `lit`, so it stays an integer. */
    const int64_t dark   = (int64_t)max - total;
    int64_t       before = 0;
    int           k      = 0;
    for (int i = 0; i < n; i++) {
        if (d[i] == 0) {
            out[i] = place_pulse(0, 0, max);
            continue;
        }
        const int64_t scaled = 2 * (int64_t)lit * ((int64_t)shift + before) + 2 * (int64_t)k * dark +
                               (int64_t)lit * d[i];
        int64_t c2 = floor_div(scaled, lit) % (2 * (int64_t)max);
        if (c2 < 0) {
            c2 += 2 * (int64_t)max;
        }
        out[i] = place_pulse(d[i], (uint32_t)c2, max);
        before += d[i];
        k++;
    }
}

void zbc_pwm_place(uint32_t dc, uint32_t dw, uint32_t max, uint32_t shift,
                   zbc_pwm_out_t *cold, zbc_pwm_out_t *warm)
{
    const uint32_t d[2] = { dc, dw };
    zbc_pwm_out_t  o[2];
    zbc_pwm_place_n(d, 2, max, shift, o);
    *cold = o[0];
    *warm = o[1];
}

uint16_t zbc_mcpwm_unit_deg(const zbc_pwm_slot_t *s, int c)
{
    const unsigned centre = s->phase_deg + (c == 1 ? 180u : 0u);
    /* Half a period back from a pulse centred on the peak is half a period on. */
    return (uint16_t)((centre + ((s->peak >> c) & 1u ? 180u : 0u)) % 360u);
}

static int light_nch(uint8_t chans)
{
    return chans < 1 ? 1 : (chans > ZBC_PWM_MAX_LIGHT_CH ? ZBC_PWM_MAX_LIGHT_CH : chans);
}

/* Whether the j-th of the k PWM lights is on MCPWM in the split `mc`. The
 * last light is the lowest bit, so counting the splits up moves the later
 * lights first. */
static bool on_mcpwm(uint32_t mc, int k, int j)
{
    return (mc >> (k - 1 - j)) & 1u;
}

/* LEDC: the lights of idx[] that `mc` leaves it, planned as zbc_pwm_plan did
 * before there was MCPWM. False when they do not fit. */
static bool plan_ledc(const uint16_t hz[], const uint8_t chans[], const int idx[], int k, uint32_t mc,
                      zbc_pwm_slot_t out[], uint16_t timer_hz[ZBC_PWM_LEDC_TIMERS])
{
    for (int t = 0; t < ZBC_PWM_LEDC_TIMERS; t++) {
        timer_hz[t] = 0;
    }
    int channels = 0;
    for (int j = 0; j < k; j++) {
        if (on_mcpwm(mc, k, j)) {
            continue;
        }
        const int i     = idx[j];
        int       timer = -1;
        for (int t = 0; t < ZBC_PWM_LEDC_TIMERS && timer < 0; t++) {
            if (timer_hz[t] == hz[i]) {
                timer = t;
            }
        }
        for (int t = 0; t < ZBC_PWM_LEDC_TIMERS && timer < 0; t++) {
            if (timer_hz[t] == 0) {
                timer = t;
            }
        }
        const int nch = light_nch(chans[i]);
        if (timer < 0 || channels + nch > ZBC_PWM_LEDC_CHANNELS) {
            return false;
        }
        timer_hz[timer] = hz[i];
        out[i] = (zbc_pwm_slot_t){ .timer = (int8_t)timer, .ch = { -1, -1, -1 } };
        for (int c = 0; c < nch; c++) {
            out[i].ch[c] = (int8_t)(channels + c);
        }
        channels += nch;
    }

    /* The lights on one timer share its period, so their pulses are spread over
     * it. Only dimmable lights: evenly, 0/180, 0/120/240. With a tunable white
     * light, whose two pulses are half a period apart, the period is cut into
     * an even number of places, one per pulse (a spare one when the count is
     * odd); tunable white lights take the pairs half a period apart in order -
     * two at 0 and 90 degrees, three at 0, 60 and 120 - and dimmable lights the
     * places left, a pair's two halves first. A three-channel light counts as a
     * pair: in a constant mix two of its channels are lit, half a period apart.
     * Light by light, in table order, so the first one is always at 0. */
    for (int t = 0; t < ZBC_PWM_LEDC_TIMERS; t++) {
        unsigned pairs_used = 0, singles = 0;
        for (int j = 0; j < k; j++) {
            const zbc_pwm_slot_t *s = &out[idx[j]];
            if (!on_mcpwm(mc, k, j) && s->timer == t) {
                pairs_used += s->ch[1] >= 0;
                singles    += s->ch[1] < 0;
            }
        }
        if (pairs_used == 0) {
            unsigned m = 0;
            for (int j = 0; j < k; j++) {
                if (!on_mcpwm(mc, k, j) && out[idx[j]].timer == t) {
                    out[idx[j]].phase_deg = (uint16_t)(360u * m++ / singles);
                }
            }
            continue;
        }
        const unsigned places = (2 * pairs_used + singles + 1) / 2 * 2;
        const unsigned half   = places / 2;
        unsigned       pair = 0, left = 0;
        for (int j = 0; j < k; j++) {
            zbc_pwm_slot_t *s = &out[idx[j]];
            if (on_mcpwm(mc, k, j) || s->timer != t) {
                continue;
            }
            unsigned place;
            if (s->ch[1] >= 0) {
                place = pair++;
            } else {
                /* The places no pair took: pairs_used, its partner, the next... */
                place = pairs_used + left / 2 + (left % 2 ? half : 0);
                left++;
            }
            s->phase_deg = (uint16_t)(360u * place / places);
        }
    }
    return true;
}

/* A unit at `hz` with its second output free, not `skip`; -1 when there is
 * none. A unit always fills its first output first. */
static int mc_unit_half(const uint16_t unit_hz[], const int8_t owner[][2], uint16_t hz, int skip)
{
    for (int u = 0; u < ZBC_PWM_MCPWM_UNITS; u++) {
        if (u != skip && unit_hz[u] == hz && owner[u][1] < 0) {
            return u;
        }
    }
    return -1;
}

/* A unit no light uses yet, not `skip`; -1 when there is none. */
static int mc_unit_free(const uint16_t unit_hz[], int skip)
{
    for (int u = 0; u < ZBC_PWM_MCPWM_UNITS; u++) {
        if (u != skip && unit_hz[u] == 0) {
            return u;
        }
    }
    return -1;
}

/* MCPWM: the lights of idx[] that `mc` gives it, in table order. A tunable
 * white light takes a unit, its first channel centred on the unit's zero and
 * its second on the peak. A dimmable light takes the second output of a unit
 * at its frequency, or a unit. A three-channel light takes a unit and one more
 * output at its frequency, centred where its first channel is, so that unit is
 * locked to the same phase. Units locked together count as one, and those at
 * one frequency are spread over half the period - two at 0 and 90 degrees,
 * three at 0, 60 and 120 - as LEDC spreads tunable white lights. False when the
 * lights do not fit. */
static bool plan_mcpwm(const uint16_t hz[], const uint8_t chans[], const int idx[], int k, uint32_t mc,
                       zbc_pwm_slot_t out[], uint16_t unit_hz[ZBC_PWM_MCPWM_UNITS])
{
    enum { UNITS = ZBC_PWM_MCPWM_UNITS };
    int8_t  owner[UNITS][2];          /* the light (as j) on each output, -1 when free */
    int8_t  role[UNITS][2];           /* which of its channels */
    uint8_t lock[UNITS];              /* the lowest unit it is locked to */
    for (int u = 0; u < UNITS; u++) {
        unit_hz[u]  = 0;
        owner[u][0] = owner[u][1] = -1;
        role[u][0]  = role[u][1] = 0;
        lock[u]     = (uint8_t)u;
    }

    for (int j = 0; j < k; j++) {
        if (!on_mcpwm(mc, k, j)) {
            continue;
        }
        const int i   = idx[j];
        const int nch = light_nch(chans[i]);
        int       u, v = -1;
        if (nch == 1) {
            u = mc_unit_half(unit_hz, owner, hz[i], -1);
            u = u >= 0 ? u : mc_unit_free(unit_hz, -1);
        } else {
            u = mc_unit_free(unit_hz, -1);
            if (u >= 0 && nch == 3) {
                v = mc_unit_half(unit_hz, owner, hz[i], u);
                v = v >= 0 ? v : mc_unit_free(unit_hz, u);
                if (v < 0) {
                    return false;
                }
            }
        }
        if (u < 0) {
            return false;
        }
        out[i] = (zbc_pwm_slot_t){ .timer = (int8_t)(ZBC_PWM_LEDC_TIMERS + u), .ch = { -1, -1, -1 } };
        unit_hz[u] = hz[i];
        for (int c = 0; c < nch && c < 2; c++) {
            const int o = nch == 1 && owner[u][0] >= 0 ? 1 : c;
            owner[u][o] = (int8_t)j;
            role[u][o]  = (int8_t)c;
            out[i].ch[c] = (int8_t)(ZBC_PWM_LEDC_CHANNELS + 2 * u + o);
        }
        if (v >= 0) {
            const int o = owner[v][0] >= 0 ? 1 : 0;
            unit_hz[v]   = hz[i];
            owner[v][o]  = (int8_t)j;
            role[v][o]   = 2;
            out[i].ch[2] = (int8_t)(ZBC_PWM_LEDC_CHANNELS + 2 * v + o);
            const uint8_t from = lock[u] > lock[v] ? lock[u] : lock[v];
            const uint8_t to   = lock[u] > lock[v] ? lock[v] : lock[u];
            for (int w = 0; w < UNITS; w++) {
                lock[w] = lock[w] == from ? to : lock[w];
            }
        }
    }

    /* Where each output's pulse is centred. A tunable white light's second
     * channel on the peak, its first and third on the zero; then a dimmable
     * light opposite the other output of its unit, or on the zero alone. */
    bool at_peak[UNITS][2], known[UNITS][2];
    for (int u = 0; u < UNITS; u++) {
        for (int o = 0; o < 2; o++) {
            const int j = owner[u][o];
            known[u][o]   = j >= 0 && light_nch(chans[idx[j]]) > 1;
            at_peak[u][o] = known[u][o] && role[u][o] == 1;
        }
    }
    for (int u = 0; u < UNITS; u++) {
        for (int o = 0; o < 2; o++) {
            if (owner[u][o] >= 0 && !known[u][o]) {
                at_peak[u][o] = known[u][1 - o] && !at_peak[u][1 - o];
                known[u][o]   = true;
            }
        }
    }

    uint16_t deg[UNITS] = { 0 };
    for (int u = 0; u < UNITS; u++) {
        unsigned count = 0, rank = 0;
        for (int w = 0; w < UNITS; w++) {
            if (unit_hz[u] && unit_hz[w] == unit_hz[u] && lock[w] == w) {
                count++;
                rank += w < lock[u];
            }
        }
        deg[u] = count ? (uint16_t)(180u * rank / count) : 0;
    }
    for (int u = 0; u < UNITS; u++) {
        for (int o = 0; o < 2; o++) {
            if (owner[u][o] < 0) {
                continue;
            }
            zbc_pwm_slot_t *s = &out[idx[owner[u][o]]];
            s->peak |= (uint8_t)(at_peak[u][o] << role[u][o]);
            if (role[u][o] == 0) {
                s->phase_deg = (uint16_t)((deg[u] + (at_peak[u][o] ? 180 : 0)) % 360);
            }
        }
    }
    return true;
}

const char *zbc_pwm_plan(const uint16_t hz[], const uint8_t chans[], size_t n, zbc_pwm_slot_t out[],
                         uint16_t timer_hz[ZBC_PWM_TIMERS])
{
    for (int t = 0; t < ZBC_PWM_TIMERS; t++) {
        timer_hz[t] = 0;
    }
    int idx[ZBC_PWM_CHANNELS];      /* the lights that take PWM; each takes a channel at least */
    int k = 0, channels = 0;
    for (size_t i = 0; i < n; i++) {
        out[i] = (zbc_pwm_slot_t){ .timer = -1, .ch = { -1, -1, -1 } };
        if (hz[i] == 0) {
            continue;
        }
        if (hz[i] < ZBC_PWM_HZ_MIN || hz[i] > ZBC_PWM_HZ_MAX) {
            return "the PWM frequency must be 100-19531 Hz";
        }
        channels += light_nch(chans[i]);
        if (channels > ZBC_PWM_CHANNELS) {
            return "no PWM channel left - the chip has 12: a three-channel light takes 3, a tunable white "
                   "light 2, a dimmable one 1";
        }
        idx[k++] = (int)i;
    }

    /* Every split of the lights between the two, LEDC taking the earlier lights
     * as long as any split lets it: at most 4096, and most cut short by the
     * channel count. */
    for (uint32_t mc = 0; mc < (1u << k); mc++) {
        int on_ledc = 0, on_mc = 0;
        for (int j = 0; j < k; j++) {
            if (on_mcpwm(mc, k, j)) {
                on_mc += light_nch(chans[idx[j]]);
            } else {
                on_ledc += light_nch(chans[idx[j]]);
            }
        }
        if (on_ledc <= ZBC_PWM_LEDC_CHANNELS && on_mc <= 2 * ZBC_PWM_MCPWM_UNITS &&
            plan_ledc(hz, chans, idx, k, mc, out, timer_hz) &&
            plan_mcpwm(hz, chans, idx, k, mc, out, timer_hz + ZBC_PWM_LEDC_TIMERS)) {
            return NULL;
        }
    }

    int freqs = 0;
    for (int j = 0; j < k; j++) {
        bool seen = false;
        for (int m = 0; m < j && !seen; m++) {
            seen = hz[idx[m]] == hz[idx[j]];
        }
        freqs += !seen;
        out[idx[j]] = (zbc_pwm_slot_t){ .timer = -1, .ch = { -1, -1, -1 } };
    }
    for (int t = 0; t < ZBC_PWM_TIMERS; t++) {
        timer_hz[t] = 0;
    }
    return freqs > ZBC_PWM_TIMERS
               ? "no PWM timer left - the chip runs at most 7 different PWM frequencies"
               : "no PWM channel left at these frequencies - six of the 12 come in pairs that run at one "
                 "frequency, so lights that share one fit more";
}
/* >>> end host-test region: cwww <<< */

/* >>> host-test region: ramp <<< */
/* A value moving from `from` to `to` between t0 and t1. t1 <= now means it has
 * arrived. Level is in thousandths so a slow fade has no visible steps.
 *
 * Most ramps run at one speed. A slider, a held button and Adaptive Lighting
 * are chains of ramps, each starting where the last one is, and a chain of eased
 * ramps pulses - a held button's sweep would stall once a second. A switch-on
 * or switch-off is one ramp that starts and ends at rest, and that one eases:
 * smootherstep, the curve ESPHome's light transitions use. The level eases in
 * light output rather than in level (RAMP_EASED_LIGHT), as ESPHome's do, so the
 * fade leaves black and returns to it a PWM count at a time; a ramp even in
 * level crosses the bottom of the range in a few large steps, the first of them
 * from dark straight to a visible glow. A colour that travels with such a fade
 * eases in mireds on the same clock (RAMP_EASED).
 *
 * A light with response curves glides instead (RAMP_GLIDE), which is ESPHome's
 * transition exactly: the brightness asked of it moves on smootherstep, and the
 * level and colour are whatever the light's own curves give at each point on the
 * way - lighting.c samples them, since curves are policy and this file knows
 * none. A light that is dark below some brightness stays dark until the glide
 * passes it and then crosses the bottom of its range in a few ticks, in the
 * middle of the fade where it moves fastest; eased on its own from black it
 * would spend the first and last 150 ms of every fade at its dimmest duties,
 * which LED spots with drivers of their own show as a cold tint and a flicker. */
typedef enum {
    RAMP_LINEAR,
    RAMP_EASED,           /* smootherstep in the value itself */
    RAMP_EASED_LIGHT,     /* a level, smootherstep in the light it gives */
    RAMP_GLIDE,           /* through glide[], evenly spaced in smootherstep time */
} ramp_shape_t;

#define RAMP_GLIDE_POINTS 16

typedef struct {
    int64_t  from, to;
    int64_t  t0, t1;
    uint8_t  shape;       /* ramp_shape_t */
    uint8_t  n;           /* RAMP_GLIDE: points in glide[] */
    uint16_t mul;         /* RAMP_GLIDE: glide[] times this is the value */
    double   y_from, y_to;  /* RAMP_EASED_LIGHT: the light at either end, worked out once */
    /* RAMP_GLIDE: the value at each step; glide[0] is replaced by `from`, which
     * is where the light really is when the glide starts. 16 bits, so eight
     * lights' two ramps cost half a kilobyte rather than one. */
    uint16_t glide[RAMP_GLIDE_POINTS];
} ramp_t;

static double smootherstep(double s)
{
    return s * s * s * (s * (s * 6.0 - 15.0) + 10.0);
}

/* The level that gives a light output, 0-1: level_light backwards. */
static int64_t light_level(double y)
{
    if (y <= 0.0) {
        return 0;
    }
    const double l = y <= 8.0 / 903.2963 ? y * 903.2963 : 116.0 * cbrt(y) - 16.0;
    return llround(l * 2540.0);
}

static int64_t ramp_now(const ramp_t *r, int64_t now)
{
    if (now >= r->t1 || r->t1 <= r->t0) {
        return r->to;
    }
    if (now <= r->t0) {
        return r->from;
    }
    if (r->shape == RAMP_LINEAR) {
        return r->from + (r->to - r->from) * (now - r->t0) / (r->t1 - r->t0);
    }
    const double e = smootherstep((double)(now - r->t0) / (double)(r->t1 - r->t0));
    if (r->shape == RAMP_EASED) {
        return r->from + llround((double)(r->to - r->from) * e);
    }
    if (r->shape == RAMP_GLIDE) {
        const double q    = e * (r->n - 1);
        int          i    = (int)q;
        i                 = i > r->n - 2 ? r->n - 2 : i;
        const int64_t a   = i == 0 ? r->from : (int64_t)r->glide[i] * r->mul;
        const int64_t b   = (int64_t)r->glide[i + 1] * r->mul;
        return a + llround((double)(b - a) * (q - i));
    }
    return light_level(r->y_from + (r->y_to - r->y_from) * e);
}

static bool ramp_active(const ramp_t *r, int64_t now)
{
    return r->t1 > now;
}

static void ramp_start(ramp_t *r, int64_t from, int64_t to, uint16_t ds, int64_t now, ramp_shape_t shape)
{
    r->from  = from;
    r->to    = to;
    r->t0    = now;
    r->t1    = now + (int64_t)ds * 100000LL;
    r->shape = (uint8_t)shape;
    /* No FPU on the C6: pow() is software, and the fade task runs every tick. */
    r->y_from = shape == RAMP_EASED_LIGHT ? level_light(from) : 0.0;
    r->y_to   = shape == RAMP_EASED_LIGHT ? level_light(to) : 0.0;
}

/* A glide from where the value is now through pts[1..n-1] (pts[0] stands for
 * `from`, so a light already lit carries on from where it is), each times `mul`.
 * Fewer than two points, or more than RAMP_GLIDE_POINTS, is a plain ramp to the
 * last one. */
static void ramp_glide(ramp_t *r, int64_t from, const uint16_t *pts, int n, uint16_t mul, uint16_t ds,
                       int64_t now)
{
    if (n < 2 || n > RAMP_GLIDE_POINTS) {
        ramp_start(r, from, n > 0 ? (int64_t)pts[n - 1] * mul : from, ds, now, RAMP_EASED);
        return;
    }
    ramp_start(r, from, (int64_t)pts[n - 1] * mul, ds, now, RAMP_GLIDE);
    r->n   = (uint8_t)n;
    r->mul = mul;
    memcpy(r->glide, pts, (size_t)n * sizeof(pts[0]));
}

static void ramp_settle(ramp_t *r, int64_t v)
{
    *r = (ramp_t){ .from = v, .to = v, .t0 = 0, .t1 = 0, .shape = RAMP_LINEAR };
}
/* >>> end host-test region: ramp <<< */

_Static_assert(RAMP_GLIDE_POINTS == LOCAL_LIGHT_GLIDE_POINTS, "a glide's points are counted in two places");

typedef struct {
    bool          used;
    uint16_t      id;
    uint8_t       kind;
    int8_t        gpio;               /* an on/off light's pin; pin[0] of a PWM one */
    bool          active_low;
    uint8_t       boot;
    uint8_t       mix;
    uint16_t      pwm_hz;
    uint32_t      cold_m, neutral_m, warm_m;  /* cold_m < neutral_m < warm_m */
    zbc_mix_curve_t curve[ZBC_PWM_MAX_LIGHT_CH];  /* by role, ZBC_CH_* */

    /* PWM: the light's channels in the order their pulses take in the period,
     * each with the LEDs it drives (`role`, ZBC_CH_*). ch[0] < 0 on a PWM light
     * means it could not be given channels and stays dark. Numbered as in
     * light_math.h: a timer from ZBC_PWM_LEDC_TIMERS on is MCPWM. */
    uint8_t       nch;
    int8_t        pin[ZBC_PWM_MAX_LIGHT_CH];
    uint8_t       role[ZBC_PWM_MAX_LIGHT_CH];
    int8_t        timer;
    int8_t        ch[ZBC_PWM_MAX_LIGHT_CH];
    uint16_t      phase_deg;          /* where in the timer's period it starts */
    uint8_t       peak;               /* MCPWM: the channels centred on the peak */
    uint32_t      max;
    bool          configured;         /* channels bound to the pins */
    bool          sent_valid;
    zbc_pwm_out_t sent[ZBC_PWM_MAX_LIGHT_CH];

    /* State. */
    bool          on;
    bool          off_at_end;         /* a fade out: switch off when the level lands */
    bool          moving;             /* a ramp ran at the last tick: its landing is news */
    uint8_t       remembered;         /* the level to come back to, 1-254 */
    ramp_t        level;              /* thousandths of a ZCL level */
    ramp_t        mireds;
    int64_t       dirty_since;        /* 0 = nothing waiting to be stored */
} live_light_t;

/* Every LEDC call is made holding s_lock: ledc_set_duty_with_hpoint and
 * ledc_update_duty are not thread-safe, and the fade task, a reload and a
 * command could otherwise interleave on one channel. A mutex rather than a
 * spinlock, because a reload configures pins and NVS is read under it. */
static live_light_t      s_live[CONFIG_ZBC_MAX_LIGHTS];
static uint16_t          s_timer_hz[ZBC_PWM_TIMERS];
/* Pins configured here, so one that stops being used is handed back rather than
 * left driven. Separate from buttons.c's table: the HTTP layer refuses a pin
 * either kind already holds, so the two never claim the same one. */
static bool              s_owned[SOC_GPIO_PIN_COUNT];
static SemaphoreHandle_t s_lock;
static TaskHandle_t      s_task;
static local_lights_fade_stats_t s_fade_stats;   /* fade_task's, under s_lock */
/* The overheat interlock - see local_lights_hold. Written under s_lock once the
 * lock exists; before that, only thermal_start() touches it. */
static volatile bool     s_held;

/* An MCPWM unit (light_math.h), under s_lock like LEDC. Its operator and
 * comparators are made at the first light planned on MCPWM and never deleted:
 * they hold the driver's group, and with it the clock divider mc_start_locked
 * fixes. Its timer is made for a frequency and deleted when no light uses the
 * unit at that frequency any more; s_timer_hz has the frequency. */
typedef struct {
    mcpwm_oper_handle_t  oper;
    mcpwm_cmpr_handle_t  cmp[2];      /* one an output */
    mcpwm_gen_handle_t   gen[2];      /* while a light's channel is bound to the output */
    mcpwm_timer_handle_t timer;       /* NULL: not in use */
    mcpwm_sync_handle_t  sync;        /* its zero, while another unit is locked to it */
    uint32_t             peak;
} mc_unit_t;
static mc_unit_t         s_mc[ZBC_PWM_MCPWM_UNITS];
static int8_t            s_mc_state;  /* 0 not started, 1 running, -1 refused */

const char *local_light_pin_problem(int gpio)
{
    const char *why = buttons_pin_problem(gpio);
    if (why) {
        return why;
    }
#if CONFIG_ZBC_UNLOCK_GPIO >= 0
    if (gpio == CONFIG_ZBC_UNLOCK_GPIO) {
        return "that pin is watched for the failsafe unlock, so it cannot drive a light";
    }
#endif
    zbc_thermal_cfg_t th;
    settings_thermal(&th);
    if (gpio >= 0 && gpio == th.gpio) {
        return "that pin reads the overheat sensor, so it cannot drive a light";
    }
    return NULL;
}

/* A light driven by PWM - tunable white or dimmable - rather than switched. */
static bool is_pwm(uint8_t kind)
{
    return zbc_light_pwm_channels(kind) > 0;
}

/* A PWM light's pins and the LEDs on each, in pulse order. Returns how many. */
static int light_channels(const zbc_light_t *c, int8_t pin[ZBC_PWM_MAX_LIGHT_CH],
                          uint8_t role[ZBC_PWM_MAX_LIGHT_CH])
{
    switch (c->kind) {
    case ZBC_LIGHT_DIM:
        pin[0] = c->gpio;
        role[0] = ZBC_CH_COLD;
        return 1;
    case ZBC_LIGHT_CWWW:
        pin[0] = c->gpio, pin[1] = c->gpio_warm;
        role[0] = ZBC_CH_COLD, role[1] = ZBC_CH_WARM;
        return 2;
    case ZBC_LIGHT_CNW:
        pin[0] = c->gpio, pin[1] = c->gpio_neutral, pin[2] = c->gpio_warm;
        role[0] = ZBC_CH_COLD, role[1] = ZBC_CH_NEUTRAL, role[2] = ZBC_CH_WARM;
        return 3;
    default:
        return 0;
    }
}

/* Every pin chosen, no two the same, and none this board refuses a light. */
static bool pwm_ready(const zbc_light_t *c)
{
    int8_t    pin[ZBC_PWM_MAX_LIGHT_CH];
    uint8_t   role[ZBC_PWM_MAX_LIGHT_CH];
    const int n = light_channels(c, pin, role);
    for (int i = 0; i < n; i++) {
        if (pin[i] < 0 || local_light_pin_problem(pin[i])) {
            return false;
        }
        for (int j = 0; j < i; j++) {
            if (pin[j] == pin[i]) {
                return false;
            }
        }
    }
    return n > 0;
}

static bool is_mc_channel(int ch)
{
    return ch >= ZBC_PWM_LEDC_CHANNELS;
}

static mc_unit_t *mc_unit_of(int ch)
{
    return &s_mc[(ch - ZBC_PWM_LEDC_CHANNELS) / 2];
}

static int mc_output_of(int ch)
{
    return (ch - ZBC_PWM_LEDC_CHANNELS) % 2;
}

/* The timer channel k of a light runs on: an MCPWM light may span two units. */
static int channel_timer(const live_light_t *l, int k)
{
    return is_mc_channel(l->ch[k]) ? ZBC_PWM_LEDC_TIMERS + (l->ch[k] - ZBC_PWM_LEDC_CHANNELS) / 2 : l->timer;
}

static live_light_t *find_locked(uint16_t id)
{
    for (size_t i = 0; i < CONFIG_ZBC_MAX_LIGHTS; i++) {
        if (s_live[i].used && s_live[i].id == id) {
            return &s_live[i];
        }
    }
    return NULL;
}

static void mark_dirty(live_light_t *l, int64_t now)
{
    if (l->boot != ZBC_LIGHT_BOOT_OFF) {
        l->dirty_since = now;        /* restarted by every change: debounced */
    }
}

/* One channel as `o`, writing only what differs from `was` (NULL: everything).
 * The GPIO matrix flips at once, while the new duty waits for the next period -
 * so the one period where a pulse starts or stops wrapping may be wrong. One
 * period long, and at phase 0 only "full" mixing ever wraps. */
static void set_channel_locked(int gpio, int ch, bool active_low, const zbc_pwm_out_t *o,
                               const zbc_pwm_out_t *was)
{
    if (!was || o->invert != was->invert) {
        esp_rom_gpio_connect_out_signal(gpio, ledc_periph_signal[0].sig_out0_idx + ch,
                                        o->invert != active_low, false);
    }
    if (!was || o->duty != was->duty || o->hpoint != was->hpoint || o->invert != was->invert) {
        ledc_set_duty_with_hpoint(LEDC_MODE, ch, o->duty, o->hpoint);
        ledc_update_duty(LEDC_MODE, ch);
    }
}

/* One MCPWM output at d counts of `peak`. Dark and full are levels forced at
 * once; between them the compare value takes over at the next zero, so the
 * period where a light leaves dark or full may be wrong. */
static void mc_set_locked(int ch, uint32_t d, uint32_t peak, bool at_peak)
{
    mc_unit_t            *m = mc_unit_of(ch);
    const int             o = mc_output_of(ch);
    const zbc_mcpwm_out_t w = zbc_mcpwm_out(d, peak, at_peak);
    if (!m->gen[o]) {
        return;
    }
    if (w.force < 0) {
        mcpwm_comparator_set_compare_value(m->cmp[o], w.cmp);
    }
    mcpwm_generator_set_force_level(m->gen[o], w.force, true);
}

/* Writes the light's channels for where it is now. Only what changed goes to
 * the hardware, so an idle light costs nothing per tick. */
static void apply_pwm_locked(live_light_t *l, int64_t now)
{
    if (!is_pwm(l->kind) || !l->configured) {
        return;
    }
    const int64_t  lvl  = l->on ? ramp_now(&l->level, now) : 0;
    const uint32_t duty = zbc_level_to_duty(lvl > 0 ? (uint32_t)lvl : 0, l->max);
    const uint32_t m    = (uint32_t)ramp_now(&l->mireds, now);

    /* What each set of LEDs gets, by role; a dimmable light is all "cold". */
    uint32_t by_role[ZBC_PWM_MAX_LIGHT_CH] = { duty, 0, 0 };
    if (!zbc_light_tunable(l->kind)) {
        /* by_role as it stands */
    } else if (l->mix == ZBC_MIX_CUSTOM) {
        zbc_mix_custom(duty, m, l->cold_m, l->warm_m, l->curve, ZBC_PWM_MAX_LIGHT_CH, by_role);
    } else if (l->kind == ZBC_LIGHT_CNW) {
        zbc_mix3(duty, m, l->cold_m, l->neutral_m, l->warm_m, l->mix, by_role);
    } else {
        zbc_cwww_mix(duty, m, l->cold_m, l->warm_m, l->mix, &by_role[ZBC_CH_COLD], &by_role[ZBC_CH_WARM]);
    }

    uint32_t d[ZBC_PWM_MAX_LIGHT_CH];
    for (int i = 0; i < l->nch; i++) {
        d[i] = by_role[l->role[i]];
    }
    if (l->timer >= ZBC_PWM_LEDC_TIMERS) {
        /* Each output's pulse is centred where the plan put it: only the duty
         * goes out. */
        for (int i = 0; i < l->nch; i++) {
            if (!l->sent_valid || l->sent[i].duty != d[i]) {
                mc_set_locked(l->ch[i], d[i], l->max, (l->peak >> i) & 1);
            }
            l->sent[i] = (zbc_pwm_out_t){ .duty = d[i] };
        }
        l->sent_valid = true;
        return;
    }
    const uint32_t shift = (uint32_t)((uint64_t)l->max * l->phase_deg / 360);
    zbc_pwm_out_t  out[ZBC_PWM_MAX_LIGHT_CH];
    zbc_pwm_place_n(d, l->nch, l->max, shift, out);
    for (int i = 0; i < l->nch; i++) {
        set_channel_locked(l->pin[i], l->ch[i], l->active_low, &out[i], l->sent_valid ? &l->sent[i] : NULL);
        l->sent[i] = out[i];
    }
    l->sent_valid = true;
}

static void drive(int gpio, bool on, bool active_low)
{
    gpio_set_level(gpio, zbc_light_pin_level(on, active_low));
}

/* ------------------------------------------------------------- fade task */

static void fade_task(void *arg)
{
    (void)arg;
    int64_t moving_at = 0;            /* the last tick that left something moving */
    for (;;) {
        uint16_t          ids[CONFIG_ZBC_MAX_LIGHTS];
        zbc_light_state_t states[CONFIG_ZBC_MAX_LIGHTS];
        int               saves    = 0;
        bool              active   = false;
        bool              landed   = false;
        int64_t           next_due = 0;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        const int64_t now = esp_timer_get_time();
        for (size_t i = 0; i < CONFIG_ZBC_MAX_LIGHTS; i++) {
            live_light_t *l = &s_live[i];
            if (!l->used || !is_pwm(l->kind) || !l->configured) {
                continue;
            }
            const bool level_moving = ramp_active(&l->level, now);
            if (!level_moving && l->off_at_end) {
                /* The fade out has landed: off, keeping the level to come back to. */
                l->on         = false;
                l->off_at_end = false;
                ramp_settle(&l->level, (int64_t)l->remembered * 1000);
                mark_dirty(l, now);
            }
            const bool moving = level_moving || ramp_active(&l->mireds, now);
            if (moving) {
                active = true;
            } else if (l->moving) {
                landed = true;
            }
            l->moving = moving;
            apply_pwm_locked(l, now);

            if (l->dirty_since) {
                const int64_t due = l->dirty_since + SAVE_AFTER_US;
                if (due <= now) {
                    ids[saves]    = l->id;
                    states[saves] = (zbc_light_state_t){
                        .on = l->on && !l->off_at_end, .level = l->remembered,
                        .mireds = (uint16_t)l->mireds.to,
                    };
                    saves++;
                    l->dirty_since = 0;
                } else if (!next_due || due < next_due) {
                    next_due = due;
                }
            }
        }
        /* The tick before asked to be woken TICK_MS on; how much later this one
         * came is the jump the fade just took. */
        if (moving_at) {
            const int64_t gap = now - moving_at;
            s_fade_stats.ticks++;
            if (gap > FADE_LATE_US) {
                s_fade_stats.late++;
            }
            if (gap / 1000 > s_fade_stats.max_gap_ms) {
                s_fade_stats.max_gap_ms = (uint32_t)(gap / 1000);
            }
        }
        moving_at = active ? now : 0;
        xSemaphoreGive(s_lock);

        /* A command bumps the revision when it is given, so the page's next poll
         * reads the light mid-ramp - still lit, during a fade out - and every poll
         * after it is "unchanged". Without this bump the room switch went back to
         * on once its optimistic off expired (12 s), with every light dark. */
        if (landed) {
            zbc_state_bump();
        }

        for (int s = 0; s < saves; s++) {
            light_db_save_state(ids[s], &states[s]);
        }

        TickType_t wait = portMAX_DELAY;
        if (active) {
            wait = pdMS_TO_TICKS(TICK_MS);
        } else if (next_due) {
            const int64_t ms = (next_due - esp_timer_get_time()) / 1000 + 1;
            wait = pdMS_TO_TICKS(ms < 1 ? 1 : ms);
        }
        ulTaskNotifyTake(pdTRUE, wait ? wait : 1);
    }
}

static void wake(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

/* ----------------------------------------------------------------- reload */

/* What a light does when it is first set up: at boot, when a pin is first
 * picked, when its type changes. */
static void boot_state(const zbc_light_t *c, live_light_t *l, int64_t now)
{
    zbc_light_state_t st;
    const bool        stored = light_db_load_state(c->id, &st);
    /* Too hot: off whatever the power-on setting says. */
    l->on         = !s_held && zbc_light_boot_on(c->boot, stored ? st.on : -1);
    l->remembered = stored && st.level ? st.level : 254;
    const uint32_t m = stored && st.mireds >= l->cold_m && st.mireds <= l->warm_m
                           ? st.mireds : (l->cold_m + l->warm_m) / 2;
    ramp_settle(&l->level, (int64_t)l->remembered * 1000);
    ramp_settle(&l->mireds, m);
    if (!stored) {
        mark_dirty(l, now);
    }
    ESP_LOGI(TAG, "Wired light %u starts %s", (unsigned)c->id, l->on ? "on" : "off");
}

static bool bind_channel(int gpio, int ch, int timer, bool active_low)
{
    const ledc_channel_config_t cc = {
        .gpio_num   = gpio,
        .speed_mode = LEDC_MODE,
        .channel    = (ledc_channel_t)ch,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = (ledc_timer_t)timer,
        .duty       = 0,
        .hpoint     = 0,
        .flags.output_invert = active_low,
    };
    const esp_err_t err = ledc_channel_config(&cc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PWM channel %d on GPIO %d refused (0x%x)", ch, gpio, err);
    }
    return err == ESP_OK;
}

/* MCPWM's operators and comparators, once. The group divides its clock once
 * for every timer in it, and the first timer made sets that divider: left at
 * the driver's default of 2, a timer later asked for the full 160 MHz, or a
 * third of it, is refused or runs at another resolution than its counts were
 * worked out for. A timer made here at 160 MHz sets 1, the operators keep the
 * group - and the divider - after it is deleted, and every timer after it is
 * divided by its own divider alone, as zbc_mcpwm_peak counts. */
static bool mc_start_locked(void)
{
    if (s_mc_state) {
        return s_mc_state > 0;
    }
    esp_err_t err = ESP_OK;
    for (int u = 0; u < ZBC_PWM_MCPWM_UNITS && err == ESP_OK; u++) {
        const mcpwm_operator_config_t oc = { .group_id = 0 };
        err = mcpwm_new_operator(&oc, &s_mc[u].oper);
        for (int o = 0; o < 2 && err == ESP_OK; o++) {
            /* A new duty from the next zero, so a pulse on the peak is never
             * cut and one on the zero is the old half and the new one once. */
            const mcpwm_comparator_config_t cc = { .flags.update_cmp_on_tez = true };
            err = mcpwm_new_comparator(s_mc[u].oper, &cc, &s_mc[u].cmp[o]);
        }
    }
    if (err == ESP_OK) {
        const mcpwm_timer_config_t tc = {
            .group_id      = 0,
            .clk_src       = MCPWM_TIMER_CLK_SRC_PLL160M,
            .resolution_hz = ZBC_PWM_MCPWM_CLK_HZ,
            .count_mode    = MCPWM_TIMER_COUNT_MODE_UP_DOWN,
            .period_ticks  = 2,
        };
        mcpwm_timer_handle_t t;
        err = mcpwm_new_timer(&tc, &t);
        if (err == ESP_OK) {
            mcpwm_del_timer(t);
        }
    }
    s_mc_state = err == ESP_OK ? 1 : -1;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MCPWM refused (0x%x): the lights planned on it stay dark", err);
    }
    return err == ESP_OK;
}

/* Unit u's timer at hz, or none for 0. The lights on it are unbound by now: a
 * light whose unit changes frequency is not carried over. */
static void mc_set_timer_locked(int u, uint16_t hz)
{
    mc_unit_t *m = &s_mc[u];
    const int  t = ZBC_PWM_LEDC_TIMERS + u;
    if (m->timer) {
        /* Deleted only once it has stopped at its zero, up to a period later:
         * the hardware timer takes a new timer's divider only from rest, and
         * one remade while it ran on kept the old divider (on the bench a unit
         * moved from 1000 Hz to 300 ran at 750). */
        mcpwm_timer_start_stop(m->timer, MCPWM_TIMER_STOP_EMPTY);
        vTaskDelay(pdMS_TO_TICKS(1000 / (s_timer_hz[t] ? s_timer_hz[t] : 100) + 2));
        mcpwm_timer_disable(m->timer);
        mcpwm_del_timer(m->timer);
        m->timer = NULL;
    }
    s_timer_hz[t] = 0;
    if (!hz) {
        return;
    }
    uint32_t                   div;
    const uint32_t             peak = zbc_mcpwm_peak(hz, &div);
    const mcpwm_timer_config_t tc   = {
        .group_id      = 0,
        .clk_src       = MCPWM_TIMER_CLK_SRC_PLL160M,
        .resolution_hz = ZBC_PWM_MCPWM_CLK_HZ / div,
        .count_mode    = MCPWM_TIMER_COUNT_MODE_UP_DOWN,
        .period_ticks  = 2 * peak,
    };
    mcpwm_timer_handle_t timer   = NULL;
    bool                 enabled = false;
    esp_err_t            err     = mcpwm_new_timer(&tc, &timer);
    if (err == ESP_OK) {
        err = mcpwm_operator_connect_timer(m->oper, timer);
    }
    if (err == ESP_OK) {
        err     = mcpwm_timer_enable(timer);
        enabled = err == ESP_OK;
    }
    if (err == ESP_OK) {
        err = mcpwm_timer_start_stop(timer, MCPWM_TIMER_START_NO_STOP);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PWM timer %d (MCPWM unit %d) refused %u Hz (0x%x)", t, u, (unsigned)hz, err);
        if (enabled) {
            mcpwm_timer_disable(timer);
        }
        if (timer) {
            mcpwm_del_timer(timer);
        }
        return;
    }
    m->timer      = timer;
    m->peak       = peak;
    s_timer_hz[t] = hz;
    ESP_LOGI(TAG, "PWM timer %d (MCPWM unit %d): %u Hz at %u counts", t, u, (unsigned)hz, (unsigned)peak);
}

/* Every unit's lock to another let go and its sync source deleted, before any
 * timer is replaced. A unit let go runs on at the phase it has. */
static void mc_unlock_locked(void)
{
    const mcpwm_timer_sync_phase_config_t none = { .sync_src = NULL };
    for (int u = 0; u < ZBC_PWM_MCPWM_UNITS; u++) {
        if (s_mc[u].timer) {
            mcpwm_timer_set_phase_on_sync(s_mc[u].timer, &none);
        }
    }
    for (int u = 0; u < ZBC_PWM_MCPWM_UNITS; u++) {
        if (s_mc[u].sync) {
            mcpwm_del_sync_src(s_mc[u].sync);
            s_mc[u].sync = NULL;
        }
    }
}

/* Locks every unit to the first unit at its frequency, at its phase deg[u]:
 * at the first's zero the unit is set to where it then is in its own period.
 * A unit already in place is set to the count it has, so locking again after
 * a reload changes nothing; a unit just made takes its place at the first
 * zero. Same clock, same period: once placed, they stay placed. */
static void mc_lock_locked(const uint16_t deg[ZBC_PWM_MCPWM_UNITS])
{
    for (int u = 0; u < ZBC_PWM_MCPWM_UNITS; u++) {
        const uint16_t hz   = s_timer_hz[ZBC_PWM_LEDC_TIMERS + u];
        int            lead = -1;
        for (int v = 0; v < u && lead < 0; v++) {
            if (hz && s_timer_hz[ZBC_PWM_LEDC_TIMERS + v] == hz) {
                lead = v;
            }
        }
        if (lead < 0) {
            continue;
        }
        mc_unit_t *m = &s_mc[lead];
        if (!m->sync) {
            const mcpwm_timer_sync_src_config_t sc = { .timer_event = MCPWM_TIMER_EVENT_EMPTY };
            if (mcpwm_new_timer_sync_src(m->timer, &sc, &m->sync) != ESP_OK) {
                ESP_LOGW(TAG, "MCPWM unit %d cannot lead unit %d: their phases are left to chance", lead, u);
                m->sync = NULL;
                continue;
            }
        }
        /* Its zero `rel` degrees after the lead's: at the lead's zero it is
         * counting down to its own, rel/180 of its peak away - or, past half a
         * period, up from it. */
        const uint32_t                        rel  = (deg[u] + 360u - deg[lead]) % 360u;
        const uint32_t                        peak = s_mc[u].peak;
        const mcpwm_timer_sync_phase_config_t pc   = {
            .sync_src    = m->sync,
            .count_value = rel <= 180 ? peak * rel / 180 : peak * (360 - rel) / 180,
            .direction   = rel > 0 && rel <= 180 ? MCPWM_TIMER_DIRECTION_DOWN : MCPWM_TIMER_DIRECTION_UP,
        };
        mcpwm_timer_set_phase_on_sync(s_mc[u].timer, &pc);
    }
}

/* Binds an MCPWM output to the pin, dark, its pulse centred on its timer's
 * zero or peak (zbc_mcpwm_out): the compare value counting up sets the level
 * the output has at the peak, counting down the level it has at the zero, and
 * the zero and the peak set those again. */
static bool mc_bind_locked(int gpio, int ch, bool active_low, bool at_peak)
{
    mc_unit_t *m = mc_unit_of(ch);
    const int  o = mc_output_of(ch);
    if (!m->timer || m->gen[o]) {
        return false;
    }
    const mcpwm_generator_config_t gc         = { .gen_gpio_num = gpio, .flags.invert_pwm = active_low };
    const mcpwm_generator_action_t at_zero_is = at_peak ? MCPWM_GEN_ACTION_LOW : MCPWM_GEN_ACTION_HIGH;
    const mcpwm_generator_action_t at_peak_is = at_peak ? MCPWM_GEN_ACTION_HIGH : MCPWM_GEN_ACTION_LOW;
    mcpwm_gen_handle_t             g          = NULL;
    esp_err_t                      err        = mcpwm_new_generator(m->oper, &gc, &g);
    if (err == ESP_OK) {
        err = mcpwm_generator_set_force_level(g, 0, true);
    }
    if (err == ESP_OK) {
        err = mcpwm_generator_set_actions_on_compare_event(
            g, MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, m->cmp[o], at_peak_is),
            MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_DOWN, m->cmp[o], at_zero_is),
            MCPWM_GEN_COMPARE_EVENT_ACTION_END());
    }
    if (err == ESP_OK) {
        err = mcpwm_generator_set_actions_on_timer_event(
            g, MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, at_zero_is),
            MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_DOWN, MCPWM_TIMER_EVENT_FULL, at_peak_is),
            MCPWM_GEN_TIMER_EVENT_ACTION_END());
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PWM channel %d (MCPWM) on GPIO %d refused (0x%x)", ch, gpio, err);
        if (g) {
            mcpwm_del_generator(g);
        }
        return false;
    }
    m->gen[o] = g;
    return true;
}

/* Dark, then the output let go. The driver leaves the pin floating; it is
 * driven at the dark level instead, until a channel takes it or the reload
 * hands it back. */
static void mc_unbind_locked(int gpio, int ch, bool active_low)
{
    mc_unit_t *m = mc_unit_of(ch);
    const int  o = mc_output_of(ch);
    if (!m->gen[o]) {
        return;
    }
    mcpwm_generator_set_force_level(m->gen[o], 0, true);
    mcpwm_del_generator(m->gen[o]);
    m->gen[o] = NULL;
    drive(gpio, false, active_low);
    gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
}

/* A channel of a light as it is stopped when not carried over: at the level
 * that leaves the LEDs dark, through whatever inversion is applied. */
static void stop_channel_locked(const live_light_t *l, int k)
{
    if (is_mc_channel(l->ch[k])) {
        mc_unbind_locked(l->pin[k], l->ch[k], l->active_low);
    } else {
        ledc_stop(LEDC_MODE, l->ch[k], l->sent[k].invert ? 1 : 0);
    }
}

void local_lights_reload(void)
{
    if (!s_lock) {
        return;
    }
    /* The table and the lights built from it are ~3.3 KB at eight lights, so they
     * are on the heap for the reload's length, not on the caller's stack: a
     * light's PATCH runs this on httpd's 6 KB, and as locals they made a 3.5 KB
     * frame that the NVS call below it took past the end (bad-og, 2026-10-10:
     * a stack protection fault on every change of a light's mix). */
    struct {
        zbc_light_t  cfg[CONFIG_ZBC_MAX_LIGHTS];
        live_light_t next[CONFIG_ZBC_MAX_LIGHTS];
    } *buf = malloc(sizeof(*buf));
    if (!buf) {
        ESP_LOGW(TAG, "no memory to apply the wired lights' table");
        return;
    }
    zbc_light_t  *cfg  = buf->cfg;
    live_light_t *next = buf->next;
    const size_t  n    = light_db_snapshot(cfg, CONFIG_ZBC_MAX_LIGHTS);

    uint16_t hz[CONFIG_ZBC_MAX_LIGHTS]    = { 0 };
    uint8_t  chans[CONFIG_ZBC_MAX_LIGHTS] = { 0 };
    for (size_t i = 0; i < n; i++) {
        if (pwm_ready(&cfg[i])) {
            hz[i]    = cfg[i].pwm_hz;
            chans[i] = zbc_light_pwm_channels(cfg[i].kind);
        }
    }
    zbc_pwm_slot_t slot[CONFIG_ZBC_MAX_LIGHTS];
    uint16_t       timer_hz[ZBC_PWM_TIMERS];
    const char    *why = zbc_pwm_plan(hz, chans, n, slot, timer_hz);
    if (why) {
        /* The HTTP layer runs the same plan before it stores anything, so this is
         * a table from a build with other limits. Dark rather than half-driven. */
        ESP_LOGW(TAG, "PWM lights left dark: %s", why);
        memset(hz, 0, sizeof(hz));
        zbc_pwm_plan(hz, chans, n, slot, timer_hz);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int64_t now = esp_timer_get_time();
    bool          wanted[SOC_GPIO_PIN_COUNT] = { 0 };
    memset(next, 0, sizeof(buf->next));

    for (size_t i = 0; i < n; i++) {
        const zbc_light_t  *c     = &cfg[i];
        live_light_t       *l     = &next[i];
        const live_light_t *old   = find_locked(c->id);
        const bool          pwm   = is_pwm(c->kind) && pwm_ready(c);
        const bool          plain = c->kind == ZBC_LIGHT_ONOFF && c->gpio >= 0 &&
                                    !local_light_pin_problem(c->gpio);
        if (!pwm && !plain) {
            continue;
        }
        l->used       = true;
        l->id         = c->id;
        l->kind       = c->kind;
        l->gpio       = c->gpio;
        l->active_low = c->active_low;
        l->boot       = c->boot;
        l->mix        = c->mix;
        l->pwm_hz     = pwm ? c->pwm_hz : 0;
        l->cold_m     = 1000000u / c->cold_k;
        l->warm_m     = 1000000u / c->warm_k;
        /* A two-channel light has no neutral; the midpoint keeps the three in order. */
        l->neutral_m  = c->kind == ZBC_LIGHT_CNW && c->neutral_k ? 1000000u / c->neutral_k
                                                                 : (l->cold_m + l->warm_m) / 2;
        memcpy(l->curve, c->curve, sizeof(l->curve));
        memset(l->pin, -1, sizeof(l->pin));
        memset(l->ch, -1, sizeof(l->ch));
        l->nch        = pwm ? (uint8_t)light_channels(c, l->pin, l->role) : 0;
        l->timer      = slot[i].timer;
        for (int k = 0; k < l->nch; k++) {
            l->ch[k] = slot[i].ch[k];
        }
        l->phase_deg  = pwm ? slot[i].phase_deg : 0;
        l->peak       = pwm ? slot[i].peak : 0;
        const bool mc = l->timer >= ZBC_PWM_LEDC_TIMERS;
        if (pwm && (!old || old->timer != l->timer || old->phase_deg != l->phase_deg)) {
            ESP_LOGI(TAG, "Wired light %u: PWM timer %d%s, phase %u deg", (unsigned)c->id, l->timer,
                     mc ? " (MCPWM)" : "", (unsigned)l->phase_deg);
        }
        l->max        = !pwm ? 0 : mc ? zbc_mcpwm_peak(c->pwm_hz, NULL) : (1u << zbc_pwm_resolution(c->pwm_hz));

        const bool same_wiring = old && old->kind == c->kind && old->gpio == l->gpio &&
                                 old->nch == l->nch && memcmp(old->pin, l->pin, sizeof(l->pin)) == 0;
        if (same_wiring) {
            /* Saving the editor must not flash the light. */
            l->on          = old->on;
            l->off_at_end  = old->off_at_end;
            l->moving      = old->moving;
            l->remembered  = old->remembered;
            l->level       = old->level;
            l->mireds      = old->mireds;
            l->dirty_since = old->dirty_since;
            if (pwm) {
                /* The two colour temperatures may have moved. */
                const int64_t m = ramp_now(&l->mireds, now);
                ramp_settle(&l->mireds, m < l->cold_m ? l->cold_m : (m > l->warm_m ? l->warm_m : m));
            }
        } else {
            boot_state(c, l, now);
        }
        if (pwm && same_wiring && old->configured && old->timer == l->timer &&
            memcmp(old->ch, l->ch, sizeof(l->ch)) == 0 && old->pwm_hz == l->pwm_hz &&
            old->peak == l->peak && old->active_low == l->active_low) {
            l->configured = true;
            l->sent_valid = old->sent_valid;
            memcpy(l->sent, old->sent, sizeof(l->sent));
        }
        wanted[l->gpio] = true;
        for (int k = 0; k < l->nch; k++) {
            wanted[l->pin[k]] = true;
        }
    }

    /* Channels that are not carried over are stopped at the level that leaves
     * the LEDs dark - through whatever inversion is currently applied. */
    for (size_t j = 0; j < CONFIG_ZBC_MAX_LIGHTS; j++) {
        const live_light_t *o = &s_live[j];
        if (!o->used || !o->configured) {
            continue;
        }
        bool kept = false;
        for (size_t i = 0; i < n && !kept; i++) {
            kept = next[i].used && next[i].configured && next[i].id == o->id;
        }
        for (int k = 0; !kept && k < o->nch; k++) {
            stop_channel_locked(o, k);
        }
    }
    for (int pin = 0; pin < SOC_GPIO_PIN_COUNT; pin++) {
        if (s_owned[pin] && !wanted[pin]) {
            gpio_reset_pin(pin);
            s_owned[pin] = false;
        }
    }

    /* Every timer on PLL_DIV, never LEDC_AUTO_CLK: the C6's low-speed timers
     * share one clock source, and auto could pick RC_FAST for a low frequency
     * and lock every later timer out ("timer clock conflict"). */
    for (int t = 0; t < ZBC_PWM_LEDC_TIMERS; t++) {
        if (timer_hz[t] == 0 || timer_hz[t] == s_timer_hz[t]) {
            continue;
        }
        const ledc_timer_config_t tc = {
            .speed_mode      = LEDC_MODE,
            .duty_resolution = (ledc_timer_bit_t)zbc_pwm_resolution(timer_hz[t]),
            .timer_num       = (ledc_timer_t)t,
            .freq_hz         = timer_hz[t],
            .clk_cfg         = LEDC_USE_PLL_DIV_CLK,
        };
        const esp_err_t err = ledc_timer_config(&tc);
        if (err == ESP_OK) {
            s_timer_hz[t] = timer_hz[t];
            ESP_LOGI(TAG, "PWM timer %d: %u Hz at %u bits", t, (unsigned)timer_hz[t],
                     (unsigned)tc.duty_resolution);
        } else {
            ESP_LOGW(TAG, "PWM timer %d refused %u Hz (0x%x)", t, (unsigned)timer_hz[t], err);
            s_timer_hz[t] = 0;
        }
    }

    /* MCPWM, started at the first light planned on it: the units' locks let go
     * while a timer is replaced, then made again for the plan's phases. */
    bool mc_wanted = false;
    for (int t = ZBC_PWM_LEDC_TIMERS; t < ZBC_PWM_TIMERS; t++) {
        mc_wanted |= timer_hz[t] != 0;
    }
    if (s_mc_state > 0 || (mc_wanted && mc_start_locked())) {
        uint16_t deg[ZBC_PWM_MCPWM_UNITS] = { 0 };
        for (size_t i = 0; i < n; i++) {
            for (int k = 0; k < chans[i] && slot[i].timer >= ZBC_PWM_LEDC_TIMERS; k++) {
                deg[(slot[i].ch[k] - ZBC_PWM_LEDC_CHANNELS) / 2] = zbc_mcpwm_unit_deg(&slot[i], k);
            }
        }
        mc_unlock_locked();
        for (int u = 0; u < ZBC_PWM_MCPWM_UNITS; u++) {
            const int t = ZBC_PWM_LEDC_TIMERS + u;
            if (timer_hz[t] != s_timer_hz[t]) {
                mc_set_timer_locked(u, timer_hz[t]);
            }
        }
        mc_lock_locked(deg);
    }

    for (size_t i = 0; i < n; i++) {
        live_light_t *l = &next[i];
        if (!l->used) {
            continue;
        }
        if (l->kind == ZBC_LIGHT_ONOFF) {
            /* The level is latched before the pin becomes an output, so a light
             * meant to stay off does not blink on while it is configured. */
            drive(l->gpio, l->on, l->active_low);
            gpio_config_t io = {
                .pin_bit_mask = 1ULL << l->gpio,
                .mode         = GPIO_MODE_OUTPUT,
                .pull_up_en   = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type    = GPIO_INTR_DISABLE,
            };
            if (gpio_config(&io) != ESP_OK) {
                ESP_LOGW(TAG, "GPIO %d refused by the driver", l->gpio);
                l->used = false;
                continue;
            }
            drive(l->gpio, l->on, l->active_low);
            s_owned[l->gpio] = true;
            if (l->boot == ZBC_LIGHT_BOOT_LAST) {
                light_db_save_state(l->id, &(zbc_light_state_t){ .on = l->on });
            }
            continue;
        }
        if (l->configured) {
            continue;
        }
        bool ready = l->timer >= 0;
        for (int k = 0; ready && k < l->nch; k++) {
            ready = s_timer_hz[channel_timer(l, k)] == l->pwm_hz;
        }
        if (!ready) {
            continue;                   /* no channels, or a timer was refused: dark */
        }
        int bound = 0;
        while (bound < l->nch &&
               (is_mc_channel(l->ch[bound])
                    ? mc_bind_locked(l->pin[bound], l->ch[bound], l->active_low, (l->peak >> bound) & 1)
                    : bind_channel(l->pin[bound], l->ch[bound], l->timer, l->active_low))) {
            bound++;
        }
        if (bound < l->nch) {
            /* An MCPWM output stays taken until it is let go. */
            for (int k = 0; k < bound; k++) {
                if (is_mc_channel(l->ch[k])) {
                    mc_unbind_locked(l->pin[k], l->ch[k], l->active_low);
                    s_owned[l->pin[k]] = true;
                }
            }
            continue;
        }
        for (int k = 0; k < l->nch; k++) {
            s_owned[l->pin[k]] = true;
        }
        l->configured = true;
        l->sent_valid = false;
        apply_pwm_locked(l, now);
    }

    _Static_assert(sizeof(s_live) == sizeof(buf->next), "s_live and the reload's lights differ");
    memcpy(s_live, next, sizeof(s_live));
    xSemaphoreGive(s_lock);
    free(buf);
    wake();
}

esp_err_t local_lights_start(void)
{
    if (s_lock) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    /* Above the web server, the Zigbee main loop and the Matter bridge (all 5 or
     * below). A ramp is computed from the clock, so a tick another task holds up
     * is taken as one jump, and near black a jump of a few ticks is several
     * times the light. At priority 3, switching a room on and off over the API
     * left one fade tick in 28 over 20 ms late, and one in 16 with the page
     * polling, the worst 109 ms - an eighth of a 0.8 s fade at once (`wired` in
     * /api/status). A tick is a few
     * hundred microseconds of LEDC writes, only while something moves; Wi-Fi
     * and lwIP stay above it. */
    if (xTaskCreate(fade_task, "lights", 3072, NULL, 6, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    local_lights_reload();
    return ESP_OK;
}

/* ------------------------------------------------------------- interlock */

void local_lights_hold(bool held)
{
    if (!s_lock) {
        s_held = held;          /* before start: reload reads it for the power-on state */
        return;
    }
    uint16_t ids[CONFIG_ZBC_MAX_LIGHTS];
    int      saves = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool changed = s_held != held;
    s_held = held;
    if (held) {
        const int64_t now = esp_timer_get_time();
        for (size_t i = 0; i < CONFIG_ZBC_MAX_LIGHTS; i++) {
            live_light_t *l = &s_live[i];
            if (!l->used || (!l->on && !l->off_at_end)) {
                continue;
            }
            /* At once, not faded: this is a relay on a board that is too hot. */
            l->on         = false;
            l->off_at_end = false;
            if (l->kind == ZBC_LIGHT_ONOFF) {
                drive(l->gpio, false, l->active_low);
                if (l->boot == ZBC_LIGHT_BOOT_LAST) {
                    ids[saves++] = l->id;
                }
            } else {
                ramp_settle(&l->level, (int64_t)l->remembered * 1000);
                apply_pwm_locked(l, now);
                mark_dirty(l, now);
            }
            ESP_LOGW(TAG, "Wired light %u switched off: too hot", (unsigned)l->id);
        }
    }
    xSemaphoreGive(s_lock);
    for (int k = 0; k < saves; k++) {
        light_db_save_state(ids[k], &(zbc_light_state_t){ .on = 0 });
    }
    if (changed) {
        zbc_state_bump();
        wake();
    }
}

bool local_lights_held(void)
{
    return s_held;
}

/* ---------------------------------------------------------------- commands */

/* What a command needs of the light it is for. */
typedef enum {
    NEED_SWITCH,                      /* any light */
    NEED_LEVEL,                       /* a light that dims: tunable white or dimmable */
    NEED_CT,                          /* tunable white, two channels or three */
} need_t;

/* Returns with s_lock held on ESP_OK. */
static esp_err_t lock_light(uint16_t id, need_t need, live_light_t **out)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    live_light_t *l   = find_locked(id);
    esp_err_t     err = ESP_OK;
    if (!l || (is_pwm(l->kind) && !l->configured)) {
        err = light_db_get(id, NULL) ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND;
    } else if ((need == NEED_LEVEL && !is_pwm(l->kind)) || (need == NEED_CT && !zbc_light_tunable(l->kind))) {
        err = ESP_ERR_NOT_SUPPORTED;
    }
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        return err;
    }
    *out = l;
    return ESP_OK;
}

static void unlock_changed(live_light_t *l, int64_t now)
{
    mark_dirty(l, now);
    xSemaphoreGive(s_lock);
    zbc_state_bump();                 /* a wired light is a device the page shows */
    wake();
}

esp_err_t local_light_set(uint16_t id, zb_onoff_cmd_t cmd)
{
    live_light_t   *l;
    const esp_err_t err = lock_light(id, NEED_SWITCH, &l);
    if (err != ESP_OK) {
        return err;
    }
    const int64_t now = esp_timer_get_time();
    const bool    was = l->on && !l->off_at_end;
    const bool    on  = cmd == ZB_ONOFF_TOGGLE ? !was : cmd == ZB_ONOFF_ON;
    if (on && s_held) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_ALLOWED;
    }

    if (l->kind == ZBC_LIGHT_ONOFF) {
        l->on = on;
        drive(l->gpio, on, l->active_low);
        const bool remember = l->boot == ZBC_LIGHT_BOOT_LAST;
        const int  pin      = l->gpio;
        xSemaphoreGive(s_lock);
        if (on != was) {
            ESP_LOGI(TAG, "Wired light %u (GPIO %d) %s", (unsigned)id, pin, on ? "on" : "off");
        }
        if (remember) {
            light_db_save_state(id, &(zbc_light_state_t){ .on = on });
        }
        return ESP_OK;
    }

    l->on         = on;
    l->off_at_end = false;
    ramp_settle(&l->level, (int64_t)l->remembered * 1000);
    unlock_changed(l, now);
    return ESP_OK;
}

esp_err_t local_light_set_level(uint16_t id, uint8_t level, uint16_t transition_ds)
{
    live_light_t   *l;
    const esp_err_t err = lock_light(id, NEED_LEVEL, &l);
    if (err != ESP_OK) {
        return err;
    }
    if (level > 0 && s_held) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_ALLOWED;
    }
    const int64_t now = esp_timer_get_time();
    if (level == 0) {
        if (l->on) {
            ramp_start(&l->level, ramp_now(&l->level, now), 0, transition_ds, now, RAMP_EASED_LIGHT);
            l->off_at_end = true;
        }
    } else {
        /* From dark, the ramp starts at the bottom rather than at the level the
         * light was switched off at: nothing is lit to jump to. That one is a
         * switch-on and eases; from lit it may be one link of a chain. */
        const bool from_dark = !l->on;
        l->on         = true;
        l->off_at_end = false;
        l->remembered = level;
        ramp_start(&l->level, from_dark ? 0 : ramp_now(&l->level, now), (int64_t)level * 1000,
                   transition_ds, now, from_dark ? RAMP_EASED_LIGHT : RAMP_LINEAR);
    }
    unlock_changed(l, now);
    return ESP_OK;
}

esp_err_t local_light_fade_in(uint16_t id, uint8_t level, uint16_t transition_ds)
{
    live_light_t   *l;
    const esp_err_t err = lock_light(id, NEED_LEVEL, &l);
    if (err != ESP_OK) {
        return err;
    }
    if (s_held) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_ALLOWED;
    }
    const int64_t now = esp_timer_get_time();
    l->on         = true;
    l->off_at_end = false;
    l->remembered = level ? level : 1;
    /* From black, not from zb_fade_in's preset to level 1: a wired light has
     * nothing to preset, and level 1 is already a glow at 13 bits. */
    ramp_start(&l->level, 0, (int64_t)l->remembered * 1000, transition_ds, now, RAMP_EASED_LIGHT);
    unlock_changed(l, now);
    return ESP_OK;
}

esp_err_t local_light_fade_off(uint16_t id, uint16_t transition_ds)
{
    live_light_t   *l;
    const esp_err_t err = lock_light(id, NEED_LEVEL, &l);
    if (err != ESP_OK) {
        return err;
    }
    const int64_t now = esp_timer_get_time();
    if (l->on && !l->off_at_end) {
        ramp_start(&l->level, ramp_now(&l->level, now), 0, transition_ds ? transition_ds : FADE_OFF_DS, now,
                   RAMP_EASED_LIGHT);
        l->off_at_end = true;
    }
    unlock_changed(l, now);
    return ESP_OK;
}

esp_err_t local_light_glide(uint16_t id, const uint8_t *level, const uint16_t *mireds, int n,
                            uint16_t transition_ds, bool off)
{
    live_light_t   *l;
    const esp_err_t err = lock_light(id, NEED_LEVEL, &l);
    if (err != ESP_OK) {
        return err;
    }
    if (n < 2 || n > RAMP_GLIDE_POINTS) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_ARG;
    }
    if (!off && s_held) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_ALLOWED;
    }
    const int64_t now = esp_timer_get_time();
    const bool    lit = l->on && !l->off_at_end;
    if (off && !lit) {
        unlock_changed(l, now);       /* dark, or already fading out: as local_light_fade_off */
        return ESP_OK;
    }

    uint16_t pts[RAMP_GLIDE_POINTS];
    for (int i = 0; i < n; i++) {
        pts[i] = level[i];
    }
    /* A lit light carries on from where it is; a dark one starts at black,
     * whatever level it was switched off at. */
    ramp_glide(&l->level, lit ? ramp_now(&l->level, now) : 0, pts, n, 1000, transition_ds, now);
    if (mireds && zbc_light_tunable(l->kind)) {
        for (int i = 0; i < n; i++) {
            const uint32_t m = mireds[i];
            pts[i] = (uint16_t)(m < l->cold_m ? l->cold_m : (m > l->warm_m ? l->warm_m : m));
        }
        /* From dark the colour starts where the path does, at once: nothing is
         * lit to show the jump, and from wherever it was left - the middle of the
         * range after a restart - the dimmest moments would be in that colour. */
        ramp_glide(&l->mireds, lit ? ramp_now(&l->mireds, now) : pts[0], pts, n, 1, transition_ds, now);
    }
    if (off) {
        l->off_at_end = true;         /* keeps the level to come back to */
    } else {
        l->on         = true;
        l->off_at_end = false;
        l->remembered = level[n - 1] ? level[n - 1] : 1;
    }
    unlock_changed(l, now);
    return ESP_OK;
}

esp_err_t local_light_set_ct(uint16_t id, uint16_t mireds, uint16_t transition_ds)
{
    live_light_t   *l;
    const esp_err_t err = lock_light(id, NEED_CT, &l);
    if (err != ESP_OK) {
        return err;
    }
    const int64_t  now = esp_timer_get_time();
    const uint32_t m   = mireds < l->cold_m ? l->cold_m : (mireds > l->warm_m ? l->warm_m : mireds);
    if (l->on && ramp_active(&l->level, now) &&
        (l->level.shape == RAMP_EASED_LIGHT || l->level.shape == RAMP_GLIDE)) {
        /* The colour of a switch-on or a switch-off (a warmth curve's, or Warm
         * while off's) moves with the light, on the fade's clock and curve,
         * whatever transition it was sent with. On its own straight line it
         * would change fastest while the light barely moved and land before it
         * (lighting.c gives a switch-on's colour a Zigbee round trip less), so a
         * light coming on would show its full-brightness colour while still
         * dim. */
        ramp_start(&l->mireds, ramp_now(&l->mireds, now), m, 0, now, RAMP_EASED);
        l->mireds.t1 = l->level.t1;
    } else {
        ramp_start(&l->mireds, ramp_now(&l->mireds, now), m, transition_ds, now, RAMP_LINEAR);
    }
    unlock_changed(l, now);
    return ESP_OK;
}

esp_err_t local_light_stop(uint16_t id)
{
    live_light_t   *l;
    const esp_err_t err = lock_light(id, NEED_LEVEL, &l);
    if (err != ESP_OK) {
        return err;
    }
    const int64_t now = esp_timer_get_time();
    if (!l->off_at_end) {
        const int64_t v = ramp_now(&l->level, now);
        ramp_settle(&l->level, v);
        if (l->on && v >= 1000) {
            l->remembered = (uint8_t)((v + 500) / 1000);
        }
    }
    unlock_changed(l, now);
    return ESP_OK;
}

bool local_light_get(uint16_t id, zbc_local_state_t *out)
{
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const live_light_t *l    = find_locked(id);
    const bool          live = l && (l->kind == ZBC_LIGHT_ONOFF || l->configured);
    if (live && out) {
        const int64_t now = esp_timer_get_time();
        out->on = l->on ? 1 : 0;
        if (is_pwm(l->kind)) {
            int64_t lvl = l->on ? (ramp_now(&l->level, now) + 500) / 1000 : l->remembered;
            out->level  = (int16_t)(lvl < 1 ? 1 : lvl);
            out->mireds = zbc_light_tunable(l->kind) ? (int32_t)ramp_now(&l->mireds, now) : -1;
        } else {
            out->level  = -1;
            out->mireds = -1;
        }
    }
    xSemaphoreGive(s_lock);
    return live;
}

int local_light_pwm_phase(uint16_t id)
{
    if (!s_lock) {
        return -1;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const live_light_t *l   = find_locked(id);
    const int           deg = l && is_pwm(l->kind) && l->configured ? l->phase_deg : -1;
    xSemaphoreGive(s_lock);
    return deg;
}

void local_lights_fade_stats(local_lights_fade_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_fade_stats;
    xSemaphoreGive(s_lock);
}

int local_light_state(uint16_t id)
{
    zbc_local_state_t st;
    return local_light_get(id, &st) ? st.on : -1;
}
