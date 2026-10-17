/* The arithmetic of a wired light that has no business depending on ESP-IDF:
 * its made-up address, the level its pin is driven to, and for a PWM light the
 * duties, how a colour temperature is shared out over the channels, where each
 * pulse sits in the period, and how the chip's timers and channels are shared
 * out.
 *
 * A leaf header in the sense colorspace.c is - stdint.h, stdbool.h and stddef.h
 * and nothing else - so the `lightaddr` region of light_db.c, the `cwww` region
 * of local_lights.c and the `pchip` region of curve_db.c compile on the host
 * against it (test_lightaddr, test_cwww).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A wired light has one endpoint, and this is its number. So does a Matter lamp:
 * the endpoint it has on its own device is kept in mlight_db, not in its address. */
#define ZBC_LIGHT_EP 1

/* What a wired light does when the coordinator starts. */
typedef enum {
    ZBC_LIGHT_BOOT_OFF = 0,
    ZBC_LIGHT_BOOT_ON,
    ZBC_LIGHT_BOOT_LAST,     /* as it was when the power went */
    ZBC_LIGHT_BOOT_COUNT,
} zbc_light_boot_t;

/* How a tunable white light shares its brightness between its channels. With
 * three, the first two are taken between each pair of neighbouring LEDs. */
typedef enum {
    ZBC_MIX_CONSTANT = 0,    /* the channels always add up to the brightness */
    ZBC_MIX_FULL,            /* each channel reaches full at the middle temperature */
    ZBC_MIX_CUSTOM,          /* a curve per channel, drawn by the user (zbc_mix_curve_t) */
    ZBC_MIX_COUNT,
} zbc_light_mix_t;

/* Fritsch-Carlson monotone cubic through (xs, ys), evaluated at t; xs strictly
 * ascending, n >= 2. In curve_db.c (region `pchip`), and the one interpolation
 * the brightness curve, the colour path and a light's mixing curves all use. */
float    zbc_pchip(const uint8_t *xs, const float *ys, int n, float t);

/* One channel's share of a tunable white light, drawn by the user: how much of
 * that channel's full output it gives at each colour temperature, at full
 * brightness. The rules of a brightness curve (zbc_curve_t), with the axes
 * this needs:
 *   x  where on the light's own range, linear in mireds: 1 is its warm LEDs'
 *      colour temperature, 100 its cold LEDs'; strictly ascending, x[0] = 1,
 *      x[n-1] = 100
 *   y  percent of the channel's full output, 0-100
 * A channel with no curve (n == 0) stays dark. */
#define ZBC_MIX_MAX_POINTS 6

typedef struct {
    uint8_t n;                         /* 0 = none; else 2..ZBC_MIX_MAX_POINTS */
    uint8_t x[ZBC_MIX_MAX_POINTS];
    uint8_t y[ZBC_MIX_MAX_POINTS];
} zbc_mix_curve_t;

/* The channels of a PWM light, in the order their pulses take in the period.
 * A dimmable light has the first, a two-channel tunable white light cold and
 * warm, a three-channel one all three. */
#define ZBC_PWM_MAX_LIGHT_CH 3
enum { ZBC_CH_COLD = 0, ZBC_CH_NEUTRAL = 1, ZBC_CH_WARM = 2 };

bool     zbc_mix_curve_valid(const zbc_mix_curve_t *c);

/* The address a wired light is known by everywhere a light is named as
 * (ieee, endpoint): groups, curves, the device routes. Written as 8 bytes most
 * significant first, the same order zbc_ieee_to_str prints. */
void     zbc_light_ieee(uint16_t id, uint8_t out[8]);

/* The light id an address stands for, or 0 when it is not a wired light's. */
uint16_t zbc_light_id_from_ieee(const uint8_t ieee[8]);

/* The same pair for a Matter lamp (mlight_db.h): one endpoint of a Matter device
 * paired to this box, known everywhere else as this address on ZBC_LIGHT_EP. A
 * range of its own, so no Matter lamp is ever taken for a wired one or the other
 * way round. */
void     zbc_mlight_ieee(uint16_t id, uint8_t out[8]);
uint16_t zbc_mlight_id_from_ieee(const uint8_t ieee[8]);

/* Whether a light comes up on. `stored` is the last state written for it: 1, 0,
 * or negative when nothing was ever stored. */
bool     zbc_light_boot_on(uint8_t boot, int stored);

/* The level to drive an on/off light's pin to for this state. */
int      zbc_light_pin_level(bool on, bool active_low);

/* ---------------------------------------------------------------- PWM */

/* The LEDC clock every timer is forced onto (PLL_DIV on the C6) and the limits
 * of the chip. local_lights.c asserts these against soc_caps.h at build time. */
#define ZBC_PWM_CLK_HZ   80000000u
#define ZBC_PWM_HZ_MIN   100u
#define ZBC_PWM_HZ_MAX   19531u     /* the fastest the clock allows at 12 bits */
#define ZBC_PWM_RES_MAX  20u
#define ZBC_PWM_CHANNELS 6
#define ZBC_PWM_TIMERS   4

/* The finest duty resolution in bits a timer can run at this frequency:
 * floor(log2(clock / hz)), capped at ZBC_PWM_RES_MAX. The fastest frequency at
 * each resolution is clock / 2^bits, which is where ESPHome's recommended
 * frequencies and the light page's presets come from: 1220 Hz is 16 bits, 2441
 * 15, 4882 14, 9765 13 and 19531 12, and below ESPHome's list 610 is 17, 305 18
 * and 152 19 - one hertz more loses a bit. */
uint8_t  zbc_pwm_resolution(uint32_t hz);

/* ZCL level, in thousandths (0-254000), to a duty out of `max` on the CIE 1931
 * lightness curve - the perceptual scale a Zigbee lamp dims on. 0 is dark, any
 * level above 0 gives at least one count, and 254000 gives `max`. */
uint32_t zbc_level_to_duty(uint32_t level_milli, uint32_t max);

/* Splits a duty between the cold and warm channels for a colour temperature in
 * mireds, clamped to [cold_m, warm_m] (cold has the fewer mireds). */
void     zbc_cwww_mix(uint32_t duty, uint32_t mireds, uint32_t cold_m, uint32_t warm_m,
                      uint8_t mix, uint32_t *dc, uint32_t *dw);

/* The same over three LEDs, cold_m < neutral_m < warm_m: zbc_cwww_mix between
 * the two LEDs either side of the colour temperature, the third dark. Out in
 * the order ZBC_CH_COLD, ZBC_CH_NEUTRAL, ZBC_CH_WARM. ZBC_MIX_CONSTANT and
 * ZBC_MIX_FULL only. */
void     zbc_mix3(uint32_t duty, uint32_t mireds, uint32_t cold_m, uint32_t neutral_m, uint32_t warm_m,
                  uint8_t mix, uint32_t out[3]);

/* Where a colour temperature sits on a light's mixing curves: 1.0 at warm_m,
 * 100.0 at cold_m, linear in mireds, clamped. */
float    zbc_mix_position(uint32_t mireds, uint32_t cold_m, uint32_t warm_m);

/* ZBC_MIX_CUSTOM: each channel's duty is `duty` scaled by its curve at this
 * colour temperature. curves[] and out[] in channel order. */
void     zbc_mix_custom(uint32_t duty, uint32_t mireds, uint32_t cold_m, uint32_t warm_m,
                        const zbc_mix_curve_t curves[], int nch, uint32_t out[]);

/* One channel as LEDC is told it: high from hpoint for duty counts, inverted
 * at the GPIO matrix when `invert` is set. hpoint + duty never exceeds max. */
typedef struct {
    uint32_t hpoint;
    uint32_t duty;
    bool     invert;
} zbc_pwm_out_t;

/* Places a light's n pulses (1..ZBC_PWM_MAX_LIGHT_CH) of d[] counts each, in
 * channel order, the first starting `shift` counts into the period - the light's
 * phase - and the dark time shared evenly between the lit ones: so two lit
 * pulses are centred exactly half a period apart (to one count), and none
 * overlap while their duties fit in the period. No pulse runs past the end of
 * the period; see CLAUDE.md for why a channel is inverted when its pulse would
 * wrap. */
void     zbc_pwm_place_n(const uint32_t d[], int n, uint32_t max, uint32_t shift, zbc_pwm_out_t out[]);

/* The two-channel case: cold and warm. */
void     zbc_pwm_place(uint32_t dc, uint32_t dw, uint32_t max, uint32_t shift,
                       zbc_pwm_out_t *cold, zbc_pwm_out_t *warm);

typedef struct {
    int8_t   timer;     /* -1 = no PWM for this light */
    int8_t   ch[ZBC_PWM_MAX_LIGHT_CH];  /* the light's channels in order, -1 past its count */
    uint16_t phase_deg; /* where in its timer's period the light starts, 0-359 */
} zbc_pwm_slot_t;

/* Shares the timers and channels out over the lights: hz[i] is light i's PWM
 * frequency, 0 for a light that takes no PWM, and chans[i] its channels, 1 for
 * a dimmable light, 2 or 3 for tunable white. Lights at the same frequency share
 * a timer and are spread over its period, so the ripple of lights that light one
 * room partly cancels. Returns NULL, or why the chip cannot run them all. */
const char *zbc_pwm_plan(const uint16_t hz[], const uint8_t chans[], size_t n, zbc_pwm_slot_t out[],
                         uint16_t timer_hz[ZBC_PWM_TIMERS]);

#ifdef __cplusplus
}
#endif
