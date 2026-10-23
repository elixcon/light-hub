#include "remote.h"

#include <stddef.h>
#include <string.h>

/* Cluster and command numbers are spelled out here rather than taken from the
 * SDK headers, because including those would make this a module that only builds
 * on the target - and the whole point of it is that it does not. They are ZCL
 * constants; they are not going to move. */
#define CL_SCENES            0x0005U
#define CL_ON_OFF            0x0006U
#define CL_LEVEL             0x0008U
#define CL_MULTISTATE_INPUT  0x0012U

#define ATTR_ON_OFF          0x0000U
#define ATTR_AQARA_CLICKS    0x8000U  /* manufacturer-specific, on the On/Off cluster */
#define ATTR_PRESENT_VALUE   0x0055U

#define CMD_ON_OFF_OFF             0x00U
#define CMD_ON_OFF_ON              0x01U
#define CMD_ON_OFF_TOGGLE          0x02U
#define CMD_ON_OFF_OFF_WITH_EFFECT 0x40U

/* IKEA's own command on the standard Scenes cluster ("tradfriArrowSingle" in
 * zigbee-herdsman): a u16 whose value 257 means the left/down side. A BILRESA
 * sends it for a double click (other values) and a triple click (257) of its
 * wheel - Zigbee2MQTT's on_double and off_double. */
#define CMD_SCENES_IKEA_ARROW     0x07U

#define CMD_LEVEL_MOVE_TO_LEVEL        0x00U
#define CMD_LEVEL_MOVE_TO_LEVEL_ON_OFF 0x04U
#define CMD_LEVEL_MOVE            0x01U
#define CMD_LEVEL_STEP            0x02U
#define CMD_LEVEL_STOP            0x03U
#define CMD_LEVEL_MOVE_ON_OFF     0x05U
#define CMD_LEVEL_STEP_ON_OFF     0x06U
#define CMD_LEVEL_STOP_ON_OFF     0x07U

/* Key 0 is "the on/off press", whichever command carried it; 1 and 2 are the two
 * directions of a dimmer.
 *
 * On and Off are not separate keys. A Hue Smart Button is one physical button
 * that sends `On` to turn on and `OffWithEffect` to turn off, so splitting them
 * by command would put one button on two keys, and a rule bound to either would
 * fire on every other press. The cost is that a two-button rocker cannot say
 * "this half always turns on" - either half toggles instead. */
#define KEY_ONOFF  0
#define KEY_DOWN   1
#define KEY_UP     2
/* Except on a Hue dimmer, which has an On button *and* an Off button, sending
 * On from one and OffWithEffect from the other. There the two are two keys. */
#define KEY_OFF    3

zb_remote_model_t remote_model(const char *model_id)
{
    if (!model_id) {
        return ZB_REMOTE_MODEL_OTHER;
    }
    /* The ModelIdentifiers these report, read off the devices themselves. */
    if (strcmp(model_id, "SYMFONISK Sound Controller") == 0) {
        return ZB_REMOTE_MODEL_E1744;
    }
    if (strcmp(model_id, "09BA") == 0) {
        return ZB_REMOTE_MODEL_E2490;
    }
    if (strcmp(model_id, "lumi.sensor_switch.aq2") == 0 || strcmp(model_id, "lumi.remote.b1acn01") == 0) {
        return ZB_REMOTE_MODEL_AQARA;
    }
    if (strcmp(model_id, "RWL020") == 0 || strcmp(model_id, "RWL021") == 0) {
        return ZB_REMOTE_MODEL_HUE_DIMMER;
    }
    if (strcmp(model_id, "ZG-101ZL") == 0) {
        return ZB_REMOTE_MODEL_TUYA_BUTTON;
    }
    return ZB_REMOTE_MODEL_OTHER;
}

/* A press from a remote that counts its own is final; from any other it is a
 * click that a second one may pair into a double. A Hue dimmer counts nothing.
 * A ZG-101ZL's On, Off or Toggle is a whole gesture already counted. */
static zb_remote_ev_t press(zb_remote_model_t model)
{
    switch (model) {
    case ZB_REMOTE_MODEL_E1744:
    case ZB_REMOTE_MODEL_E2490:
    case ZB_REMOTE_MODEL_AQARA:
    case ZB_REMOTE_MODEL_TUYA_BUTTON: return ZB_REMOTE_SINGLE;
    default:                          return ZB_REMOTE_CLICK;
    }
}

zb_remote_ev_t remote_decode_attr(zb_remote_model_t model, uint16_t cluster_id, uint16_t attr_id,
                                  uint32_t value, uint8_t *key)
{
    /* Aqara's newer wireless switches (lumi.remote.b1acn01 and its relatives)
     * report PresentValue on Multistate Input and decode the gestures
     * themselves. This is the whole of that device's protocol. */
    if (cluster_id == CL_MULTISTATE_INPUT && attr_id == ATTR_PRESENT_VALUE) {
        if (key) {
            *key = KEY_ONOFF;
        }
        /* It counts its own presses: a 1 is never half of a double. */
        switch (value) {
        case 0:   return ZB_REMOTE_HOLD;
        case 1:   return ZB_REMOTE_SINGLE;
        case 2:   return ZB_REMOTE_DOUBLE;
        case 3:   return ZB_REMOTE_TRIPLE;
        case 255: return ZB_REMOTE_RELEASE;
        default:  return ZB_REMOTE_NONE;
        }
    }

    /* The older revision (lumi.sensor_switch.aq2) has no Multistate Input at
     * all: it advertises On/Off as an *input* cluster, so it looks like a lamp,
     * and reports its clicks as changes to that attribute. Anything past a
     * double arrives on a manufacturer-specific attribute of its own. */
    if (cluster_id == CL_ON_OFF && attr_id == ATTR_AQARA_CLICKS) {
        if (key) {
            *key = KEY_ONOFF;
        }
        switch (value) {
        case 2:   return ZB_REMOTE_DOUBLE;
        case 3:   return ZB_REMOTE_TRIPLE;
        /* Quadruple and "five or more". There is no gesture for them, so they
         * collapse to a press rather than being dropped: pressing a button four
         * times should still switch the light. */
        case 4:
        case 128: return ZB_REMOTE_SINGLE;
        default:  return ZB_REMOTE_NONE;
        }
    }

    if (cluster_id == CL_ON_OFF && attr_id == ATTR_ON_OFF) {
        /* One press is a pair of reports and only one edge may count, or every
         * click fires twice. A lumi.sensor_switch.aq2 puts `1` and then `0` on
         * air in the same millisecond; the falling edge is taken, as the
         * documentation for these devices describes. The other edge is ignored
         * rather than unrecognised - it is half of a press already counted. */
        if (value != 0) {
            return ZB_REMOTE_IGNORED;
        }
        if (key) {
            *key = KEY_ONOFF;
        }
        return press(model);
    }

    return ZB_REMOTE_NONE;
}

zb_remote_ev_t remote_decode_cmd(zb_remote_model_t model, uint16_t cluster_id, uint8_t cmd_id,
                                 const uint8_t *payload, uint16_t len, uint8_t *key)
{
    /* A standard remote - an IKEA rocker, a Hue dimmer - reports nothing at all.
     * It sends the commands it would send to a lamp, at whatever its own binding
     * table points at, which is why a bind has to be in place before any of this
     * can happen. */
    if (cluster_id == CL_ON_OFF) {
        switch (cmd_id) {
        /* OffWithEffect is how a Hue Smart Button says "off", so it is a press too,
         * not only a command this coordinator sends to fade a lamp out. */
        case CMD_ON_OFF_OFF:
        case CMD_ON_OFF_ON:
        case CMD_ON_OFF_TOGGLE:
        case CMD_ON_OFF_OFF_WITH_EFFECT:
            if (key) {
                *key = model == ZB_REMOTE_MODEL_HUE_DIMMER && cmd_id != CMD_ON_OFF_ON &&
                       cmd_id != CMD_ON_OFF_TOGGLE ? KEY_OFF : KEY_ONOFF;
            }
            return press(model);
        /* A Tuya scene switch, counting for itself. A ZG-101ZL's hold is one frame
         * about a second in, not repeated however long it is held, and nothing
         * follows on release (Zigbee2MQTT has only "hold"). So it is a tick:
         * silence ends it, where as a HOLD waiting for a Stop that never comes it
         * would sweep the lights until ZBTN_HOLD_MAX_US. A dimming rule moves one
         * step per hold. */
        case ZB_REMOTE_CMD_TUYA_ACTION:
            if (len < 1 || !payload || payload[0] > 2) {
                return ZB_REMOTE_NONE;
            }
            if (key) {
                *key = KEY_ONOFF;
            }
            return payload[0] == 0 ? ZB_REMOTE_SINGLE
                 : payload[0] == 1 ? ZB_REMOTE_DOUBLE : ZB_REMOTE_HOLD_TICK;
        default:
            return ZB_REMOTE_NONE;
        }
    }

    /* Guarded to the one command: a real RecallScene is still not a press. On a
     * BILRESA it is the wheel pressed twice or three times, so it belongs to the
     * press key. On an IKEA rocker's side buttons, which zigbee-herdsman names
     * the same command after, it is still left and right. */
    if (cluster_id == CL_SCENES && cmd_id == CMD_SCENES_IKEA_ARROW) {
        if (len < 2 || !payload) {
            return ZB_REMOTE_NONE;
        }
        const uint16_t value = (uint16_t)(payload[0] | (payload[1] << 8));
        if (model == ZB_REMOTE_MODEL_E2490) {
            if (key) {
                *key = KEY_ONOFF;
            }
            return value == 257 ? ZB_REMOTE_TRIPLE : ZB_REMOTE_DOUBLE;
        }
        if (key) {
            *key = value == 257 ? KEY_DOWN : KEY_UP;
        }
        return ZB_REMOTE_CLICK;
    }

    /* The Hue dimmer's own account of each button: what Zigbee2MQTT reads, and
     * what gives the On and Off buttons a hold and the up and down buttons a
     * single press distinct from a hold - its endpoint 1 commands give neither.
     * The press edge is ignored: a click is the short release, as on a pin, so
     * that a hold never starts as a click. Buttons 1-4 are On, Up, Down, Off. */
    if (cluster_id == ZB_REMOTE_CL_HUE && cmd_id == 0x00 && model == ZB_REMOTE_MODEL_HUE_DIMMER) {
        if (len < 5 || !payload || payload[0] < 1 || payload[0] > 4) {
            return ZB_REMOTE_NONE;
        }
        static const uint8_t        keys[4] = { KEY_ONOFF, KEY_UP, KEY_DOWN, KEY_OFF };
        static const zb_remote_ev_t evs[4]  = {
            ZB_REMOTE_IGNORED,                                 /* press: wait and see */
            ZB_REMOTE_HOLD, ZB_REMOTE_CLICK, ZB_REMOTE_RELEASE,
        };
        if (payload[4] > 3) {
            return ZB_REMOTE_NONE;
        }
        if (payload[4] != 0 && key) {
            *key = keys[payload[0] - 1];
        }
        return evs[payload[4]];
    }

    if (cluster_id == CL_LEVEL) {
        switch (cmd_id) {
        /* Move means "start ramping and keep going" - continuous, and only ended by
         * the Stop that must follow it. That is a hold.
         *
         * Step is not. It is one discrete nudge, and a remote that dims by
         * repeating it is doing its own sweep: a Hue Smart Button held for six
         * seconds sends seven Steps about 800 ms apart, and reverses at the rail
         * by itself. So a Step is a click, and a dim_* rule on the short press
         * nudges once per Step, tracking the device's own sweep and reversal.
         *
         * Read as a hold, a short press of that button - Step, then Stop 23 ms
         * later, then OffWithEffect - would fire the long-press rule and never
         * the off rule. The first byte is the direction, 0 up and 1 down. */
        case CMD_LEVEL_MOVE:
        case CMD_LEVEL_MOVE_ON_OFF:
        case CMD_LEVEL_STEP:
        case CMD_LEVEL_STEP_ON_OFF:
            if (len < 1 || !payload) {
                return ZB_REMOTE_NONE;
            }
            /* Except on an E1744, where a Step is no nudge at all: up is its
             * button pressed twice and down three times (Zigbee2MQTT's
             * brightness_step_up / _down). Same press key as its single press. */
            if (model == ZB_REMOTE_MODEL_E1744 &&
                (cmd_id == CMD_LEVEL_STEP || cmd_id == CMD_LEVEL_STEP_ON_OFF)) {
                if (key) {
                    *key = KEY_ONOFF;
                }
                return payload[0] == 0 ? ZB_REMOTE_DOUBLE : ZB_REMOTE_TRIPLE;
            }
            if (key) {
                *key = payload[0] == 0 ? KEY_UP : KEY_DOWN;
            }
            return (cmd_id == CMD_LEVEL_STEP || cmd_id == CMD_LEVEL_STEP_ON_OFF)
                       ? ZB_REMOTE_CLICK : ZB_REMOTE_HOLD;
        /* Stop carries no direction, so it ends whichever key of this endpoint
         * is holding - which is the only one that can be. */
        case CMD_LEVEL_STOP:
        case CMD_LEVEL_STOP_ON_OFF:
            if (key) {
                *key = ZB_REMOTE_KEY_ANY;
            }
            return ZB_REMOTE_RELEASE;
        default:
            return ZB_REMOTE_NONE;
        }
    }

    return ZB_REMOTE_NONE;
}

/* How long after its Tuya action a command is that action's echo: measured
 * 190-200 ms on a ZG-101ZL, and well short of a second gesture, which starts
 * with an action of its own and so moves the window on. */
#define TUYA_ECHO_US  (500LL * 1000)

bool remote_is_echo(uint16_t cluster_id, uint8_t cmd_id, int64_t since_action_us)
{
    return cluster_id == CL_ON_OFF &&
           (cmd_id == CMD_ON_OFF_OFF || cmd_id == CMD_ON_OFF_ON || cmd_id == CMD_ON_OFF_TOGGLE) &&
           since_action_us >= 0 && since_action_us <= TUYA_ECHO_US;
}

/* How close the On/Off follows a level-0 frame when that zero is the click's and
 * not the wheel's (about 1 ms), and how long the restore after it may take to
 * arrive (about 100 ms). */
#define WHEEL_GLITCH_US   (50LL * 1000)
#define WHEEL_RESTORE_US  (250LL * 1000)

bool remote_wheel_frame(uint16_t cluster_id, uint8_t cmd_id, const uint8_t *payload, uint16_t len,
                        uint8_t *level)
{
    if (cluster_id != CL_LEVEL ||
        (cmd_id != CMD_LEVEL_MOVE_TO_LEVEL && cmd_id != CMD_LEVEL_MOVE_TO_LEVEL_ON_OFF) ||
        len < 1 || !payload) {
        return false;
    }
    if (level) {
        *level = payload[0] > 254 ? 254 : payload[0];
    }
    return true;
}

static void wheel_apply(zb_wheel_t *st, uint8_t level)
{
    if (st->have && level != st->last) {
        st->dir = level > st->last ? 1 : -1;
    } else if (level == 0) {
        st->dir = -1;      /* still turning down against the bottom */
    } else if (level >= 254) {
        st->dir = 1;       /* and up against the top */
    }
    /* Equal in the middle keeps the direction it had: a repeat while turning. */
    st->last = level;
    st->have = 1;
}

zb_remote_ev_t remote_wheel_level(zb_wheel_t *st, uint8_t level, int64_t now_us, uint8_t *key)
{
    if (now_us < st->quiet_until) {
        /* The restore after a click puts the level back where it was. */
        st->last         = level;
        st->have         = 1;
        st->zero_pending = 0;
        return ZB_REMOTE_IGNORED;
    }

    bool acted = false;
    if (st->zero_pending) {
        st->zero_pending = 0;
        if (now_us - st->zero_at <= WHEEL_RESTORE_US) {
            wheel_apply(st, 0);     /* no click came: that zero was the wheel's */
            acted = true;
        } else {
            st->last = 0;           /* the end of an old turn; remember, do not act */
            st->have = 1;
        }
    }

    if (level == 0) {
        /* Nothing can tell this apart from a click until the next frame, or the
         * click, arrives - so wait for one of them. A turn that is really
         * pressing against the bottom only loses a tenth of a second to it. */
        st->zero_pending = 1;
        st->zero_at      = now_us;
        if (!acted) {
            return ZB_REMOTE_IGNORED;
        }
    } else {
        wheel_apply(st, level);
    }

    if (st->dir == 0) {
        return ZB_REMOTE_IGNORED;
    }
    if (key) {
        *key = st->dir > 0 ? KEY_UP : KEY_DOWN;
    }
    return ZB_REMOTE_HOLD_TICK;
}

void remote_wheel_note_onoff(zb_wheel_t *st, int64_t now_us)
{
    if (st->zero_pending && now_us - st->zero_at <= WHEEL_GLITCH_US) {
        st->zero_pending = 0;
        st->quiet_until  = now_us + WHEEL_RESTORE_US;
    }
}

const char *remote_event_name(zb_remote_ev_t ev)
{
    switch (ev) {
    /* One word for both: to somebody learning a key, a press is a press. */
    case ZB_REMOTE_CLICK:
    case ZB_REMOTE_SINGLE:  return "click";
    case ZB_REMOTE_DOUBLE:  return "double";
    case ZB_REMOTE_TRIPLE:  return "triple";
    /* The same word for both: to somebody learning a key, a wheel turning is a
     * hold, and the editor already knows what to do with one. */
    case ZB_REMOTE_HOLD:
    case ZB_REMOTE_HOLD_TICK: return "hold";
    case ZB_REMOTE_RELEASE: return "release";
    case ZB_REMOTE_IGNORED: return "ignored";
    default:                return "none";
    }
}
