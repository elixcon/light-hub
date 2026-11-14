/* A gradient: a few colours spread along a light strip, with the strip blending
 * between them - what a Hue gradient lightstrip does, and what a wired strip of
 * addressable LEDs is made to do here.
 *
 * Two things live in this file. The gradient itself, and where on it a point of
 * the strip falls. And Philips' own wire format for it: the command a Hue
 * gradient lamp takes on cluster 0xFC03 and the state it reports there, reverse
 * engineered by Bifrost (github.com/chrivers/bifrost, doc/hue-zigbee-format.md)
 * and zigbee2mqtt (zigbee-herdsman-converters, lib/philips.ts). A Light Node's
 * strip speaks the same format, so the hub has one way to talk to both.
 *
 * A leaf module in the sense colorspace.c is: stdint.h, stdbool.h, stddef.h and
 * nothing else, so it compiles with `cc` and both ends of the format - what the
 * hub sends and what a lamp makes of it - can be run against each other on the
 * host (test_gradient, test_huegrad). Nothing here can be tried on a real lamp
 * from a desk without one.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The Hue app places up to five colours; the lamp itself takes nine. */
#define ZBC_GRADIENT_MAX 5

/* 1 to ZBC_GRADIENT_MAX colours, the first at the start of the strip and the
 * last at its end, evenly spaced. CIE 1931 xy as ZCL and Matter count them, in
 * 65536ths. One colour is a strip in a single colour. n == 0 is "none". */
typedef struct {
    uint8_t  n;
    uint16_t x[ZBC_GRADIENT_MAX];
    uint16_t y[ZBC_GRADIENT_MAX];
} zbc_gradient_t;

bool zbc_gradient_valid(const zbc_gradient_t *g);

/* The colour at `pos` along the strip, 0 its start and 1 its end: a straight
 * line in xy between the two colours either side, which is the blend
 * colorspace.h describes. Out as fractions, 0-1. */
void zbc_gradient_at(const zbc_gradient_t *g, float pos, float *x, float *y);

/* Where pixel i of n sits: 0 for the first, 1 for the last, the middle for a
 * strip of one. */
float zbc_gradient_pixel_pos(int i, int n);

/* The same colours from the other end. */
void zbc_gradient_reverse(zbc_gradient_t *g);

/* Whether two gradients are the same to within `tol` 65536ths in each of x and
 * y at every colour, and have the same number of colours. */
bool zbc_gradient_near(const zbc_gradient_t *a, const zbc_gradient_t *b, uint16_t tol);

/* ------------------------------------------------------- Philips' format */

#define ZBC_HUE_CLUSTER      0xFC03u
#define ZBC_HUE_MANUF        0x100Bu    /* Signify */
#define ZBC_HUE_CMD_STATE    0x00u      /* sets the fields its flags name */
#define ZBC_HUE_ATTR_STATE   0x0002u    /* an octet string of the same layout */

/* Twelve bits each of x and y, scaled so that 4095 is the corner of the wide
 * gamut, in three bytes: x's low byte, x's high nibble with y's low nibble, y's
 * high byte. The entertainment frames (zb_coordinator.c) pack colours the same
 * way. 0.8264 is Bifrost's value for y; older code elsewhere has 0.8431. */
#define ZBC_HUE_GAMUT_X 0.7347f
#define ZBC_HUE_GAMUT_Y 0.8264f

void zbc_hue_pack_xy(float x, float y, uint8_t out[3]);
void zbc_hue_unpack_xy(const uint8_t in[3], float *x, float *y);

/* The flags that open a command or a state, and say which fields follow. The
 * fields come in the order listed here, which is not the order of the bits. */
#define ZBC_HUE_F_ON          (1u << 0)   /* u8 */
#define ZBC_HUE_F_BRIGHTNESS  (1u << 1)   /* u8, 1-254 */
#define ZBC_HUE_F_MIREDS      (1u << 2)   /* u16 */
#define ZBC_HUE_F_XY          (1u << 3)   /* u16 x, u16 y, in 65536ths */
#define ZBC_HUE_F_FADE        (1u << 4)   /* u16 */
#define ZBC_HUE_F_EFFECT      (1u << 5)   /* u8 */
#define ZBC_HUE_F_COLORS      (1u << 8)   /* the gradient's colours, see below */
#define ZBC_HUE_F_EFFECT_SPEED (1u << 7)  /* u8 */
#define ZBC_HUE_F_PARAMS      (1u << 6)   /* u8 scale, u8 offset, in eighths */
#define ZBC_HUE_F_KNOWN       0x01FFu

#define ZBC_HUE_COLORS_MAX    9
#define ZBC_HUE_STYLE_LINEAR  0x00

/* The longest command zbc_hue_gradient_encode writes, and the longest state
 * zbc_hue_state_encode does. */
#define ZBC_HUE_GRADIENT_LEN_MAX (2 + 2 + 5 + 3 * ZBC_GRADIENT_MAX + 2)
#define ZBC_HUE_STATE_LEN_MAX    (2 + 1 + 1 + 4 + 5 + 3 * ZBC_GRADIENT_MAX + 2)

/* The body of command 0 that shows this gradient: fade speed, the colours, and
 * how they are laid out - linear, one colour per stretch of the strip, starting
 * at its first LED. `fade` is Philips' fade speed; its clients send 2 to 8, and
 * 4 by default. Returns the length, or 0 when the gradient is not valid or
 * `cap` is too small. */
size_t zbc_hue_gradient_encode(const zbc_gradient_t *g, uint16_t fade, uint8_t *out, size_t cap);

typedef struct {
    uint16_t       flags;         /* ZBC_HUE_F_*: which of the fields below were there */
    bool           on;
    uint8_t        brightness;
    uint16_t       mireds;
    uint16_t       x, y;
    uint16_t       fade;
    uint8_t        effect;
    uint8_t        effect_speed;
    /* With ZBC_HUE_F_COLORS. `colors` is how many the frame carried, up to nine;
     * `gradient` has them, thinned evenly to ZBC_GRADIENT_MAX when there were
     * more. */
    uint8_t        colors;
    uint8_t        style;
    zbc_gradient_t gradient;
    uint8_t        scale, offset; /* with ZBC_HUE_F_PARAMS */
} zbc_hue_state_t;

/* Reads a command's body or a state attribute's value. False for one that does
 * not parse to its last byte, names a flag nobody knows, or carries more than
 * nine colours - which is what a lamp rejects too. */
bool   zbc_hue_state_decode(const uint8_t *buf, size_t len, zbc_hue_state_t *out);

/* What a lamp answers a read of its state with: on, brightness, and its
 * gradient when it shows one of two colours or more, or its plain colour. The
 * fields taken are the ones `st->flags` names among those. Returns the length,
 * 0 when `cap` is too small. */
size_t zbc_hue_state_encode(const zbc_hue_state_t *st, uint8_t *out, size_t cap);

/* How long a fade speed takes, and the fade speed for a transition, as far as
 * anyone knows: zigbee2mqtt hands a transition in tenths of a second straight
 * through, so that is what is done here, with 0 left as "at once". */
uint16_t zbc_hue_fade_from_ds(uint16_t transition_ds);

#ifdef __cplusplus
}
#endif
