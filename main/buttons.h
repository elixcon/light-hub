/* Physical buttons: sampling the pins, telling one gesture from another, and
 * turning the winning rule into a lighting command.
 *
 * Sampling is a 10 ms poll rather than an interrupt. At that rate the reading is
 * debounced by construction, there is no ISR service to install, nothing has to
 * live in IRAM, and the 802.15.4 task is left entirely alone - which matters on
 * a chip where one radio is already being time-shared.
 *
 * The gesture machine (btn_step) is deliberately free of ESP-IDF: it takes a raw
 * level and a timestamp and returns an event, so it can be lifted into a file of
 * its own and compiled with cc to be driven over synthetic press trains. Timing
 * faults here show up on hardware only as "the switch feels wrong sometimes",
 * which is not a thing anyone can debug from a serial log. Same reasoning as
 * zbc_pchip and the multipart parser in ota.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "button_db.h"
#include "remote.h"

#ifdef __cplusplus
extern "C" {
#endif

/* All in microseconds, because that is what esp_timer_get_time() speaks. */
#define BTN_POLL_US      (10 * 1000)    /* sample rate */
#define BTN_DEBOUNCE_US  (30 * 1000)    /* three identical samples before believing one */
#define BTN_LONG_US     (600 * 1000)    /* a hold announces itself while still held */
#define BTN_DOUBLE_US   (300 * 1000)    /* gap from release to the next press */

/* Two different things.
 *
 * BTN_STEP_US is what the number in the editor means: "6 % steps" is 6 % every
 * 250 ms, so 24 % per second and a little over four seconds end to end.
 *
 * BTN_REPEAT_US is how often a command actually goes out, and is much longer: a
 * command every 250 ms saturates the link, the lamps' own reports come back out
 * of order, and a lamp can still be climbing seconds after the last command.
 *
 * It costs nothing in smoothness, because the transition sent with each command
 * is the gap to the next one, so the lamp interpolates continuously. What a long
 * interval does cost is how far the last command is still travelling when the
 * button comes up, which is what lighting_target_stop_ramp() is for. The sweep
 * speed is unchanged, because each command is worked out from how long the
 * button has been held, not by adding a step to the last one. */
#define BTN_STEP_US     (250 * 1000)
#define BTN_REPEAT_US  (1000 * 1000)

/* One gesture. LONG arrives while the button is still down, and is followed by a
 * LONG_REPEAT every BTN_REPEAT_US until it is let go. */
typedef enum {
    ZBC_EMIT_NONE = 0,
    ZBC_EMIT_SHORT,
    ZBC_EMIT_LONG,
    ZBC_EMIT_LONG_REPEAT,
    ZBC_EMIT_LONG_RELEASE,   /* the hold ended - stop the lamps where they are */
    ZBC_EMIT_DOUBLE,
    ZBC_EMIT_TRIPLE,         /* only ever from a remote that counted it itself */
    ZBC_EMIT_MOTION,         /* a sensor: motion after a quiet room */
    ZBC_EMIT_CLEAR,          /* a sensor: no motion for its clear_s */
} zbc_btn_emit_t;

typedef struct {
    uint8_t raw;             /* last raw reading, already un-inverted */
    uint8_t stable;          /* what we believe: 1 = pressed */
    uint8_t swallow;         /* the rest of this press is already spoken for */
    uint8_t pending_short;   /* a short press waiting out the double-press window */
    int64_t raw_since;       /* when the raw reading last changed */
    int64_t pressed_at;
    int64_t released_at;
    uint8_t long_fired;
    int64_t next_repeat;
    uint8_t primed;          /* a switch: the position it was found in is known */
} zbc_btn_fsm_t;

/* One 10 ms tick for one button. `level` is the pin as read; `inverted` says a
 * low pin means pressed. `want_double` is false when this button has no
 * double-press rules, and then a short press is emitted the instant the button
 * comes up instead of waiting out the window - otherwise every single click on
 * every button in the house would feel 300 ms late. */
zbc_btn_emit_t btn_step(zbc_btn_fsm_t *st, int level, bool inverted,
                        bool want_double, int64_t now_us);

/* The same for a switch that stays where it is put: every debounced flip, either
 * way, is one ZBC_EMIT_SHORT, and nothing else is ever emitted. The position it
 * is in when first read - at boot, or after its wiring changed - is taken as
 * it stands, so a switch left on does not toggle the light at every restart. */
zbc_btn_emit_t btn_switch_step(zbc_btn_fsm_t *st, int level, bool inverted, int64_t now_us);

/* How long a hold runs before it is ended by the clock rather than by the
 * remote. A release is a single frame on a link that drops them, and without a
 * cap a lost one leaves a command a second going out for ever. Long enough that
 * nobody reaches it with a finger on a button. */
#define ZBTN_HOLD_MAX_US  (30 * 1000 * 1000)

/* How long a hold made of ZB_REMOTE_HOLD_TICKs survives without one: one and a
 * half times the gap the remote itself leaves between frames, within these
 * bounds, and ZBTN_TICK_QUIET_US until a gap has been measured.
 *
 * It has to be learned. A BILRESA sends every 310-330 ms while it turns and
 * about once a second pressed against the top of its range (some write-ups say
 * 101 ms). A fixed timeout shorter than the gap ends the hold between every
 * pair of frames: each frame starts a new sweep and each silence sends a
 * release, and on a turn down the light goes down and back up on every frame. */
#define ZBTN_TICK_QUIET_US      (800 * 1000)
#define ZBTN_TICK_QUIET_MIN_US  (350 * 1000)
#define ZBTN_TICK_QUIET_MAX_US  (1500 * 1000)

/* A tick hold also repeats at the remote's own pace, and its commands aim only
 * that far ahead, rather than BTN_REPEAT_US. A sweep command aims one interval
 * beyond where the sweep is, so a full second of look-ahead is a second the light
 * coasts on after the wheel stops, on top of the silence it takes to notice. Not
 * below this, so a fast remote costs at most two commands a second per lamp. */
#define ZBTN_TICK_REPEAT_MIN_US (500 * 1000)
/* A longer gap than this is a new turn, not the remote's cadence. */
#define ZBTN_TICK_GAP_MAX_US    (1500 * 1000)

/* How long a Stop waits before it ends a Move hold. An IKEA SYMFONISK (E1744)
 * turned steadily puts a Stop on air every 0.2-1.7 s and the next Move 21-67 ms
 * after it, so taking each Stop at its word would end and restart the sweep
 * several times a second. A Move inside this window carries the hold on. The
 * cost is a release that lands this much later - while the lamp is still
 * ramping anyway, and the release reckons from the moment it lands. */
#define ZBTN_STOP_GRACE_US      (150 * 1000)

/* The same three gestures, from a press that arrived over the mesh. Kept apart
 * from zbc_btn_fsm_t because almost none of that state applies: there is no raw
 * level, nothing to debounce, and no threshold to cross - the remote has already
 * decided. */
typedef struct {
    uint8_t pending_click;   /* a click waiting out the double-press window */
    uint8_t holding;
    uint8_t tick_hold;       /* the hold ends on silence, not on a release */
    int64_t clicked_at;
    int64_t held_since;
    int64_t next_repeat;
    int64_t last_tick;
    int64_t tick_gap;        /* the remote's last measured gap between ticks, 0 until one is */
    int64_t stop_at;         /* a Stop is waiting out ZBTN_STOP_GRACE_US until then; 0 if not */
} zbc_zbtn_fsm_t;

/* Ends a hold now, whatever kind and whatever is pending. LONG_RELEASE if one
 * was running, NONE otherwise. */
zbc_btn_emit_t zbtn_end_now(zbc_zbtn_fsm_t *st);

/* One step for one Zigbee button. `ev` is ZB_REMOTE_NONE when this is the 10 ms
 * sampler calling, which is what produces the repeats during a hold and the
 * short press at the end of the double-press window. */
zbc_btn_emit_t zbtn_step(zbc_zbtn_fsm_t *st, zb_remote_ev_t ev, bool want_double,
                         int64_t now_us);

/* A decoded press from a remote. Called from the Zigbee task, so it takes no
 * mutex and does not block: it steps the machine under the sampler's spinlock
 * and posts to the same queue a pin does. `key` may be ZB_REMOTE_KEY_ANY, which
 * a Stop uses to end whichever key of that endpoint is holding. */
void buttons_remote_event(const uint8_t ieee[8], uint8_t ep, uint8_t key, zb_remote_ev_t ev);

/* An occupancy report from a motion sensor, delivered by zb_coordinator in the
 * Zigbee task (zb_sensor_cb_t). Same contract as buttons_remote_event. */
void buttons_sensor_event(const uint8_t ieee[8], uint8_t ep, bool occupied, bool reports_clear);

/* Milliseconds since this Zigbee button last heard from its remote, or this
 * sensor from its sensor, or -1 when it is neither or has heard nothing. The
 * GPIO equivalent is buttons_pin_state: both exist so the UI can tell
 * "mis-configured" from "nobody has touched it". */
int64_t buttons_last_event_ms(uint16_t id);

/* A sensor's state as its rules see it: `occupied` once motion has run its
 * rules and the no-motion ones are still to come, and the milliseconds until
 * those are due (-1 when none are). False when the button is not a sensor. */
bool buttons_sensor_state(uint16_t id, bool *occupied, int64_t *clear_in_ms);

/* The button's actions still waiting out their delays: how many, and the
 * milliseconds until the first. False when there are none. */
bool buttons_delayed(uint16_t id, int *count, int64_t *next_ms);

/* Drops the button's waiting actions: it was changed or deleted, and what it
 * scheduled belongs to rules that may no longer exist. */
void buttons_forget_delayed(uint16_t id);

/* Starts the sampler and the task that runs the actions. Safe to call before
 * Zigbee is up: the pins are read either way, so wiring can be checked from the
 * setup portal, but actions are skipped until the stack is running. */
esp_err_t buttons_start(void);

/* Re-reads button_db and reconfigures the pins. Called whenever the HTTP layer
 * changes a button, so a pin change takes effect without a reboot. */
void buttons_reload(void);

/* The live reading, for the UI - a wiring mistake is otherwise invisible.
 * `pressed` is the debounced logical state, `level` the raw pin. False when the
 * button has no pin yet. */
bool buttons_pin_state(uint16_t id, bool *pressed, int *level);

/* Runs a gesture as though it had been pressed, for the UI's Test button. A
 * sensor's ZBC_EV_MOTION or ZBC_EV_CLEAR runs that event's rules and leaves what
 * the sensor last reported alone.
 *
 * A long press is simulated by holding the pin down for `hold_ms` rather than by
 * posting the event, so the whole real path runs - debounce, the hold threshold,
 * the repeats and the release that stops the lamps. Firing a bare long press
 * would exercise none of that, which is the part worth testing. A short or a
 * double press has no repeats and no release, so those are posted directly.
 * hold_ms is ignored for anything but a long press, and 0 means a default. */
esp_err_t buttons_fire(uint16_t id, zbc_btn_event_t ev, int hold_ms);

/* Why this pin cannot be used, or NULL when it can. GPIO knowledge lives here
 * rather than in the web layer, which has no business knowing where the flash
 * is wired. -1 means "no pin yet" and is allowed. */
const char *buttons_pin_problem(int gpio);

#ifdef __cplusplus
}
#endif
