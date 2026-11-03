/* The monotone cubic every curve is drawn with, on its own so a wired light's
 * custom mix can use it without the curve table: the node has one and not the
 * other. A leaf: no ESP-IDF, compiled whole in the host tests' pchip region. */
#include <math.h>
#include <stdint.h>

#include "light_math.h"

/* >>> host-test region: pchip <<< */
/* Fritsch-Carlson monotone cubic Hermite interpolation.
 *
 * The two tangents of a segment are the average of the secants either side,
 * except where the secants change sign (a turning point, tangent 0) or where the
 * average would be steep enough for the cubic to overshoot - the classic bound is
 * three times the smaller neighbouring secant. That clamp is the whole trick: it
 * is what keeps the curve inside the box its control points define, so a
 * brightness ramp can never dip or bulge past the values the user placed. */
float zbc_pchip(const uint8_t *xs, const float *ys, int n, float t)
{
    if (n < 2) {
        return n == 1 ? ys[0] : 0.0f;
    }
    if (t <= xs[0]) {
        return ys[0];
    }
    if (t >= xs[n - 1]) {
        return ys[n - 1];
    }

    int i = 0;
    while (i < n - 2 && t > xs[i + 1]) {
        i++;
    }

    const float h = (float)xs[i + 1] - (float)xs[i];
    const float d = (ys[i + 1] - ys[i]) / h;

    /* Secants of the neighbouring segments, falling back to this one at the ends
     * so the curve leaves the first point and meets the last one cleanly. */
    const float d_prev = i > 0 ? (ys[i] - ys[i - 1]) / ((float)xs[i] - (float)xs[i - 1]) : d;
    const float d_next = i < n - 2 ? (ys[i + 2] - ys[i + 1]) / ((float)xs[i + 2] - (float)xs[i + 1]) : d;

    float m0, m1;
    if (i == 0) {
        m0 = d;
    } else if (d_prev * d <= 0.0f) {
        m0 = 0.0f;
    } else {
        m0 = (d_prev + d) / 2.0f;
        const float limit = 3.0f * fminf(fabsf(d_prev), fabsf(d));
        if (fabsf(m0) > limit) {
            m0 = m0 > 0.0f ? limit : -limit;
        }
    }
    if (i == n - 2) {
        m1 = d;
    } else if (d * d_next <= 0.0f) {
        m1 = 0.0f;
    } else {
        m1 = (d + d_next) / 2.0f;
        const float limit = 3.0f * fminf(fabsf(d), fabsf(d_next));
        if (fabsf(m1) > limit) {
            m1 = m1 > 0.0f ? limit : -limit;
        }
    }

    const float s  = (t - (float)xs[i]) / h;
    const float s2 = s * s;
    const float s3 = s2 * s;
    return (2.0f * s3 - 3.0f * s2 + 1.0f) * ys[i]
         + (s3 - 2.0f * s2 + s) * h * m0
         + (-2.0f * s3 + 3.0f * s2) * ys[i + 1]
         + (s3 - s2) * h * m1;
}
/* >>> end host-test region: pchip <<< */
