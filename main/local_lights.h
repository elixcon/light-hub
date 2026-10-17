/* The pins behind the wired lights in light_db: a plain output for an on/off
 * light, one LEDC PWM channel for a dimmable one, two for a tunable white one.
 *
 * Nothing here touches Zigbee or takes the stack lock, so any task may command a
 * wired light - the HTTP handlers, the buttons task and the Matter bridge all
 * reach it through lighting.c, which is the only caller. The calls mirror the
 * zb_* ones one for one, so lighting.c routes one call to one call.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "zb_coordinator.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What a wired light is showing, in the units a Zigbee lamp reports: level
 * 0-254 (the level it comes back to while it is off, as a lamp switched off with
 * OffWithEffect keeps its CurrentLevel), mireds; -1 where it does not apply. */
typedef struct {
    int8_t  on;
    int16_t level;
    int32_t mireds;
} zbc_local_state_t;

/* Drives every wired pin to its power-on state and starts the task that runs the
 * fades. Before buttons_start, and before the setup-portal return in app_main,
 * so a wired light is never left floating. */
esp_err_t local_lights_start(void);

/* Re-reads light_db, re-plans the PWM timers and channels, and reconfigures what
 * changed, handing back any pin no longer used. A light whose wiring did not
 * change keeps its state, so saving the editor does not flash it. */
void      local_lights_reload(void);

/* Every call answers ESP_ERR_NOT_FOUND for an unknown light and
 * ESP_ERR_INVALID_STATE for one that is not wired up yet. The level calls answer
 * ESP_ERR_NOT_SUPPORTED for an on/off light, the colour call for any light but
 * a tunable white one. */

/* Instant, at the level the light last had. A toggle is resolved here. */
esp_err_t local_light_set(uint16_t id, zb_onoff_cmd_t cmd);
/* Move-to-level-with-on-off: level 0 fades out and switches off. A fade from
 * dark or to off eases in and out; one between two lit levels runs at one speed,
 * since it may be a link in a slider's or a held button's chain. */
esp_err_t local_light_set_level(uint16_t id, uint8_t level, uint16_t transition_ds);
/* On, easing up from black rather than from where the level was left. */
esp_err_t local_light_fade_in(uint16_t id, uint8_t level, uint16_t transition_ds);
/* OffWithEffect's mirror: eases out and switches off, keeping the level to come
 * back to. Over transition_ds - the fade setting - where a Zigbee lamp's effect
 * is a fixed 0.8 s; 0 means that 0.8 s. */
esp_err_t local_light_fade_off(uint16_t id, uint16_t transition_ds);
/* A switch-on (off false) or a fading switch-off as ESPHome fades: the brightness
 * asked of the light moves on smootherstep over transition_ds, and `level` and
 * `mireds` (NULL: the colour is left alone) are what the light's curves give at
 * n evenly spaced points on that way, 2 to 16. A light already lit starts from
 * where it is rather than from level[0]; a dark one from black, in mireds[0]. */
#define LOCAL_LIGHT_GLIDE_POINTS 16
esp_err_t local_light_glide(uint16_t id, const uint8_t *level, const uint16_t *mireds, int n,
                            uint16_t transition_ds, bool off);
/* Clamped to the light's own two colour temperatures. Sent while a switch-on or
 * switch-off is fading, it travels with that fade rather than on its own. */
esp_err_t local_light_set_ct(uint16_t id, uint16_t mireds, uint16_t transition_ds);
/* Ends a level ramp where it has got to. A fade out is left to finish. */
esp_err_t local_light_stop(uint16_t id);

/* False when the light is unknown or not wired up. */
bool      local_light_get(uint16_t id, zbc_local_state_t *out);
/* 1 on, 0 off, -1 when the light is not wired up. */
int       local_light_state(uint16_t id);
/* Where in its PWM timer's period a dimming light starts, in degrees (0-359;
 * lights sharing a frequency are spread apart), -1 when it is not driven. */
int       local_light_pwm_phase(uint16_t id);

/* How evenly the fade task has run while something moved, since boot: `ticks`
 * it ran with a fade in progress, `late` of them more than two ticks after the
 * one before, and the longest gap. A ramp is computed from the clock, so a late
 * tick is not lost but taken as one jump - a step that shows near black. */
typedef struct {
    uint32_t ticks;
    uint32_t late;
    uint32_t max_gap_ms;
} local_lights_fade_stats_t;
void      local_lights_fade_stats(local_lights_fade_stats_t *out);

/* Overheat interlock (thermal.c). Holding switches every wired light off at once
 * and refuses anything that would light one again - On, a toggle to on, a level
 * above zero, a fade in - with ESP_ERR_NOT_ALLOWED; off, stop and colour still
 * work. Letting go changes no output: nothing comes back on by itself. Safe to
 * call before local_lights_start(), so a board that boots too hot never drives
 * a light on. */
void      local_lights_hold(bool held);
bool      local_lights_held(void);

/* Why a light cannot drive this pin, or NULL when it can. buttons_pin_problem,
 * plus the failsafe pin: it is sampled as an input whether or not a button uses
 * it, and driving it would take away the only way back into a locked box - and
 * the overheat sensor's pin, which is an analog input. */
const char *local_light_pin_problem(int gpio);

#ifdef __cplusplus
}
#endif
