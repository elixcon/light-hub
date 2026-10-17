/* Colour-space conversions, shared by the curve evaluator and the radio.
 *
 * A response curve can run from a colour stop to a colour-temperature stop, and
 * the only place those two things are the same kind of thing is the CIE 1931
 * chromaticity diagram: a kelvin value is a point on the Planckian locus, a
 * hue/saturation pair is a point inside the sRGB triangle, and a straight line
 * between them is the blend. So curve_db.c needs these to interpolate, and
 * zb_coordinator.c needs them to hand a lamp the result in whatever units that
 * lamp accepts.
 *
 * This is a leaf module in the same sense settings.c is: it depends on nothing,
 * not even ESP-IDF - only math.h. That is deliberate and worth keeping. It is
 * what lets the whole file be compiled with `cc` and checked against known
 * chromaticities and over random round trips, the same property zbc_pchip,
 * btn_step and ota.c's multipart parser have, and for the same reason: a sign
 * error in here reaches the outside world only as a lamp that goes an odd
 * colour somewhere in the middle of its range, which nothing in a serial log
 * explains.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* CIE 1931 xy of a blackbody at `kelvin`, by the Kim et al. cubic spline
 * approximation to the Planckian locus. Valid over 1667-25000 K; the input is
 * clamped to that, which is far wider than any lamp's own range.
 *
 * Spot values, for anyone checking a change against a chromaticity chart:
 *   2700 K -> (0.4593, 0.4107)   3300 K -> (0.4170, 0.3964)
 *   4000 K -> (0.3805, 0.3767)   6000 K -> (0.3220, 0.3318)  */
void zbc_kelvin_to_xy(int kelvin, float *x, float *y);

/* Hue in degrees (0-359) and saturation in percent (0-100) to CIE xy.
 *
 * Value is fixed at 1: xy carries no brightness, and the lamp's own level is
 * what sets that. HSV is treated as gamma-encoded sRGB, linearised, and taken
 * through the standard D65 primaries. */
void zbc_hs_to_xy(int hue_deg, int sat_pct, float *x, float *y);

/* The inverse, for a lamp that accepts only hue and saturation.
 *
 * An xy outside the sRGB triangle - which the Planckian locus below about
 * 1900 K is, so the Candlelight preset reaches this - produces a negative
 * channel. Clamping each channel at zero is the obvious fix and the wrong one:
 * it moves the hue. Adding the same amount to all three desaturates towards
 * white and leaves the differences between the channels, which is what the hue
 * is made of, alone.
 *
 * Round-trips with zbc_hs_to_xy to within a degree and a percent for anything
 * inside the triangle. Degrees and percent come back as floats so a caller
 * scaling to ZCL's 0-254 rounds once rather than twice. */
void zbc_xy_to_hs(float x, float y, float *hue_deg, float *sat_pct);

#ifdef __cplusplus
}
#endif
