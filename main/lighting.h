/* Lighting policy: everything between "light this to N %" and the Zigbee
 * commands that follow from it.
 *
 * This is where a brightness percentage stops being a number sent to one lamp
 * and becomes an intent: it is spread over the members of a group, and each
 * lamp's own response curve decides what it does about it - which level, which
 * colour temperature, whether it should be off at all. zb_coordinator knows
 * nothing of curves or groups, and http_server.c is left as transport.
 *
 * Safe to call from any task; the zb_* functions take the stack lock themselves.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "al_db.h"
#include "device_db.h"
#include "group_db.h"
#include "sun.h"
#include "zb_coordinator.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ZCL level 1-254 <-> percent 1-100. Level 0 is "off" territory, so the scale
 * starts at 1 and a request for 1 % still produces a level the lamp can show. */
int      zbc_level_to_pct(int level);
uint8_t  zbc_pct_to_level(int pct);

const zbc_endpoint_t *lighting_find_endpoint(const zbc_device_t *dev, uint8_t ep);

/* device_db_get, except that a wired light's made-up address (light_db.h)
 * answers too, with a device row built for it: one endpoint that can switch.
 * Anything that may be handed a group member or a light from the Lights page
 * asks this rather than device_db. out may be NULL to ask "does it exist". */
bool      lighting_get_device(const uint8_t ieee[ZBC_IEEE_LEN], zbc_device_t *out);

/* Direct colour commands for one endpoint, routed like everything else here so
 * a wired light is reached without http_server.c calling zb_* itself. The colour
 * temperature is clamped to the light's own range. */
esp_err_t lighting_set_color_temp(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint16_t mireds,
                                  uint16_t transition_ds);
esp_err_t lighting_set_color_hs(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint8_t hue,
                                uint8_t saturation, uint16_t transition_ds);

/* input_pct is what the user asked for, 1-100; what the lamp is told depends on
 * its curve. Returns ESP_ERR_NOT_FOUND for an unknown endpoint and
 * ESP_ERR_NOT_SUPPORTED for one that cannot dim. */
esp_err_t lighting_set_brightness(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep,
                                  int input_pct, uint16_t transition_ds);

/* Plain on/off for one endpoint, keeping the curve bookkeeping honest: a lamp
 * switched on by hand must not stay marked as "already off", or a curve that
 * maps to zero would skip the Off it needs to send next time. */
esp_err_t lighting_set_onoff(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, zb_onoff_cmd_t cmd);

/* The device is gone: what was last told to each of its endpoints, and the
 * level each comes back at, go with it. A wired light's or a Matter lamp's
 * made-up address is given out again, and the next light to get it must not
 * start from this one's. */
void      lighting_forget_device(const uint8_t ieee[ZBC_IEEE_LEN]);

/* True if any member of the group is currently known to be on: by its own word,
 * or by ours while that is too recent for its word to have caught up - a room
 * switched off a moment ago is off, though its last report still says on. */
bool      lighting_group_any_on(const zbc_group_t *g);

/* Fans out over the group's members. ESP_ERR_NOT_FOUND for an unknown group,
 * ESP_ERR_NOT_SUPPORTED when no member can dim, ESP_ERR_INVALID_STATE when the
 * group is empty or every member refused. */
esp_err_t lighting_group_set_brightness(uint16_t group_id, int input_pct, uint16_t transition_ds);
esp_err_t lighting_group_set_onoff(uint16_t group_id, zb_onoff_cmd_t cmd, uint16_t transition_ds);
/* lighting_group_set_brightness for a request that also means "switch on": a
 * member that can only switch, and has no curve to read a brightness through,
 * is switched on instead of being skipped. */
esp_err_t lighting_group_switch_on_to(uint16_t group_id, int input_pct, uint16_t transition_ds);

/* What the group is showing now, for something to decide against - a button rule
 * asking "is this room below 30 %", say. `on` is lighting_group_any_on(); `pct`
 * is the commanded value, ZBC_UNKNOWN when nothing has been asked for since the
 * last reboot. False for an unknown group. */
bool      lighting_group_state(uint16_t group_id, bool *on, int *pct);

/* True when no member is known to be on and at least one is known to be off -
 * "dark", as opposed to lighting_group_any_on() being false because the members
 * have stopped answering and nobody knows. Known as lighting_group_any_on()
 * knows it. A group that does not exist is dark. */
bool      lighting_group_known_off(uint16_t group_id);

/* Moves the group's brightness by delta_pct, clamped to 1-100, and reports where
 * it landed so a caller sweeping the range can tell it has hit a rail. A step up
 * from a group that is off starts at the bottom and switches it on -
 * move-to-level-with-on-off does that part for free.
 *
 * This is lighting_group_set_brightness with the arithmetic in front of it, so
 * curves, deadbands, last_pct and the settle reads all still apply. */
/* `coarse` widens the colour deadbands, exactly as lighting_group_ramp_to does
 * and for the same reason: a remote that dims by repeating a nudge is running a
 * sweep, and at the ordinary deadbands a lamp with a warmth curve gets a colour
 * command beside almost every level one. Pass false for a one-off nudge from a
 * short press, where there is no stream to thin out. */
esp_err_t lighting_group_nudge(uint16_t group_id, int delta_pct,
                               uint16_t transition_ds, bool coarse, int *landed_pct);

/* One step of a sweep. Like lighting_group_set_brightness, but with the colour
 * deadbands widened: a held button walks the whole of a warmth curve in a couple
 * of seconds, and the ordinary ones put a colour command beside every level one.
 *
 * Callers should aim one interval *ahead* of where the sweep is now and pass
 * that interval as the transition - see run_dim(). */
esp_err_t lighting_group_ramp_to(uint16_t group_id, int input_pct, uint16_t transition_ds);

/* Ends a sweep: every member is told to stop where it is, and `ended_pct` - the
 * caller's reckoning of where that is - becomes the group's brightness, so the
 * next hold carries on from the light in front of you rather than from the level
 * the last command was still aiming at. Call this when a held button is
 * released. */
esp_err_t lighting_group_stop_ramp(uint16_t group_id, int ended_pct);

/* ------------------------------------------------------ a room or one light
 *
 * What a button rule acts on: a room, or one light's endpoint - a Zigbee lamp, a
 * wired light or a Matter lamp, by the address lighting_get_device() answers
 * for. Each call does for the one what the lighting_group_* call of the same
 * name does for the other, so a caller holding a target never asks which it is:
 * the branch between them lives here, beside the one between the lights' kinds. */

typedef struct {
    uint16_t group_id;              /* the room, when ep is 0 */
    uint8_t  ieee[ZBC_IEEE_LEN];    /* the light, when ep is not 0 */
    uint8_t  ep;
} lighting_target_t;

/* For a light, `pct` is the brightness last asked for, then the level it holds
 * while lit, then ZBC_UNKNOWN; a switch-only light that is on reads 100. False
 * for a room or a light that does not exist. */
bool      lighting_target_state(const lighting_target_t *t, bool *on, int *pct);
/* Known to be off, as lighting_group_known_off. A light that does not exist is dark. */
bool      lighting_target_known_off(const lighting_target_t *t);
/* A light takes the fade setting whatever `transition_ds` says, as the Lights
 * page's switch does; a room is given it. */
esp_err_t lighting_target_set_onoff(const lighting_target_t *t, zb_onoff_cmd_t cmd,
                                    uint16_t transition_ds);
esp_err_t lighting_target_set_brightness(const lighting_target_t *t, int input_pct,
                                         uint16_t transition_ds);
esp_err_t lighting_target_nudge(const lighting_target_t *t, int delta_pct, uint16_t transition_ds,
                                bool coarse, int *landed_pct);
esp_err_t lighting_target_ramp_to(const lighting_target_t *t, int input_pct, uint16_t transition_ds);
esp_err_t lighting_target_stop_ramp(const lighting_target_t *t, int ended_pct);

/* ---------------------------------------------------------- Adaptive Lighting
 *
 * A mode that is started - by the group's sun button in the web UI, a button
 * rule, or a Matter controller - and that then works out a brightness and a
 * kelvin offset from the sun (sun.h) and the group's own day (al_db_day) once a
 * minute and sends them through the ordinary group path: the level curve still
 * decides each lamp's share, and the offset is added to whatever colour
 * temperature the lamp's own warmth curve gives, or to the day's base for a lamp
 * with no curve. A lamp in two rooms running the mode is moved by both, each by
 * its own day: the last tick wins. It ends
 * when the group is switched off or changed by hand. A restart resumes it:
 * which groups run it is stored (al_db), and lighting_al_resume picks them up.
 *
 * Whether a group offers it at all, and how Matter sees it, is the group's
 * exposure (al_db.h). */

typedef struct {
    uint8_t     expose;      /* zbc_al_expose_t */
    bool        active;      /* the mode is running, including while it waits */
    const char *reason;      /* "off", "on", "no_clock", "no_location" */
    int         pct;         /* what the group's day says now, -1 without a clock or a place */
    int         offset_k;
} lighting_al_status_t;

/* Starts the mode, or re-enters it on a group that is already lit: the group
 * moves to the sun's brightness and colour over `transition_ds`. Without a clock
 * or a location the group is switched on as usual and the mode waits.
 * ESP_ERR_NOT_SUPPORTED when the group's exposure is Off; nothing is touched then. */
esp_err_t lighting_group_start_adaptive(uint16_t group_id, uint16_t transition_ds);
/* Ends the mode. The lights stay where they are. */
void      lighting_group_stop_adaptive(uint16_t group_id);
bool      lighting_group_al_status(uint16_t group_id, lighting_al_status_t *out);
/* The group's exposure; Off also ends a running mode. */
esp_err_t lighting_group_set_adaptive_expose(uint16_t group_id, uint8_t expose);
/* The group is gone. */
void      lighting_group_forget_adaptive(uint16_t group_id);
/* Somebody saved a curve for one of a group's lights - a change made by hand. */
void      lighting_note_manual_color(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep);
/* Something every day depends on changed - the location, the time zone.
 * Re-evaluates now instead of at the next minute. */
void      lighting_al_wake(void);
/* Marks the groups that were running the mode before the restart as running
 * again. Sends nothing: the first tick that finds a group lit moves it, and a dark
 * one waits to be lit, as a running group always does. Before Matter starts, so
 * the bridge never reports the mode off and then on; not in the setup portal. */
void      lighting_al_resume(void);
/* Starts the once-a-minute task. Call after Zigbee and Matter are up. */
esp_err_t lighting_al_start(void);
/* Stores the group's day (al_db_set_day) and re-evaluates the group now.
 * ESP_ERR_NOT_FOUND for an unknown group, ESP_ERR_INVALID_ARG for a day out of
 * range. What it sends now is lighting_group_al_status's pct and offset_k. */
esp_err_t lighting_group_set_adaptive_day(uint16_t group_id, const zbc_al_profile_t *day);
/* The sun's day around `utc_s` as it is (`real`) and as the group's mode uses it
 * (`used`: sunrise and sunset moved into its day's limits, when there is a time
 * zone to read them in). False without a location. */
bool      lighting_al_day(uint16_t group_id, int64_t utc_s, zbc_sun_day_t *real, zbc_sun_day_t *used);
/* Whether the sun is down here now: between the real sunset and sunrise, not a
 * room's stretched day; all day in a polar night. False without a clock or a
 * location, and *down is then left alone. A button's "when the sun is down". */
bool      lighting_sun_is_down(bool *down);

/* ------------------------------------------------- entertainment streams
 *
 * A room's Hue colour lamps driven by a stream of pixels (the WLED interface):
 * one pixel per lamp, in the room's member order, at most ZB_ENT_MAX_LAMPS. A
 * command by hand to a streaming lamp or its room ends the stream, and the
 * stream's frames are then dropped until its source has paused for its own
 * timeout. Only one stream at a time. Only with CONFIG_ZBC_WLED. */

/* Returned to a command that would reach a streaming lamp by any way other than
 * a hand - an Adaptive Lighting tick. Quiet, like ZB_ERR_UNREACHABLE: nothing
 * is recorded as sent and nothing warns. Never returned without streams. */
#define LIGHTING_ERR_STREAMING ((esp_err_t)0x5A0002)

#if CONFIG_ZBC_WLED

/* The lamps of a room that would take part, in pixel order. */
size_t    lighting_stream_lamps(uint16_t group_id, zb_ent_lamp_t *out, size_t max);

/* The latest pixels for a room: `n` of them, RGB, scaled by `master` (0-255).
 * The first starts a stream; it ends when none arrives for `timeout_us`. With
 * settings_wled_others_off() the start also switches off the room's lights that
 * cannot take part, which the stream then holds off as it holds its lamps - a
 * hand on one takes back that light alone.
 * ESP_ERR_NOT_FOUND for a room with no lamp that can stream,
 * ESP_ERR_INVALID_STATE while dropped after a hand or another room streams. */
esp_err_t lighting_stream_put(uint16_t group_id, const uint8_t *rgb, size_t n, uint8_t master,
                              int64_t timeout_us);

/* Ends a room's stream as its source asks: the lamps go back to how they were
 * before it, and lights it switched off come back on if they were lit; or with
 * `room_off` the whole room is switched off. Also lifts a
 * hand's override - the source has said it is done. False when no stream of
 * that room was running, and nothing was switched. */
bool      lighting_stream_stop(uint16_t group_id, bool room_off);

typedef enum {
    LIGHTING_STREAM_END_NONE = 0,
    LIGHTING_STREAM_END_SOURCE,   /* the source said so */
    LIGHTING_STREAM_END_TIMEOUT,  /* nothing arrived for its timeout */
    LIGHTING_STREAM_END_HAND,     /* a command by hand */
    LIGHTING_STREAM_END_FAILED,   /* the lamps could not be made ready */
} lighting_stream_end_t;

typedef struct {
    bool                  active;
    bool                  override;        /* dropping frames after a hand */
    uint16_t              group_id;
    uint8_t               lamps;
    uint8_t               lights_off;      /* the room's other lights it holds switched off */
    uint32_t              sessions;        /* since boot, as every counter here */
    uint32_t              packets;         /* taken */
    uint32_t              dropped;         /* dropped after a hand */
    uint32_t              frames;          /* sent to the lamps */
    uint32_t              frames_failed;
    uint32_t              late;            /* ticks that found the next one due already */
    int64_t               started_us;      /* this or the last stream */
    int64_t               ended_us;
    lighting_stream_end_t last_end;
} lighting_stream_status_t;

void      lighting_stream_status(lighting_stream_status_t *out);
#endif

#ifdef __cplusplus
}
#endif
