#include "btn_gesture.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

/* ------------------------------------------------------------ the gesture machine */

/* >>> host-test region: btnstep <<< */

/* No ESP-IDF below this line and none above it in this function - see btn_gesture.h.
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
