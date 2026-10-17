#include <math.h>

#include "colorspace.h"

/* sRGB transfer function, both directions. HSV here is treated as gamma-encoded
 * sRGB - which is what a colour wheel on a screen actually hands you - so it has
 * to be linearised before any matrix touches it, and re-encoded on the way back
 * or the saturation comes home systematically wrong. */
static float srgb_expand(float v)
{
    return v > 0.04045f ? powf((v + 0.055f) / 1.055f, 2.4f) : v / 12.92f;
}

static float srgb_compress(float v)
{
    return v <= 0.0031308f ? 12.92f * v : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

void zbc_kelvin_to_xy(int kelvin, float *out_x, float *out_y)
{
    /* The approximation is only fitted over this span. A stop is validated to
     * 1000-10000 K, so the bottom end is reachable and has to be clamped rather
     * than extrapolated - outside the fit the cubic leaves the locus entirely. */
    if (kelvin < 1667) {
        kelvin = 1667;
    } else if (kelvin > 25000) {
        kelvin = 25000;
    }

    const float t  = (float)kelvin;
    const float t2 = t * t;
    const float t3 = t2 * t;

    float x;
    if (t <= 4000.0f) {
        x = -0.2661239e9f / t3 - 0.2343589e6f / t2 + 0.8776956e3f / t + 0.179910f;
    } else {
        x = -3.0258469e9f / t3 + 2.1070379e6f / t2 + 0.2226347e3f / t + 0.240390f;
    }

    const float x2 = x * x;
    const float x3 = x2 * x;

    float y;
    if (t <= 2222.0f) {
        y = -1.1063814f * x3 - 1.34811020f * x2 + 2.18555832f * x - 0.20219683f;
    } else if (t <= 4000.0f) {
        y = -0.9549476f * x3 - 1.37418593f * x2 + 2.09137015f * x - 0.16748867f;
    } else {
        y = 3.0817580f * x3 - 5.87338670f * x2 + 3.75112997f * x - 0.37001483f;
    }

    *out_x = x;
    *out_y = y;
}

void zbc_hs_to_xy(int hue_deg, int sat_pct, float *out_x, float *out_y)
{
    float h = fmodf((float)hue_deg, 360.0f);
    if (h < 0.0f) {
        h += 360.0f;
    }
    float s = (float)sat_pct / 100.0f;
    s = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);

    /* HSV at V = 1: xy carries no brightness and the lamp's own level is what
     * sets that, so the value is pinned and only the chroma varies. */
    const float c = s;
    const float m = 1.0f - c;
    const float k = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));

    float r, g, b;
    if (h < 60.0f)       { r = c; g = k; b = 0.0f; }
    else if (h < 120.0f) { r = k; g = c; b = 0.0f; }
    else if (h < 180.0f) { r = 0.0f; g = c; b = k; }
    else if (h < 240.0f) { r = 0.0f; g = k; b = c; }
    else if (h < 300.0f) { r = k; g = 0.0f; b = c; }
    else                 { r = c; g = 0.0f; b = k; }
    r += m; g += m; b += m;

    r = srgb_expand(r);
    g = srgb_expand(g);
    b = srgb_expand(b);

    const float big_x = r * 0.4124f + g * 0.3576f + b * 0.1805f;
    const float big_y = r * 0.2126f + g * 0.7152f + b * 0.0722f;
    const float big_z = r * 0.0193f + g * 0.1192f + b * 0.9505f;
    float sum = big_x + big_y + big_z;
    if (sum <= 0.0f) {
        sum = 1.0f;
    }

    *out_x = big_x / sum;
    *out_y = big_y / sum;
}

void zbc_xy_to_hs(float x, float y, float *out_hue, float *out_sat)
{
    if (y < 1e-4f) {          /* not a chromaticity anything can be made of */
        *out_hue = 0.0f;
        *out_sat = 0.0f;
        return;
    }

    /* Y = 1, the same convention zbc_hs_to_xy uses going the other way. */
    const float big_x = x / y;
    const float big_y = 1.0f;
    const float big_z = (1.0f - x - y) / y;

    float r =  3.2406f * big_x - 1.5372f * big_y - 0.4986f * big_z;
    float g = -0.9689f * big_x + 1.8758f * big_y + 0.0415f * big_z;
    float b =  0.0557f * big_x - 0.2040f * big_y + 1.0570f * big_z;

    /* Outside the sRGB triangle - a real colour with no sRGB mixture - and at
     * least one channel is negative. See the header: lifting all three by the
     * same amount walks the point towards white along a line of constant hue,
     * where clamping each at zero would swing it. */
    const float low = fminf(fminf(r, g), b);
    if (low < 0.0f) {
        r -= low;
        g -= low;
        b -= low;
    }

    /* Normalise while still linear. The forward direction's triple also had a
     * maximum of 1 at this point, so this is exactly what undoes it - scaling
     * after the gamma step instead would not. */
    const float top = fmaxf(fmaxf(r, g), b);
    if (top <= 0.0f) {
        *out_hue = 0.0f;
        *out_sat = 0.0f;
        return;
    }
    r /= top;
    g /= top;
    b /= top;

    r = srgb_compress(r);
    g = srgb_compress(g);
    b = srgb_compress(b);

    /* The maximum is 1 by construction, so value is 1 and saturation is chroma. */
    const float lo = fminf(fminf(r, g), b);
    const float c  = 1.0f - lo;

    float h;
    if (c < 1e-6f) {
        h = 0.0f;
    } else if (r >= g && r >= b) {
        h = 60.0f * fmodf((g - b) / c + 6.0f, 6.0f);
    } else if (g >= b) {
        h = 60.0f * ((b - r) / c + 2.0f);
    } else {
        h = 60.0f * ((r - g) / c + 4.0f);
    }

    *out_hue = h;
    *out_sat = c * 100.0f;
}
