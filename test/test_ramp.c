/* How a wired light's level and colour move over time, from main/local_lights.c.
 *
 * Lifted rather than copied: see test/Makefile. Sampled every 10 ms, as the fade
 * task runs, and judged on the PWM counts that reach a pin at 9765 Hz (13 bits)
 * rather than on the formulas. A fault here is a fade that steps near black, a
 * colour that lands before the light does, or a held button's sweep that stalls
 * once a second - all of them "the light is not smooth", which no log explains. */
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
#include "ramp.inc"

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

#define MAX    8192u                 /* 13 bits, 9765 Hz */
#define TICK   10000                 /* the fade task's 10 ms */
#define T0     1000000               /* any start time but zero */

static uint32_t duty_at(const ramp_t *r, int64_t now)
{
    const int64_t lvl = ramp_now(r, now);
    return zbc_level_to_duty(lvl > 0 ? (uint32_t)lvl : 0, MAX);
}

/* A switch-on from black to `level` over `ds`, or the switch-off back. */
static void check_fade(int level, uint16_t ds)
{
    const int64_t to  = (int64_t)level * 1000;
    const int64_t end = T0 + (int64_t)ds * 100000;
    const uint32_t full = zbc_level_to_duty((uint32_t)to, MAX);
    ramp_t on, off;
    ramp_start(&on, 0, to, ds, T0, RAMP_EASED_LIGHT);
    ramp_start(&off, to, 0, ds, T0, RAMP_EASED_LIGHT);

    EXPECT(duty_at(&on, T0) == 0, "level %d: a switch-on starts lit", level);
    EXPECT(duty_at(&on, end) == full, "level %d: a switch-on ends at %u, not %u", level,
           duty_at(&on, end), full);
    EXPECT(duty_at(&off, T0) == full, "level %d: a switch-off starts at %u, not %u", level,
           duty_at(&off, T0), full);
    EXPECT(duty_at(&off, end) == 0, "level %d: a switch-off ends lit", level);

    /* Out of black and back into it a count at a time: at the fade setting's
     * 0.8 s and longer, the first tick of a switch-on and the last of a
     * switch-off are the faintest light there is. A ramp even in level starts at
     * level 1's four counts and adds eight a tick. A shorter fade has fewer
     * ticks to do it in, and still starts under a hundredth of the light. */
    const uint32_t first = ds >= 8 ? 1 : full / 100 + 1;
    EXPECT(duty_at(&on, T0 + TICK) <= first, "level %d ds %u: first tick of a switch-on is %u counts",
           level, ds, duty_at(&on, T0 + TICK));
    EXPECT(duty_at(&off, end - TICK) <= first, "level %d ds %u: last tick of a switch-off is %u counts",
           level, ds, duty_at(&off, end - TICK));

    /* Smootherstep in light: half way in time is half the light, either way. */
    const int64_t mid = T0 + (end - T0) / 2;
    EXPECT(abs((int)duty_at(&on, mid) - (int)(full / 2)) <= 1, "level %d: half way on is %u of %u",
           level, duty_at(&on, mid), full);
    EXPECT(abs((int)duty_at(&off, mid) - (int)(full / 2)) <= 1, "level %d: half way off is %u of %u",
           level, duty_at(&off, mid), full);

    /* Never back on itself, and the two are mirror images. Eased at both ends,
     * so no tick moves the light more than the ease's steepest point does:
     * 1.875 times what an even ramp would, plus rounding. */
    uint32_t was_on = 0, was_off = full;
    const double step_max = 1.875 * full * TICK / (double)(end - T0) + 2.0;
    for (int64_t t = T0; t <= end; t += TICK) {
        const uint32_t d_on = duty_at(&on, t), d_off = duty_at(&off, t);
        EXPECT(d_on >= was_on, "level %d: a switch-on dims at %lld ms", level, (long long)(t - T0) / 1000);
        EXPECT(d_off <= was_off, "level %d: a switch-off brightens at %lld ms", level,
               (long long)(t - T0) / 1000);
        EXPECT(d_on - was_on <= step_max, "level %d: a switch-on jumps %u counts at %lld ms", level,
               d_on - was_on, (long long)(t - T0) / 1000);
        EXPECT(abs((int)d_on - (int)duty_at(&off, end - (t - T0))) <= 1,
               "level %d: switch-on and switch-off differ at %lld ms", level, (long long)(t - T0) / 1000);
        was_on  = d_on;
        was_off = d_off;
    }
}

int main(void)
{
    /* ---- level <-> light ---- */
    for (int64_t m = 0; m <= 254000; m += 7) {
        const int64_t back = light_level(level_light(m));
        EXPECT(llabs(back - m) <= 1, "level %lld milli comes back as %lld", (long long)m, (long long)back);
    }

    /* ---- one speed: a chain of these must not stall where they join ---- */
    ramp_t r;
    ramp_start(&r, 50000, 150000, 10, T0, RAMP_LINEAR);
    EXPECT(ramp_now(&r, T0) == 50000 && ramp_now(&r, T0 + 500000) == 100000 &&
           ramp_now(&r, T0 + 1000000) == 150000 && ramp_now(&r, T0 + 5000000) == 150000,
           "an even ramp is not even");
    /* A held button: one link a second, each from where the last one is. The
     * speed at a join is the speed on either side of it. */
    ramp_start(&r, 0, 100000, 10, T0, RAMP_LINEAR);
    ramp_t next;
    ramp_start(&next, ramp_now(&r, T0 + 1000000), 200000, 10, T0 + 1000000, RAMP_LINEAR);
    EXPECT(ramp_now(&r, T0 + 1000000) - ramp_now(&r, T0 + 1000000 - TICK) ==
               ramp_now(&next, T0 + 1000000 + TICK) - ramp_now(&next, T0 + 1000000),
           "a chain of even ramps changes speed where two links join");

    /* ---- a switch-on or switch-off ---- */
    const int      levels[] = { 3, 13, 30, 76, 140, 211, 254 };
    const uint16_t fades[]  = { 2, 8, 10, 30 };
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        for (size_t j = 0; j < sizeof(fades) / sizeof(fades[0]); j++) {
            check_fade(levels[i], fades[j]);
        }
    }

    /* ---- the colour travelling with it ---- */
    ramp_t ct;
    ramp_start(&ct, 250, 555, 8, T0, RAMP_EASED);
    EXPECT(ramp_now(&ct, T0) == 250 && ramp_now(&ct, T0 + 800000) == 555, "an eased colour misses its ends");
    EXPECT(llabs(ramp_now(&ct, T0 + 400000) * 2 - (250 + 555)) <= 1, "an eased colour is not half way at half time");
    int64_t was = 250;
    for (int64_t t = T0; t <= T0 + 800000; t += TICK) {
        EXPECT(ramp_now(&ct, t) >= was, "an eased colour turns back at %lld ms", (long long)(t - T0) / 1000);
        was = ramp_now(&ct, t);
    }
    /* Started in step with a fade, it keeps step: the same share of the way at
     * every tick, so the light is warmest exactly as it is dimmest. */
    ramp_t lvl;
    ramp_start(&lvl, 200000, 0, 8, T0, RAMP_EASED_LIGHT);
    ramp_start(&ct, 250, 555, 0, T0, RAMP_EASED);
    ct.t1 = lvl.t1;
    for (int64_t t = T0; t <= lvl.t1; t += TICK) {
        const double light = level_light(ramp_now(&lvl, t)) / level_light(200000);
        const double warm  = (double)(ramp_now(&ct, t) - 250) / (555 - 250);
        EXPECT(fabs(light + warm - 1.0) < 0.01, "colour and light out of step at %lld ms: %.3f + %.3f",
               (long long)(t - T0) / 1000, light, warm);
    }

    /* ---- a glide along a light's curves ---- */
    /* A light dark below 40 % (a ceiling's spots), switched on with its room to
     * 100 %: the levels its curve gives at 16 even steps of the room's brightness. */
    const uint16_t spots[16] = { 0, 0, 0, 0, 0, 0, 0, 38, 92, 140, 168, 191, 209, 226, 240, 254 };
    ramp_t g;
    ramp_glide(&g, 0, spots, 16, 1000, 8, T0);
    EXPECT(ramp_now(&g, T0) == 0 && ramp_now(&g, T0 + 800000) == 254000, "a glide misses its ends");
    /* A microsecond before the end smootherstep rounds to exactly 1: the glide is
     * at its last point there. Reading one past it would not show here - that
     * point is weighted zero - but -fsanitize=address reports it. */
    EXPECT(llabs(ramp_now(&g, T0 + 800000 - 1) - 254000) <= 1, "a glide's last microsecond is at %lld",
           (long long)ramp_now(&g, T0 + 800000 - 1));
    /* Half way in time is half way along the brightness: between steps 7 and 8. */
    EXPECT(ramp_now(&g, T0 + 400000) == (38000 + 92000) / 2, "a glide is at %lld at half time",
           (long long)ramp_now(&g, T0 + 400000));
    /* Dark until the glide passes 40 %, then across the bottom of its range in a
     * few ticks: the time between black and the first lit step (level 38, 1.9 %
     * duty at 13 bits) is one step of the glide where it moves fastest. Faded from
     * black on its own it spends about 100 ms there, at either end of every fade. */
    int below_glide = 0, below_eased = 0;
    ramp_t eased;
    ramp_start(&eased, 0, 254000, 8, T0, RAMP_EASED_LIGHT);
    int64_t last = 0;
    bool    mono = true;
    for (int64_t t = T0 + TICK; t < T0 + 800000; t += TICK) {
        const int64_t v = ramp_now(&g, t);
        below_glide += v > 0 && v < 38000;
        below_eased += ramp_now(&eased, t) > 0 && ramp_now(&eased, t) < 38000;
        mono = mono && v >= last;
        last = v;
    }
    EXPECT(mono, "a glide through rising points falls");
    EXPECT(below_glide <= 4, "a glide spends %d ticks between black and its first lit step", below_glide);
    EXPECT(below_eased >= 8, "the comparison is off: an eased fade spends only %d ticks there", below_eased);

    /* Its colour on the same steps keeps step with it: at each tick both are the
     * same share of the way between the same two steps. */
    const uint16_t warmth[16] = { 438, 438, 438, 438, 438, 438, 438, 438, 436, 434, 431, 427, 422, 415, 406, 396 };
    ramp_t gc;
    ramp_glide(&gc, warmth[0], warmth, 16, 1, 8, T0);
    for (int64_t t = T0; t <= T0 + 800000; t += TICK) {
        const double q = smootherstep((double)(t - T0) / 800000.0) * 15;
        const int    i = q >= 15 ? 14 : (int)q;
        const double want = warmth[i] + (warmth[i + 1] - warmth[i]) * (q - i);
        EXPECT(fabs((double)ramp_now(&gc, t) - want) <= 0.5, "a glide's colour is %lld at %lld ms, not %.1f",
               (long long)ramp_now(&gc, t), (long long)(t - T0) / 1000, want);
    }

    /* A light already lit starts from where it is, not from the curve's first
     * step: a switch-off glides down from the light's own level, which a shift or
     * a rounding can have put a little off the curve. */
    const uint16_t down[16] = { 150, 140, 120, 92, 60, 38, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    ramp_glide(&g, 149500, down, 16, 1000, 8, T0);
    EXPECT(ramp_now(&g, T0 + TICK) > 149000 && ramp_now(&g, T0 + TICK) <= 149500,
           "a glide from a lit light jumps to %lld", (long long)ramp_now(&g, T0 + TICK));
    EXPECT(ramp_now(&g, T0 + 800000) == 0, "a glide down does not end dark");
    /* Too few or too many points is a plain eased ramp to the last one. */
    ramp_glide(&g, 0, down, 1, 1000, 8, T0);
    EXPECT(g.shape == RAMP_EASED && ramp_now(&g, T0 + 800000) == 150000, "a one-point glide is not a ramp to it");

    /* Settled, and a ramp of no time at all, are where they are going. */
    ramp_settle(&r, 42000);
    EXPECT(ramp_now(&r, T0) == 42000 && !ramp_active(&r, T0), "a settled ramp moves");
    ramp_start(&r, 0, 90000, 0, T0, RAMP_EASED_LIGHT);
    EXPECT(ramp_now(&r, T0) == 90000, "a ramp of no time does not arrive at once");

    if (failures) {
        printf("\n%d of %ld checks failed\n", failures, checks);
        return 1;
    }
    printf("all %ld checks passed\n", checks);
    return 0;
}
