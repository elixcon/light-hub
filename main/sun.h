/* Where the sun is in its day, for Adaptive Lighting.
 *
 * A leaf module in the sense colorspace.c is: math.h and nothing else, so it
 * compiles with `cc` as it stands and test/test_sun.c drives it without
 * hardware. Everything is UTC. The sun's position depends only on the moment
 * and the place - longitude alone puts solar noon where it is - so there is no
 * timezone anywhere in this, and the page formats the timestamps in the
 * viewer's own local time.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The solar day around one moment: the solar noon nearest to it, and the
 * sunrise and sunset either side of that noon, as Unix timestamps. */
typedef struct {
    int64_t rise;
    int64_t noon;
    int64_t set;
    /* 1 when the sun never sets that day, -1 when it never rises, 0 otherwise.
     * rise and set are meaningless unless this is 0. */
    int8_t  polar;
} zbc_sun_day_t;

/* NOAA's simplified solar position, good to a minute or so away from the
 * poles. False for a latitude or longitude out of range. */
bool   zbc_sun_day(int64_t utc_s, double lat_deg, double lon_deg, zbc_sun_day_t *out);

/* How far through its day the sun is: 1 at solar noon, 0 at sunrise and
 * sunset, -1 at solar midnight, eased with a half cosine between each pair so
 * nothing lurches at the joins. A polar day is 1 throughout and a polar night
 * -1. */
double zbc_sun_phase(int64_t utc_s, const zbc_sun_day_t *day);

/* Moves a day's sunrise and sunset into windows of local clock time, for a user
 * who wants the evening to start no earlier than 21:00 whatever the season.
 * Each window is {earliest, latest} in minutes after local midnight, -1 for no
 * bound, on the local date that holds the day's solar noon; `utc_offset_s` is
 * the zone's offset east at that noon (tz.h). The result stays at least half an
 * hour clear of noon and of the solar midnights either side, where the day
 * hands over to the next, so the phase stays continuous and the half cosines
 * keep some length. Polar days and nights are left as they are. */
void   zbc_sun_stretch(zbc_sun_day_t *day, int32_t utc_offset_s, const int16_t rise_win[2],
                       const int16_t set_win[2]);

/* The value for a phase, given the value at solar midnight and at solar noon.
 * Sunrise and sunset land halfway between the two. */
int    zbc_al_mix(double phase, int at_midnight, int at_noon);

#ifdef __cplusplus
}
#endif
