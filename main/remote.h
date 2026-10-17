/* What a Zigbee remote just did, worked out from the frame it sent.
 *
 * This is a leaf module in the same sense as colorspace.c: it depends on nothing
 * but the C library - not ESP-IDF, not the Zigbee SDK, not device_db - so it compiles
 * with cc and every row of its tables can be driven without hardware. Everything
 * model-specific about a remote lives here and nowhere else, which is what makes
 * "this lamp sends its clicks differently" a one-line change rather than a hunt
 * through the ZCL handling.
 *
 * The vocabulary is deliberately the one buttons.c already has. A remote that
 * decodes its own double presses says so; one that does not sends two clicks and
 * zbtn_step makes the double out of them, exactly as btn_step does for a finger
 * on a pin.
 *
 * Which remote sent a frame matters, and the caller says (remote_model()). The
 * same command means different things on different devices: a Level Step is a
 * Hue Smart Button's hold and an E1744's double press, and IKEA's Scenes "arrow"
 * is a rocker's side button and a BILRESA's double click. Decoded by command
 * alone, those double and triple presses would be filed as clicks of the turn
 * keys. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZB_REMOTE_NONE = 0,  /* not a button press at all */
    ZB_REMOTE_CLICK,     /* one discrete press, which a second may yet make a double */
    ZB_REMOTE_DOUBLE,    /* the device itself says two */
    ZB_REMOTE_HOLD,      /* a hold began */
    ZB_REMOTE_RELEASE,   /* the hold ended */
    /* Part of a press, and deliberately not acted on - the other edge of one
     * this decoder has already counted. Distinct from NONE because a caller that
     * logs what it did not understand would otherwise report it, and a line
     * saying a frame was not understood is how somebody decides a remote is
     * unsupported. */
    ZB_REMOTE_IGNORED,
    /* A hold that has no Stop of its own: each frame of a stream keeps it alive
     * and silence ends it. An IKEA BILRESA wheel streams absolute levels while
     * it turns and never says when it has stopped. Deliberately not the same as
     * HOLD - an Aqara sends nothing at all between its hold and its release, so a
     * quiet timeout on every hold would cut those short. */
    ZB_REMOTE_HOLD_TICK,
    /* One press from a remote that counts its own presses, so no second one
     * will follow to make it a double: it fires at once, whatever double rules
     * the key has. A CLICK on such a key would wait out the double window for
     * nothing, on every press. */
    ZB_REMOTE_SINGLE,
    ZB_REMOTE_TRIPLE,    /* the device itself says three */
} zb_remote_ev_t;

/* The remotes whose frames need to be told apart from another remote's same
 * frames. Anything else is OTHER and decodes by command alone. */
typedef enum {
    ZB_REMOTE_MODEL_OTHER = 0,
    ZB_REMOTE_MODEL_E1744,     /* IKEA SYMFONISK sound controller */
    ZB_REMOTE_MODEL_E2490,     /* IKEA BILRESA scroll wheel, Zigbee mode */
    ZB_REMOTE_MODEL_AQARA,     /* lumi.sensor_switch.aq2, lumi.remote.b1acn01 */
    /* Philips Hue dimmer switch gen 1 (RWL020, RWL021): four buttons, and its On
     * and Off are two of them - on every other remote they are one key. Once
     * bound, it also says press / hold / release per button on Philips' own
     * cluster (ZB_REMOTE_CL_HUE), which is the richer of its two languages. */
    ZB_REMOTE_MODEL_HUE_DIMMER,
} zb_remote_model_t;

/* From the Basic cluster's ModelIdentifier, as device_db stores it. NULL and
 * unknown strings are OTHER. */
zb_remote_model_t remote_model(const char *model_id);

/* Which of the buttons that share one endpoint this was. A single-button remote
 * only ever produces key 0; a rocker produces 1 for its down/off half and 2 for
 * its up/on half, which is the shape both On/Off and Level Control commands
 * carry. A Hue dimmer's Off button is key 3. */
#define ZB_REMOTE_KEY_MAX  8

/* Philips' manufacturer-specific cluster, on the dimmer's endpoint 2: command
 * 0x00 ("hueNotification" in zigbee-herdsman) is button, three unknown bytes,
 * then 0 press, 1 hold (repeated while held), 2 short release, 3 long release. */
#define ZB_REMOTE_CL_HUE   0xFC00

/* A ZCL Stop carries no direction, so it ends whichever key this endpoint has
 * holding. Nothing else uses it. */
#define ZB_REMOTE_KEY_ANY  0xFF

/* An attribute report or read response from an endpoint we believe is a switch.
 * `value` is the attribute widened to 32 bits; the callers that matter are all
 * uint8 or uint16. Returns ZB_REMOTE_NONE, and leaves *key alone, for anything
 * that is not a press. */
zb_remote_ev_t remote_decode_attr(zb_remote_model_t model, uint16_t cluster_id, uint16_t attr_id,
                                  uint32_t value, uint8_t *key);

/* A cluster-specific command sent *to* us - what a standard remote does instead
 * of reporting. `payload` may be NULL when `len` is 0. */
zb_remote_ev_t remote_decode_cmd(zb_remote_model_t model, uint16_t cluster_id, uint8_t cmd_id,
                                 const uint8_t *payload, uint16_t len, uint8_t *key);

/* A scroll wheel that sends absolute levels rather than presses - the IKEA
 * BILRESA (E2490) in Zigbee mode, one MoveToLevel every 310-330 ms while it
 * turns and about one a second against the top of its range. The direction is
 * the difference from the previous level, so the state lives with the caller,
 * one per endpoint; these functions only ever touch the struct they are given.
 * Zeroed is the initial state. */
typedef struct {
    uint8_t last;          /* the last level acted on, valid when `have` */
    uint8_t have;
    int8_t  dir;           /* +1 up, -1 down, 0 not known yet */
    uint8_t zero_pending;  /* a level-0 frame waiting to see whether a click follows */
    int64_t zero_at;
    int64_t quiet_until;   /* level frames up to here are the restore after a click */
} zb_wheel_t;

/* True, with the level, when this frame is a wheel's MoveToLevel. The BILRESA's
 * is malformed - no option fields, and 255 where lamps stop at 254 - so only the
 * level byte is required, and 255 comes back as 254. */
bool remote_wheel_frame(uint16_t cluster_id, uint8_t cmd_id, const uint8_t *payload, uint16_t len,
                        uint8_t *level);

/* One level frame. Returns ZB_REMOTE_HOLD_TICK with key 2 (up) or 1 (down) while
 * the wheel is turning, and ZB_REMOTE_IGNORED for a frame that is part of a turn
 * but says nothing yet - the first after a boot, or one being held back. */
zb_remote_ev_t remote_wheel_level(zb_wheel_t *st, uint8_t level, int64_t now_us, uint8_t *key);

/* An On/Off command from the same endpoint. A wheel click is preceded by a
 * level-0 frame about 1 ms earlier and followed by a restore about 100 ms later;
 * this is what tells that zero apart from a turn that really reached the bottom. */
void remote_wheel_note_onoff(zb_wheel_t *st, int64_t now_us);

/* For logs and for the API. Never NULL. */
const char *remote_event_name(zb_remote_ev_t ev);

#ifdef __cplusplus
}
#endif
