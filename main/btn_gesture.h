/* What a wired button is, below any rule: reading a pin, telling one gesture from
 * another, and the press-5-times failsafe. Shared with the node (node/), which sends
 * its gestures to the hub rather than running rules on them.
 *
 * Pure functions of a level and a timestamp, compiled with cc by the host tests, for
 * the reasons at the top of buttons.h. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* All in microseconds, because that is what esp_timer_get_time() speaks. */
#define BTN_POLL_US      (10 * 1000)    /* sample rate */
#define BTN_DEBOUNCE_US  (30 * 1000)    /* three identical samples before believing one */
#define BTN_LONG_US     (600 * 1000)    /* a hold announces itself while still held */
#define BTN_DOUBLE_US   (300 * 1000)    /* gap from release to the next press */

/* How often a hold repeats; see BTN_STEP_US in buttons.h for why it is this long. */
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

/* Why this pin cannot be used, or NULL when it can. GPIO knowledge lives here
 * rather than in the web layer, which has no business knowing where the flash
 * is wired. -1 means "no pin yet" and is allowed. */
const char *buttons_pin_problem(int gpio);

/* >>> host-test region: unlock <<< */

/* Five presses, each 0.5-2.5 s after the one before, open the web UI to everyone
 * for a couple of minutes - see auth.c. Keep going at the same pace and the tenth
 * restarts the box into its setup hotspot, keeping the stored Wi-Fi - see
 * wifi_manager_setup_hotspot(). One series rather than two gestures: the web UI
 * opening half way there costs nothing, and there is one thing to remember. A
 * stumble anywhere starts the series over from one.
 *
 * The rhythm is what makes it deliberate: a double click is far quicker, a hold
 * produces no presses at all, and nobody switching a light keeps a steady pace
 * five times running, let alone ten.
 *
 * It counts press-down edges beside the gesture machines rather than inside them,
 * so it sees every press whatever rules the button has - and the lights those
 * presses switch still switch, because hiding them would mean delaying every
 * click in the house. A pure function of (state, input, now) with no ESP-IDF, for
 * the reason btn_step is one. */
#define UNLOCK_TAPS        5
#define HOTSPOT_TAPS       10
#define UNLOCK_GAP_MIN_US  (500 * 1000)
/* 2.5 s, not 1.5 s: a steady press that feels like "about once a second"
 * arrives every 1.9-2.1 s. The rhythm is what makes it deliberate, not its
 * speed. */
#define UNLOCK_GAP_MAX_US  (2500 * 1000)

typedef struct {
    int64_t raw_since;   /* the 64-bit fields first, so the bytes after them pack */
    int64_t last_tap_us;
    uint8_t raw;         /* last raw reading, already un-inverted */
    uint8_t stable;      /* debounced: 1 = pressed */
    uint8_t count;       /* presses so far in this series */
} zbc_unlock_fsm_t;

typedef enum {
    UNLOCK_NONE = 0,
    UNLOCK_OPEN,      /* the fifth press in rhythm: open the web UI */
    UNLOCK_HOTSPOT,   /* the tenth: restart into the setup hotspot */
} zbc_unlock_t;

/* One press, and what it completed. */
static inline zbc_unlock_t unlock_tap(zbc_unlock_fsm_t *st, int64_t now_us)
{
    /* No "is a series running" test is needed: from a count of zero, both
     * branches land on one, including for the very first press after boot. */
    const int64_t gap = now_us - st->last_tap_us;
    if (gap >= UNLOCK_GAP_MIN_US && gap <= UNLOCK_GAP_MAX_US) {
        st->count++;
    } else {
        /* Out of rhythm. Not a reset to zero: this press is the first of a new
         * series, or a stumble on the second press would cost two. */
        st->count = 1;
    }
    st->last_tap_us = now_us;
    /* The series runs on past the fifth press, so that is an equality test: the
     * sixth to ninth complete nothing. */
    if (st->count == UNLOCK_TAPS) {
        return UNLOCK_OPEN;
    }
    if (st->count >= HOTSPOT_TAPS) {
        st->count = 0;
        return UNLOCK_HOTSPOT;
    }
    return UNLOCK_NONE;
}

/* One 10 ms sample of a pin, debounced exactly as btn_step debounces it. */
static inline zbc_unlock_t unlock_level(zbc_unlock_fsm_t *st, int level, bool inverted, int64_t now_us)
{
    const uint8_t pressed = (uint8_t)(inverted ? (level == 0) : (level != 0));
    if (pressed != st->raw) {
        st->raw       = pressed;
        st->raw_since = now_us;
    }
    if (pressed != st->stable && now_us - st->raw_since >= BTN_DEBOUNCE_US) {
        st->stable = pressed;
        return pressed ? unlock_tap(st, now_us) : UNLOCK_NONE;
    }
    return UNLOCK_NONE;
}

/* >>> end host-test region: unlock <<< */

#ifdef __cplusplus
}
#endif
