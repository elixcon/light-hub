#include "gradient.h"

#include <string.h>

/* See gradient.h. No ESP-IDF in here, and no math.h either. */

bool zbc_gradient_valid(const zbc_gradient_t *g)
{
    return g && g->n >= 1 && g->n <= ZBC_GRADIENT_MAX;
}

static float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* The same blend over plain arrays of fractions, so the decoder can thin nine
 * colours with it before they are a zbc_gradient_t. */
static void blend_at(const float *xs, const float *ys, int n, float pos, float *x, float *y)
{
    if (n <= 0) {
        *x = 0.0f;
        *y = 0.0f;
        return;
    }
    if (n == 1) {
        *x = xs[0];
        *y = ys[0];
        return;
    }
    const float f = clamp01(pos) * (float)(n - 1);
    int         i = (int)f;
    if (i >= n - 1) {
        i = n - 2;
    }
    const float t = f - (float)i;
    *x = xs[i] + (xs[i + 1] - xs[i]) * t;
    *y = ys[i] + (ys[i + 1] - ys[i]) * t;
}

void zbc_gradient_at(const zbc_gradient_t *g, float pos, float *x, float *y)
{
    float xs[ZBC_GRADIENT_MAX], ys[ZBC_GRADIENT_MAX];
    const int n = zbc_gradient_valid(g) ? g->n : 0;
    for (int i = 0; i < n; i++) {
        xs[i] = (float)g->x[i] / 65536.0f;
        ys[i] = (float)g->y[i] / 65536.0f;
    }
    blend_at(xs, ys, n, pos, x, y);
}

float zbc_gradient_pixel_pos(int i, int n)
{
    if (n <= 1) {
        return 0.5f;
    }
    return clamp01((float)i / (float)(n - 1));
}

void zbc_gradient_reverse(zbc_gradient_t *g)
{
    if (!zbc_gradient_valid(g)) {
        return;
    }
    for (int i = 0, j = g->n - 1; i < j; i++, j--) {
        const uint16_t x = g->x[i], y = g->y[i];
        g->x[i] = g->x[j];
        g->y[i] = g->y[j];
        g->x[j] = x;
        g->y[j] = y;
    }
}

static bool near16(uint16_t a, uint16_t b, uint16_t tol)
{
    return (a > b ? a - b : b - a) <= tol;
}

bool zbc_gradient_near(const zbc_gradient_t *a, const zbc_gradient_t *b, uint16_t tol)
{
    if (!zbc_gradient_valid(a) || !zbc_gradient_valid(b) || a->n != b->n) {
        return false;
    }
    for (int i = 0; i < a->n; i++) {
        if (!near16(a->x[i], b->x[i], tol) || !near16(a->y[i], b->y[i], tol)) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------- Philips' format */

static uint16_t quant12(float v, float max)
{
    if (!(v > 0.0f)) {
        return 0;
    }
    const float q = v * 4095.0f / max;
    return q >= 4095.0f ? 4095 : (uint16_t)q;
}

void zbc_hue_pack_xy(float x, float y, uint8_t out[3])
{
    const uint16_t qx = quant12(x, ZBC_HUE_GAMUT_X);
    const uint16_t qy = quant12(y, ZBC_HUE_GAMUT_Y);
    out[0] = (uint8_t)(qx & 0xFF);
    out[1] = (uint8_t)(((qx >> 8) & 0x0F) | ((qy & 0x0F) << 4));
    out[2] = (uint8_t)(qy >> 4);
}

void zbc_hue_unpack_xy(const uint8_t in[3], float *x, float *y)
{
    const uint16_t qx = (uint16_t)(in[0] | ((in[1] & 0x0F) << 8));
    const uint16_t qy = (uint16_t)((in[2] << 4) | (in[1] >> 4));
    *x = (float)qx * ZBC_HUE_GAMUT_X / 4095.0f;
    *y = (float)qy * ZBC_HUE_GAMUT_Y / 4095.0f;
}

/* A fraction as 65536ths, to the nearest and never past the top. */
static uint16_t to_u16(float v)
{
    const float s = clamp01(v) * 65536.0f + 0.5f;
    return s >= 65535.0f ? 65535 : (uint16_t)s;
}

/* The colours as the frame carries them: a length that counts what follows it,
 * the count in a high nibble, the style, two bytes nobody has seen used, then
 * three bytes a colour. */
static size_t put_colors(const zbc_gradient_t *g, uint8_t *out)
{
    size_t at = 0;
    out[at++] = (uint8_t)(4 + 3 * g->n);
    out[at++] = (uint8_t)(g->n << 4);
    out[at++] = ZBC_HUE_STYLE_LINEAR;
    out[at++] = 0;
    out[at++] = 0;
    for (int i = 0; i < g->n; i++) {
        /* Half a 65536th up: the packing cuts off rather than rounds, and a
         * colour read from a lamp and sent back must come out as the same
         * twelve bits, not one below. */
        zbc_hue_pack_xy(((float)g->x[i] + 0.5f) / 65536.0f, ((float)g->y[i] + 0.5f) / 65536.0f, &out[at]);
        at += 3;
    }
    return at;
}

/* Scale: how many colours are stretched over the strip, in eighths. Offset 0:
 * the first colour is at the first LED. */
static size_t put_params(const zbc_gradient_t *g, uint8_t *out)
{
    out[0] = (uint8_t)(g->n << 3);
    out[1] = 0;
    return 2;
}

size_t zbc_hue_gradient_encode(const zbc_gradient_t *g, uint16_t fade, uint8_t *out, size_t cap)
{
    if (!zbc_gradient_valid(g) || !out || cap < (size_t)(2 + 2 + 5 + 3 * g->n + 2)) {
        return 0;
    }
    const uint16_t flags = ZBC_HUE_F_FADE | ZBC_HUE_F_COLORS | ZBC_HUE_F_PARAMS;
    size_t at = 0;
    out[at++] = (uint8_t)flags;
    out[at++] = (uint8_t)(flags >> 8);
    out[at++] = (uint8_t)fade;
    out[at++] = (uint8_t)(fade >> 8);
    at += put_colors(g, &out[at]);
    at += put_params(g, &out[at]);
    return at;
}

bool zbc_hue_state_decode(const uint8_t *buf, size_t len, zbc_hue_state_t *out)
{
    if (!buf || !out || len < 2) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    const uint16_t flags = (uint16_t)(buf[0] | (buf[1] << 8));
    if (flags & (uint16_t)~ZBC_HUE_F_KNOWN) {
        return false;
    }
    out->flags = flags;
    size_t at = 2;

#define NEED(n) do { if (len - at < (size_t)(n)) { return false; } } while (0)
    if (flags & ZBC_HUE_F_ON) {
        NEED(1);
        out->on = buf[at++] != 0;
    }
    if (flags & ZBC_HUE_F_BRIGHTNESS) {
        NEED(1);
        out->brightness = buf[at++];
    }
    if (flags & ZBC_HUE_F_MIREDS) {
        NEED(2);
        out->mireds = (uint16_t)(buf[at] | (buf[at + 1] << 8));
        at += 2;
    }
    if (flags & ZBC_HUE_F_XY) {
        NEED(4);
        out->x = (uint16_t)(buf[at] | (buf[at + 1] << 8));
        out->y = (uint16_t)(buf[at + 2] | (buf[at + 3] << 8));
        at += 4;
    }
    if (flags & ZBC_HUE_F_FADE) {
        NEED(2);
        out->fade = (uint16_t)(buf[at] | (buf[at + 1] << 8));
        at += 2;
    }
    if (flags & ZBC_HUE_F_EFFECT) {
        NEED(1);
        out->effect = buf[at++];
    }
    if (flags & ZBC_HUE_F_COLORS) {
        NEED(1);
        const size_t size = buf[at++];
        NEED(size);
        if (size < 4) {
            return false;
        }
        const uint8_t count = buf[at] >> 4;
        if ((buf[at] & 0x0F) != 0 || count < 1 || count > ZBC_HUE_COLORS_MAX ||
            size != (size_t)(4 + 3 * count)) {
            return false;
        }
        out->colors = count;
        out->style  = buf[at + 1];
        float xs[ZBC_HUE_COLORS_MAX], ys[ZBC_HUE_COLORS_MAX];
        for (int i = 0; i < count; i++) {
            zbc_hue_unpack_xy(&buf[at + 4 + 3 * i], &xs[i], &ys[i]);
        }
        at += size;
        /* More colours than are kept here: the same strip, read at fewer points. */
        const int n = count > ZBC_GRADIENT_MAX ? ZBC_GRADIENT_MAX : count;
        out->gradient.n = (uint8_t)n;
        for (int i = 0; i < n; i++) {
            float x, y;
            blend_at(xs, ys, count, n > 1 ? (float)i / (float)(n - 1) : 0.0f, &x, &y);
            out->gradient.x[i] = to_u16(x);
            out->gradient.y[i] = to_u16(y);
        }
    }
    if (flags & ZBC_HUE_F_EFFECT_SPEED) {
        NEED(1);
        out->effect_speed = buf[at++];
    }
    if (flags & ZBC_HUE_F_PARAMS) {
        NEED(2);
        out->scale  = buf[at++];
        out->offset = buf[at++];
    }
#undef NEED
    return at == len;
}

size_t zbc_hue_state_encode(const zbc_hue_state_t *st, uint8_t *out, size_t cap)
{
    if (!st || !out) {
        return 0;
    }
    const bool gradient = (st->flags & ZBC_HUE_F_COLORS) && zbc_gradient_valid(&st->gradient);
    uint16_t   flags    = st->flags & (ZBC_HUE_F_ON | ZBC_HUE_F_BRIGHTNESS | ZBC_HUE_F_XY);
    if (gradient) {
        flags |= ZBC_HUE_F_COLORS | ZBC_HUE_F_PARAMS;
    }
    size_t need = 2;
    need += (flags & ZBC_HUE_F_ON) ? 1 : 0;
    need += (flags & ZBC_HUE_F_BRIGHTNESS) ? 1 : 0;
    need += (flags & ZBC_HUE_F_XY) ? 4 : 0;
    need += gradient ? (size_t)(5 + 3 * st->gradient.n + 2) : 0;
    if (cap < need) {
        return 0;
    }
    size_t at = 0;
    out[at++] = (uint8_t)flags;
    out[at++] = (uint8_t)(flags >> 8);
    if (flags & ZBC_HUE_F_ON) {
        out[at++] = st->on ? 1 : 0;
    }
    if (flags & ZBC_HUE_F_BRIGHTNESS) {
        out[at++] = st->brightness;
    }
    if (flags & ZBC_HUE_F_XY) {
        out[at++] = (uint8_t)st->x;
        out[at++] = (uint8_t)(st->x >> 8);
        out[at++] = (uint8_t)st->y;
        out[at++] = (uint8_t)(st->y >> 8);
    }
    if (gradient) {
        at += put_colors(&st->gradient, &out[at]);
        at += put_params(&st->gradient, &out[at]);
    }
    return at;
}

uint16_t zbc_hue_fade_from_ds(uint16_t transition_ds)
{
    return transition_ds;
}
