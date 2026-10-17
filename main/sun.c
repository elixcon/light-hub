#include <math.h>

#include "sun.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RAD (M_PI / 180.0)
#define DAY_S  86400
#define HALF_S 43200

static double wrap360(double a)
{
    a = fmod(a, 360.0);
    return a < 0 ? a + 360.0 : a;
}

/* One evaluation of the NOAA solar calculator at Julian day `jd`: when solar
 * noon falls, in minutes after 00:00 UTC of that day, and the cosine of the
 * hour angle at which the sun's upper limb meets the horizon (90.833 degrees
 * from the zenith: refraction and the sun's radius). */
static void noaa(double jd, double lat, double lon, double *noon_min, double *cos_ha)
{
    const double T  = (jd - 2451545.0) / 36525.0;
    const double L0 = wrap360(280.46646 + T * (36000.76983 + T * 0.0003032));
    const double M  = 357.52911 + T * (35999.05029 - 0.0001537 * T);
    const double e  = 0.016708634 - T * (0.000042037 + 0.0000001267 * T);
    const double C  = sin(M * RAD) * (1.914602 - T * (0.004817 + 0.000014 * T)) +
                      sin(2 * M * RAD) * (0.019993 - 0.000101 * T) +
                      sin(3 * M * RAD) * 0.000289;
    const double omega  = 125.04 - 1934.136 * T;
    const double lambda = L0 + C - 0.00569 - 0.00478 * sin(omega * RAD);
    const double eps0   = 23.0 + (26.0 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0;
    const double eps    = eps0 + 0.00256 * cos(omega * RAD);
    const double decl   = asin(sin(eps * RAD) * sin(lambda * RAD));

    double y = tan(eps * RAD / 2.0);
    y *= y;
    /* The equation of time, in minutes: how far the real sun runs ahead of a
     * sun that crossed the meridian at exactly 12:00 every day. */
    const double eot = 4.0 / RAD *
                       (y * sin(2 * L0 * RAD) - 2 * e * sin(M * RAD) +
                        4 * e * y * sin(M * RAD) * cos(2 * L0 * RAD) -
                        0.5 * y * y * sin(4 * L0 * RAD) - 1.25 * e * e * sin(2 * M * RAD));

    *noon_min = 720.0 - 4.0 * lon - eot;
    *cos_ha   = (cos(90.833 * RAD) - sin(lat * RAD) * sin(decl)) / (cos(lat * RAD) * cos(decl));
}

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

bool zbc_sun_day(int64_t utc_s, double lat, double lon, zbc_sun_day_t *out)
{
    if (!out || !(lat >= -90.0 && lat <= 90.0) || !(lon >= -180.0 && lon <= 180.0)) {
        return false;
    }
    int64_t day0 = floor_div(utc_s, DAY_S) * DAY_S;
    double  noon_min = 720.0, cos_ha = 0.0;

    /* The solar noon nearest to the moment, so that the night either side of
     * it belongs to the right day. Choosing the day is kept apart from refining
     * it: if the number of refinement passes depended on the moment, two
     * moments a minute apart would get a noon a few seconds apart, and the
     * phase would wobble backwards where it is flattest. */
    for (int pass = 0; pass < 3; pass++) {
        noaa((double)day0 / DAY_S + 2440587.5 + 0.5, lat, lon, &noon_min, &cos_ha);
        const int64_t noon = day0 + (int64_t)llround(noon_min * 60.0);
        if (utc_s - noon > HALF_S) {
            day0 += DAY_S;
        } else if (noon - utc_s > HALF_S) {
            day0 -= DAY_S;
        } else {
            break;
        }
    }
    /* Then the same two passes for every moment of that day: the sun evaluated
     * at the day's own noon, which is what the rise and set are symmetric about. */
    noon_min = 720.0;
    for (int pass = 0; pass < 2; pass++) {
        noaa((double)day0 / DAY_S + 2440587.5 + noon_min / 1440.0, lat, lon, &noon_min, &cos_ha);
    }

    out->noon = day0 + (int64_t)llround(noon_min * 60.0);
    if (cos_ha < -1.0) {
        out->polar = 1;
        out->rise  = out->set = out->noon;
    } else if (cos_ha > 1.0) {
        out->polar = -1;
        out->rise  = out->set = out->noon;
    } else {
        const int64_t half = (int64_t)llround(acos(cos_ha) / RAD * 4.0 * 60.0);
        out->polar = 0;
        out->rise  = out->noon - half;
        out->set   = out->noon + half;
    }
    return true;
}

/* How close a moved sunrise or sunset may come to noon or to a solar midnight. */
#define STRETCH_MARGIN_S 1800

/* `t` moved into a window, and then kept within lo..hi - only when the window
 * moved it, so a day nobody limited is the sun's own even where it is shorter
 * than the margins. */
static int64_t clamp_window(int64_t t, int64_t local_midnight, const int16_t win[2], int64_t lo, int64_t hi)
{
    int64_t w = t;
    if (win[0] >= 0 && w < local_midnight + win[0] * 60) {
        w = local_midnight + win[0] * 60;
    }
    if (win[1] >= 0 && w > local_midnight + win[1] * 60) {
        w = local_midnight + win[1] * 60;
    }
    if (w == t) {
        return t;
    }
    return w < lo ? lo : (w > hi ? hi : w);
}

void zbc_sun_stretch(zbc_sun_day_t *d, int32_t off, const int16_t rise_win[2], const int16_t set_win[2])
{
    if (d->polar) {
        return;
    }
    /* 00:00 local on the date of the day's noon, in UTC. */
    const int64_t local_midnight = floor_div(d->noon + off, DAY_S) * DAY_S - off;
    d->rise = clamp_window(d->rise, local_midnight, rise_win, d->noon - HALF_S + STRETCH_MARGIN_S,
                           d->noon - STRETCH_MARGIN_S);
    d->set  = clamp_window(d->set, local_midnight, set_win, d->noon + STRETCH_MARGIN_S,
                           d->noon + HALF_S - STRETCH_MARGIN_S);
}

/* 0 at u = 0, 1 at u = 1, level at both ends - so the phase is smooth where one
 * stretch hands over to the next. */
static double ease(double u)
{
    if (u <= 0.0) {
        return 0.0;
    }
    if (u >= 1.0) {
        return 1.0;
    }
    return (1.0 - cos(M_PI * u)) / 2.0;
}

double zbc_sun_phase(int64_t t, const zbc_sun_day_t *d)
{
    if (d->polar) {
        return d->polar;
    }
    if (t >= d->rise && t <= d->set) {
        if (t <= d->noon) {
            return ease((double)(t - d->rise) / (double)(d->noon - d->rise));
        }
        return ease((double)(d->set - t) / (double)(d->set - d->noon));
    }
    if (t > d->set) {
        const int64_t midnight = d->noon + HALF_S;
        return -ease((double)(t - d->set) / (double)(midnight - d->set));
    }
    const int64_t midnight = d->noon - HALF_S;
    return -ease((double)(d->rise - t) / (double)(d->rise - midnight));
}

int zbc_al_mix(double phase, int at_midnight, int at_noon)
{
    if (phase < -1.0) {
        phase = -1.0;
    } else if (phase > 1.0) {
        phase = 1.0;
    }
    return (int)lround(at_midnight + (at_noon - at_midnight) * (phase + 1.0) / 2.0);
}
