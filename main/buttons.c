#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "auth.h"
#include "buttons.h"
#include "lighting.h"
#include "settings.h"
#include "state_rev.h"
#include "wifi_manager.h"
#include "zb_coordinator.h"

static const char *TAG = "buttons";

/* ------------------------------------------------------------ the gesture machine */

/* >>> host-test region: btnstep <<< */

/* No ESP-IDF below this line and none above it in this function - see buttons.h.
 * Everything it needs arrives as arguments so a host build can drive it. */
zbc_btn_emit_t btn_step(zbc_btn_fsm_t *st, int level, bool inverted,
                        bool want_double, int64_t now_us)
{
    const uint8_t pressed = (uint8_t)(inverted ? (level == 0) : (level != 0));

    if (pressed != st->raw) {
        st->raw       = pressed;
        st->raw_since = now_us;
    }

    int edge = 0;   /* +1 down, -1 up */
    if (pressed != st->stable && now_us - st->raw_since >= BTN_DEBOUNCE_US) {
        st->stable = pressed;
        edge       = pressed ? 1 : -1;
    }

    if (edge > 0) {
        st->pressed_at = now_us;
        st->long_fired = 0;
        if (st->pending_short) {
            /* The second press of a double. It is announced on the way down
             * rather than on the way up, which is what makes a double-click feel
             * immediate; the rest of this press is then spoken for. */
            st->pending_short = 0;
            st->swallow       = 1;
            return ZBC_EMIT_DOUBLE;
        }
        st->swallow = 0;
        return ZBC_EMIT_NONE;
    }

    if (edge < 0) {
        const uint8_t was_long = st->long_fired;
        const uint8_t swallowed = st->swallow;
        st->long_fired = 0;
        st->swallow    = 0;
        if (was_long) {
            /* The lamps are part-way through a ramp that nothing else will end.
             * This is the event that stops them where the finger left them. */
            return ZBC_EMIT_LONG_RELEASE;
        }
        if (swallowed) {
            return ZBC_EMIT_NONE;   /* already reported as a double press */
        }
        if (want_double) {
            st->pending_short = 1;
            st->released_at   = now_us;
            return ZBC_EMIT_NONE;
        }
        return ZBC_EMIT_SHORT;
    }

    if (st->stable) {
        if (st->swallow) {
            return ZBC_EMIT_NONE;
        }
        if (!st->long_fired && now_us - st->pressed_at >= BTN_LONG_US) {
            st->long_fired  = 1;
            st->next_repeat = now_us + BTN_REPEAT_US;
            return ZBC_EMIT_LONG;
        }
        if (st->long_fired && now_us >= st->next_repeat) {
            st->next_repeat = now_us + BTN_REPEAT_US;
            return ZBC_EMIT_LONG_REPEAT;
        }
        return ZBC_EMIT_NONE;
    }

    if (st->pending_short && now_us - st->released_at >= BTN_DOUBLE_US) {
        st->pending_short = 0;
        return ZBC_EMIT_SHORT;
    }
    return ZBC_EMIT_NONE;
}

zbc_btn_emit_t btn_switch_step(zbc_btn_fsm_t *st, int level, bool inverted, int64_t now_us)
{
    const uint8_t pos = (uint8_t)(inverted ? (level == 0) : (level != 0));

    if (!st->primed) {
        /* First reading: remember it, and believe it once it has held for the
         * debounce, without calling that a flip. */
        if (st->raw_since == 0 || pos != st->raw) {
            st->raw       = pos;
            st->raw_since = now_us ? now_us : 1;
        }
        if (now_us - st->raw_since >= BTN_DEBOUNCE_US) {
            st->stable = pos;
            st->primed = 1;
        }
        return ZBC_EMIT_NONE;
    }

    if (pos != st->raw) {
        st->raw       = pos;
        st->raw_since = now_us;
    }
    if (pos != st->stable && now_us - st->raw_since >= BTN_DEBOUNCE_US) {
        st->stable = pos;
        return ZBC_EMIT_SHORT;
    }
    return ZBC_EMIT_NONE;
}
/* >>> end host-test region: btnstep <<< */

/* --------------------------------------------------- the remote gesture machine */

/* >>> host-test region: zremote <<< */

zbc_btn_emit_t zbtn_end_now(zbc_zbtn_fsm_t *st)
{
    if (!st->holding) {
        return ZBC_EMIT_NONE;
    }
    st->holding   = 0;
    st->tick_hold = 0;
    st->stop_at   = 0;
    return ZBC_EMIT_LONG_RELEASE;
}

/* How much silence ends a tick hold - see ZBTN_TICK_QUIET_US for why it is
 * learned from the remote rather than fixed. */
static int64_t zbtn_tick_quiet(const zbc_zbtn_fsm_t *st)
{
    if (st->tick_gap <= 0) {
        return ZBTN_TICK_QUIET_US;
    }
    int64_t q = st->tick_gap * 3 / 2;
    if (q < ZBTN_TICK_QUIET_MIN_US) {
        q = ZBTN_TICK_QUIET_MIN_US;
    }
    if (q > ZBTN_TICK_QUIET_MAX_US) {
        q = ZBTN_TICK_QUIET_MAX_US;
    }
    return q;
}

/* How often a hold repeats, which is also how far ahead its commands aim: the
 * remote's own gap for a tick hold once one has been measured, a second for
 * everything else - see ZBTN_TICK_REPEAT_MIN_US. */
static int64_t zbtn_repeat_us(const zbc_zbtn_fsm_t *st)
{
    if (!st->tick_hold || st->tick_gap <= 0) {
        return BTN_REPEAT_US;
    }
    if (st->tick_gap < ZBTN_TICK_REPEAT_MIN_US) {
        return ZBTN_TICK_REPEAT_MIN_US;
    }
    return st->tick_gap > BTN_REPEAT_US ? BTN_REPEAT_US : st->tick_gap;
}

/* The same job as btn_step, for a press that arrived over the mesh instead of
 * down a wire. A separate machine because btn_step is wrong for it twice over:
 * it debounces for three samples, which a decoded frame does not need, and it
 * waits out BTN_LONG_US before believing a hold the remote has already spent
 * 400 ms deciding on - a held button would not start dimming for a second.
 *
 * What it emits is identical, so dispatch(), pick_case(), run_hold() and
 * run_release() do not know which kind of button it is, and the sweep stays
 * covered by test_sweep.c.
 *
 * Driven from two places as a pure function of (state, event, now): the 10 ms
 * sampler passes ZB_REMOTE_NONE and gets the repeats and the timeout, the
 * Zigbee callback passes a decoded event. No ESP-IDF below this line - see
 * buttons.h. */
zbc_btn_emit_t zbtn_step(zbc_zbtn_fsm_t *st, zb_remote_ev_t ev, bool want_double, int64_t now_us)
{
    switch (ev) {
    case ZB_REMOTE_HOLD_TICK:
    case ZB_REMOTE_HOLD:
        if (ev == ZB_REMOTE_HOLD_TICK) {
            /* Measured across holds as well as within one: the cadence belongs
             * to the remote, and a release that fired while the cadence was
             * still being learned must not stop the next hold learning it. */
            if (st->last_tick > 0 && now_us - st->last_tick < ZBTN_TICK_GAP_MAX_US) {
                st->tick_gap = now_us - st->last_tick;
            }
            st->last_tick = now_us;
        }
        /* A repeat of a hold that is already running is not a new hold. Some
         * remotes re-send Move while the button is down, and starting over would
         * flip a reversing dimmer round in the middle of a sweep. */
        if (st->holding) {
            /* And a Move that follows a Stop inside the grace carries the hold
             * on - see ZBTN_STOP_GRACE_US. */
            if (ev == ZB_REMOTE_HOLD) {
                st->stop_at = 0;
            }
            return ZBC_EMIT_NONE;
        }
        st->tick_hold    = ev == ZB_REMOTE_HOLD_TICK;
        st->holding      = 1;
        st->stop_at      = 0;
        st->held_since   = now_us;
        st->next_repeat  = now_us + zbtn_repeat_us(st);
        st->pending_click = 0;
        return ZBC_EMIT_LONG;

    case ZB_REMOTE_RELEASE:
        /* A release with nothing held is what a remote sends when its hold never
         * reached us, and stopping lamps that are not ramping is harmless but
         * pointless. */
        if (!st->holding) {
            return ZBC_EMIT_NONE;
        }
        /* A Move hold's Stop may be followed by the next Move of the same turn,
         * so it waits out ZBTN_STOP_GRACE_US; the sampler ends it after that. */
        if (!st->tick_hold) {
            if (st->stop_at == 0) {
                st->stop_at = now_us + ZBTN_STOP_GRACE_US;
            }
            return ZBC_EMIT_NONE;
        }
        return zbtn_end_now(st);

    case ZB_REMOTE_DOUBLE:
        /* The device decoded this one itself, so there is nothing to wait for. */
        st->pending_click = 0;
        return ZBC_EMIT_DOUBLE;

    case ZB_REMOTE_TRIPLE:
        st->pending_click = 0;
        return ZBC_EMIT_TRIPLE;

    case ZB_REMOTE_SINGLE:
        /* A remote that counts its own presses has already waited for a second
         * one; waiting again here would only make every press later. */
        st->pending_click = 0;
        return ZBC_EMIT_SHORT;

    case ZB_REMOTE_CLICK:
        if (st->pending_click && now_us - st->clicked_at < BTN_DOUBLE_US) {
            st->pending_click = 0;
            return ZBC_EMIT_DOUBLE;
        }
        st->clicked_at = now_us;
        /* Same bargain btn_step strikes: waiting out the double window costs
         * every single click 300 ms, so it is only paid by a button that has
         * something to do with a double press. */
        if (!want_double) {
            st->pending_click = 0;
            return ZBC_EMIT_SHORT;
        }
        st->pending_click = 1;
        return ZBC_EMIT_NONE;

    case ZB_REMOTE_NONE:
    default:
        break;
    }

    if (st->holding) {
        /* A Stop nothing carried on from. */
        if (st->stop_at && now_us >= st->stop_at) {
            return zbtn_end_now(st);
        }
        /* A wheel never says it has stopped; it just stops sending. */
        if (st->tick_hold && now_us - st->last_tick >= zbtn_tick_quiet(st)) {
            return zbtn_end_now(st);
        }
        /* A release is one frame, on a link that drops them. Without this a lost
         * one sweeps the lights for ever, a command a second, and the only way
         * out is the rail - which dim_cycle turns round at. */
        if (now_us - st->held_since >= ZBTN_HOLD_MAX_US) {
            return zbtn_end_now(st);
        }
        /* No new sweep command while a Stop is pending: it would aim a whole
         * interval ahead of a turn that has probably ended. */
        if (st->stop_at) {
            return ZBC_EMIT_NONE;
        }
        if (now_us >= st->next_repeat) {
            st->next_repeat = now_us + zbtn_repeat_us(st);
            return ZBC_EMIT_LONG_REPEAT;
        }
        return ZBC_EMIT_NONE;
    }

    if (st->pending_click && now_us - st->clicked_at >= BTN_DOUBLE_US) {
        st->pending_click = 0;
        return ZBC_EMIT_SHORT;
    }
    return ZBC_EMIT_NONE;
}

/* >>> end host-test region: zremote <<< */

/* ------------------------------------------------------------ the failsafe gesture */

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
    uint8_t raw;         /* last raw reading, already un-inverted */
    uint8_t stable;      /* debounced: 1 = pressed */
    int64_t raw_since;
    uint8_t count;       /* presses so far in this series */
    int64_t last_tap_us;
} zbc_unlock_fsm_t;

typedef enum {
    UNLOCK_NONE = 0,
    UNLOCK_OPEN,      /* the fifth press in rhythm: open the web UI */
    UNLOCK_HOTSPOT,   /* the tenth: restart into the setup hotspot */
} zbc_unlock_t;

/* One press, and what it completed. */
static zbc_unlock_t unlock_tap(zbc_unlock_fsm_t *st, int64_t now_us)
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
static zbc_unlock_t unlock_level(zbc_unlock_fsm_t *st, int level, bool inverted, int64_t now_us)
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

/* ------------------------------------------------------------------- live state */

/* One light or room a held button is sweeping. A hold moves every dimming rule
 * of its case at once, each from its own target's level at its own step size. */
typedef struct {
    lighting_target_t target;
    int16_t           from;     /* where it started, on the target's scale; ZBC_UNKNOWN until read */
    int16_t           rate;     /* percent per second, so the clock says where it is */
    uint8_t           action;   /* zbc_btn_action_t, which says which way it goes */
    bool              active;   /* something to move, and to stop on release */
} btn_sweep_t;

/* What the sampler needs, and nothing else. Kept apart from button_db because
 * the esp_timer callback must not take a mutex: it copies this under a spinlock
 * held for a few dozen bytes instead. */
typedef struct {
    bool          used;
    uint16_t      id;
    uint8_t       src;         /* zbc_btn_src_t */
    int8_t        gpio;
    bool          inverted;
    bool          is_switch;          /* ZBC_INPUT_SWITCH: btn_switch_step, not btn_step */
    bool          want_double;
    /* Source: Zigbee. Copied here rather than looked up in button_db, because
     * the sampler runs in the esp_timer task and must not take a mutex. */
    uint8_t       ieee[ZBC_BTN_IEEE_LEN];
    uint8_t       zb_ep;
    uint8_t       zb_key;
    int64_t       last_event_us;   /* for the UI, where a pin would show a dot */
    /* The Test button's end of a hold on a Zigbee button: there is no pin to let
     * go of, so the release comes from the clock instead. */
    int64_t       sim_hold_until;
    /* When this button last nudged, so a stream of nudges can be told from a
     * one-off and given a transition that tracks it. See run_nudges(). */
    int64_t       last_nudge_us;
    zbc_btn_fsm_t fsm;
    zbc_zbtn_fsm_t zfsm;
    zbc_unlock_fsm_t unlock;   /* the press-5-times failsafe, see above */
    /* Which way a reversing dimmer is going. Flipped when a hold begins and
     * again when a light lands on a rail, so a new hold always undoes the
     * direction the last one ended on. Shared by every reversing rule of the
     * button, so the lights one hold dims turn round together. */
    int8_t        dim_dir;
    /* When the current sweeps started, so each command works out its target
     * from the clock rather than from the command before it. A slow or dropped
     * tick then costs a step of smoothness, never a change of speed. */
    int64_t       ramp_since;
    /* One per dimming rule of the case the hold runs - see run_hold(). */
    btn_sweep_t   sweep[ZBC_BTN_MAX_SWEEPS];
    /* The first rule of the case this hold runs, in its gesture's list; -1
     * when none. Chosen when the hold begins and kept to its end: see dispatch(). */
    int8_t        hold_case;
    /* How far ahead this hold's commands aim and how long each takes; 0 means
     * BTN_REPEAT_US. Set by run_case before each step, see zbtn_repeat_us(). */
    int32_t       hold_iv_us;
    /* While this is in the future the sampler reads the button as held, so a
     * test drives the same machine a finger does. */
    int64_t       sim_until;
} btn_live_t;

static btn_live_t     s_live[CONFIG_ZBC_MAX_BUTTONS];
static portMUX_TYPE   s_live_mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t  s_queue;
static esp_timer_handle_t s_poll_timer;

typedef struct {
    uint16_t       id;
    zbc_btn_emit_t emit;
    /* zbc_unlock_t. For UNLOCK_OPEN the sampler has already opened the failsafe
     * and this only gets it logged; UNLOCK_HOTSPOT is carried out by the task,
     * because it writes flash. */
    uint8_t        unlock;
} btn_msg_t;

/* The failsafe pin, when no configured button uses it: then it is sampled here
 * with its own counter. When a button does use it, that button's slot counts. */
static zbc_unlock_fsm_t s_unlock_pin;
static bool             s_unlock_self;

/* Sampler context: the spinlock inside auth_failsafe_open() and a non-blocking
 * post, nothing else. The failsafe is opened here rather than from the task so a
 * full queue can lose the log line but never the unlock. The hotspot cannot be:
 * it writes flash and restarts, so a full queue loses it - and the answer is ten
 * more presses, which is the right cost for something that takes the box off
 * its network. */
static void note_unlock(uint16_t id, zbc_unlock_t what)
{
    if (what == UNLOCK_OPEN) {
        auth_failsafe_open();
    }
    const btn_msg_t msg = { .id = id, .unlock = (uint8_t)what };
    xQueueSend(s_queue, &msg, 0);
}

/* ---------------------------------------------------------------- pin ownership */

/* Pins we configured, so one that stops being used is handed back rather than
 * left pulled in whatever direction it happened to have. */
static bool s_owned[SOC_GPIO_PIN_COUNT];

/* Returns whether the failsafe pin is left to the sampler to watch on its own. */
static bool apply_pins(const zbc_button_t *cfg, size_t n)
{
    bool wanted[SOC_GPIO_PIN_COUNT] = { 0 };

    for (size_t i = 0; i < n; i++) {
        const int pin = cfg[i].gpio;
        if (pin < 0 || pin >= SOC_GPIO_PIN_COUNT) {
            continue;
        }
        wanted[pin] = true;

        gpio_config_t io = {
            .pin_bit_mask = 1ULL << pin,
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = cfg[i].pull == ZBC_PULL_UP   ? GPIO_PULLUP_ENABLE   : GPIO_PULLUP_DISABLE,
            .pull_down_en = cfg[i].pull == ZBC_PULL_DOWN ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&io);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "GPIO %d refused by the driver (0x%x)", pin, err);
            wanted[pin] = false;
            continue;
        }
        s_owned[pin] = true;
    }

    for (int pin = 0; pin < SOC_GPIO_PIN_COUNT; pin++) {
        if (s_owned[pin] && !wanted[pin]) {
            gpio_reset_pin(pin);
            s_owned[pin] = false;
        }
    }

    /* The failsafe pin is watched whether or not anybody made a button of it, so
     * a bare board can always be let back into. A button on that pin wins: its
     * wiring is the truth, and its own slot counts the presses. Not recorded in
     * s_owned, so the loop above never hands it back. */
#if CONFIG_ZBC_UNLOCK_GPIO >= 0
    const int up = CONFIG_ZBC_UNLOCK_GPIO;
    if (up < SOC_GPIO_PIN_COUNT && !wanted[up]) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << up,
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        return gpio_config(&io) == ESP_OK;
    }
#endif
    return false;
}

void buttons_reload(void)
{
    /* On the heap, not the stack. Both tables are CONFIG_ZBC_MAX_BUTTONS long,
     * about 4 KB at 16, and this runs on whatever task calls it - app_main's
     * 5120-byte stack at boot, which they would overflow, and an HTTP handler's
     * afterwards. */
    zbc_button_t *cfg  = calloc(CONFIG_ZBC_MAX_BUTTONS, sizeof(*cfg));
    btn_live_t   *next = calloc(CONFIG_ZBC_MAX_BUTTONS, sizeof(*next));
    if (!cfg || !next) {
        ESP_LOGE(TAG, "No memory to reload the buttons; keeping the ones already loaded");
        free(cfg);
        free(next);
        return;
    }
    const size_t n = button_db_snapshot(cfg, CONFIG_ZBC_MAX_BUTTONS);

    const bool unlock_self = apply_pins(cfg, n);

    /* Rebuilt rather than patched, but the gesture state of a button whose pin
     * and polarity did not change is carried over: reloading while someone has a
     * finger on a button must not read as a fresh press. */
    for (size_t i = 0; i < n; i++) {
        next[i].used        = true;
        next[i].id          = cfg[i].id;
        next[i].src         = cfg[i].src;
        next[i].gpio        = cfg[i].gpio;
        next[i].inverted    = cfg[i].inverted;
        next[i].is_switch   = cfg[i].src == ZBC_BTN_SRC_GPIO && cfg[i].input == ZBC_INPUT_SWITCH;
        next[i].want_double = !next[i].is_switch && cfg[i].rule_count[ZBC_EV_DOUBLE] > 0;
        memcpy(next[i].ieee, cfg[i].ieee, ZBC_BTN_IEEE_LEN);
        next[i].zb_ep       = cfg[i].zb_ep;
        next[i].zb_key      = cfg[i].zb_key;
        next[i].dim_dir     = -1;   /* the first hold flips this, so it dims up */
        next[i].hold_case   = -1;
    }

    portENTER_CRITICAL(&s_live_mux);
    for (size_t i = 0; i < n; i++) {
        for (int j = 0; j < CONFIG_ZBC_MAX_BUTTONS; j++) {
            /* Matching on the source as well as the pin: a button switched from
             * a pin to a remote is a different button as far as a half-finished
             * gesture is concerned, and carrying one over would invent a press. */
            if (s_live[j].used && s_live[j].id == next[i].id && s_live[j].src == next[i].src &&
                s_live[j].gpio == next[i].gpio && s_live[j].inverted == next[i].inverted &&
                s_live[j].is_switch == next[i].is_switch &&
                s_live[j].zb_ep == next[i].zb_ep && s_live[j].zb_key == next[i].zb_key &&
                memcmp(s_live[j].ieee, next[i].ieee, ZBC_BTN_IEEE_LEN) == 0) {
                /* The whole live block, not just the gesture state: a reload
                 * in the middle of a sweep must not restart it or lose the
                 * lights it has to stop. */
                next[i].fsm        = s_live[j].fsm;
                next[i].zfsm       = s_live[j].zfsm;
                next[i].unlock     = s_live[j].unlock;
                next[i].sim_hold_until = s_live[j].sim_hold_until;
                next[i].last_nudge_us  = s_live[j].last_nudge_us;
                next[i].last_event_us = s_live[j].last_event_us;
                next[i].dim_dir    = s_live[j].dim_dir;
                next[i].ramp_since = s_live[j].ramp_since;
                memcpy(next[i].sweep, s_live[j].sweep, sizeof(next[i].sweep));
                next[i].hold_case  = s_live[j].hold_case;
                next[i].sim_until  = s_live[j].sim_until;
                break;
            }
        }
    }
    memcpy(s_live, next, sizeof(s_live));
    s_unlock_self = unlock_self;
    portEXIT_CRITICAL(&s_live_mux);
    free(next);
    free(cfg);
}

/* -------------------------------------------------------------------- sampling */

/* Runs in the esp_timer task: no locks, no blocking, no lighting calls. All it
 * does is decide that something happened and post it. */
static void poll_cb(void *arg)
{
    (void)arg;
    const int64_t now = esp_timer_get_time();

    for (int i = 0; i < CONFIG_ZBC_MAX_BUTTONS; i++) {
        portENTER_CRITICAL(&s_live_mux);
        const bool     used     = s_live[i].used;
        const uint8_t  src      = s_live[i].src;
        const int8_t   pin      = s_live[i].gpio;
        const bool     inverted = s_live[i].inverted;
        const bool     want_dbl = s_live[i].want_double;
        const bool     is_sw    = s_live[i].is_switch;
        const uint16_t id       = s_live[i].id;
        portEXIT_CRITICAL(&s_live_mux);

        /* A Zigbee button has no pin to read. All this tick does for one is give
         * zbtn_step a clock: the repeats during a hold, the hold that timed out,
         * and the short press at the end of the double-press window. */
        if (used && src == ZBC_BTN_SRC_ZIGBEE) {
            portENTER_CRITICAL(&s_live_mux);
            zbc_btn_emit_t zemit = ZBC_EMIT_NONE;
            if (s_live[i].used && s_live[i].id == id && s_live[i].src == src) {
                zb_remote_ev_t ev = ZB_REMOTE_NONE;
                if (s_live[i].sim_hold_until && s_live[i].sim_hold_until <= now) {
                    s_live[i].sim_hold_until = 0;
                    ev = ZB_REMOTE_RELEASE;
                }
                zemit = zbtn_step(&s_live[i].zfsm, ev, want_dbl, now);
            }
            portEXIT_CRITICAL(&s_live_mux);
            if (zemit != ZBC_EMIT_NONE) {
                const btn_msg_t msg = { .id = id, .emit = zemit };
                xQueueSend(s_queue, &msg, 0);
            }
            continue;
        }

        /* A button with no pin yet is stepped only while the Test button holds
         * it, and for as long afterwards as its machine is still in the middle
         * of something - reading it as let go, so the release is seen. Stopped
         * the moment the test ends, the machine would never see the finger come
         * up: no release, the hold left open with its sweeps, and the next test
         * a string of repeats of the old one. */
        portENTER_CRITICAL(&s_live_mux);
        const bool sim  = used && s_live[i].sim_until > now;
        const bool busy = used && (s_live[i].fsm.stable || s_live[i].fsm.raw ||
                                   s_live[i].fsm.pending_short);
        portEXIT_CRITICAL(&s_live_mux);

        if (!used || (pin < 0 && !sim && !busy)) {
            continue;
        }
        /* A simulated hold reads as pressed whichever way the button is wired,
         * so the polarity is applied backwards here to survive btn_step. */
        const int level = sim ? (inverted ? 0 : 1)
                              : (pin >= 0 ? gpio_get_level(pin) : (inverted ? 1 : 0));

        portENTER_CRITICAL(&s_live_mux);
        /* Re-check: a reload between the two critical sections would have moved
         * this slot, and stepping a machine that no longer belongs to this pin
         * would invent a press. */
        zbc_btn_emit_t emit     = ZBC_EMIT_NONE;
        zbc_unlock_t   unlocked = UNLOCK_NONE;
        bool           edge     = false;
        if (s_live[i].used && s_live[i].id == id && s_live[i].gpio == pin) {
            const uint8_t was = s_live[i].fsm.stable;
            emit     = is_sw ? btn_switch_step(&s_live[i].fsm, level, inverted, now)
                             : btn_step(&s_live[i].fsm, level, inverted, want_dbl, now);
            unlocked = unlock_level(&s_live[i].unlock, level, inverted, now);
            edge     = s_live[i].fsm.stable != was;
        }
        portEXIT_CRITICAL(&s_live_mux);
        if (edge) {
            zbc_state_bump();         /* the editor shows the debounced pin */
        }

        if (unlocked != UNLOCK_NONE) {
            note_unlock(id, unlocked);
        }
        if (emit != ZBC_EMIT_NONE) {
            const btn_msg_t msg = { .id = id, .emit = emit };
            /* Dropped rather than waited on. A lost repeat during a dim sweep is
             * invisible; blocking the sampler would cost the next press. */
            xQueueSend(s_queue, &msg, 0);
        }
    }

#if CONFIG_ZBC_UNLOCK_GPIO >= 0
    portENTER_CRITICAL(&s_live_mux);
    const bool self = s_unlock_self;
    portEXIT_CRITICAL(&s_live_mux);
    if (self) {
        /* Only this task touches s_unlock_pin, so it needs no lock of its own. */
        const int level = gpio_get_level(CONFIG_ZBC_UNLOCK_GPIO);
        const zbc_unlock_t u = unlock_level(&s_unlock_pin, level, true, now);
        if (u != UNLOCK_NONE) {
            note_unlock(0, u);
        }
    }
#endif
}

/* --------------------------------------------------------------- running a rule */

/* Which rules a gesture runs. A gesture's rules read as cases: a rule, and the
 * rules after it marked ZBC_RULE_AND, which run with it. The first case whose
 * head's condition holds runs whole, and the rest are not looked at - so
 * "when off: kitchen on, and the reading lamp to 30 %; otherwise both off" is
 * two cases of two rules each.
 *
 * Pure functions of the rules and a condition callback, extracted by
 * `test/Makefile` into test_clause.c: a mistake here shows on a wall as a
 * button that sometimes does the wrong half of what it was told. */
/* >>> host-test region: clause <<< */

/* One past the last rule of the case starting at `head`. */
static int case_end(const zbc_btn_rule_t *rules, int n, int head)
{
    int end = head + 1;
    while (end < n && (rules[end].flags & ZBC_RULE_AND)) {
        end++;
    }
    return end;
}

/* True when some rule of the case has something to do. A case with none - every
 * target deleted, or nothing but "do nothing" - is passed over rather than
 * swallowing the press, as a lone unassigned rule always was. */
static bool case_runs(const zbc_btn_rule_t *rules, int head, int end)
{
    for (int r = head; r < end; r++) {
        if (button_db_rule_assigned(&rules[r]) && rules[r].action != ZBC_ACT_NONE) {
            return true;
        }
    }
    return false;
}

/* The first rule of the first case that applies, and in *end one past its last;
 * -1 when none does. The head's condition is asked of the head's own target, so
 * "always" needs no target at all - a case whose head's room was deleted still
 * runs the rules joined to it - and any other condition needs one. */
static int pick_case(const zbc_btn_rule_t *rules, int n,
                     bool (*holds)(const zbc_btn_rule_t *), int *end)
{
    for (int head = 0; head < n;) {
        const int e = case_end(rules, n, head);
        if (case_runs(rules, head, e) &&
            (rules[head].cond == ZBC_COND_ALWAYS ||
             (button_db_rule_assigned(&rules[head]) && holds(&rules[head])))) {
            *end = e;
            return head;
        }
        head = e;
    }
    return -1;
}

/* What the case's toggles become. One toggle is left to its target, which
 * decides from its own state. Two or more are decided once, before anything is
 * sent: if any of them is lit they all go off, otherwise they all come on - the
 * way a room already toggles its lamps, so a half-lit set never swaps halves. */
static zb_onoff_cmd_t case_toggle(const zbc_btn_rule_t *rules, int n,
                                  bool (*lit)(const zbc_btn_rule_t *))
{
    int toggles = 0;
    for (int r = 0; r < n; r++) {
        if (rules[r].action == ZBC_ACT_TOGGLE && button_db_rule_assigned(&rules[r])) {
            toggles++;
        }
    }
    if (toggles < 2) {
        return ZB_ONOFF_TOGGLE;
    }
    for (int r = 0; r < n; r++) {
        if (rules[r].action == ZBC_ACT_TOGGLE && button_db_rule_assigned(&rules[r]) &&
            lit(&rules[r])) {
            return ZB_ONOFF_OFF;
        }
    }
    return ZB_ONOFF_ON;
}
/* >>> end host-test region: clause <<< */

/* One step of a sweep.
 *
 * The target comes from how long the button has been held, not from adding a
 * step to whatever went out last. That is what lets the commands go out at half
 * the rate the editor's number implies without the sweep becoming half as fast:
 * each command simply covers the ground the clock says it should.
 *
 * The transition matches the gap to the next command, so one ramp runs into the
 * next instead of the lamp arriving early and waiting. */
/* The sweep model below is extracted verbatim by `test/Makefile` and compiled
 * with `cc`, so the rails, the pause at them and the reckoning on release can be
 * driven over holds of every length. A timing fault here shows on hardware only
 * as "the switch feels wrong sometimes", which no serial log explains.
 *
 * Moving or deleting a marker breaks `make -C test`, on purpose.
 */
/* >>> host-test region: sweep <<< */

static bool is_dim(uint8_t action)
{
    return action == ZBC_ACT_DIM_UP || action == ZBC_ACT_DIM_DOWN ||
           action == ZBC_ACT_DIM_CYCLE;
}

/* A rule's target as lighting.c takes it: a room, or one light. */
static lighting_target_t rule_target(const zbc_btn_rule_t *r)
{
    lighting_target_t t = { .group_id = r->group_id, .ep = r->ep };
    memcpy(t.ieee, r->ieee, sizeof(t.ieee));
    return t;
}

/* Which way a sweep goes: its own way for a plain dim, the button's shared
 * direction for a reversing one. */
static int8_t sweep_dir(const btn_sweep_t *w, int8_t cycle_dir)
{
    return w->action == ZBC_ACT_DIM_UP ? 1 : w->action == ZBC_ACT_DIM_DOWN ? -1 : cycle_dir;
}

/* Which way a nudge should go.
 *
 * A remote that dims by repeating a ZCL Step carries a direction in every one,
 * and for a one-button remote it is not the user's: a Hue Smart Button always
 * starts a hold going down and only turns round past its own internal bottom.
 * ZBC_ACT_DIM_CYCLE makes each new hold go the other way, and has to work here
 * as well as for a held pin.
 *
 * `fresh` says this nudge begins a new stream rather than continuing one - the
 * nudge equivalent of a new hold. */
static int8_t nudge_dir(int8_t *dim_dir, uint8_t action, bool fresh)
{
    if (action != ZBC_ACT_DIM_CYCLE) {
        return action == ZBC_ACT_DIM_DOWN ? -1 : 1;
    }
    if (fresh) {
        *dim_dir = (int8_t)(*dim_dir >= 0 ? -1 : 1);
    }
    return *dim_dir;
}

/* And the other half of cycling: turn round on arrival at a rail, so a hold long
 * enough to reach the top comes back down rather than sitting there. Same rule
 * run_hold applies to a swept hold. */
static void nudge_note_landing(int8_t *dim_dir, uint8_t action, int8_t dir, int landed)
{
    if (action != ZBC_ACT_DIM_CYCLE) {
        return;
    }
    if ((dir > 0 && landed >= 100) || (dir < 0 && landed <= 1)) {
        *dim_dir = (int8_t)-dir;
    }
}
/* Where the sweep should be at time `at`, from the clock. Before `since` - the
 * pause after a turn, which is dated to end in the future - it rests where it
 * turned: counted backwards, a light not at the rail would be reckoned as
 * having moved the other way. */
static int ramp_pos(int from, int8_t dir, int pct_per_s, int64_t since, int64_t at)
{
    if (at < since) {
        at = since;
    }
    int pos = from + (int)((int64_t)dir * pct_per_s * (at - since) / 1000000);
    if (pos < 1) {
        pos = 1;
    } else if (pos > 100) {
        pos = 100;
    }
    return pos;
}

/* One step of a hold, for every dimming rule of the case at once.
 *
 * Each command aims one interval ahead of where its sweep is now, with a
 * transition of exactly that interval. Aiming at where the sweep is now asks the
 * lamp to travel a distance it has already travelled: it arrives early, waits,
 * and jerks off again when the next command lands. Aimed an interval ahead, the
 * lamp is still moving at the sweep's own rate when the next command extends
 * the target, so it never stops.
 *
 * `rules` is the case, and is read only when the hold begins (`first`): from
 * then on the sweeps carry their own targets, so the lamps are stopped on
 * release even if the rules were edited in the meantime.
 *
 * ZCL Move, which a Hue dimmer sends its bulbs, is smoother still but not
 * usable here; see CLAUDE.md before reaching for it. */
static void run_hold(int slot, const zbc_btn_rule_t *rules, int n, bool first, int64_t now)
{
    btn_sweep_t sw[ZBC_BTN_MAX_SWEEPS];

    portENTER_CRITICAL(&s_live_mux);
    if (first) {
        bool cycles = false;
        int  k      = 0;
        memset(s_live[slot].sweep, 0, sizeof(s_live[slot].sweep));
        for (int r = 0; r < n && k < ZBC_BTN_MAX_SWEEPS; r++) {
            if (!is_dim(rules[r].action) || !button_db_rule_assigned(&rules[r])) {
                continue;
            }
            /* The editor's number is a step every BTN_STEP_US; as a speed that
             * is step * (1 s / 250 ms) percent per second. */
            const int step = rules[r].action_arg ? rules[r].action_arg : 6;
            btn_sweep_t *w = &s_live[slot].sweep[k++];
            w->target = rule_target(&rules[r]);
            w->from   = ZBC_UNKNOWN;
            w->rate   = (int16_t)((int64_t)step * 1000000 / BTN_STEP_US);
            w->action = rules[r].action;
            w->active = true;
            cycles    = cycles || rules[r].action == ZBC_ACT_DIM_CYCLE;
        }
        if (cycles) {
            /* A new hold undoes the direction the last one ended on. */
            s_live[slot].dim_dir = (int8_t)-s_live[slot].dim_dir;
        }
        s_live[slot].ramp_since = now;
    }
    memcpy(sw, s_live[slot].sweep, sizeof(sw));
    const int8_t  cyc   = s_live[slot].dim_dir;
    const int64_t since = s_live[slot].ramp_since;
    const int64_t iv    = s_live[slot].hold_iv_us > 0 ? s_live[slot].hold_iv_us : BTN_REPEAT_US;
    portEXIT_CRITICAL(&s_live_mux);

    /* Where each sweep starts, read on its first tick. */
    bool read = false;
    for (int k = 0; k < ZBC_BTN_MAX_SWEEPS; k++) {
        if (!sw[k].active || sw[k].from != ZBC_UNKNOWN) {
            continue;
        }
        read = true;
        bool on  = false;
        int  pct = -1;
        if (!lighting_target_state(&sw[k].target, &on, &pct)) {
            sw[k].active = false;
            continue;
        }
        /* Unknown is not dark. Members that have missed a few reads are neither on
         * nor off, so "any on" is false, yet a turn down must still do something.
         * The level last commanded is still known, so start from it unless a
         * member says off. */
        const int from = (pct > 0 && (on || !lighting_target_known_off(&sw[k].target))) ? pct : 0;
        if (from == 0 && sweep_dir(&sw[k], cyc) < 0) {
            /* Dark already, and asked to go darker. Nothing to sweep, but a
             * reversing dimmer should come back up on the next hold. */
            sw[k].active = false;
            continue;
        }
        sw[k].from = (int16_t)from;
    }

    /* At the top or the bottom there is nowhere left to aim. A plain up or down
     * is finished; a reversing dim turns round and sweeps back from the rail -
     * every reversing light of the hold at once, when the first gets there, so
     * they stay together.
     *
     * Only when the sweep is heading into the rail: otherwise this also fires on
     * the tick that arrives while the light is still resting at the rail, and
     * the sweep turns round every tick without ever leaving.
     *
     * Checked for all of them before anything is sent. The turn re-dates every
     * sweep, and one sent earlier in the same tick would be travelling to where
     * the model no longer says it is: a release in the pause would be reckoned
     * short of it, and the light would jump back. */
    bool turn = false;
    for (int k = 0; k < ZBC_BTN_MAX_SWEEPS; k++) {
        if (!sw[k].active || sw[k].action != ZBC_ACT_DIM_CYCLE) {
            continue;
        }
        const int here = ramp_pos(sw[k].from, cyc, sw[k].rate, since, now);
        if ((cyc > 0 && here >= 100) || (cyc < 0 && here <= 1)) {
            turn = true;
        }
    }
    if (turn) {
        for (int k = 0; k < ZBC_BTN_MAX_SWEEPS; k++) {
            if (sw[k].active) {
                sw[k].from = (int16_t)ramp_pos(sw[k].from, sweep_dir(&sw[k], cyc), sw[k].rate, since, now);
            }
        }
        portENTER_CRITICAL(&s_live_mux);
        memcpy(s_live[slot].sweep, sw, sizeof(sw));
        s_live[slot].dim_dir = (int8_t)-cyc;
        /* The sweep resumes at the next tick, not now. This tick sends no command,
         * so the light rests at the rail for a whole BTN_REPEAT_US - long enough
         * to let go there - and the model has to say so. Dated from now, the
         * model would run an interval ahead of the lamp: the next command would
         * travel at twice the sweep rate, and a release in that interval would be
         * reckoned up to an interval's travel away and make the light jump. A
         * plain dim in the same hold rests with it, for the one interval. */
        s_live[slot].ramp_since = now + iv;
        portEXIT_CRITICAL(&s_live_mux);
        return;
    }
    if (read) {
        portENTER_CRITICAL(&s_live_mux);
        memcpy(s_live[slot].sweep, sw, sizeof(sw));
        portEXIT_CRITICAL(&s_live_mux);
    }

    for (int k = 0; k < ZBC_BTN_MAX_SWEEPS; k++) {
        if (!sw[k].active) {
            continue;
        }
        const int8_t dir   = sweep_dir(&sw[k], cyc);
        const int    here  = ramp_pos(sw[k].from, dir, sw[k].rate, since, now);
        const int    ahead = ramp_pos(sw[k].from, dir, sw[k].rate, since, now + iv);
        if ((dir > 0 && here >= 100) || (dir < 0 && here <= 1)) {
            continue;         /* a plain dim at its rail is finished */
        }
        lighting_target_ramp_to(&sw[k].target, ahead, (uint16_t)(iv / 100000));
    }
}

/* The finger came off. Whatever the lamps are still travelling towards, they
 * stop here. */
static void run_release(int slot, int64_t now)
{
    btn_sweep_t sw[ZBC_BTN_MAX_SWEEPS];

    portENTER_CRITICAL(&s_live_mux);
    memcpy(sw, s_live[slot].sweep, sizeof(sw));
    const int8_t  cyc   = s_live[slot].dim_dir;
    const int64_t since = s_live[slot].ramp_since;
    for (int k = 0; k < ZBC_BTN_MAX_SWEEPS; k++) {
        s_live[slot].sweep[k].active = false;
        s_live[slot].sweep[k].from   = ZBC_UNKNOWN;
    }
    portEXIT_CRITICAL(&s_live_mux);

    /* Where each sweep is at the moment the finger came up - not where the last
     * command was aiming, a full interval further on. Worked out rather than
     * read back: level reports lag badly and unevenly, and a stale one would
     * start the next hold from a number that was never true (a reversing dimmer
     * then climbs on consecutive holds instead of alternating). */
    for (int k = 0; k < ZBC_BTN_MAX_SWEEPS; k++) {
        if (!sw[k].active || sw[k].from == ZBC_UNKNOWN) {
            continue;
        }
        lighting_target_stop_ramp(&sw[k].target,
                                  ramp_pos(sw[k].from, sweep_dir(&sw[k], cyc), sw[k].rate, since, now));
    }
}
/* >>> end host-test region: sweep <<< */

/* A dim nudge must not use the global fade time.
 *
 * That is the time a lamp takes to fade on or off. A remote that dims by
 * repeating a nudge - a Hue Smart Button sends a ZCL Step about every 800 ms
 * while held - would with an 800 ms fade command an 800 ms ramp every 800 ms,
 * leaving the light a whole step behind the thumb and the first step slow to
 * arrive.
 *
 * So a nudge that closely follows another is part of a sweep, and its
 * transition is the gap the last one measured: the lamp travels one step in
 * exactly the time before the next command extends it, at whatever rate the
 * remote uses. A nudge on its own is a single press and should be quick.
 *
 * The same test decides the colour deadbands, for the reason lighting.h gives:
 * a sweep at the ordinary ones puts a colour command beside almost every level
 * command on a lamp with a warmth curve. */
#define BTN_NUDGE_STREAM_US (2500 * 1000)  /* a gap longer than this is a fresh press */
#define BTN_NUDGE_MIN_DS    1              /* 100 ms: brisk, but not a snap */
#define BTN_NUDGE_MAX_DS    15             /* a lost step must not leave the lamp crawling */
#define BTN_NUDGE_ONE_OFF_DS 2

static int64_t nudge_gap(int slot, int64_t now)
{
    portENTER_CRITICAL(&s_live_mux);
    const int64_t last = s_live[slot].last_nudge_us;
    portEXIT_CRITICAL(&s_live_mux);
    return last == 0 ? INT64_MAX : now - last;
}

static uint16_t nudge_ds(int64_t gap)
{
    if (gap > BTN_NUDGE_STREAM_US) {
        return BTN_NUDGE_ONE_OFF_DS;
    }
    int64_t ds = gap / 100000;
    if (ds < BTN_NUDGE_MIN_DS) {
        ds = BTN_NUDGE_MIN_DS;
    } else if (ds > BTN_NUDGE_MAX_DS) {
        ds = BTN_NUDGE_MAX_DS;
    }
    return (uint16_t)ds;
}

/* A dim bound to a short or a double press - or a remote's Step, which is a
 * click - is a single nudge of each dimming rule's target. The stream is one
 * per button, so how long ago the last nudge was, the transition and the
 * reversing direction are worked out once for the press. */
static void run_nudges(int slot, const zbc_btn_rule_t *rules, int n, int64_t now)
{
    bool any = false, cycles = false;
    for (int r = 0; r < n; r++) {
        if (is_dim(rules[r].action) && button_db_rule_assigned(&rules[r])) {
            any    = true;
            cycles = cycles || rules[r].action == ZBC_ACT_DIM_CYCLE;
        }
    }
    if (!any) {
        return;
    }

    /* Read the stream state once: nudge_ds and the direction both depend on how
     * long ago the last nudge was, and it must not move between them. */
    const int64_t  gap      = nudge_gap(slot, now);
    const bool     sweeping = gap <= BTN_NUDGE_STREAM_US;
    const uint16_t tds      = nudge_ds(gap);

    portENTER_CRITICAL(&s_live_mux);
    const int8_t cyc = cycles ? nudge_dir(&s_live[slot].dim_dir, ZBC_ACT_DIM_CYCLE, !sweeping) : 1;
    s_live[slot].last_nudge_us = now;
    portEXIT_CRITICAL(&s_live_mux);

    /* The first reversing light to report where it landed decides a turn at a
     * rail, so the reversing lights of the press turn together. */
    bool turned = false;
    for (int r = 0; r < n; r++) {
        const zbc_btn_rule_t *rule = &rules[r];
        if (!is_dim(rule->action) || !button_db_rule_assigned(rule)) {
            continue;
        }
        const int8_t dir = rule->action == ZBC_ACT_DIM_CYCLE ? cyc
                         : rule->action == ZBC_ACT_DIM_DOWN ? -1 : 1;
        const lighting_target_t t = rule_target(rule);
        int landed = -1;
        lighting_target_nudge(&t, dir * (rule->action_arg ? rule->action_arg : 6), tds, sweeping, &landed);
        if (rule->action == ZBC_ACT_DIM_CYCLE && !turned && landed >= 0) {
            turned = true;
            portENTER_CRITICAL(&s_live_mux);
            nudge_note_landing(&s_live[slot].dim_dir, rule->action, dir, landed);
            portEXIT_CRITICAL(&s_live_mux);
        }
    }
}

static bool cond_holds(const zbc_btn_rule_t *r)
{
    bool on  = false;
    int  pct = -1;
    const lighting_target_t t = rule_target(r);
    if (!lighting_target_state(&t, &on, &pct)) {
        return false;
    }
    /* Not knowing counts as off and as nothing, the way lighting_group_any_on()
     * already reads it - so a button still does something after a reboot. */
    const int level = pct < 0 ? 0 : (on ? pct : 0);

    switch (r->cond) {
    case ZBC_COND_ALWAYS: return true;
    case ZBC_COND_OFF:    return !on;
    case ZBC_COND_ON:     return on;
    case ZBC_COND_BELOW:  return level < r->cond_arg;
    case ZBC_COND_ABOVE:  return level > r->cond_arg;
    default:              return false;
    }
}

/* Lit, as a toggle decides it: a room with any member on, a light that is on. */
static bool rule_lit(const zbc_btn_rule_t *r)
{
    bool on = false;
    const lighting_target_t t = rule_target(r);
    return lighting_target_state(&t, &on, NULL) && on;
}

/* One case - the rule that won and the rules joined to it - in order. */
static void run_case(int slot, const zbc_btn_rule_t *rules, int n, zbc_btn_emit_t emit, int64_t now)
{
    const uint16_t ds = settings_transition_ds();

    /* Before anything is sent: a dim in the same case may light what a toggle
     * is asked about. */
    const zb_onoff_cmd_t toggle = emit == ZBC_EMIT_LONG_REPEAT ? ZB_ONOFF_TOGGLE
                                                               : case_toggle(rules, n, rule_lit);

    /* Only a hold sweeps. A dim bound to a short or a double press is a single
     * nudge, which is a reasonable thing to want. */
    if (emit == ZBC_EMIT_LONG || emit == ZBC_EMIT_LONG_REPEAT) {
        portENTER_CRITICAL(&s_live_mux);
        s_live[slot].hold_iv_us = (int32_t)zbtn_repeat_us(&s_live[slot].zfsm);
        portEXIT_CRITICAL(&s_live_mux);
        run_hold(slot, rules, n, emit == ZBC_EMIT_LONG, now);
    } else {
        run_nudges(slot, rules, n, now);
    }

    /* Everything else fires once; the repeats of a hold are ignored. */
    if (emit == ZBC_EMIT_LONG_REPEAT) {
        return;
    }

    for (int r = 0; r < n; r++) {
        const zbc_btn_rule_t *rule = &rules[r];
        if (!button_db_rule_assigned(rule)) {
            continue;          /* the room or the light it named was deleted */
        }
        const lighting_target_t t = rule_target(rule);
        switch (rule->action) {
        case ZBC_ACT_ON:     lighting_target_set_onoff(&t, ZB_ONOFF_ON, ds);  break;
        case ZBC_ACT_OFF:    lighting_target_set_onoff(&t, ZB_ONOFF_OFF, ds); break;
        case ZBC_ACT_TOGGLE: lighting_target_set_onoff(&t, toggle, ds);       break;
        case ZBC_ACT_SET:    lighting_target_set_brightness(&t, rule->action_arg, ds); break;
        /* A room's mode; refused, and nothing happens, for a group whose
         * exposure is Off. button_db never stores one for a light. */
        case ZBC_ACT_ADAPTIVE:
            if (rule->ep == 0) {
                lighting_group_start_adaptive(rule->group_id, ds);
            }
            break;
        default: break;
        }
    }
}

static const char *emit_name(zbc_btn_emit_t e)
{
    switch (e) {
    case ZBC_EMIT_SHORT:        return "short press";
    case ZBC_EMIT_LONG:         return "long press";
    case ZBC_EMIT_LONG_REPEAT:  return "long press (held)";
    case ZBC_EMIT_LONG_RELEASE: return "long press released";
    case ZBC_EMIT_DOUBLE:       return "double press";
    case ZBC_EMIT_TRIPLE:       return "triple press";
    default:                   return "?";
    }
}

static int slot_of(uint16_t id)
{
    int slot = -1;
    portENTER_CRITICAL(&s_live_mux);
    for (int i = 0; i < CONFIG_ZBC_MAX_BUTTONS; i++) {
        if (s_live[i].used && s_live[i].id == id) {
            slot = i;
            break;
        }
    }
    portEXIT_CRITICAL(&s_live_mux);
    return slot;
}

static void dispatch(const btn_msg_t *msg)
{
    /* A release stops whatever is ramping and asks nothing else. Going back to
     * the rules for it would be wrong twice over: the conditions may have moved
     * on during the sweep, and there may no longer be a rule at all - and the
     * lamps still have to be told to stop. */
    if (msg->emit == ZBC_EMIT_LONG_RELEASE) {
        const int slot = slot_of(msg->id);
        if (slot >= 0) {
            portENTER_CRITICAL(&s_live_mux);
            s_live[slot].hold_case = -1;
            portEXIT_CRITICAL(&s_live_mux);
            if (zb_is_running()) {
                run_release(slot, esp_timer_get_time());
            }
        }
        return;
    }

    zbc_button_t b;
    if (!button_db_get(msg->id, &b)) {
        return;
    }

    zbc_btn_event_t ev;
    switch (msg->emit) {
    case ZBC_EMIT_SHORT:  ev = ZBC_EV_SHORT;  break;
    case ZBC_EMIT_DOUBLE: ev = ZBC_EV_DOUBLE; break;
    case ZBC_EMIT_TRIPLE: ev = ZBC_EV_TRIPLE; break;
    default:              ev = ZBC_EV_LONG;   break;
    }

    /* A repeat during a sweep would otherwise put four lines a second in the log
     * and drown everything the radio has to say. */
    if (msg->emit != ZBC_EMIT_LONG_REPEAT) {
        ESP_LOGI(TAG, "%s on '%s'", emit_name(msg->emit), b.name);
    }

    if (!zb_is_running()) {
        /* Reaching zb_set_onoff() before esp_zigbee_init() asserts and panics,
         * and a stored device still has a short address, so the lookup that
         * would otherwise stop us succeeds. Guard here or the setup portal is
         * one button press away from a reboot loop. */
        if (msg->emit != ZBC_EMIT_LONG_REPEAT) {
            ESP_LOGW(TAG, "Zigbee is not running - '%s' does nothing yet", b.name);
        }
        return;
    }

    const int slot = slot_of(msg->id);
    if (slot < 0) {
        return;
    }

    int n = 0;
    const zbc_btn_rule_t *rules = button_db_rules(&b, ev, &n);
    int head = -1, end = 0;
    if (msg->emit == ZBC_EMIT_LONG_REPEAT) {
        /* The case the hold began with, not whichever holds now. A hold changes
         * the very state the conditions read - "when off: on, otherwise dim"
         * would switch the room on and then, a second later, start a sweep in
         * the middle of the hold from nowhere - and the sweeps in progress
         * belong to the case that started them. */
        portENTER_CRITICAL(&s_live_mux);
        head = s_live[slot].hold_case;
        portEXIT_CRITICAL(&s_live_mux);
        if (head >= 0 && head < n) {
            end = case_end(rules, n, head);
        } else {
            head = -1;
        }
    } else {
        head = pick_case(rules, n, cond_holds, &end);
        if (msg->emit == ZBC_EMIT_LONG) {
            portENTER_CRITICAL(&s_live_mux);
            s_live[slot].hold_case = (int8_t)head;
            portEXIT_CRITICAL(&s_live_mux);
        }
    }
    if (head < 0) {
        return;
    }

    run_case(slot, &rules[head], end - head, msg->emit, esp_timer_get_time());
}

static void buttons_task(void *arg)
{
    (void)arg;
    btn_msg_t msg;
    while (xQueueReceive(s_queue, &msg, portMAX_DELAY) == pdTRUE) {
        if (msg.unlock == UNLOCK_HOTSPOT) {
            ESP_LOGW(TAG, "Ten presses on %s %d: restarting into the setup hotspot, keeping the stored Wi-Fi",
                     msg.id ? "button" : "GPIO", msg.id ? (int)msg.id : CONFIG_ZBC_UNLOCK_GPIO);
            wifi_manager_setup_hotspot();   /* returns only if the flag could not be stored */
            continue;
        }
        if (msg.unlock == UNLOCK_OPEN) {
            if (msg.id) {
                ESP_LOGW(TAG, "Failsafe opened by button %u: the web UI is open to everyone for %d s",
                         msg.id, auth_failsafe_left_s());
            } else {
                ESP_LOGW(TAG, "Failsafe opened by GPIO %d: the web UI is open to everyone for %d s",
                         CONFIG_ZBC_UNLOCK_GPIO, auth_failsafe_left_s());
            }
            continue;
        }
        dispatch(&msg);
    }
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------------- public */

const char *buttons_pin_problem(int gpio)
{
    if (gpio == -1) {
        return NULL;                       /* a button without a pin is just unfinished */
    }
    if (gpio < 0 || gpio >= SOC_GPIO_PIN_COUNT) {
        return "that pin does not exist on this chip";
    }
#if CONFIG_IDF_TARGET_ESP32C6
    /* GPIO 12 and 13 are the USB port's D- and D+, and allowed: the console is
     * the UART, and a converted Shelly wires its wall switch input to GPIO 12.
     * Using one disconnects USB serial while the pin is configured; flashing over
     * USB still works from the ROM bootloader (hold BOOT while plugging in). The
     * page says so beside those two pins. */
    if (gpio >= 24 && gpio <= 30) {
        return "GPIO 24-30 are wired to the flash chip";
    }
#endif
    return NULL;
}

esp_err_t buttons_start(void)
{
    if (s_queue) {
        return ESP_OK;
    }
    s_queue = xQueueCreate(8, sizeof(btn_msg_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(buttons_task, "buttons", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    buttons_reload();

    /* zb_coordinator must not know that buttons exist - buttons.c already calls
     * into it, and a call back the other way would close the loop. So it offers
     * a handler to register, the way the SDK's own ZCL action handler does. */
    zb_set_remote_handler(buttons_remote_event);

    /* Periodic rather than the re-arm-from-the-callback pattern used everywhere
     * else here. That pattern exists so a job can choose its next interval -
     * the Wi-Fi retry backs off, the neighbour scan slows down. A fixed sampler
     * has no such choice, and re-arming it a hundred times a second would be
     * work for its own sake. */
    const esp_timer_create_args_t args = { .callback = poll_cb, .name = "btn_poll" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_poll_timer), TAG, "poll timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_poll_timer, BTN_POLL_US), TAG, "poll start");

    ESP_LOGI(TAG, "Watching buttons every %d ms", BTN_POLL_US / 1000);
    return ESP_OK;
}

/* A decoded press from a remote, delivered by zb_coordinator in the Zigbee task.
 * Same contract as the sampler: spinlock only, no blocking, no lighting calls -
 * the queue and the buttons task do the rest. */
void buttons_remote_event(const uint8_t ieee[ZBC_BTN_IEEE_LEN], uint8_t ep, uint8_t key,
                          zb_remote_ev_t ev)
{
    if (!s_queue || !ieee || ev == ZB_REMOTE_NONE) {
        return;
    }
    const int64_t now = esp_timer_get_time();

    /* One dial cannot turn both ways at once. A hold starting on one key ends a
     * hold on any other key of the same endpoint first, or a wheel turned back
     * would run two sweeps against each other until the old one timed out - a
     * BILRESA's for ZBTN_TICK_QUIET_US, an E1744's until its Stop. The release is
     * queued ahead of the new hold, so the lamps are stopped before they turn. */
    /* Except that a Move-based hold keeps its direction until its Stop. An IKEA
     * SYMFONISK (E1744) turned steadily one way sends Move down, Move up, Move
     * down, Move up - same rate, consecutive tsns, no Stop between - and only the
     * first is the way the dial is turning (deCONZ and zigbee2mqtt see the same
     * from firmware 2.3.080). The first Move after silence or a Stop is reliable;
     * what follows it before the next Stop is noise. So an opposite Move is
     * ignored while a Move hold runs, and honoured - ending the old hold at once -
     * once a Stop is pending, since that Move starts a new run. Reversing the
     * dial without a pause therefore waits for the remote's next Stop. A wheel's
     * tick holds are left alone: their direction comes from the levels, which do
     * not lie. */
    if ((ev == ZB_REMOTE_HOLD || ev == ZB_REMOTE_HOLD_TICK) && key != ZB_REMOTE_KEY_ANY) {
        bool latched = false;
        for (int i = 0; i < CONFIG_ZBC_MAX_BUTTONS; i++) {
            portENTER_CRITICAL(&s_live_mux);
            zbc_btn_emit_t emit = ZBC_EMIT_NONE;
            uint16_t       id   = 0;
            if (s_live[i].used && s_live[i].src == ZBC_BTN_SRC_ZIGBEE && s_live[i].zb_ep == ep &&
                memcmp(s_live[i].ieee, ieee, ZBC_BTN_IEEE_LEN) == 0 &&
                s_live[i].zb_key != key && s_live[i].zfsm.holding) {
                if (ev == ZB_REMOTE_HOLD && !s_live[i].zfsm.tick_hold && !s_live[i].zfsm.stop_at) {
                    latched = true;
                } else {
                    id   = s_live[i].id;
                    emit = zbtn_end_now(&s_live[i].zfsm);
                }
            }
            portEXIT_CRITICAL(&s_live_mux);
            if (emit != ZBC_EMIT_NONE) {
                const btn_msg_t msg = { .id = id, .emit = emit };
                xQueueSend(s_queue, &msg, 0);
            }
        }
        if (latched) {
            return;
        }
    }

    /* A key of ANY comes from a ZCL Stop, which carries no direction: it ends
     * whichever key of this endpoint is holding, and there can only be one. */
    for (int i = 0; i < CONFIG_ZBC_MAX_BUTTONS; i++) {
        portENTER_CRITICAL(&s_live_mux);
        zbc_btn_emit_t emit     = ZBC_EMIT_NONE;
        uint16_t       id       = 0;
        zbc_unlock_t   unlocked = UNLOCK_NONE;
        if (s_live[i].used && s_live[i].src == ZBC_BTN_SRC_ZIGBEE && s_live[i].zb_ep == ep &&
            memcmp(s_live[i].ieee, ieee, ZBC_BTN_IEEE_LEN) == 0 &&
            (key == ZB_REMOTE_KEY_ANY ? s_live[i].zfsm.holding != 0 : s_live[i].zb_key == key)) {
            id = s_live[i].id;
            s_live[i].last_event_us = now;
            zbc_state_bump();         /* "last heard" on the button's page */
            emit = zbtn_step(&s_live[i].zfsm, ev, s_live[i].want_double, now);
            /* Clicks on the on/off key only. A Hue Smart Button decodes each Level
             * Step as a click on key 1 or 2, and sends one about every 800 ms while
             * held - so counting those would let a four-second hold open the box. */
            if ((ev == ZB_REMOTE_CLICK || ev == ZB_REMOTE_SINGLE) && key == 0) {
                unlocked = unlock_tap(&s_live[i].unlock, now);
            }
        }
        portEXIT_CRITICAL(&s_live_mux);

        if (unlocked != UNLOCK_NONE) {
            note_unlock(id, unlocked);
        }

        if (emit != ZBC_EMIT_NONE) {
            const btn_msg_t msg = { .id = id, .emit = emit };
            xQueueSend(s_queue, &msg, 0);
        }
    }
}

/* How long ago this button last heard from its remote, in milliseconds, or -1
 * when it is not a Zigbee button or has never been heard from. The pin dot is
 * the only way to tell a wiring mistake from a configuration one; this is the
 * same thing for a remote nobody has pressed yet. */
int64_t buttons_last_event_ms(uint16_t id)
{
    int64_t seen = 0;
    bool    zigbee = false;
    portENTER_CRITICAL(&s_live_mux);
    for (int i = 0; i < CONFIG_ZBC_MAX_BUTTONS; i++) {
        if (s_live[i].used && s_live[i].id == id) {
            zigbee = s_live[i].src == ZBC_BTN_SRC_ZIGBEE;
            seen   = s_live[i].last_event_us;
            break;
        }
    }
    portEXIT_CRITICAL(&s_live_mux);
    if (!zigbee || seen == 0) {
        return -1;
    }
    return (esp_timer_get_time() - seen) / 1000;
}

bool buttons_pin_state(uint16_t id, bool *pressed, int *level)
{
    int8_t pin    = -1;
    bool   stable = false;

    portENTER_CRITICAL(&s_live_mux);
    for (int i = 0; i < CONFIG_ZBC_MAX_BUTTONS; i++) {
        if (s_live[i].used && s_live[i].id == id) {
            /* A Zigbee button has no pin to read, and reporting one would be a
             * wiring diagnosis of a button with no wiring. */
            pin    = s_live[i].src == ZBC_BTN_SRC_ZIGBEE ? -1 : s_live[i].gpio;
            stable = s_live[i].fsm.stable != 0;
            break;
        }
    }
    portEXIT_CRITICAL(&s_live_mux);

    if (pin < 0) {
        return false;
    }
    if (level) {
        *level = gpio_get_level(pin);
    }
    if (pressed) {
        *pressed = stable;
    }
    return true;
}

esp_err_t buttons_fire(uint16_t id, zbc_btn_event_t ev, int hold_ms)
{
    if (!s_queue || ev >= ZBC_EV_COUNT) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!button_db_get(id, NULL)) {
        return ESP_ERR_NOT_FOUND;
    }

    const int slot = slot_of(id);
    if (slot < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    portENTER_CRITICAL(&s_live_mux);
    const bool zigbee = s_live[slot].src == ZBC_BTN_SRC_ZIGBEE;
    portEXIT_CRITICAL(&s_live_mux);

    if (ev == ZBC_EV_LONG) {
        if (hold_ms <= 0) {
            hold_ms = 2500;
        } else if (hold_ms > 20000) {
            hold_ms = 20000;      /* a test, not a way to leave a light ramping */
        }
        if (zigbee) {
            /* There is no pin to hold down, so the hold is started here and
             * ended by the timer below - which still drives zbtn_step, so the
             * repeats and the release that stops the lamps are the real ones. */
            portENTER_CRITICAL(&s_live_mux);
            s_live[slot].sim_hold_until = esp_timer_get_time() + (int64_t)hold_ms * 1000;
            zbc_btn_emit_t emit = zbtn_step(&s_live[slot].zfsm, ZB_REMOTE_HOLD,
                                            s_live[slot].want_double, esp_timer_get_time());
            portEXIT_CRITICAL(&s_live_mux);
            if (emit != ZBC_EMIT_NONE) {
                const btn_msg_t hold = { .id = id, .emit = emit };
                xQueueSend(s_queue, &hold, 0);
            }
            return ESP_OK;
        }
        /* Long enough to clear the debounce and the hold threshold on top of
         * whatever sweep was asked for. */
        portENTER_CRITICAL(&s_live_mux);
        s_live[slot].sim_until = esp_timer_get_time() +
                                 (int64_t)hold_ms * 1000 + BTN_LONG_US + BTN_DEBOUNCE_US * 2;
        portEXIT_CRITICAL(&s_live_mux);
        return ESP_OK;
    }

    if (zigbee) {
        /* Through the same machine a real press goes through, so a test of a
         * button with double-press rules waits out the window exactly as the
         * remote would make it. */
        portENTER_CRITICAL(&s_live_mux);
        const zb_remote_ev_t press = ev == ZBC_EV_SHORT  ? ZB_REMOTE_CLICK
                                   : ev == ZBC_EV_TRIPLE ? ZB_REMOTE_TRIPLE : ZB_REMOTE_DOUBLE;
        zbc_btn_emit_t emit = zbtn_step(&s_live[slot].zfsm, press,
                                        s_live[slot].want_double, esp_timer_get_time());
        portEXIT_CRITICAL(&s_live_mux);
        if (emit == ZBC_EMIT_NONE) {
            return ESP_OK;   /* held back for the double window; the sampler emits it */
        }
        const btn_msg_t msg = { .id = id, .emit = emit };
        return xQueueSend(s_queue, &msg, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
    }

    const btn_msg_t msg = {
        .id   = id,
        .emit = ev == ZBC_EV_SHORT ? ZBC_EMIT_SHORT
              : ev == ZBC_EV_TRIPLE ? ZBC_EMIT_TRIPLE : ZBC_EMIT_DOUBLE,
    };
    return xQueueSend(s_queue, &msg, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}
