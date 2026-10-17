/* Per-light response curves, persisted to NVS.
 *
 * A curve maps the brightness the user asked for - "light this room to 40 %" -
 * to what one particular lamp should do about it: its own output level, and the
 * colour it shows getting there. That is what lets a group slider light a room
 * rather than set every lamp to the same number.
 *
 * Same shape as device_db and group_db: fixed static table, own recursive mutex,
 * hands out copies, own NVS blob with its own version.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "device_db.h"
#include "light_math.h"   /* zbc_pchip */

#ifdef __cplusplus
extern "C" {
#endif

#define ZBC_CURVE_MAX_POINTS 6

/* One monotone curve. The control points are interpolated with a Fritsch-Carlson
 * monotone cubic (PCHIP): a piecewise polynomial whose tangents are clamped so no
 * segment can overshoot the points either side of it. A single polynomial fitted
 * through the same points would oscillate between them, leave the output range
 * and reverse direction - a lamp that dims as the slider goes up. */
typedef struct {
    uint8_t  n;                            /* 0 = no curve, pass the input through; else 2..6 */
    uint8_t  x[ZBC_CURVE_MAX_POINTS];      /* input %, strictly ascending, x[0]=1, x[n-1]=100 */
    uint16_t y[ZBC_CURVE_MAX_POINTS];      /* percent for level, kelvin for ct */
} zbc_curve_t;

/* Colour is one path, not two mappings.
 *
 * A lamp is in one colour mode or the other at any instant, but a curve need
 * not be: "deep magenta at 1 %, daylight at 100 %" is an ordinary thing to want.
 * So there is one ordered list of stops, each either a colour temperature or a
 * colour, and the segment between two stops decides how they are joined:
 *
 *   kelvin -> kelvin   monotone cubic over kelvin, sent as MoveToColorTemperature
 *   colour -> colour   monotone cubic over unwrapped hue and saturation
 *   kelvin <-> colour  a straight line in CIE xy, sent as MoveToColor
 *
 * The mixed case has to be xy because that is the only space in which a colour
 * temperature and a hue are the same kind of thing - a kelvin value is a point
 * on the Planckian locus - and a line across it reads as a clean wash-out rather
 * than a swing through intermediate hues.
 *
 * A stop where the kinds meet belongs to both segments, and is read as the
 * kelvin one: the colour is identical either way, but the command is not, and a
 * colour temperature is what the warmth run either side of it will be sending.
 *
 * Interpolation looks at the whole maximal run of same-kind stops around the
 * segment, not just its two ends, so a pure-warmth path is one monotone cubic
 * through all its stops.
 *
 * Crossing a kelvin/colour boundary changes the command type, and a lamp's own
 * 3300 K is not exactly Planckian 3300 K, so there can be a small step there.
 * That is inherent: it keeps the lamp's white calibration and its mired limits
 * on the stretches that are pure warmth. */
#define ZBC_CHROMA_MAX_STOPS 8

typedef enum {
    ZBC_STOP_KELVIN = 0,
    ZBC_STOP_COLOR  = 1,
} zbc_stop_kind_t;

/* Parallel arrays rather than an array of structs, so a run can be handed to
 * zbc_pchip as &p->x[a] with no copying. */
typedef struct {
    uint8_t  n;                                /* 0 = no path; else 2..8 */
    uint8_t  x[ZBC_CHROMA_MAX_STOPS];          /* input %, ascending, x[0]=1, x[n-1]=100 */
    uint8_t  kind[ZBC_CHROMA_MAX_STOPS];       /* zbc_stop_kind_t */
    uint16_t kelvin[ZBC_CHROMA_MAX_STOPS];     /* ZBC_STOP_KELVIN */
    uint16_t hue[ZBC_CHROMA_MAX_STOPS];        /* ZBC_STOP_COLOR, degrees 0-359 */
    uint8_t  sat[ZBC_CHROMA_MAX_STOPS];        /* ZBC_STOP_COLOR, percent 0-100 */
} zbc_chroma_path_t;

/* Which of the three colour commands a target calls for: exactly one. */
typedef enum {
    ZBC_OUT_NONE = 0,
    ZBC_OUT_CT,          /* kelvin */
    ZBC_OUT_HS,          /* hue and saturation */
    ZBC_OUT_XY,          /* CIE xy, a blend across a kind change */
} zbc_color_out_t;

typedef struct {
    bool     in_use;
    uint8_t  ieee[ZBC_IEEE_LEN];
    uint8_t  ep;
    zbc_curve_t       level;    /* y is an output brightness percent; 0 means off */
    zbc_chroma_path_t chroma;
    /* Runtime only, reset on save and on reboot: what the slider last asked for,
     * and what was last actually put on air, so a one-percent slider step does
     * not re-send a colour that has barely moved.
     *
     * sent_mode is what keeps that honest across a kind change. The three
     * channels track independently, so at a boundary each one is compared
     * against a stale value of its own, both deadbands fall open and every step
     * puts a colour command on air. A change of mode is therefore always sent,
     * and it clears the channels it did not use. */
    int16_t  last_input_pct;
    int32_t  sent_kelvin;
    int16_t  sent_hue, sent_sat;
    int32_t  sent_x, sent_y;
    int8_t   sent_mode;         /* zbc_color_out_t */
    int8_t   sent_off;
} zbc_curveset_t;

/* What one input percentage means for one lamp. */
typedef struct {
    int  pct;        /* 0 = switch the lamp off, else 1-100 */
    zbc_color_out_t out;
    int  kelvin;     /* ZBC_OUT_CT */
    int  hue;        /* ZBC_OUT_HS, degrees */
    int  sat;        /* ZBC_OUT_HS, percent */
    uint16_t cie_x;  /* ZBC_OUT_XY, a fraction of 65536 as ZCL carries it */
    uint16_t cie_y;
} zbc_lamp_target_t;

esp_err_t curve_db_init(void);

bool      curve_db_get(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, zbc_curveset_t *out);
/* Validates and stores; a curve with n == 0 clears that mapping. */
esp_err_t curve_db_set(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, const zbc_curveset_t *in);
esp_err_t curve_db_clear(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep);
/* True when this endpoint has any curve at all, for the device list. */
bool      curve_db_active(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep,
                          bool *level, bool *chroma);

/* Maps an input percentage. With no stored curve this is the identity: pct comes
 * straight through and neither colour output is set. */
void      curve_db_eval(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, int input_pct,
                        zbc_lamp_target_t *out);

/* The level curve read backwards: the input that would have left this endpoint
 * at `level_pct`. A switch-on after a restart knows only where the lamp is,
 * which is the curve's output, while the colour path and the slider both read
 * the input - and on a curve that puts 50 % at 20 %, taking the one for the
 * other puts the colour back at the wrong point and the slider at 20. Without a
 * level curve the two are the same number. A flat stretch of the curve goes to
 * its highest input, so "back at full" reads as 100. */
int       curve_db_input_for(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, int level_pct);

/* The last input percentage this endpoint was commanded to, or ZBC_UNKNOWN.
 * The UI slider binds to this, not to the level the lamp reports: with a curve
 * in the way those two are different numbers and the reported one would never
 * agree with the optimistic value. */
int       curve_db_input_pct(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep);

/* Records the brightness request, separately from the colour bookkeeping below:
 * "no colour command went out" is expressed by not calling that. */
void      curve_db_note_sent(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, int input_pct,
                             bool off);
/* Records a colour command that actually reached the air, so the deadbands and
 * the mode check above have something to compare the next one against. */
void      curve_db_note_color_sent(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep,
                                   const zbc_lamp_target_t *t);

/* Forgets what was last put on air, keeping what was asked for: something else
 * has moved the lamp (an entertainment stream), so the deadbands must not
 * believe the last command still stands, or the next one is held back as
 * already there. */
void      curve_db_forget_sent(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep);

void      curve_db_forget_device(const uint8_t ieee[ZBC_IEEE_LEN]);
esp_err_t curve_db_erase(void);

/* Whether a stored blob of this table is one the boot load takes rather than
 * discards. The one statement of that, shared with the configuration restore
 * (config_backup.c), which refuses a file whose table the load would throw away. */
bool      curve_db_blob_ok(const void *blob, size_t len);

/* zbc_pchip, the interpolation every curve here uses, is declared in
 * light_math.h: a wired light's mixing curves use it too. */

#ifdef __cplusplus
}
#endif
