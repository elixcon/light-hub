/* How each group exposes Adaptive Lighting, what a Matter "turn on" does to it,
 * and the day it follows, persisted.
 *
 * Adaptive Lighting itself is a mode that is started and ended at runtime (see
 * lighting.h); what is stored is whether a group offers it, how Matter
 * controllers see it, the day each group follows, and which groups were running
 * it, so that a restart - an update, a power cut - picks the mode up where it
 * was. Its own NVS blobs rather than fields in zbc_group_t: the group record has
 * no spare bytes, and growing it bumps GROUP_BLOB_VERSION, which discards every
 * group somebody set up. The turn-on choice lives here for the same reason, in
 * what was the record's spare byte.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A room's day. Two values each, one at solar midnight and one at solar noon,
 * and the sun decides the rest (sun.h). The colour is an *offset* in kelvin added
 * to what each lamp's own warmth curve gives, so the curves keep their shape; a
 * lamp with colour temperature but no curve is shifted from `k_base` instead.
 * Every room has its own; one never set follows al_db_day's defaults. */
#define ZBC_AL_OFFSET_K_LIMIT 3000
#define ZBC_AL_BASE_K_MIN     1500
#define ZBC_AL_BASE_K_MAX     8000
/* Points a shape may have between its two ends. With the ends that is the
 * page's curve editor's limit, ZBC_CURVE_MAX_POINTS (lighting.c asserts it). */
#define ZBC_AL_SHAPE_POINTS   4
#define ZBC_AL_NO_TIME        (-1)
#define ZBC_AL_DAY_MIN        1440

typedef struct {
    int16_t  k_midnight;    /* kelvin added at solar midnight, usually negative: warmer */
    int16_t  k_noon;        /* kelvin added at solar noon */
    uint16_t k_base;        /* where a lamp with no warmth curve is shifted from */
    uint8_t  pct_midnight;  /* brightness at solar midnight, 1-100 */
    uint8_t  pct_noon;      /* brightness at solar noon, 1-100 */
    /* Limits on the sun's day: {earliest, latest} for sunrise and for sunset, in
     * minutes after local midnight in the stored time zone (settings_tz),
     * ZBC_AL_NO_TIME for none (zbc_sun_stretch). Without a time zone they are
     * kept and do nothing: read as UTC they would be an hour or more out. */
    int16_t  rise_win[2];
    int16_t  set_win[2];
    /* The day's shape between the two values above: points inside the line from
     * the midnight value to the noon value, x 2-99 on a scale where 1 is solar
     * midnight, 50.5 sunrise and sunset and 100 noon, ascending. None is the
     * straight line, which is the two-value profile exactly. */
    uint8_t  n_pct;
    uint8_t  n_k;
    uint8_t  pct_x[ZBC_AL_SHAPE_POINTS];
    uint8_t  pct_y[ZBC_AL_SHAPE_POINTS];   /* 1-100 */
    uint8_t  k_x[ZBC_AL_SHAPE_POINTS];
    int16_t  k_y[ZBC_AL_SHAPE_POINTS];     /* within +-ZBC_AL_OFFSET_K_LIMIT */
} zbc_al_profile_t;

typedef enum {
    /* Not offered: no Matter exposure, no sun button, the button action and the
     * API refuse. */
    ZBC_AL_EXPOSE_OFF = 0,
    /* A Mode Select cluster ("Normal" / "Adaptive") on the group's own light
     * endpoint - the standard Matter way. */
    ZBC_AL_EXPOSE_MODE_SELECT,
    /* A second on/off endpoint "<name> Adaptive" that mirrors the mode - for Apple
     * Home, which does not show Mode Select. */
    ZBC_AL_EXPOSE_ENDPOINT,
    ZBC_AL_EXPOSE_COUNT,
} zbc_al_expose_t;

/* What a Matter controller's "turn on" does to a group, dark or lit - Alexa's "turn on
 * the basement", a tap in Apple Home. Only Matter's On: the web page and the
 * buttons say what they want with their own actions. Stored, so the values are
 * fixed; LAST is 0, which is what every record written before this existed holds. */
typedef enum {
    ZBC_TURN_ON_LAST = 0,     /* back as it was: the group's last brightness */
    ZBC_TURN_ON_ADAPTIVE,     /* start Adaptive Lighting, where the group offers it */
    ZBC_TURN_ON_FULL,         /* full brightness */
    ZBC_TURN_ON_COUNT,
} zbc_turn_on_t;

esp_err_t al_db_init(void);
/* ZBC_AL_EXPOSE_OFF for a group that has never been set. */
uint8_t   al_db_expose(uint16_t group_id);
esp_err_t al_db_set(uint16_t group_id, uint8_t expose);
/* ZBC_TURN_ON_LAST for a group that has never been set. */
uint8_t   al_db_turn_on(uint16_t group_id);
esp_err_t al_db_set_turn_on(uint16_t group_id, uint8_t turn_on);
/* The groups running the mode, as last stored: read once at boot to resume them.
 * Returns how many ids were written to `ids`. */
size_t    al_db_running(uint16_t *ids, size_t max);
/* Stores the groups running the mode now, in any order. Writes only when the set
 * differs from what is stored, so it can be called on every tick. */
esp_err_t al_db_set_running(const uint16_t *ids, size_t n);
/* The group's day: the one stored for it, or the defaults - a little warmer and
 * dimmer than the lamps' own curves at night, a little cooler at noon, no limits,
 * no shape. */
void      al_db_day(uint16_t group_id, zbc_al_profile_t *out);
/* ESP_ERR_INVALID_ARG for a value out of range; nothing is stored then. Does not
 * check that the group exists - lighting_group_set_adaptive_day does. */
esp_err_t al_db_set_day(uint16_t group_id, const zbc_al_profile_t *day);
/* The test al_db_set_day applies, for a caller that checks a whole request
 * before storing any of it. */
bool      al_db_day_ok(const zbc_al_profile_t *day);
/* Whether a day has a sunrise or sunset limit, and whether any group's does -
 * which needs the time zone they are read in. */
bool      al_db_day_has_limits(const zbc_al_profile_t *day);
bool      al_db_any_limits(void);
/* Every setting back to its default, the day included, and the group off the
 * running list, for a group that is deleted - its id may be given out again. */
esp_err_t al_db_forget(uint16_t group_id);
esp_err_t al_db_erase(void);

/* Whether a stored blob of this table ("adaptive") or of the days ("al_days") is
 * one the boot load takes rather than discards. The one statement of that, shared
 * with the configuration restore (config_backup.c), which refuses a file whose
 * table the load would throw away. */
bool      al_db_blob_ok(const void *blob, size_t len);
bool      al_db_days_blob_ok(const void *blob, size_t len);

#ifdef __cplusplus
}
#endif
