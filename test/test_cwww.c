/* The PWM arithmetic of a tunable white light, from main/local_lights.c.
 *
 * Lifted rather than copied: see test/Makefile. The waveforms are rebuilt count
 * by count from what LEDC would be told, so the assertions are about what comes
 * out of the pins - duty, the half period between the two pulse centres, no
 * pulse running past the end of the period - rather than about the formulas.
 * A fault here is flicker or a channel stuck on, which no log explains. */
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "light_math.h"
#include "pchip.inc"
#include "cwww.inc"

static int  failures;
static long checks;

#define EXPECT(cond, ...) do {                     \
        checks++;                                  \
        if (!(cond)) {                             \
            if (failures < 20) {                   \
                printf("\n  ");                    \
                printf(__VA_ARGS__);               \
            }                                      \
            failures++;                            \
        }                                          \
    } while (0)

static uint32_t rng = 0x2545F491;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

/* A channel's output at counter value k, as the hardware produces it. */
static bool high_at(const zbc_pwm_out_t *o, uint32_t k)
{
    const bool pulse = k >= o->hpoint && k < o->hpoint + o->duty;
    return pulse != o->invert;
}

/* The centre of a channel's high time, on the circle of one period. */
static double centre(const zbc_pwm_out_t *o, uint32_t max)
{
    if (!o->invert) {
        return o->hpoint + o->duty / 2.0;
    }
    return fmod(o->hpoint + o->duty + (max - o->duty) / 2.0, max);
}

static double circular(double a, double b, uint32_t max)
{
    double d = fmod(fabs(a - b), max);
    return d > max / 2.0 ? max - d : d;
}

static void check_place(uint32_t dc, uint32_t dw, uint32_t max, uint32_t shift)
{
    zbc_pwm_out_t c, w;
    zbc_pwm_place(dc, dw, max, shift, &c, &w);
#define AT "dc %u dw %u max %u shift %u: "
#define ARGS dc, dw, max, shift

    EXPECT(c.hpoint < max && w.hpoint < max, AT "hpoint %u/%u out of range", ARGS, c.hpoint, w.hpoint);
    EXPECT(c.hpoint + c.duty <= max, AT "cold runs past the period (%u + %u)", ARGS, c.hpoint, c.duty);
    EXPECT(w.hpoint + w.duty <= max, AT "warm runs past the period (%u + %u)", ARGS, w.hpoint, w.duty);
    /* Dark never needs inverting, so switching off never changes a channel's form. */
    EXPECT(dc > 0 || !c.invert, AT "a dark cold channel was inverted", ARGS);
    EXPECT(dw > 0 || !w.invert, AT "a dark warm channel was inverted", ARGS);
    if (shift == 0) {
        /* Phase 0 is the layout a lone light always had. */
        EXPECT(!c.invert && c.hpoint == 0, AT "cold does not start the period", ARGS);
    }

    uint32_t hc = 0, hw = 0, both = 0;
    for (uint32_t k = 0; k < max; k++) {
        const bool a = high_at(&c, k), b = high_at(&w, k);
        hc += a;
        hw += b;
        both += a && b;
    }
    EXPECT(hc == dc, AT "cold is high for %u counts", ARGS, hc);
    EXPECT(hw == dw, AT "warm is high for %u counts", ARGS, hw);
    if (dc + dw <= max) {
        EXPECT(both == 0, AT "the pulses overlap by %u counts", ARGS, both);
        EXPECT(shift != 0 || !w.invert, AT "inverted although it fits", ARGS);
    }
    if (dc > 0 && dc < max) {
        const double off = circular(centre(&c, max), dc / 2.0 + shift, max);
        EXPECT(off <= 1.0, AT "cold centred %.1f from dc/2 + shift", ARGS, off);
    }
    if (dc > 0 && dc < max && dw > 0 && dw < max) {
        const double off = circular(centre(&c, max), centre(&w, max), max);
        EXPECT(fabs(off - max / 2.0) <= 1.0, AT "centres %.1f apart, not %.1f", ARGS, off, max / 2.0);
    }
#undef AT
#undef ARGS
}

/* As one channel's own duty grows from dark to full around a fixed centre, it
 * changes form - plain to inverted - at most once: every change costs one wrong
 * period, so a fade must not flicker between the two. */
static void check_one_crossing(uint32_t max, uint32_t shift, uint32_t other)
{
    int  flips[2] = { 0, 0 };
    bool was[2]   = { false, false };
    for (uint32_t d = 0; d <= max; d++) {
        zbc_pwm_out_t c, w;
        zbc_pwm_place(d, other, max, shift, &c, &w);  /* cold sweeps, warm fixed */
        if (d > 0 && c.invert != was[0]) {
            flips[0]++;
        }
        was[0] = c.invert;
        zbc_pwm_place(other, d, max, shift, &c, &w);  /* warm sweeps, cold fixed */
        if (d > 0 && w.invert != was[1]) {
            flips[1]++;
        }
        was[1] = w.invert;
    }
    EXPECT(flips[0] <= 1, "max %u shift %u: cold changed form %d times over its range", max, shift, flips[0]);
    EXPECT(flips[1] <= 1, "max %u shift %u other %u: warm changed form %d times over its range",
           max, shift, other, flips[1]);
}

/* Channel counts: every light tunable white, or every light dimmable. */
static const uint8_t CW[16]  = { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 };
static const uint8_t DIM[16] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };

/* The size of harmonic m of the light that n identical lights at the phases
 * zbc_pwm_plan gives put out together, in counts: tunable white at dc and dw,
 * or with `dims`, dimmable at dc (dw unused - a dimmable light is the cold
 * channel alone, as local_lights.c drives it). */
static double ripple_of(bool dims, uint32_t dc, uint32_t dw, uint32_t max, int n, int m)
{
    const uint16_t hz[6] = { 4882, 4882, 4882, 4882, 4882, 4882 };
    zbc_pwm_slot_t s[6];
    uint16_t       th[ZBC_PWM_TIMERS];
    zbc_pwm_plan(hz, dims ? DIM : CW, (size_t)n, s, th);
    double re = 0, im = 0;
    for (int i = 0; i < n; i++) {
        zbc_pwm_out_t c, w;
        zbc_pwm_place(dc, dims ? 0 : dw, max, (uint32_t)((uint64_t)max * s[i].phase_deg / 360), &c, &w);
        for (uint32_t k = 0; k < max; k++) {
            const int    lit = high_at(&c, k) + (dims ? 0 : high_at(&w, k));
            const double a   = 2.0 * M_PI * m * k / max;
            re += lit * cos(a);
            im -= lit * sin(a);
        }
    }
    return sqrt(re * re + im * im);
}

static double ripple(uint32_t dc, uint32_t dw, uint32_t max, int n, int m)
{
    return ripple_of(false, dc, dw, max, n, m);
}

/* An MCPWM output at tick t of a period of 2 * peak ticks, counted from its
 * timer's zero, as local_lights.c sets the generator up: the compare value
 * counting up sets the level it has at the peak and counting down the level it
 * has at the zero, and the zero and the peak set those levels again. */
static bool mc_high_at(const zbc_mcpwm_out_t *o, uint32_t peak, bool at_peak, uint32_t t)
{
    if (o->force >= 0) {
        return o->force == 1;
    }
    const bool     up = t < peak;
    const uint32_t c  = up ? t : 2 * peak - t;
    const bool     z  = up ? c < o->cmp : c <= o->cmp;  /* in the pulse around the zero */
    const bool     p  = up ? c >= o->cmp : c > o->cmp;  /* in the pulse around the peak */
    return at_peak ? p : z;
}

static bool mc_light(const zbc_pwm_slot_t *s)
{
    return s->timer >= ZBC_PWM_LEDC_TIMERS;
}

static int mc_unit(int ch)
{
    return (ch - ZBC_PWM_LEDC_CHANNELS) / 2;
}

/* Every light the plan accepts gets channels of its own on one of the two
 * peripherals, as many as it has, on timers at its frequency. On LEDC no two
 * pulses on one timer are put in the same place. On MCPWM a tunable white
 * light's pair is half a period apart and a third channel where its first is,
 * every unit gets one phase from every channel on it, and without three-channel
 * lights no two pulses at one frequency are put in the same place. A set LEDC
 * alone can run stays on LEDC. */
static void check_plan(const uint16_t hz[], const uint8_t chans[], size_t n)
{
    zbc_pwm_slot_t s[16];
    uint16_t       th[ZBC_PWM_TIMERS];
    if (zbc_pwm_plan(hz, chans, n, s, th) != NULL) {
        return;
    }
    bool     used[ZBC_PWM_CHANNELS] = { 0 };
    uint16_t at[ZBC_PWM_TIMERS][ZBC_PWM_CHANNELS];
    int      count[ZBC_PWM_TIMERS] = { 0 };
    int      unit_deg[ZBC_PWM_MCPWM_UNITS] = { -1, -1, -1 };
    int      total_ch = 0, freqs = 0;
    bool     on_mc = false, three_on_mc = false;
    for (size_t i = 0; i < n; i++) {
        bool seen = !hz[i];
        for (size_t j = 0; j < i && !seen; j++) {
            seen = hz[j] == hz[i];
        }
        freqs += !seen;
        if (!hz[i]) {
            EXPECT(s[i].timer < 0, "light %zu takes no PWM and was given a timer", i);
            continue;
        }
        total_ch += chans[i];
        const bool mc = mc_light(&s[i]);
        const int  t  = s[i].timer;
        on_mc       |= mc;
        three_on_mc |= mc && chans[i] == 3;
        EXPECT(t >= 0 && t < ZBC_PWM_TIMERS && th[t] == hz[i], "light %zu: timer %d at the wrong frequency", i, t);
        for (int k = 0; k < ZBC_PWM_MAX_LIGHT_CH; k++) {
            const int c = s[i].ch[k];
            if (k >= chans[i]) {
                EXPECT(c == -1, "light %zu with %u channels was given a channel %d", i, chans[i], k);
                continue;
            }
            EXPECT(c >= 0 && c < ZBC_PWM_CHANNELS && !used[c], "light %zu: channel %d reused", i, c);
            if (c < 0 || c >= ZBC_PWM_CHANNELS) {
                continue;
            }
            used[c] = true;
            EXPECT(mc == (c >= ZBC_PWM_LEDC_CHANNELS), "light %zu: channel %d on the other peripheral", i, c);
            if (!mc) {
                continue;
            }
            const int u = mc_unit(c);
            EXPECT(th[ZBC_PWM_LEDC_TIMERS + u] == hz[i], "light %zu: channel %d on a unit at %u Hz", i, c,
                   th[ZBC_PWM_LEDC_TIMERS + u]);
            const int d = zbc_mcpwm_unit_deg(&s[i], k);
            EXPECT(unit_deg[u] < 0 || unit_deg[u] == d, "unit %d asked for %d and %d degrees", u, unit_deg[u], d);
            unit_deg[u] = d;
        }
        EXPECT(s[i].phase_deg < 360, "light %zu: phase %u out of range", i, s[i].phase_deg);
        if (!mc) {
            EXPECT(s[i].peak == 0, "light %zu on LEDC has MCPWM centres", i);
            if (chans[i] == 3) {
                EXPECT(s[i].ch[1] == s[i].ch[0] + 1 && s[i].ch[2] == s[i].ch[0] + 2,
                       "light %zu: channels %d %d %d not in a row", i, s[i].ch[0], s[i].ch[1], s[i].ch[2]);
            }
            EXPECT(chans[i] == 1 || s[i].phase_deg < 180, "light %zu: phase %u out of range", i, s[i].phase_deg);
            at[t][count[t]++] = s[i].phase_deg;
            if (chans[i] >= 2) {      /* a three-channel light shows two pulses in a constant mix */
                at[t][count[t]++] = (uint16_t)((s[i].phase_deg + 180) % 360);
            }
            continue;
        }
        EXPECT(mc_unit(s[i].ch[0]) == t - ZBC_PWM_LEDC_TIMERS, "light %zu: timer %d is not its first unit", i, t);
        if (chans[i] >= 2) {
            EXPECT(mc_unit(s[i].ch[1]) == mc_unit(s[i].ch[0]), "light %zu: its pair is on two units", i);
            EXPECT(((s[i].peak >> 0) & 1) != ((s[i].peak >> 1) & 1), "light %zu: its pair is not half a period apart", i);
        }
        if (chans[i] == 3) {
            EXPECT(((s[i].peak >> 2) & 1) == 0 && (s[i].peak & 1) == 0,
                   "light %zu: its third channel is not where its first is", i);
        }
    }
    for (int t = 0; t < ZBC_PWM_LEDC_TIMERS; t++) {
        for (int a = 0; a < count[t]; a++) {
            for (int b = a + 1; b < count[t]; b++) {
                EXPECT(at[t][a] != at[t][b], "timer %d: two pulses at %u degrees", t, at[t][a]);
            }
        }
    }
    /* Pulses at one frequency on MCPWM, from the units' phases and centres. */
    for (size_t i = 0; i < n && !three_on_mc; i++) {
        if (!hz[i] || !mc_light(&s[i])) {
            continue;
        }
        for (size_t j = i; j < n; j++) {
            if (hz[j] != hz[i] || !mc_light(&s[j])) {
                continue;
            }
            for (int a = 0; a < chans[i]; a++) {
                for (int b = 0; b < chans[j]; b++) {
                    if (j == i && b <= a) {
                        continue;
                    }
                    const int ca = (unit_deg[mc_unit(s[i].ch[a])] + ((s[i].peak >> a) & 1) * 180) % 360;
                    const int cb = (unit_deg[mc_unit(s[j].ch[b])] + ((s[j].peak >> b) & 1) * 180) % 360;
                    EXPECT(ca != cb, "lights %zu and %zu: two MCPWM pulses at %d degrees", i, j, ca);
                }
            }
        }
    }
    EXPECT(!on_mc || total_ch > ZBC_PWM_LEDC_CHANNELS || freqs > ZBC_PWM_LEDC_TIMERS,
           "%zu lights on %d channels at %d frequencies went to MCPWM", n, total_ch, freqs);
}

/* The size of harmonic m of the light the MCPWM lights of a plan put out
 * together, channel k of light i at d[i][k] counts of `peak`, every unit at the
 * phase the plan asks for, over a period of 2 * peak ticks. */
static double mc_ripple(const zbc_pwm_slot_t s[], const uint8_t chans[], size_t n, const uint32_t d[][3],
                        uint32_t peak, int m)
{
    const uint32_t period = 2 * peak;
    double         re = 0, im = 0;
    for (size_t i = 0; i < n; i++) {
        if (!mc_light(&s[i])) {
            continue;
        }
        for (int k = 0; k < chans[i]; k++) {
            const bool            at_peak = (s[i].peak >> k) & 1;
            const zbc_mcpwm_out_t o       = zbc_mcpwm_out(d[i][k], peak, at_peak);
            const uint32_t        zero    = (uint32_t)((uint64_t)period * zbc_mcpwm_unit_deg(&s[i], k) / 360);
            for (uint32_t t = 0; t < period; t++) {
                if (mc_high_at(&o, peak, at_peak, (t + period - zero) % period)) {
                    const double a = 2.0 * M_PI * m * t / period;
                    re += cos(a);
                    im -= sin(a);
                }
            }
        }
    }
    return sqrt(re * re + im * im);
}

/* Every sequence of lights of one to three channels, at most `left` channels
 * more after the n in ch[], planned at one frequency: all must fit. Returns how
 * many it planned. */
static int every_mix(unsigned left, uint8_t ch[12], size_t n)
{
    int tried = 0;
    if (n > 0) {
        const uint16_t hz[12] = { 2441, 2441, 2441, 2441, 2441, 2441, 2441, 2441, 2441, 2441, 2441, 2441 };
        zbc_pwm_slot_t s[12];
        uint16_t       th[ZBC_PWM_TIMERS];
        const char    *why = zbc_pwm_plan(hz, ch, n, s, th);
        EXPECT(why == NULL, "%zu lights on %u channels at one frequency were refused (%s)", n,
               ZBC_PWM_CHANNELS - left, why ? why : "");
        check_plan(hz, ch, n);
        tried++;
    }
    for (uint8_t c = 1; c <= 3 && c <= left && n < 12; c++) {
        ch[n] = c;
        tried += every_mix(left - c, ch, n + 1);
    }
    return tried;
}

/* The phases zbc_pwm_plan gives n lights at one frequency, against `want`. */
static void check_phases(const uint8_t chans[], size_t n, const uint16_t want[], const char *what)
{
    const uint16_t hz[8] = { 4882, 4882, 4882, 4882, 4882, 4882, 4882, 4882 };
    zbc_pwm_slot_t s[8];
    uint16_t       th[ZBC_PWM_TIMERS];
    const char    *why = zbc_pwm_plan(hz, chans, n, s, th);
    EXPECT(why == NULL, "%s: refused (%s)", what, why ? why : "");
    for (size_t i = 0; i < n && !why; i++) {
        EXPECT(s[i].phase_deg == want[i], "%s: light %zu at %u degrees, not %u", what, i, s[i].phase_deg, want[i]);
    }
}

int main(void)
{
    /* ---- placement ---- */
    /* At every phase a light can be given (0, 60, 90, 120 degrees), and some others. */
    const uint32_t shifts64[] = { 0, 1, 10, 16, 21, 31, 32, 42, 63 };
    for (size_t s = 0; s < sizeof(shifts64) / sizeof(shifts64[0]); s++) {
        for (uint32_t dc = 0; dc <= 64; dc++) {       /* every pair at 6 bits */
            for (uint32_t dw = 0; dw <= 64; dw++) {
                check_place(dc, dw, 64, shifts64[s]);
            }
        }
        for (uint32_t o = 0; o <= 64; o += 8) {
            check_one_crossing(64, shifts64[s], o);
        }
    }
    for (int i = 0; i < 3000; i++) {                  /* 13 bits: 9765 Hz */
        const uint32_t max = 8192;
        check_place(rnd() % (max + 1), rnd() % (max + 1), max, i % 3 ? rnd() % max : 0);
    }
    for (int i = 0; i < 240; i++) {                   /* 16 bits: ~1 kHz */
        const uint32_t max = 65536;
        check_place(rnd() % (max + 1), rnd() % (max + 1), max, i % 3 ? rnd() % max : 0);
    }
    /* The two edges of the crossing into the inverted form. */
    for (uint32_t dc = 1; dc < 8192; dc += 97) {
        for (uint32_t shift = 0; shift < 8192; shift += 1365) {
            check_place(dc, 8192 - dc, 8192, shift);
            check_place(dc, 8193 - dc > 8192 ? 8192 : 8193 - dc, 8192, shift);
        }
    }
    check_one_crossing(4096, 4096 / 4, 1000);
    check_one_crossing(4096, 4096 / 6, 3000);
    check_one_crossing(4096, 4096 / 3, 0);

    /* ---- the interpolation every curve uses ---- */
    {
        const uint8_t xs[][6] = { { 1, 100 }, { 1, 30, 60, 100 }, { 1, 2, 3, 50, 99, 100 }, { 1, 62, 77, 83, 95, 100 } };
        const int     ns[]    = { 2, 4, 6, 6 };
        for (int r = 0; r < 400; r++) {
            const int c = r % 4, n = ns[c];
            float     ys[6];
            for (int i = 0; i < n; i++) {
                ys[i] = (float)(rnd() % 101);
            }
            for (int i = 0; i < n; i++) {
                EXPECT(fabsf(zbc_pchip(xs[c], ys, n, xs[c][i]) - ys[i]) < 1e-3f, "pchip misses its point %d", i);
            }
            /* Inside the box of the two points either side of every segment. */
            for (int i = 0; i + 1 < n; i++) {
                const float lo = fminf(ys[i], ys[i + 1]), hi = fmaxf(ys[i], ys[i + 1]);
                for (float t = xs[c][i]; t <= xs[c][i + 1]; t += 0.25f) {
                    const float v = zbc_pchip(xs[c], ys, n, t);
                    if (v < lo - 1e-3f || v > hi + 1e-3f) {
                        EXPECT(false, "pchip leaves its box: %.2f at %.2f, between %.0f and %.0f", v, t, lo, hi);
                        break;
                    }
                }
            }
        }
    }

    /* ---- three LEDs: constant total and full output ---- */
    {
        const uint32_t cold_m = 154, neutral_m = 250, warm_m = 370;    /* 6500, 4000, 2700 K */
        for (uint32_t duty = 0; duty <= 8192; duty += 512) {
            uint32_t prev[3] = { 0, 0, 0 };
            for (uint32_t m = 100; m <= 450; m++) {
                uint32_t o[3];
                zbc_mix3(duty, m, cold_m, neutral_m, warm_m, ZBC_MIX_CONSTANT, o);
                EXPECT(o[0] + o[1] + o[2] == duty, "constant: duty %u m %u gave %u + %u + %u", duty, m, o[0], o[1], o[2]);
                EXPECT(o[0] == 0 || o[2] == 0, "constant: duty %u m %u lights cold and warm together", duty, m);
                if (m > 100) {
                    /* Cold only falls and warm only rises as it gets warmer; neutral
                     * rises up to its own LEDs and falls after them. */
                    EXPECT(o[0] <= prev[0] && o[2] >= prev[2], "constant: duty %u not monotonic at m %u", duty, m);
                    EXPECT(m <= neutral_m ? o[1] >= prev[1] : o[1] <= prev[1],
                           "constant: duty %u neutral turns the wrong way at m %u", duty, m);
                    /* No jump: the LEDs move by at most a ninetieth of the duty per mired. */
                    for (int k = 0; k < 3; k++) {
                        const uint32_t step = o[k] > prev[k] ? o[k] - prev[k] : prev[k] - o[k];
                        EXPECT(step <= duty / 90 + 1, "constant: duty %u channel %d jumps %u at m %u", duty, k, step, m);
                    }
                }
                memcpy(prev, o, sizeof(prev));

                zbc_mix3(duty, m, cold_m, neutral_m, warm_m, ZBC_MIX_FULL, o);
                EXPECT(o[0] <= duty && o[1] <= duty && o[2] <= duty, "full: duty %u m %u over the duty", duty, m);
            }
            uint32_t o[3];
            zbc_mix3(duty, cold_m, cold_m, neutral_m, warm_m, ZBC_MIX_CONSTANT, o);
            EXPECT(o[0] == duty && o[1] == 0 && o[2] == 0, "the cold end is not pure cold (%u %u %u)", o[0], o[1], o[2]);
            zbc_mix3(duty, neutral_m, cold_m, neutral_m, warm_m, ZBC_MIX_CONSTANT, o);
            EXPECT(o[0] == 0 && o[1] == duty && o[2] == 0, "the neutral LEDs' own is not pure neutral (%u %u %u)",
                   o[0], o[1], o[2]);
            zbc_mix3(duty, warm_m, cold_m, neutral_m, warm_m, ZBC_MIX_CONSTANT, o);
            EXPECT(o[0] == 0 && o[1] == 0 && o[2] == duty, "the warm end is not pure warm (%u %u %u)", o[0], o[1], o[2]);
            zbc_mix3(duty, (cold_m + neutral_m) / 2, cold_m, neutral_m, warm_m, ZBC_MIX_FULL, o);
            EXPECT(o[0] == duty && o[1] == duty && o[2] == 0, "full: cold and neutral not both full half way");
        }
    }

    /* ---- drawn mixing curves ---- */
    {
        const uint32_t cold_m = 154, warm_m = 370;
        EXPECT(zbc_mix_position(warm_m, cold_m, warm_m) == 1.0f, "the warm LEDs are not at 1");
        EXPECT(zbc_mix_position(cold_m, cold_m, warm_m) == 100.0f, "the cold LEDs are not at 100");
        EXPECT(zbc_mix_position(500, cold_m, warm_m) == 1.0f && zbc_mix_position(100, cold_m, warm_m) == 100.0f,
               "a colour temperature outside the LEDs is not clamped");
        EXPECT(fabsf(zbc_mix_position(262, cold_m, warm_m) - 50.5f) < 0.01f, "half way in mireds is not 50.5");

        /* The Hue preset, as the page ships it. */
        const zbc_mix_curve_t hue[3] = {
            { 6, { 1, 23, 50, 62, 68, 100 }, { 0, 10, 45, 81, 100, 100 } },    /* cold */
            { 6, { 1, 14, 53, 65, 77, 100 }, { 0, 14, 75, 100, 71, 10 } },     /* neutral */
            { 6, { 1, 62, 77, 83, 95, 100 }, { 100, 100, 50, 32, 6, 0 } },     /* warm */
        };
        for (int c = 0; c < 3; c++) {
            EXPECT(zbc_mix_curve_valid(&hue[c]), "the Hue preset's channel %d is not a valid curve", c);
        }
        for (uint32_t m = cold_m; m <= warm_m; m++) {
            for (uint32_t duty = 1; duty <= 16384; duty *= 4) {
                uint32_t o[3];
                zbc_mix_custom(duty, m, cold_m, warm_m, hue, 3, o);
                const float at = zbc_mix_position(m, cold_m, warm_m);
                for (int c = 0; c < 3; c++) {
                    float ys[6];
                    for (int k = 0; k < 6; k++) {
                        ys[k] = hue[c].y[k];
                    }
                    const float want = duty * zbc_pchip(hue[c].x, ys, 6, at) / 100.0f;
                    if (fabsf((float)o[c] - want) > 1.0f || o[c] > duty) {
                        EXPECT(false, "custom: m %u duty %u channel %d gave %u, not %.1f", m, duty, c, o[c], want);
                    }
                }
            }
        }
        uint32_t o[3];
        zbc_mix_custom(1000, warm_m, cold_m, warm_m, hue, 3, o);
        EXPECT(o[0] == 0 && o[1] == 0 && o[2] == 1000, "the Hue preset at its warm end gave %u %u %u", o[0], o[1], o[2]);
        zbc_mix_custom(1000, cold_m, cold_m, warm_m, hue, 3, o);
        EXPECT(o[0] == 1000 && o[1] == 100 && o[2] == 0, "the Hue preset at its cold end gave %u %u %u", o[0], o[1], o[2]);
        const zbc_mix_curve_t none[3] = { { 0 }, { 0 }, { 0 } };
        zbc_mix_custom(1000, 262, cold_m, warm_m, none, 3, o);
        EXPECT(o[0] == 0 && o[1] == 0 && o[2] == 0, "a channel with no curve is lit");

        /* What a stored curve may be: the brightness curve's rules. */
        EXPECT(zbc_mix_curve_valid(&(zbc_mix_curve_t){ 0 }), "no curve was refused");
        EXPECT(!zbc_mix_curve_valid(&(zbc_mix_curve_t){ 1, { 1 }, { 50 } }), "one point was accepted");
        EXPECT(!zbc_mix_curve_valid(&(zbc_mix_curve_t){ 2, { 2, 100 }, { 0, 100 } }), "a curve from 2 was accepted");
        EXPECT(!zbc_mix_curve_valid(&(zbc_mix_curve_t){ 2, { 1, 99 }, { 0, 100 } }), "a curve to 99 was accepted");
        EXPECT(!zbc_mix_curve_valid(&(zbc_mix_curve_t){ 3, { 1, 50, 50 }, { 0, 1, 2 } }), "a repeated x was accepted");
        EXPECT(!zbc_mix_curve_valid(&(zbc_mix_curve_t){ 2, { 1, 100 }, { 0, 101 } }), "101 %% was accepted");
        EXPECT(!zbc_mix_curve_valid(&(zbc_mix_curve_t){ 7, { 1, 2, 3, 4, 5, 100 }, { 0 } }), "seven points were accepted");
    }

    /* ---- three pulses in one period ---- */
    for (int r = 0; r < 20000; r++) {
        const uint32_t max   = r % 2 ? 4096 : 64;
        const uint32_t shift = r % 3 ? rnd() % max : 0;
        uint32_t       d[3];
        for (int k = 0; k < 3; k++) {
            d[k] = r % 5 == 0 ? 0 : rnd() % (max + 1);   /* some dark ones */
        }
        if (r % 7 == 0) {
            d[rnd() % 3] = max;                          /* and some full ones */
        }
        zbc_pwm_out_t o[3];
        zbc_pwm_place_n(d, 3, max, shift, o);
        uint32_t lit = 0, total = 0, high[3] = { 0, 0, 0 }, overlap = 0;
        for (int k = 0; k < 3; k++) {
            EXPECT(o[k].hpoint < max && o[k].hpoint + o[k].duty <= max, "3ch: channel %d runs past the period", k);
            EXPECT(d[k] > 0 || !o[k].invert, "3ch: a dark channel was inverted");
            lit += d[k] > 0;
            total += d[k];
        }
        for (uint32_t t = 0; t < max; t++) {
            int n = 0;
            for (int k = 0; k < 3; k++) {
                const bool h = high_at(&o[k], t);
                high[k] += h;
                n += h;
            }
            overlap += n > 1;
        }
        for (int k = 0; k < 3; k++) {
            EXPECT(high[k] == d[k], "3ch: channel %d high for %u counts, not %u", k, high[k], d[k]);
        }
        if (total <= max) {
            EXPECT(overlap == 0, "3ch: %u, %u, %u in %u overlap by %u counts", d[0], d[1], d[2], max, overlap);
        }
        /* The dark time between lit pulses, each to the next, is shared evenly. */
        if (lit >= 2 && total <= max) {
            double starts[3];
            int    m = 0;
            for (int k = 0; k < 3; k++) {
                if (d[k] > 0 && d[k] < max) {
                    starts[m++] = fmod(centre(&o[k], max) - d[k] / 2.0 + max, max);
                }
            }
            if (m == (int)lit) {
                const double gap = (double)(max - total) / lit;
                int          j   = 0;
                for (int k = 0; k < 3; k++) {
                    if (d[k] == 0) {
                        continue;
                    }
                    const double end  = starts[j] + d[k];
                    const double next = starts[(j + 1) % m];
                    const double g    = fmod(next - end + 2.0 * max, max);
                    if (fabs(g - gap) > 1.0 && fabs(g - gap - max) > 1.0) {
                        EXPECT(false, "3ch: %u, %u, %u in %u: a gap of %.1f, not %.1f", d[0], d[1], d[2], max, g, gap);
                        break;
                    }
                    j++;
                }
            }
        }
    }

    /* ---- several lights on one timer ---- */
    /* Identical lights in one room, spread over the period: their combined light
     * ripples at a higher harmonic than one light's. One light's pair cancels the
     * first harmonic only when the two channels are equal; two lights at 0 and 90
     * degrees cancel the second whatever the mix, and three at 0, 60 and 120 the
     * second and fourth. */
    const uint32_t pairs[][2] = { { 700, 700 }, { 1500, 300 }, { 200, 2900 }, { 3000, 600 }, { 1800, 1800 } };
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        const uint32_t dc = pairs[i][0], dw = pairs[i][1], max = 4096;
        const double   one = ripple(dc, dw, max, 1, 2);
        EXPECT(one > 50, "dc %u dw %u: one light has no second harmonic to cancel (%.1f)", dc, dw, one);
        EXPECT(ripple(dc, dw, max, 2, 2) < 1e-6 * max,
               "dc %u dw %u: two lights keep %.2f of the second harmonic", dc, dw, ripple(dc, dw, max, 2, 2));
        EXPECT(ripple(dc, dw, max, 3, 2) < 0.02 * one,
               "dc %u dw %u: three lights keep %.2f of %.2f", dc, dw, ripple(dc, dw, max, 3, 2), one);
        EXPECT(ripple(dc, dw, max, 3, 4) < 0.02 * ripple(dc, dw, max, 1, 4) + 1,
               "dc %u dw %u: three lights keep the fourth harmonic", dc, dw);
        if (dc == dw) {
            for (int m = 1; m <= 3; m++) {
                EXPECT(ripple(dc, dw, max, 2, m) < 1e-6 * max, "dc %u dw %u: harmonic %d left by two", dc, dw, m);
            }
        }
    }

    /* ---- mixing ---- */
    const uint32_t cold_m = 154, warm_m = 370;        /* 6500 K and 2700 K */
    for (uint32_t duty = 0; duty <= 8192; duty += 1024) {
        uint32_t pc = 0, pw = 0, fc = 0, fw = 0;
        for (uint32_t m = 100; m <= 450; m++) {
            uint32_t dc, dw;
            zbc_cwww_mix(duty, m, cold_m, warm_m, ZBC_MIX_CONSTANT, &dc, &dw);
            EXPECT(dc + dw == duty, "constant: duty %u m %u gave %u + %u", duty, m, dc, dw);
            if (m > 100) {
                EXPECT(dc <= pc && dw >= pw, "constant: duty %u not monotonic at m %u", duty, m);
            }
            pc = dc;
            pw = dw;

            zbc_cwww_mix(duty, m, cold_m, warm_m, ZBC_MIX_FULL, &dc, &dw);
            EXPECT(dc <= duty && dw <= duty, "full: duty %u m %u over the duty (%u, %u)", duty, m, dc, dw);
            if (m > 100) {
                EXPECT(dc <= fc && dw >= fw, "full: duty %u not monotonic at m %u", duty, m);
            }
            fc = dc;
            fw = dw;
        }
        uint32_t dc, dw;
        zbc_cwww_mix(duty, cold_m, cold_m, warm_m, ZBC_MIX_CONSTANT, &dc, &dw);
        EXPECT(dc == duty && dw == 0, "constant: the cold end is not pure cold (%u, %u)", dc, dw);
        zbc_cwww_mix(duty, warm_m, cold_m, warm_m, ZBC_MIX_CONSTANT, &dc, &dw);
        EXPECT(dc == 0 && dw == duty, "constant: the warm end is not pure warm (%u, %u)", dc, dw);
        zbc_cwww_mix(duty, (cold_m + warm_m) / 2, cold_m, warm_m, ZBC_MIX_FULL, &dc, &dw);
        EXPECT(dc == duty && dw == duty, "full: the middle is not both at full (%u, %u of %u)", dc, dw, duty);
        zbc_cwww_mix(duty, cold_m, cold_m, warm_m, ZBC_MIX_FULL, &dc, &dw);
        EXPECT(dc == duty && dw == 0, "full: the cold end is not pure cold (%u, %u)", dc, dw);
    }
    {
        uint32_t dc, dw;                              /* a degenerate range does not blow up */
        zbc_cwww_mix(1000, 250, 250, 250, ZBC_MIX_CONSTANT, &dc, &dw);
        EXPECT(dc + dw == 1000, "a zero-width range lost duty (%u, %u)", dc, dw);
    }

    /* ---- the brightness curve ---- */
    const uint32_t maxes[] = { 4096, 8192, 16384, 65536, 1u << 19 };
    for (size_t i = 0; i < sizeof(maxes) / sizeof(maxes[0]); i++) {
        const uint32_t max  = maxes[i];
        uint32_t       prev = 0;
        EXPECT(zbc_level_to_duty(0, max) == 0, "max %u: level 0 is not dark", max);
        EXPECT(zbc_level_to_duty(1, max) >= 1, "max %u: the faintest level is dark", max);
        EXPECT(zbc_level_to_duty(1000, max) >= 1, "max %u: level 1 is dark", max);
        EXPECT(zbc_level_to_duty(254000, max) == max, "max %u: level 254 is not full", max);
        for (uint32_t lm = 0; lm <= 254000; lm++) {
            const uint32_t d = zbc_level_to_duty(lm, max);
            if (d < prev || d > max) {
                EXPECT(false, "max %u: level %u.%03u gave %u after %u", max, lm / 1000, lm % 1000, d, prev);
                break;
            }
            prev = d;
        }
        /* L* 50 is 18.4 % of the light: the curve is perceptual, not linear. */
        const double half = (double)zbc_level_to_duty(127000, max) / max;
        EXPECT(fabs(half - 0.1842) < 0.005, "max %u: level 127 gave %.4f, not about 0.184", max, half);
    }

    /* ---- resolution ---- */
    /* The light page's presets - ESPHome's recommended frequencies and three
     * below them: each is the fastest at its resolution, so one hertz more
     * loses a bit. */
    const struct { uint32_t hz; uint8_t bits; } picks[] = {
        { 152, 19 }, { 305, 18 }, { 610, 17 },
        { 1220, 16 }, { 2441, 15 }, { 4882, 14 }, { 9765, 13 }, { 19531, 12 },
    };
    for (size_t i = 0; i < sizeof(picks) / sizeof(picks[0]); i++) {
        const uint32_t hz = picks[i].hz;
        EXPECT(zbc_pwm_resolution(hz) == picks[i].bits, "%u Hz is %u bits, not %u",
               hz, zbc_pwm_resolution(hz), picks[i].bits);
        EXPECT(zbc_pwm_resolution(hz + 1) == picks[i].bits - 1, "%u Hz is %u bits, not %u",
               hz + 1, zbc_pwm_resolution(hz + 1), picks[i].bits - 1);
    }
    EXPECT(zbc_pwm_resolution(1000) == 16, "1000 Hz is %u bits, not 16", zbc_pwm_resolution(1000));
    EXPECT(zbc_pwm_resolution(100) == 19, "100 Hz is %u bits, not 19", zbc_pwm_resolution(100));
    EXPECT(zbc_pwm_resolution(50) == 20, "50 Hz is %u bits, not capped at 20", zbc_pwm_resolution(50));
    for (uint32_t hz = ZBC_PWM_HZ_MIN; hz <= ZBC_PWM_HZ_MAX; hz++) {
        const uint8_t  r   = zbc_pwm_resolution(hz);
        const uint64_t div = (uint64_t)ZBC_PWM_CLK_HZ / ((uint64_t)hz << r);
        if (div < 1 || (r < ZBC_PWM_RES_MAX && (uint64_t)ZBC_PWM_CLK_HZ / ((uint64_t)hz << (r + 1)) >= 1)) {
            EXPECT(false, "%u Hz: %u bits is not the finest the clock allows", hz, r);
            break;
        }
    }

    /* ---- MCPWM's counts and outputs ---- */
    {
        const struct { uint32_t hz, div, peak; } at[] = {
            { 19531, 1, 4096 }, { 4882, 1, 16387 }, { 1221, 1, 65520 }, { 1220, 2, 32787 }, { 100, 13, 61538 },
        };
        for (size_t i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
            uint32_t       div;
            const uint32_t peak = zbc_mcpwm_peak(at[i].hz, &div);
            EXPECT(peak == at[i].peak && div == at[i].div, "%u Hz on MCPWM: %u counts at a divider of %u, not %u at %u",
                   at[i].hz, peak, div, at[i].peak, at[i].div);
        }
        EXPECT(zbc_mcpwm_peak(0, NULL) == 0, "0 Hz has MCPWM counts");
        for (uint32_t hz = ZBC_PWM_HZ_MIN; hz <= ZBC_PWM_HZ_MAX; hz++) {
            uint32_t       div;
            const uint32_t peak  = zbc_mcpwm_peak(hz, &div);
            const double   tick  = (double)(ZBC_PWM_MCPWM_CLK_HZ / div);
            const double   error = fabs(tick / (2.0 * peak) - hz) / hz;
            const bool     finer = div == 1 || (ZBC_PWM_MCPWM_CLK_HZ / (div - 1) + hz) / (2 * hz) > ZBC_PWM_MCPWM_PEAK_MAX;
            const bool     ledc  = hz < 1221 ? peak > 32768 : peak >= (1u << zbc_pwm_resolution(hz));
            if (peak > ZBC_PWM_MCPWM_PEAK_MAX || div < 1 || div > 256 || error > 0.5 / peak + 1e-12 || !finer || !ledc) {
                EXPECT(false, "%u Hz on MCPWM: %u counts at a divider of %u, %.5f %% off", hz, peak, div, 100 * error);
                break;
            }
        }

        /* Rebuilt tick by tick: 2d ticks high, around the zero or the peak; dark
         * and full forced. */
        const uint32_t peaks[] = { 2, 3, 64, 4096, 16387, 65535 };
        for (size_t p = 0; p < sizeof(peaks) / sizeof(peaks[0]); p++) {
            const uint32_t peak = peaks[p];
            for (int r = 0; r < 400; r++) {
                const uint32_t d = peak <= 64 ? (uint32_t)r % (peak + 1) : (r < 2 ? (uint32_t)r * peak : rnd() % peak);
                for (int at_peak = 0; at_peak < 2; at_peak++) {
                    const zbc_mcpwm_out_t o = zbc_mcpwm_out(d, peak, at_peak);
                    EXPECT(d == 0 ? o.force == 0 : d >= peak ? o.force == 1 : o.force == -1 && o.cmp > 0 && o.cmp < peak,
                           "peak %u d %u: forced %d, compare %u", peak, d, o.force, o.cmp);
                    for (uint32_t t = 0; t < 2 * peak; t++) {
                        const bool want = at_peak ? t + d >= peak && t < peak + d : t < d || t >= 2 * peak - d;
                        if (mc_high_at(&o, peak, at_peak, t) != want) {
                            EXPECT(false, "peak %u d %u %s: tick %u is %s", peak, d, at_peak ? "on the peak" : "on the zero",
                                   t, want ? "low" : "high");
                            break;
                        }
                    }
                }
            }
        }

        /* Lights on MCPWM cancel each other's ripple as they do on LEDC: two
         * tunable white lights at 0 and 90 degrees the second harmonic, three
         * at 0, 60 and 120 the second and fourth, two dimmable lights on one
         * unit the first. A peak that 60 degrees divides into whole ticks. */
        const uint16_t f[12]  = { 4882, 4882, 4882, 4882, 4882, 4882, 4882, 4882, 4882, 4882, 4882, 4882 };
        const uint32_t peak   = 4320;
        zbc_pwm_slot_t s[12];
        uint16_t       th[ZBC_PWM_TIMERS];
        uint32_t       d[12][3];
        const uint32_t pairs_mc[][2] = { { 700, 700 }, { 1500, 300 }, { 200, 2900 }, { 3000, 600 } };
        for (size_t i = 0; i < sizeof(pairs_mc) / sizeof(pairs_mc[0]); i++) {
            for (int l = 0; l < 6; l++) {
                d[l][0] = pairs_mc[i][0], d[l][1] = pairs_mc[i][1], d[l][2] = 0;
            }
            zbc_pwm_plan(f, CW, 4, s, th);
            const double one2 = mc_ripple(s, CW, 4, d, peak, 2), one4 = mc_ripple(s, CW, 4, d, peak, 4);
            EXPECT(one2 > 50, "dc %u dw %u: one light on MCPWM has no second harmonic (%.1f)", d[0][0], d[0][1], one2);
            zbc_pwm_plan(f, CW, 5, s, th);
            EXPECT(mc_ripple(s, CW, 5, d, peak, 2) < 1e-6 * peak, "dc %u dw %u: two lights on MCPWM keep %.2f of %.2f",
                   d[0][0], d[0][1], mc_ripple(s, CW, 5, d, peak, 2), one2);
            zbc_pwm_plan(f, CW, 6, s, th);
            EXPECT(mc_ripple(s, CW, 6, d, peak, 2) < 1e-6 * peak && mc_ripple(s, CW, 6, d, peak, 4) < 1e-6 * peak + 0.02 * one4,
                   "dc %u dw %u: three lights on MCPWM keep the second or fourth harmonic", d[0][0], d[0][1]);
        }
        for (uint32_t duty = 300; duty < peak - 1000; duty += 1000) {
            for (int l = 0; l < 8; l++) {
                d[l][0] = duty, d[l][1] = d[l][2] = 0;
            }
            zbc_pwm_plan(f, DIM, 7, s, th);
            const double one = mc_ripple(s, DIM, 7, d, peak, 1);
            zbc_pwm_plan(f, DIM, 8, s, th);
            EXPECT(one > 50 && mc_ripple(s, DIM, 8, d, peak, 1) < 1e-6 * peak,
                   "duty %u: two dimmable lights on one unit keep %.2f of %.2f", duty, mc_ripple(s, DIM, 8, d, peak, 1), one);
        }
        /* A three-channel light on MCPWM: either pair a constant mix lights is
         * half a period apart, so equal halves cancel the first harmonic. */
        const uint8_t c3[] = { 3, 3, 3 };
        zbc_pwm_plan(f, c3, 3, s, th);
        EXPECT(mc_light(&s[2]), "the third three-channel light is not on MCPWM");
        for (int lit = 0; lit < 2; lit++) {
            d[2][0] = lit ? 0 : 1200, d[2][1] = 1200, d[2][2] = lit ? 1200 : 0;
            EXPECT(mc_ripple(s, c3, 3, d, peak, 1) < 1e-6 * peak, "a three-channel light on MCPWM lit on %s keeps %.2f",
                   lit ? "neutral and warm" : "cold and neutral", mc_ripple(s, c3, 3, d, peak, 1));
        }
    }

    /* ---- sharing out timers and channels ---- */
    {
        zbc_pwm_slot_t s[16];
        uint16_t       th[ZBC_PWM_TIMERS];
        const uint16_t same[] = { 4000, 0, 4000, 4000 };
        EXPECT(zbc_pwm_plan(same, CW, 4, s, th) == NULL, "three lights at one frequency were refused");
        EXPECT(s[0].timer == s[2].timer && s[2].timer == s[3].timer, "one frequency took several timers");
        EXPECT(s[1].timer == -1 && s[1].ch[0] == -1, "a light with no PWM was given some");
        bool used[ZBC_PWM_CHANNELS] = { 0 };
        const int li[] = { 0, 2, 3 };
        for (int i = 0; i < 3; i++) {
            const zbc_pwm_slot_t *x = &s[li[i]];
            EXPECT(x->ch[0] >= 0 && x->ch[0] < ZBC_PWM_CHANNELS && !used[x->ch[0]], "channel reused");
            used[x->ch[0]] = true;
            EXPECT(x->ch[1] >= 0 && x->ch[1] < ZBC_PWM_CHANNELS && !used[x->ch[1]], "channel reused");
            used[x->ch[1]] = true;
        }

        EXPECT(s[0].phase_deg == 0 && s[2].phase_deg == 60 && s[3].phase_deg == 120,
               "three lights on one timer at %u, %u, %u degrees", s[0].phase_deg, s[2].phase_deg,
               s[3].phase_deg);

        const uint16_t two[] = { 4882, 4882 };
        EXPECT(zbc_pwm_plan(two, CW, 2, s, th) == NULL, "two lights were refused");
        EXPECT(s[0].phase_deg == 0 && s[1].phase_deg == 90, "two lights on one timer at %u and %u degrees",
               s[0].phase_deg, s[1].phase_deg);

        const uint16_t one[] = { 0, 4882 };
        EXPECT(zbc_pwm_plan(one, CW, 2, s, th) == NULL && s[1].phase_deg == 0, "a lone light is not at 0 degrees");

        const uint16_t mixed[] = { 500, 1000, 2000 };
        EXPECT(zbc_pwm_plan(mixed, CW, 3, s, th) == NULL, "three frequencies were refused");
        EXPECT(s[0].timer != s[1].timer && s[1].timer != s[2].timer && s[0].timer != s[2].timer,
               "different frequencies shared a timer");
        EXPECT(s[0].phase_deg == 0 && s[1].phase_deg == 0 && s[2].phase_deg == 0,
               "a light alone on its timer was moved off 0 degrees");

        const uint16_t split[] = { 4882, 1220, 4882 };
        EXPECT(zbc_pwm_plan(split, CW, 3, s, th) == NULL, "two frequencies were refused");
        EXPECT(s[0].phase_deg == 0 && s[1].phase_deg == 0 && s[2].phase_deg == 90,
               "phases %u, %u, %u: only lights sharing a timer are spread", s[0].phase_deg, s[1].phase_deg,
               s[2].phase_deg);

        /* Tunable white lights: three on LEDC, then a unit each on MCPWM, its
         * first channel on the unit's zero and its second on the peak - so six
         * fit and a seventh does not. */
        const uint16_t f7[] = { 1000, 1000, 1000, 1000, 1000, 1000, 1000 };
        const char    *why  = zbc_pwm_plan(f7, CW, 4, s, th);
        EXPECT(why == NULL && !mc_light(&s[2]) && mc_light(&s[3]), "a fourth tunable white light did not go to MCPWM");
        EXPECT(s[3].timer == ZBC_PWM_LEDC_TIMERS && s[3].ch[0] == 6 && s[3].ch[1] == 7 && s[3].phase_deg == 0 &&
               s[3].peak == 2, "the fourth tunable white light is not unit 0's pair");
        EXPECT(th[ZBC_PWM_LEDC_TIMERS] == 1000 && th[ZBC_PWM_LEDC_TIMERS + 1] == 0, "MCPWM timers at %u and %u Hz",
               th[ZBC_PWM_LEDC_TIMERS], th[ZBC_PWM_LEDC_TIMERS + 1]);
        EXPECT(zbc_pwm_plan(f7, CW, 6, s, th) == NULL, "six tunable white lights were refused");
        EXPECT(s[3].phase_deg == 0 && s[4].phase_deg == 60 && s[5].phase_deg == 120,
               "three tunable white lights on MCPWM at %u, %u, %u degrees", s[3].phase_deg, s[4].phase_deg,
               s[5].phase_deg);
        why = zbc_pwm_plan(f7, CW, 7, s, th);
        EXPECT(why && strstr(why, "no PWM channel"), "a seventh tunable white light was accepted");

        const uint16_t top[] = { 19531, 100 };
        EXPECT(zbc_pwm_plan(top, CW, 2, s, th) == NULL, "the highest and lowest frequencies were refused");

        const uint16_t bad[] = { 19532 };
        why = zbc_pwm_plan(bad, CW, 1, s, th);
        EXPECT(why && strstr(why, "100-19531"), "19532 Hz was accepted");

        const uint16_t low[] = { 99 };
        why = zbc_pwm_plan(low, CW, 1, s, th);
        EXPECT(why && strstr(why, "100-19531"), "99 Hz was accepted");

        /* Dimmable lights: one channel each, so twelve fit at one frequency -
         * six on LEDC, two to a unit on MCPWM, half a period apart - and a
         * thirteenth does not; a tunable white light with ten of them fills the
         * chip. */
        const uint16_t thirteen[13] = { 4882, 4882, 4882, 4882, 4882, 4882, 4882, 4882, 4882, 4882, 4882, 4882, 4882 };
        EXPECT(zbc_pwm_plan(thirteen, DIM, 6, s, th) == NULL && !mc_light(&s[5]), "six dimmable lights left LEDC");
        EXPECT(zbc_pwm_plan(thirteen, DIM, 12, s, th) == NULL, "twelve dimmable lights were refused");
        const uint16_t dim_deg[6] = { 0, 180, 60, 240, 120, 300 };
        for (int i = 0; i < 6; i++) {
            EXPECT(s[6 + i].ch[0] == 6 + i && s[6 + i].phase_deg == dim_deg[i],
                   "dimmable light %d on channel %d at %u degrees, not %d at %u", 6 + i, s[6 + i].ch[0],
                   s[6 + i].phase_deg, 6 + i, dim_deg[i]);
        }
        why = zbc_pwm_plan(thirteen, DIM, 13, s, th);
        EXPECT(why && strstr(why, "no PWM channel"), "a thirteenth dimmable light was accepted");
        const uint8_t cw_dims[12] = { 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
        EXPECT(zbc_pwm_plan(thirteen, cw_dims, 11, s, th) == NULL, "tunable white and ten dimmable were refused");
        why = zbc_pwm_plan(thirteen, cw_dims, 12, s, th);
        EXPECT(why && strstr(why, "no PWM channel"), "thirteen channels were accepted");

        /* Frequencies: four on LEDC, and one per unit on MCPWM. */
        const uint16_t eight[] = { 200, 300, 400, 500, 600, 700, 800, 900 };
        EXPECT(zbc_pwm_plan(eight, DIM, 7, s, th) == NULL, "seven frequencies were refused");
        why = zbc_pwm_plan(eight, DIM, 8, s, th);
        EXPECT(why && strstr(why, "no PWM timer"), "an eighth frequency was accepted");
        /* Two dimmable lights at one frequency share a unit, at two they do not. */
        const uint16_t shared[] = { 200, 300, 400, 500, 600, 600, 700, 800 };
        EXPECT(zbc_pwm_plan(shared, DIM, 8, s, th) == NULL, "eight dimmable lights at seven frequencies were refused");
        EXPECT(mc_unit(s[4].ch[0]) == mc_unit(s[5].ch[0]), "two dimmable lights at one frequency took two units");
        /* Two three-channel lights fill LEDC; two more fit MCPWM only at one
         * frequency, whichever lights those are. */
        const uint8_t  c3[] = { 3, 3, 3, 3 };
        const uint16_t last_pair[] = { 1000, 2000, 3000, 3000 };
        EXPECT(zbc_pwm_plan(last_pair, c3, 4, s, th) == NULL && !mc_light(&s[1]) && mc_light(&s[2]) &&
                   mc_light(&s[3]), "four three-channel lights, the last two at one frequency, were refused");
        const uint16_t first_pair[] = { 3000, 1000, 3000, 2000 };
        EXPECT(zbc_pwm_plan(first_pair, c3, 4, s, th) == NULL && mc_light(&s[0]) && !mc_light(&s[1]) &&
                   mc_light(&s[2]) && !mc_light(&s[3]),
               "four three-channel lights, the first and third at one frequency, were refused");
        const uint16_t no_pair[] = { 1000, 2000, 3000, 4000 };
        why = zbc_pwm_plan(no_pair, c3, 4, s, th);
        EXPECT(why && strstr(why, "no PWM channel left at these frequencies"),
               "four three-channel lights at four frequencies were accepted");
    }

    /* ---- where the lights on one timer are put ---- */
    {
        /* Only dimmable: evenly around the whole period. */
        check_phases(DIM, 1, (const uint16_t[]){ 0 }, "one dimmable");
        check_phases(DIM, 2, (const uint16_t[]){ 0, 180 }, "two dimmable");
        check_phases(DIM, 3, (const uint16_t[]){ 0, 120, 240 }, "three dimmable");
        check_phases(DIM, 4, (const uint16_t[]){ 0, 90, 180, 270 }, "four dimmable");
        check_phases(DIM, 6, (const uint16_t[]){ 0, 60, 120, 180, 240, 300 }, "six dimmable");
        /* Tunable white takes the pairs half a period apart, dimmable lights the rest. */
        check_phases((const uint8_t[]){ 2, 1 }, 2, (const uint16_t[]){ 0, 90 }, "tunable, dimmable");
        check_phases((const uint8_t[]){ 1, 2 }, 2, (const uint16_t[]){ 90, 0 }, "dimmable, tunable");
        check_phases((const uint8_t[]){ 2, 1, 1 }, 3, (const uint16_t[]){ 0, 90, 270 }, "tunable, 2 dimmable");
        check_phases((const uint8_t[]){ 2, 2, 1 }, 3, (const uint16_t[]){ 0, 60, 120 }, "2 tunable, dimmable");
        check_phases((const uint8_t[]){ 2, 2, 1, 1 }, 4, (const uint16_t[]){ 0, 60, 120, 300 }, "2 tunable, 2 dimmable");
        check_phases((const uint8_t[]){ 2, 1, 1, 1 }, 4, (const uint16_t[]){ 0, 60, 240, 120 }, "tunable, 3 dimmable");

        /* Every mix of up to six lights of one, two or three channels on two
         * frequencies. */
        for (size_t n = 1; n <= 6; n++) {
            unsigned kinds_n = 1;
            for (size_t i = 0; i < n; i++) {
                kinds_n *= 3;
            }
            for (unsigned kinds = 0; kinds < kinds_n; kinds++) {
                for (unsigned freqs = 0; freqs < (1u << n); freqs++) {
                    uint16_t hz[6];
                    uint8_t  ch[6];
                    unsigned k = kinds;
                    for (size_t i = 0; i < n; i++, k /= 3) {
                        hz[i] = (freqs >> i) & 1 ? 1220 : 4882;
                        ch[i] = (uint8_t)(1 + k % 3);
                    }
                    check_plan(hz, ch, n);
                }
            }
        }

        /* Every mix of lights at one frequency with no more than the chip's
         * twelve channels fits: LEDC takes up to six, and MCPWM up to six. */
        const int mixes = every_mix(ZBC_PWM_CHANNELS, (uint8_t[12]){ 0 }, 0);
        EXPECT(mixes == 2030, "%d mixes at one frequency were tried, not all 2030", mixes);
        /* And at up to four frequencies, up to twelve lights at random. */
        const uint16_t pick_hz[] = { 4882, 1220, 9765, 305 };
        for (int r = 0; r < 60000; r++) {
            uint8_t  ch[12];
            uint16_t hz[12];
            const size_t n = 1 + rnd() % 12;
            const int    f = 1 + rnd() % 4;
            for (size_t i = 0; i < n; i++) {
                ch[i] = (uint8_t)(1 + rnd() % 3);
                hz[i] = rnd() % 8 == 0 ? 0 : pick_hz[rnd() % f];
            }
            check_plan(hz, ch, n);
        }

        /* Three-channel lights: three channels each, so two fill LEDC, and two
         * more take MCPWM's three units, the unit between them locked to both. */
        const uint16_t f[] = { 4882, 4882, 4882, 4882, 4882, 4882 };
        zbc_pwm_slot_t s3[8];
        uint16_t       th3[ZBC_PWM_TIMERS];
        EXPECT(zbc_pwm_plan(f, (const uint8_t[]){ 3, 3 }, 2, s3, th3) == NULL, "two three-channel lights were refused");
        EXPECT(s3[0].ch[0] == 0 && s3[0].ch[2] == 2 && s3[1].ch[0] == 3 && s3[1].ch[2] == 5,
               "two three-channel lights on channels %d-%d and %d-%d", s3[0].ch[0], s3[0].ch[2], s3[1].ch[0],
               s3[1].ch[2]);
        EXPECT(zbc_pwm_plan(f, (const uint8_t[]){ 3, 2, 1 }, 3, s3, th3) == NULL,
               "three, two and one channels were refused");
        EXPECT(zbc_pwm_plan(f, (const uint8_t[]){ 3, 3, 1 }, 3, s3, th3) == NULL && s3[2].ch[0] == 6,
               "a dimmable light beside two three-channel lights did not take MCPWM");
        EXPECT(zbc_pwm_plan(f, (const uint8_t[]){ 3, 3, 3, 3 }, 4, s3, th3) == NULL,
               "four three-channel lights were refused");
        EXPECT(s3[2].ch[0] == 6 && s3[2].ch[1] == 7 && s3[2].ch[2] == 8 && s3[3].ch[0] == 10 && s3[3].ch[1] == 11 &&
                   s3[3].ch[2] == 9, "two three-channel lights on MCPWM on %d %d %d and %d %d %d", s3[2].ch[0],
               s3[2].ch[1], s3[2].ch[2], s3[3].ch[0], s3[3].ch[1], s3[3].ch[2]);
        EXPECT(s3[2].phase_deg == 0 && s3[3].phase_deg == 0 && s3[2].peak == 2 && s3[3].peak == 2,
               "two three-channel lights on MCPWM at %u and %u degrees", s3[2].phase_deg, s3[3].phase_deg);
        const char *why3 = zbc_pwm_plan(f, (const uint8_t[]){ 3, 3, 3, 3, 1 }, 5, s3, th3);
        EXPECT(why3 && strstr(why3, "no PWM channel"), "thirteen channels were accepted");
        /* A dimmable light on the unit a three-channel light's third channel
         * took is centred opposite it, with the second. */
        EXPECT(zbc_pwm_plan(f, (const uint8_t[]){ 3, 3, 3, 1 }, 4, s3, th3) == NULL && s3[3].ch[0] == 9 &&
                   s3[3].phase_deg == 180 && s3[3].peak == 1,
               "the dimmable light beside a three-channel light on MCPWM on %d at %u degrees", s3[3].ch[0],
               s3[3].phase_deg);
        check_phases((const uint8_t[]){ 3, 3 }, 2, (const uint16_t[]){ 0, 90 }, "two three-channel");
        check_phases((const uint8_t[]){ 3, 1 }, 2, (const uint16_t[]){ 0, 90 }, "three-channel, dimmable");

        /* Two identical dimmable lights at 0 and 180 degrees cancel each other's
         * first harmonic, the largest there is; three at 120-degree steps the
         * first and second. */
        const uint32_t duties[] = { 300, 1000, 2048, 3100, 3900 };
        for (size_t i = 0; i < sizeof(duties) / sizeof(duties[0]); i++) {
            const uint32_t d = duties[i], max = 4096;
            const double   one = ripple_of(true, d, 0, max, 1, 1);
            EXPECT(one > 50, "duty %u: one dimmable light has no first harmonic (%.1f)", d, one);
            EXPECT(ripple_of(true, d, 0, max, 2, 1) < 1e-6 * max, "duty %u: two dimmable lights keep %.2f",
                   d, ripple_of(true, d, 0, max, 2, 1));
            for (int m = 1; m <= 2; m++) {
                EXPECT(ripple_of(true, d, 0, max, 3, m) < 0.02 * ripple_of(true, d, 0, max, 1, m) + 1,
                       "duty %u: three dimmable lights keep harmonic %d", d, m);
            }
        }
    }

    if (failures) {
        printf("\n%d of %ld checks failed\n", failures, checks);
        return 1;
    }
    printf("all %ld checks passed\n", checks);
    return 0;
}
