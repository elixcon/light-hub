/* Matter over Wi-Fi bridge - see matter_bridge.h for what it is.
 *
 * Three rules shape this file.
 *
 * - **Act on commands, never on attribute changes.** CHIP's level-control server
 *   carries out a transition by stepping CurrentLevel one unit per tick, and every
 *   step fires the attribute callback. Driving Zigbee from there would put up to
 *   254 commands on air for one fade. So each command is caught in on_command(),
 *   which sees the target and the transition time once, and attribute_cb() does
 *   nothing. The stepped attribute is left to Matter: it is what the controller
 *   is shown.
 *
 * - **Nothing that takes the Zigbee lock runs in the CHIP task.** on_command()
 *   only decodes and posts; bridge_task() calls lighting.c. The CHIP task holds
 *   the CHIP lock while it calls us, and the lighting calls can block on the
 *   Zigbee lock for as long as the mainloop holds it.
 *
 * - **Wi-Fi and mDNS belong to wifi_manager.** CHIP's station state machine
 *   calls esp_wifi_connect() every 100 ms whenever it thinks it should be
 *   connected, which would fight wifi_manager's backoff and its AP fallback, so it
 *   is put in application-controlled mode. And CHIP renames the mDNS host to its
 *   own operational name whenever it publishes, which takes `zigbee.local` away;
 *   keep_hostname() puts it back as a delegated host.
 *
 * Groups are reconciled, not hooked: once a second the group list is compared
 * with the bridged endpoints, so http_server.c and group_db know nothing of this
 * file. A faster pass copies each group's state into its Matter attributes, which
 * is how a change from the web UI, a button or a remote reaches the controller.
 *
 * A Zigbee device endpoint somebody asked to see on its own (mexp_db.h) is
 * reconciled the same way, beside the rooms: a lamp becomes the Matter light its
 * clusters make it, driven through lighting.c's one-light calls exactly as a room
 * is through the group calls; each button of a remote a Generic Switch; a motion
 * sensor an Occupancy Sensor. The buttons report through buttons.c, which posts
 * every gesture here (matter_bridge_button_event), and the sensor's state is read
 * from its rules' own, so its no-motion time applies to the controllers too.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <esp_matter.h>
#include <esp_matter_bridge.h>

#include <app-common/zap-generated/cluster-objects.h>
#include <app/CommandHandler.h>
#include <app/InteractionModelEngine.h>
#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>
#include <app/clusters/mode-select-server/supported-modes-manager.h>
#include <app/clusters/switch-server/CodegenIntegration.h>
#include <clusters/occupancy_sensing/integration.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include "btn_gesture.h"
#include "button_db.h"
#include "buttons.h"
#include "group_db.h"
#include "heap_watch.h"
#include "lighting.h"
#include "matter_bridge.h"
#include "matter_ctl.h"
#include "mexp_db.h"
#include "settings.h"
#include "wifi_manager.h"

static const char *TAG = "matter";

using namespace esp_matter;
using namespace chip::app::Clusters;

/* Everything Matter keeps lives here - the CHIP namespaces, esp_matter's
 * attributes, the bridge's endpoint list and our own group map - so erasing it is
 * the whole of a Matter reset (see partitions.csv and sdkconfig.defaults). */
#define MB_NVS_PART    "nvs_matter"
#define MB_NVS_NS      "zbc_matter"
#define MB_NVS_KEY     "groups"
/* The "<name> Adaptive" endpoints, in a map of their own: the one above keeps its
 * layout and version, so no light loses the id controllers know it by. */
#define MB_NVS_KEY_AL  "al_groups"
#define MB_MAP_VERSION 1
/* The device endpoints shown on their own, in a third: which device, endpoint and
 * button each Matter endpoint stands for. */
#define MB_NVS_KEY_DEV     "devices"
#define MB_DEV_MAP_VERSION 1

/* Endpoint 0 - the root node, which node::create() makes for us. It carries the
 * Basic Information cluster, so it is where the coordinator's own name goes. */
#define MB_ROOT_ENDPOINT 0

/* Reconciling endpoints and refreshing the status run once a second; copying
 * each group's state into Matter runs four times as often. That copy is the
 * bridge's share of the delay before a controller sees a change made elsewhere,
 * and costs two attribute reads per group. */
#define MB_SYNC_US     (1000 * 1000)
#define MB_STATE_US    (250 * 1000)
/* The subscriptions CHIP keeps are read from flash on every tenth refresh. */
#define MB_KEPT_EVERY  10
/* How long after a Matter command the bridge keeps its hands off that group's
 * attributes. CHIP is stepping CurrentLevel towards the target meanwhile, and
 * writing the target over the top of that is noise on every subscription. */
#define MB_QUIET_MS    2000
/* Level units per second for a Move that names no rate: the whole range in about
 * three seconds, close to a held button's sweep. */
#define MB_DEFAULT_RATE 80

/* The least free heap a new device endpoint is made with. Each one is heap for as
 * long as it exists - measured on hardware, a switch under 1 KB, a light 4-10 KB,
 * a colour light the most - and a controller's traffic takes about 32 KB on top of
 * where the heap rests (a hub with four fabrics, over two days, 2026-11). One that
 * would be made below this waits, counted in devices_skipped, until there is the
 * room; one that already has its endpoint keeps it, at a restart too. */
#define MB_DEV_HEAP_MIN (48 * 1024)

static_assert(MAX_BRIDGED_DEVICE_COUNT >= 2 * CONFIG_ZBC_MAX_GROUPS + CONFIG_ZBC_MAX_MATTER_EXPOSED,
              "CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT is too small: every group needs an endpoint for "
              "its light and one for its Adaptive Lighting switch, every device shown on its own one "
              "(CONFIG_ZBC_MAX_MATTER_EXPOSED), plus one for the root and one for the aggregator");

/* >>> host-test region: matter <<< */

/* What a Matter command asks of a room or a lamp, or what a button did. Decoded in
 * the CHIP task (a button: in the buttons task), carried out in the bridge task -
 * plain data, so that posting one is all either task does. */
typedef enum {
    MB_NONE = 0,
    MB_ON,
    MB_OFF,
    MB_TOGGLE,
    MB_LEVEL,      /* move to `level` over `transition_ds` */
    MB_STEP,       /* nudge by `step` level units in `dir` */
    MB_MOVE,       /* sweep from `level` in `dir` at `rate` level units a second */
    MB_STOP,
    MB_AL_START,   /* start or re-enter the group's Adaptive Lighting mode */
    MB_AL_STOP,    /* end it; the lights stay where they are */
    MB_AL_TOGGLE,  /* whichever of the two the mode is not */
    MB_CT,         /* a lamp: colour temperature `a` mireds over `transition_ds` */
    MB_HS,         /* a lamp: hue `a`, saturation `b`, both 0-254 */
    MB_XY,         /* a lamp: CIE x `a`, y `b`, 0-65279 */
    MB_SWITCH,     /* button `button_id` made gesture `emit` (zbc_btn_emit_t) */
} mb_kind_t;

typedef struct {
    uint8_t  kind;
    bool     if_on;          /* the variants without OnOff leave a dark group dark */
    int8_t   dir;
    uint8_t  level;          /* MB_LEVEL: target. MB_MOVE: start, 0 when unknown */
    uint8_t  step;
    uint8_t  rate;
    uint8_t  emit;           /* MB_SWITCH */
    /* The slot it is for, by its Matter endpoint - the light's, for its Adaptive
     * switch too. esp_matter never gives an id out twice, so a slot removed and
     * another made while this waited in the queue cannot be mistaken for it. */
    uint16_t endpoint_id;
    uint16_t button_id;      /* MB_SWITCH */
    uint16_t a, b;           /* MB_CT, MB_HS, MB_XY */
    uint16_t transition_ds;
    bool     timed;          /* the controller named the transition, as decoded */
    bool     matter_on;      /* if_on: the light's Matter OnOff when the command arrived */
    bool     switch_on;      /* MB_LEVEL: it switches the group on too - WithOnOff, or an On went with it */
    int64_t  at_us;          /* when it arrived */
} mb_intent_t;

/* The transition a controller's level command is carried out with. Alexa sends 0
 * with every brightness it sets - "as fast as possible" - which is the controller
 * having no opinion rather than anybody asking for a snap: nobody says "set the
 * basement to 30 % instantly". So 0 is read like a transition left out (null),
 * and both get the fade the web UI, the buttons and the remotes use. Whoever
 * wants a snap everywhere sets the fade to Instant. A transition the controller
 * did name is kept. */
static uint16_t mb_transition_ds(bool named, uint16_t asked_ds, uint16_t fade_ds)
{
    return named && asked_ds > 0 ? asked_ds : fade_ds;
}

/* Whether one of the variants without OnOff finds the group dark, and so leaves
 * it dark. Lit is what the controller sees - the light's Matter OnOff, which a
 * Matter On sets at once - or any member that has reported itself lit, which
 * reaches that attribute only with the next sync pass. The lamps alone are not
 * enough: Alexa's "set to 100 %" on a dark light is On and, 200 ms later, a plain
 * MoveToLevel, and a lamp reports itself lit a round trip after its frame goes
 * out, or never if it ignores on-change reporting. */
static bool mb_stays_dark(bool if_on, bool matter_on, bool lamps_on)
{
    return if_on && !matter_on && !lamps_on;
}

/* What a Matter "turn on" does - an On nothing replaced, or a Toggle that found
 * the group dark. The values are zbc_turn_on_t's, the group's stored choice
 * (asserted below this region).
 *
 * Lit or dark: "turn on the basement" said to a basement dimmed to 10 % asks for
 * the room's turn-on as much as it does from dark. An On that only comes with a
 * brightness - "set to 30 %", from Alexa or from Siri - does not get here: it is
 * taken as part of the brightness (mb_on_hold_take). Adaptive Lighting only where
 * the group offers it; elsewhere the On is the plain one. */
typedef enum {
    MB_ON_AS_WAS = 0,     /* the group's last brightness, as an On always was */
    MB_ON_ADAPTIVE,
    MB_ON_FULL,
} mb_on_t;

static mb_on_t mb_turn_on(uint8_t choice, bool al_offered)
{
    if (choice == MB_ON_ADAPTIVE) {
        return al_offered ? MB_ON_ADAPTIVE : MB_ON_AS_WAS;
    }
    return choice == MB_ON_FULL ? MB_ON_FULL : MB_ON_AS_WAS;
}

/* How long an On waits for a brightness to follow it. Alexa's "set to 30 %" on a
 * dark light is On and then a plain MoveToLevel 170-400 ms later (measured), and
 * the On alone switches the group back on at its last brightness: a lamp whose
 * curve is dark at 30 % but lit at the last brightness came on and went off
 * again, and the rest rose towards the old level and turned back. Held, the On is
 * replaced by the brightness, which switches the group on at 30 % in one go.
 * Every On waits, lit group or not: a lit group's On takes the room's turn-on as
 * well, and a brightness that follows it has to replace it just the same. The
 * wait only ever shows as the room changing that much later. */
#define MB_ON_HOLD_US (500 * 1000)

/* How long after a brightness an On still belongs to it. Siri's "set to 30 %" is
 * the other way round from Alexa's: MoveToLevelWithOnOff and then On, 248 ms later
 * (measured), on a lit room as well. Carried out on its own, that On is the room's
 * turn-on, which takes the room from 30 % to Adaptive Lighting or full. Longer
 * than the gap by plenty: one lost frame, resent, must not bring that back, and
 * "turn on" said on purpose within a second of a brightness is not a thing. */
#define MB_ON_AFTER_LEVEL_US (1000 * 1000)

typedef struct {
    bool        held;
    mb_intent_t on;
    int64_t     due_us;
    mb_intent_t last;        /* the last command other than an On, which an On may belong to */
} mb_on_hold_t;

/* What to carry out now that `in` has arrived, in order, into `out`; returns how
 * many. A brightness or an off replaces a held On: each says where the group ends
 * up, and the On would only have lit it on the way there. The brightness then
 * lights the group whatever variant it was - the On said to. Anything else starts
 * from what the On leaves (a toggle, a step, a sweep), so the On goes first. A
 * second On while one waits is the same request, and the first keeps its time.
 *
 * An On that comes just after a brightness is the same pair in the other order.
 * The brightness has gone out by then, so what is left is what the On adds to it:
 * nothing if it switched the group on, and otherwise that switch-on. The gap is
 * between their arrivals, not `now_us`, which is when this one got its turn: a
 * room of lamps can keep the On in the queue for as long as it takes to send them
 * the brightness. */
static int mb_on_hold_take(mb_on_hold_t *h, const mb_intent_t *in, int64_t now_us, mb_intent_t out[2])
{
    if (in->kind == MB_ON) {
        if (h->last.kind == MB_LEVEL && in->at_us - h->last.at_us < MB_ON_AFTER_LEVEL_US) {
            if (h->last.switch_on) {
                return 0;
            }
            h->last.if_on     = false;
            h->last.switch_on = true;
            out[0]            = h->last;
            return 1;
        }
        if (!h->held) {
            h->held   = true;
            h->on     = *in;
            h->due_us = now_us + MB_ON_HOLD_US;
        }
        return 0;
    }
    int n = 0;
    const bool replaces = h->held && (in->kind == MB_LEVEL || in->kind == MB_OFF);
    if (h->held && !replaces) {
        out[n++] = h->on;
    }
    h->held  = false;
    out[n]   = *in;
    if (replaces) {
        out[n].if_on = false;
        /* The brightness carries the On's switch-on, members that can only
         * switch included: they have no brightness to follow. */
        out[n].switch_on = in->kind == MB_LEVEL;
    }
    h->last = out[n];
    return n + 1;
}

/* The held On, once its time is up and nothing has replaced it. */
static bool mb_on_hold_due(mb_on_hold_t *h, int64_t now_us, mb_intent_t *out)
{
    if (!h->held || now_us < h->due_us) {
        return false;
    }
    h->held = false;
    *out    = h->on;
    return true;
}

/* How often a sweep sends. A Matter Move is edge-triggered - one command starts
 * it and a Stop ends it - and that shape is not sent to a lamp (see Move/Stop in
 * CLAUDE.md): lose the Stop and it runs to a rail, and some lamps ignore Stop
 * anyway. So a Move becomes the shape a held button uses: each command aims one
 * interval beyond where the sweep is now, with a transition of exactly the time
 * it takes to get there. The lamp is still moving when the next command extends
 * the target, and a lost command is repaired by the next one a second later. */
#define MB_MOVE_INTERVAL_US (1000 * 1000)

typedef struct {
    bool    active;
    int8_t  dir;
    uint8_t from;            /* level the sweep started at, 1-254 */
    uint8_t rate;            /* level units per second */
    int64_t since_us;
    int64_t next_us;         /* when the next command is due */
} mb_move_t;

/* Where a sweep is `elapsed_us` after it began, clamped to the rails. */
static int mb_move_level(const mb_move_t *m, int64_t elapsed_us)
{
    int64_t travelled = (int64_t)m->rate * elapsed_us / 1000000;
    int64_t level     = m->from + (m->dir > 0 ? travelled : -travelled);
    return level < 1 ? 1 : (level > 254 ? 254 : (int)level);
}

/* The level to aim at if a command is due at `now`, with its transition; 0 when
 * nothing is due. A sweep resting on a rail ends by itself - a Matter Move needs
 * no Stop to finish, and the command that reached the rail was the last one. */
static int mb_move_tick(mb_move_t *m, int64_t now_us, uint16_t *transition_ds)
{
    if (!m->active || m->rate == 0 || now_us < m->next_us) {
        return 0;
    }
    int here  = mb_move_level(m, now_us - m->since_us);
    int ahead = mb_move_level(m, now_us - m->since_us + MB_MOVE_INTERVAL_US);
    if (ahead == here) {
        m->active = false;
        return 0;
    }
    /* The time the span takes at the sweep's own rate, rounded up. Shorter than
     * that and the lamp arrives early and waits, which is the stepping this is
     * here to avoid; the last span before a rail is the one that is shorter than
     * a whole interval. */
    int span   = ahead > here ? ahead - here : here - ahead;
    int ds     = (span * 10 + m->rate - 1) / m->rate;
    *transition_ds = (uint16_t)(ds < 1 ? 1 : ds);
    m->next_us = now_us + MB_MOVE_INTERVAL_US;
    return ahead;
}

/* Where a sweep had got to when it was stopped. Reckoned, never read back from
 * the lamps, for the reason run_release() in buttons.c gives: their reports lag,
 * and unevenly. */
static int mb_move_stop(mb_move_t *m, int64_t now_us)
{
    m->active = false;
    return mb_move_level(m, now_us - m->since_us);
}

/* Whether a Matter CurrentLevel already shows `pct`. Matter counts 1-254 and a
 * group counts percent, so a level set from a controller comes back from the group
 * as a percentage that maps to a slightly different level. Writing that back would
 * nudge the controller's slider by one every time it was used - the same
 * exact-equality trap as a group's last_pct and a curve's input_pct. */
static bool mb_level_shows(int matter_level, int pct)
{
    return matter_level >= 1 && matter_level <= 254 && zbc_level_to_pct(matter_level) == pct;
}

/* A Step's size in level units as a percentage. Never 0: a step that rounds to
 * nothing is a button that does nothing. */
static int mb_step_pct(int step)
{
    int pct = (step * 100 + 127) / 254;
    return pct < 1 ? 1 : pct;
}

/* Adaptive Lighting from Matter. The two modes of the Mode Select cluster on a
 * group's light, when the group exposes the mode that way. */
#define MB_MODE_NORMAL   0
#define MB_MODE_ADAPTIVE 1

/* What a ChangeToMode asks of the mode: 1 start it, 0 end it, -1 nothing - an
 * unknown mode, which CHIP's own handler refuses as well. */
static int mb_al_intent_for_mode(uint8_t mode)
{
    return mode == MB_MODE_ADAPTIVE ? 1 : (mode == MB_MODE_NORMAL ? 0 : -1);
}

/* A Toggle on the "<name> Adaptive" switch follows the mode, not the switch's own
 * attribute: the attribute is what the controller last saw, and the mode may have
 * ended since - dimmed by hand - without the controller having caught up. */
static bool mb_al_toggle_starts(bool running)
{
    return !running;
}

/* What a Matter endpoint stands for: a room, or one device endpoint shown on its
 * own (mexp_db.h). A remote is one accessory: a bridged node of its own, with a
 * switch endpoint below it for each button. Stored in the device map, so the
 * values are fixed. */
typedef enum {
    MB_SLOT_ROOM = 0,
    MB_SLOT_LAMP,
    MB_SLOT_SWITCH,      /* one button, a child of its remote's MB_SLOT_REMOTE */
    MB_SLOT_OCCUPANCY,
    MB_SLOT_REMOTE,      /* the remote itself: its name, and whether it is there */
} mb_slot_kind_t;

/* Which Matter light a Zigbee lamp is shown as: the most it can do, from its
 * clusters. A colour lamp is an Extended Color Light, which has colour temperature
 * whether the lamp does or not - Matter has no colour light without it - and
 * lighting_set_color_temp keeps a lamp to the range it says it has. */
typedef enum {
    MB_LAMP_ONOFF = 0,
    MB_LAMP_DIMMABLE,
    MB_LAMP_CT,
    MB_LAMP_COLOR,
} mb_lamp_t;

static mb_lamp_t mb_lamp_type(bool level, bool ct, bool color)
{
    return color ? MB_LAMP_COLOR : (ct ? MB_LAMP_CT : (level ? MB_LAMP_DIMMABLE : MB_LAMP_ONOFF));
}

/* What a lamp's endpoint is built with: its light type and, for a colour light,
 * whether it takes hue and saturation as well as x and y. An endpoint is built once
 * - a cluster cannot be added to an enabled endpoint - so a lamp whose clusters say
 * something else now is rebuilt at the next restart, on the same id. */
#define MB_SHAPE_HS 0x10

static uint8_t mb_lamp_shape(bool level, bool ct, bool color, bool hs)
{
    const mb_lamp_t t = mb_lamp_type(level, ct, color);
    return (uint8_t)((uint8_t)t | (t == MB_LAMP_COLOR && hs ? MB_SHAPE_HS : 0));
}

static mb_lamp_t mb_shape_type(uint8_t shape)
{
    return (mb_lamp_t)(shape & 0x0F);
}

/* A Generic Switch's events, as the Switch cluster names them. */
typedef enum {
    MB_SW_INITIAL_PRESS = 0,
    MB_SW_SHORT_RELEASE,
    MB_SW_LONG_PRESS,
    MB_SW_LONG_RELEASE,
    MB_SW_MULTI_ONGOING,
    MB_SW_MULTI_COMPLETE,
} mb_sw_event_t;

typedef struct {
    uint8_t event;   /* mb_sw_event_t */
    uint8_t count;   /* the multi-press ones: presses so far, or in all */
} mb_sw_step_t;

#define MB_SW_MAX_STEPS       5
/* Single and double press, as the hub's own gestures tell them apart; a triple
 * only ever comes from a remote that counted it itself, and is not reported. */
#define MB_SW_MULTI_PRESS_MAX 2

static void mb_sw_put(mb_sw_step_t *out, int *n, uint8_t event, uint8_t count)
{
    out[*n].event = event;
    out[*n].count = count;
    (*n)++;
}

/* The events one of the hub's gestures is reported as, in order, into `out`;
 * returns how many. The switch has the momentary, release, long-press and
 * multi-press features, so a press is a sequence: pressed, released, and once the
 * hub knows no second press is coming, how many there were. The hub only knows a
 * press by then - a gesture is decided at its end - so a controller is told the
 * whole sequence at once, which is what it acts on anyway: Apple Home and Home
 * Assistant trigger on MultiPressComplete and LongPress. */
static int mb_switch_events(uint8_t emit, mb_sw_step_t out[MB_SW_MAX_STEPS])
{
    int n = 0;
    switch (emit) {
    case ZBC_EMIT_SHORT:
        mb_sw_put(out, &n, MB_SW_INITIAL_PRESS, 0);
        mb_sw_put(out, &n, MB_SW_SHORT_RELEASE, 0);
        mb_sw_put(out, &n, MB_SW_MULTI_COMPLETE, 1);
        break;
    case ZBC_EMIT_DOUBLE:
        mb_sw_put(out, &n, MB_SW_INITIAL_PRESS, 0);
        mb_sw_put(out, &n, MB_SW_SHORT_RELEASE, 0);
        mb_sw_put(out, &n, MB_SW_MULTI_ONGOING, 2);
        mb_sw_put(out, &n, MB_SW_SHORT_RELEASE, 0);
        mb_sw_put(out, &n, MB_SW_MULTI_COMPLETE, 2);
        break;
    case ZBC_EMIT_LONG:
        mb_sw_put(out, &n, MB_SW_INITIAL_PRESS, 0);
        mb_sw_put(out, &n, MB_SW_LONG_PRESS, 0);
        break;
    case ZBC_EMIT_LONG_RELEASE:
        mb_sw_put(out, &n, MB_SW_LONG_RELEASE, 0);
        break;
    default:
        break;
    }
    return n;
}

/* CurrentPosition once a step has happened: 1 pressed, 0 at rest. */
static uint8_t mb_sw_position(uint8_t event)
{
    return event == MB_SW_INITIAL_PRESS || event == MB_SW_LONG_PRESS || event == MB_SW_MULTI_ONGOING ? 1 : 0;
}

/* The UniqueID a device endpoint is known by: the device's address and endpoint.
 * That is what tells a controller this is the light it saw before, however the
 * device is renamed. A remote's buttons have none: they are endpoints below the
 * remote, not bridged devices. At most 32 characters, the attribute's limit:
 * "elixcon-" and the 16 hex digits leave room for the endpoint. */
static int mb_device_uid(char *out, size_t cap, const uint8_t ieee[8], uint8_t ep)
{
    char hex[17];
    for (int i = 0; i < 8; i++) {
        snprintf(hex + 2 * i, 3, "%02x", ieee[i]);
    }
    return snprintf(out, cap, "elixcon-%s-%u", hex, (unsigned)ep);
}

/* >>> end host-test region: matter <<< */

static_assert((int)MB_ON_AS_WAS == (int)ZBC_TURN_ON_LAST && (int)MB_ON_ADAPTIVE == (int)ZBC_TURN_ON_ADAPTIVE &&
                  (int)MB_ON_FULL == (int)ZBC_TURN_ON_FULL,
              "mb_turn_on reads the stored zbc_turn_on_t values");

/* Rooms take the first CONFIG_ZBC_MAX_GROUPS slots and devices the rest, so a
 * remote with many buttons can never cost a room its light. */
#define MB_DEV_SLOTS CONFIG_ZBC_MAX_MATTER_EXPOSED
#define MB_SLOTS     (CONFIG_ZBC_MAX_GROUPS + MB_DEV_SLOTS)
/* NodeLabel's 32 characters and a terminator. */
#define MB_LABEL_LEN 33

static_assert(ZBC_GROUP_NAME_LEN <= MB_LABEL_LEN && ZBC_NAME_LEN <= MB_LABEL_LEN &&
                  ZBC_BUTTON_NAME_LEN <= MB_LABEL_LEN,
              "a slot's name holds a group's, a device's or a button's");
static_assert((int)MB_SLOT_LAMP == (int)MATTER_DEV_LAMP && (int)MB_SLOT_SWITCH == (int)MATTER_DEV_SWITCH &&
                  (int)MB_SLOT_OCCUPANCY == (int)MATTER_DEV_OCCUPANCY && (int)MB_SLOT_REMOTE == (int)MATTER_DEV_REMOTE,
              "matter_device_t.kind is the slot's");

typedef struct {
    bool     used;
    uint8_t  kind;            /* mb_slot_kind_t */
    uint16_t group_id;        /* ROOM */
    /* The others: which device endpoint, and for a switch which of its buttons. */
    uint8_t  ieee[ZBC_IEEE_LEN];
    uint8_t  ep;
    uint8_t  key;
    uint16_t button_id;       /* SWITCH, OCCUPANCY: the button record that speaks for it */
    uint8_t  built_shape;     /* LAMP: mb_lamp_shape() it was built with */
    bool     rebuild;         /* LAMP: the lamp's clusters say another shape now */
    uint16_t ct_min, ct_max;  /* LAMP: the physical range it was built with, mireds */
    uint16_t endpoint_id;
    /* A bridged endpoint: a room, a lamp, a sensor, a remote. A remote's button is
     * not one - esp_matter_bridge only hangs devices off the aggregator - but a
     * plain endpoint below its remote, `ep_handle`. */
    esp_matter_bridge::device_t *dev;
    endpoint_t *ep_handle;   /* SWITCH */
    /* ROOM: the group's name; the others: the label. A SWITCH's is its Descriptor
     * tag's label, which the Descriptor reads in place and only once - so it stays
     * as the endpoint was made with, and a renamed button shows its new name after
     * a restart. */
    char     name[MB_LABEL_LEN];
    /* Written by the CHIP task, read by the bridge task: 32 bits so a read can
     * never be torn, compared with a signed difference so it survives the wrap. */
    volatile uint32_t quiet_until_ms;
    volatile uint32_t color_quiet_until_ms;   /* LAMP: the colour has a window of its own */
    mb_move_t move;          /* bridge task only */
    mb_on_hold_t on_hold;    /* bridge task only */
    matter_group_t diag;     /* bridge task only; copied out by refresh_status */
    /* Devices: Reachable as the bridge last wrote it, the only writer; -1 until the
     * first write. */
    int8_t   reachable;
    /* Adaptive Lighting. Whether the light was built with the Mode Select cluster
     * is decided when it is created or resumed and never changes while it runs:
     * a cluster cannot be added to an enabled endpoint, and rebuilding the endpoint
     * would change its id, which controllers key on. The "<name> Adaptive" switch
     * can come and go at any time. Bridge task only, but for the quiet window. */
    bool     mode_select_live;
    bool     expose_logged;
    uint16_t al_endpoint_id;  /* 0 when there is no switch */
    esp_matter_bridge::device_t *al_dev;
    volatile uint32_t al_quiet_until_ms;
} mb_slot_t;

typedef struct {
    uint16_t group_id;
    uint16_t endpoint_id;
} mb_map_entry_t;

/* Which group each bridged endpoint stands for. Controllers key on the endpoint
 * id, so a group has to come back on the same one after a reboot or every
 * controller would see its lights replaced by strangers. esp_matter_bridge
 * persists the endpoints; this persists what they mean. */
typedef struct {
    uint8_t        version;
    uint8_t        count;
    mb_map_entry_t e[CONFIG_ZBC_MAX_GROUPS];
} mb_map_t;

/* The same for the device endpoints. Stored as its header and `count` entries, so
 * a build with another CONFIG_ZBC_MAX_MATTER_EXPOSED reads it and the devices keep
 * their ids (load_dev_map). */
typedef struct {
    uint8_t  ieee[ZBC_IEEE_LEN];
    uint8_t  ep;
    uint8_t  key;
    uint8_t  kind;           /* mb_slot_kind_t */
    uint8_t  reserved;
    uint16_t endpoint_id;
} mb_dev_entry_t;

typedef struct {
    uint8_t        version;
    uint8_t        count;
    mb_dev_entry_t e[MB_DEV_SLOTS];
} mb_dev_map_t;

/* One device endpoint as mexp_db and the device's own state say it should be
 * shown: what reconcile() makes the slots match, and what a resumed endpoint is
 * built from. */
typedef struct {
    uint8_t  kind;           /* mb_slot_kind_t, never ROOM */
    uint8_t  ieee[ZBC_IEEE_LEN];
    uint8_t  ep;
    uint8_t  key;
    uint8_t  position;       /* SWITCH: which button of the remote, from 1 */
    uint8_t  shape;          /* LAMP: mb_lamp_shape(), 0 for the others */
    uint16_t button_id;
    uint16_t ct_min, ct_max; /* LAMP, mireds, 0 unknown */
    char     label[MB_LABEL_LEN];
} mb_want_t;

static node_t           *s_node;
static uint16_t          s_aggregator_ep;
static mb_slot_t         s_slots[MB_SLOTS];
static QueueHandle_t     s_queue;
static SemaphoreHandle_t s_status_mtx;
static matter_status_t   s_status;
static uint32_t          s_delegate_ip;
/* Too big for the bridge task's stack; only that task touches them, and the
 * start before it. */
static zbc_group_t       s_groups[CONFIG_ZBC_MAX_GROUPS];
static mb_want_t         s_want[MB_DEV_SLOTS];
static zbc_device_t      s_dev;
/* Device endpoints asked for that have no Matter endpoint, as of the last pass,
 * and whether that was for want of heap (MB_DEV_HEAP_MIN). */
static uint8_t           s_skipped;
static bool              s_heap_short;

/* The subscriptions CHIP keeps to resume, summed per fabric. Bridge task only. */
typedef struct {
    uint8_t  index;
    uint8_t  count;
    uint8_t  tries;
    uint16_t min_s;
    uint16_t max_s;
} mb_kept_t;
static mb_kept_t s_kept[MATTER_MAX_LISTED_FABRICS];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static mb_slot_t *slot_by_endpoint(uint16_t endpoint_id)
{
    for (auto &s : s_slots) {
        if (s.used && s.endpoint_id == endpoint_id) {
            return &s;
        }
    }
    return nullptr;
}

static mb_slot_t *slot_by_al_endpoint(uint16_t endpoint_id)
{
    for (auto &s : s_slots) {
        if (s.used && s.al_endpoint_id && s.al_endpoint_id == endpoint_id) {
            return &s;
        }
    }
    return nullptr;
}

static mb_slot_t *slot_by_group(uint16_t group_id)
{
    for (auto &s : s_slots) {
        if (s.used && s.kind == MB_SLOT_ROOM && s.group_id == group_id) {
            return &s;
        }
    }
    return nullptr;
}

static mb_slot_t *switch_by_button(uint16_t button_id)
{
    for (auto &s : s_slots) {
        if (s.used && s.kind == MB_SLOT_SWITCH && s.button_id == button_id) {
            return &s;
        }
    }
    return nullptr;
}

static mb_slot_t *slot_by_device(uint8_t kind, const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint8_t key)
{
    for (auto &s : s_slots) {
        if (s.used && s.kind == kind && s.ep == ep && s.key == key && memcmp(s.ieee, ieee, ZBC_IEEE_LEN) == 0) {
            return &s;
        }
    }
    return nullptr;
}

/* A free slot in the rooms' range, or in the devices'. */
static mb_slot_t *slot_free(bool device)
{
    const int from = device ? CONFIG_ZBC_MAX_GROUPS : 0;
    const int to   = device ? MB_SLOTS : CONFIG_ZBC_MAX_GROUPS;
    for (int i = from; i < to; i++) {
        if (!s_slots[i].used) {
            return &s_slots[i];
        }
    }
    return nullptr;
}

/* What lighting.c is asked to change for the slot: the room, or the one light. */
static lighting_target_t slot_target(const mb_slot_t &s)
{
    lighting_target_t t = {};
    if (s.kind == MB_SLOT_ROOM) {
        t.group_id = s.group_id;
    } else {
        memcpy(t.ieee, s.ieee, ZBC_IEEE_LEN);
        t.ep = s.ep;
    }
    return t;
}

static const zbc_group_t *group_in(const zbc_group_t *groups, size_t n, uint16_t id)
{
    for (size_t i = 0; i < n; i++) {
        if (groups[i].id == id) {
            return &groups[i];
        }
    }
    return nullptr;
}

/* ------------------------------------------------------------ persistence */

/* A map exactly `size` bytes long, or false with nothing read. */
static bool load_blob(const char *key, void *buf, size_t size)
{
    nvs_handle_t h;
    if (nvs_open_from_partition(MB_NVS_PART, MB_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t    len = size;
    const bool ok = nvs_get_blob(h, key, buf, &len) == ESP_OK && len == size;
    nvs_close(h);
    return ok;
}

static void save_blob(const char *key, const void *buf, size_t size)
{
    nvs_handle_t h;
    if (nvs_open_from_partition(MB_NVS_PART, MB_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "Could not open %s to save the map \"%s\"", MB_NVS_PART, key);
        return;
    }
    if (nvs_set_blob(h, key, buf, size) != ESP_OK || nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "Could not save the map \"%s\"", key);
    }
    nvs_close(h);
}

static void load_map(const char *key, mb_map_t *map)
{
    if (!load_blob(key, map, sizeof(*map)) || map->version != MB_MAP_VERSION ||
        map->count > CONFIG_ZBC_MAX_GROUPS) {
        memset(map, 0, sizeof(*map));
    }
}

/* The group a stored endpoint stands for, 0 when this map does not have it. Group
 * ids start at 1. */
static uint16_t map_group(const mb_map_t *map, uint16_t endpoint_id)
{
    for (int i = 0; i < map->count; i++) {
        if (map->e[i].endpoint_id == endpoint_id) {
            return map->e[i].group_id;
        }
    }
    return 0;
}

static void save_map(const char *key, bool adaptive)
{
    mb_map_t map = {};
    map.version = MB_MAP_VERSION;
    for (const auto &s : s_slots) {
        if (s.used && s.kind == MB_SLOT_ROOM && (!adaptive || s.al_endpoint_id)) {
            map.e[map.count].group_id    = s.group_id;
            map.e[map.count].endpoint_id = adaptive ? s.al_endpoint_id : s.endpoint_id;
            map.count++;
        }
    }
    save_blob(key, &map, sizeof(map));
}

#define MB_DEV_MAP_HEAD offsetof(mb_dev_map_t, e)

/* Any length of header and whole entries. Earlier builds stored the full array,
 * unused entries and all, which reads the same way. A map with more entries than
 * this build has slots keeps the first MB_DEV_SLOTS, and the rest come back on
 * new ids. */
static void load_dev_map(mb_dev_map_t *map)
{
    memset(map, 0, sizeof(*map));
    nvs_handle_t h;
    if (nvs_open_from_partition(MB_NVS_PART, MB_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t   len = 0;
    uint8_t *buf = nullptr;
    if (nvs_get_blob(h, MB_NVS_KEY_DEV, nullptr, &len) == ESP_OK && len >= MB_DEV_MAP_HEAD &&
        (len - MB_DEV_MAP_HEAD) % sizeof(mb_dev_entry_t) == 0 &&
        (buf = static_cast<uint8_t *>(malloc(len))) != nullptr &&
        nvs_get_blob(h, MB_NVS_KEY_DEV, buf, &len) == ESP_OK) {
        const size_t  stored  = (len - MB_DEV_MAP_HEAD) / sizeof(mb_dev_entry_t);
        const uint8_t version = buf[offsetof(mb_dev_map_t, version)];
        const uint8_t count   = buf[offsetof(mb_dev_map_t, count)];
        if (version == MB_DEV_MAP_VERSION && count <= stored) {
            map->version = version;
            map->count   = (uint8_t)(count < MB_DEV_SLOTS ? count : MB_DEV_SLOTS);
            memcpy(map->e, buf + MB_DEV_MAP_HEAD, map->count * sizeof(mb_dev_entry_t));
        }
    }
    free(buf);
    nvs_close(h);
}

/* The device endpoint a stored endpoint stands for, nullptr when the map does not
 * have it. */
static const mb_dev_entry_t *map_device(const mb_dev_map_t *map, uint16_t endpoint_id)
{
    for (int i = 0; i < map->count; i++) {
        if (map->e[i].endpoint_id == endpoint_id) {
            return &map->e[i];
        }
    }
    return nullptr;
}

static void save_dev_map(void)
{
    mb_dev_map_t map = {};
    map.version = MB_DEV_MAP_VERSION;
    for (const auto &s : s_slots) {
        if (s.used && s.kind != MB_SLOT_ROOM && map.count < MB_DEV_SLOTS) {
            mb_dev_entry_t &e = map.e[map.count++];
            memcpy(e.ieee, s.ieee, ZBC_IEEE_LEN);
            e.ep          = s.ep;
            e.key         = s.key;
            e.kind        = s.kind;
            e.endpoint_id = s.endpoint_id;
        }
    }
    save_blob(MB_NVS_KEY_DEV, &map, MB_DEV_MAP_HEAD + map.count * sizeof(mb_dev_entry_t));
}

static void save_maps(void)
{
    save_map(MB_NVS_KEY, false);
    save_map(MB_NVS_KEY_AL, true);
    save_dev_map();
}

/* ------------------------------------------------------ Matter -> lights */

template <typename T>
static bool decode_level(TLVReader &tlv, mb_intent_t &in)
{
    T cmd;
    if (chip::app::DataModel::Decode(tlv, cmd) != CHIP_NO_ERROR) {
        return false;
    }
    in.kind  = MB_LEVEL;
    in.level = cmd.level < 1 ? 1 : (cmd.level > 254 ? 254 : cmd.level);
    if (!cmd.transitionTime.IsNull()) {
        in.transition_ds = cmd.transitionTime.Value();
        in.timed         = true;
    }
    return true;
}

template <typename T>
static bool decode_step(TLVReader &tlv, mb_intent_t &in)
{
    T cmd;
    if (chip::app::DataModel::Decode(tlv, cmd) != CHIP_NO_ERROR || cmd.stepSize == 0) {
        return false;
    }
    in.kind = MB_STEP;
    in.dir  = cmd.stepMode == LevelControl::StepModeEnum::kUp ? 1 : -1;
    in.step = cmd.stepSize;
    if (!cmd.transitionTime.IsNull()) {
        in.transition_ds = cmd.transitionTime.Value();
        in.timed         = true;
    }
    return true;
}

template <typename T>
static bool decode_move(TLVReader &tlv, mb_intent_t &in)
{
    T cmd;
    if (chip::app::DataModel::Decode(tlv, cmd) != CHIP_NO_ERROR) {
        return false;
    }
    /* A rate of 0 is defined as "no effect". */
    uint8_t rate = cmd.rate.IsNull() ? MB_DEFAULT_RATE : cmd.rate.Value();
    if (rate == 0) {
        return false;
    }
    in.kind = MB_MOVE;
    in.dir  = cmd.moveMode == LevelControl::MoveModeEnum::kUp ? 1 : -1;
    in.rate = rate;
    return true;
}

/* A colour command for a lamp shown on its own. Hue and saturation count 0-254 in
 * Matter as in ZCL, and x and y 0-65279 in both, so nothing is converted. A move to
 * one of hue or saturation keeps the other where the controller sees it, read before
 * CHIP's handler moves anything. The moves, steps and colour loops are not carried
 * out: CHIP's handler moves the attributes, and the sync pass puts them back. */
static uint8_t read_u8(uint16_t endpoint_id, uint32_t cluster, uint32_t attr)
{
    esp_matter_attr_val_t v = esp_matter_invalid(nullptr);
    return attribute::get_val(endpoint_id, cluster, attr, &v) == ESP_OK ? v.val.u8 : 0;
}

static bool decode_color(const ConcreteCommandPath &path, TLVReader &tlv, mb_intent_t &in)
{
    namespace CC = ColorControl::Commands;
    namespace CA = ColorControl::Attributes;
    switch (path.mCommandId) {
    case CC::MoveToColorTemperature::Id: {
        CC::MoveToColorTemperature::DecodableType cmd;
        if (chip::app::DataModel::Decode(tlv, cmd) != CHIP_NO_ERROR || cmd.colorTemperatureMireds == 0) {
            return false;
        }
        in.kind          = MB_CT;
        in.a             = cmd.colorTemperatureMireds;
        in.transition_ds = cmd.transitionTime;
        in.timed         = true;
        return true;
    }
    case CC::MoveToHueAndSaturation::Id: {
        CC::MoveToHueAndSaturation::DecodableType cmd;
        if (chip::app::DataModel::Decode(tlv, cmd) != CHIP_NO_ERROR) {
            return false;
        }
        in.a             = cmd.hue;
        in.b             = cmd.saturation;
        in.transition_ds = cmd.transitionTime;
        break;
    }
    case CC::MoveToHue::Id: {
        CC::MoveToHue::DecodableType cmd;
        if (chip::app::DataModel::Decode(tlv, cmd) != CHIP_NO_ERROR) {
            return false;
        }
        in.a             = cmd.hue;
        in.b             = read_u8(path.mEndpointId, ColorControl::Id, CA::CurrentSaturation::Id);
        in.transition_ds = cmd.transitionTime;
        break;
    }
    case CC::MoveToSaturation::Id: {
        CC::MoveToSaturation::DecodableType cmd;
        if (chip::app::DataModel::Decode(tlv, cmd) != CHIP_NO_ERROR) {
            return false;
        }
        in.a             = read_u8(path.mEndpointId, ColorControl::Id, CA::CurrentHue::Id);
        in.b             = cmd.saturation;
        in.transition_ds = cmd.transitionTime;
        break;
    }
    case CC::MoveToColor::Id: {
        CC::MoveToColor::DecodableType cmd;
        if (chip::app::DataModel::Decode(tlv, cmd) != CHIP_NO_ERROR) {
            return false;
        }
        in.kind          = MB_XY;
        in.a             = cmd.colorX;
        in.b             = cmd.colorY;
        in.transition_ds = cmd.transitionTime;
        in.timed         = true;
        return true;
    }
    default:
        return false;
    }
    in.kind  = MB_HS;
    in.a     = in.a > 254 ? 254 : in.a;
    in.b     = in.b > 254 ? 254 : in.b;
    in.timed = true;
    return true;
}

/* What a controller sent, as it arrived, for /api/matter: the serial port restarts
 * the box, and "the light did something else from this controller" needs the
 * commands to answer it. A level only for a move to a level, a transition only
 * when the controller named one. */
static void note_command(const ConcreteCommandPath &path, uint8_t fabric, const mb_intent_t &in)
{
    if (!s_status_mtx) {
        return;
    }
    matter_command_t c = {};
    c.at_ms            = now_ms();
    c.endpoint         = path.mEndpointId;
    c.fabric           = fabric;
    c.cluster          = (uint16_t)path.mClusterId;
    c.command          = (uint8_t)path.mCommandId;
    c.level            = in.kind == MB_LEVEL ? in.level : 0;
    c.transition_ds    = in.timed ? in.transition_ds : MATTER_NO_TRANSITION;
    xSemaphoreTake(s_status_mtx, portMAX_DELAY);
    s_status.commands[s_status.commands_seen % MATTER_RECENT_COMMANDS] = c;
    s_status.commands_seen++;
    xSemaphoreGive(s_status_mtx);
}

/* Runs in the CHIP task with the CHIP lock held, before CHIP's own handler.
 * Returning ESP_OK lets that handler run too, which is what keeps the Matter
 * attributes - OnOff, and the stepped CurrentLevel - telling the controller the
 * truth about what it asked for. */
static esp_err_t on_command(const ConcreteCommandPath &path, TLVReader &tlv, void *opaque)
{
    (void)opaque;
    mb_slot_t *slot      = slot_by_endpoint(path.mEndpointId);
    const bool al_switch = !slot && (slot = slot_by_al_endpoint(path.mEndpointId)) != nullptr;
    /* A remote, its switches and a sensor take no commands of ours: only
     * Identify, which CHIP's own handler answers. */
    if (!slot || (slot->kind != MB_SLOT_ROOM && slot->kind != MB_SLOT_LAMP)) {
        return ESP_OK;
    }

    const uint16_t fade_ds = settings_transition_ds();
    mb_intent_t    in      = {};
    in.endpoint_id         = slot->endpoint_id;
    in.transition_ds       = fade_ds;
    in.at_us               = esp_timer_get_time();

    bool ok = true;
    if (al_switch) {
        /* The "<name> Adaptive" switch: on starts the mode, off ends it and leaves
         * the light alone. */
        switch (path.mClusterId == OnOff::Id ? path.mCommandId : 0xFFFFFFFFu) {
        case OnOff::Commands::On::Id:
        case OnOff::Commands::OnWithRecallGlobalScene::Id:
        case OnOff::Commands::OnWithTimedOff::Id:
            in.kind = MB_AL_START;
            break;
        case OnOff::Commands::Off::Id:
        case OnOff::Commands::OffWithEffect::Id:
            in.kind = MB_AL_STOP;
            break;
        case OnOff::Commands::Toggle::Id:
            in.kind = MB_AL_TOGGLE;
            break;
        default:
            ok = false;
        }
    } else if (path.mClusterId == OnOff::Id) {
        switch (path.mCommandId) {
        case OnOff::Commands::On::Id:
        case OnOff::Commands::OnWithRecallGlobalScene::Id:
        case OnOff::Commands::OnWithTimedOff::Id:
            in.kind = MB_ON;
            break;
        case OnOff::Commands::Off::Id:
        case OnOff::Commands::OffWithEffect::Id:
            in.kind = MB_OFF;
            break;
        case OnOff::Commands::Toggle::Id:
            in.kind = MB_TOGGLE;
            break;
        default:
            ok = false;
        }
    } else if (path.mClusterId == LevelControl::Id) {
        namespace LC = LevelControl::Commands;
        switch (path.mCommandId) {
        case LC::MoveToLevel::Id:
            in.if_on = true;
            ok = decode_level<LC::MoveToLevel::DecodableType>(tlv, in);
            break;
        case LC::MoveToLevelWithOnOff::Id:
            /* It switches the group on by its name, members that can only switch
             * included, which leaves nothing for an On that comes after it. */
            in.switch_on = true;
            ok = decode_level<LC::MoveToLevelWithOnOff::DecodableType>(tlv, in);
            break;
        case LC::Step::Id:
            in.if_on = true;
            ok = decode_step<LC::Step::DecodableType>(tlv, in);
            break;
        case LC::StepWithOnOff::Id:
            ok = decode_step<LC::StepWithOnOff::DecodableType>(tlv, in);
            break;
        case LC::Move::Id:
            in.if_on = true;
            ok = decode_move<LC::Move::DecodableType>(tlv, in);
            break;
        case LC::MoveWithOnOff::Id:
            ok = decode_move<LC::MoveWithOnOff::DecodableType>(tlv, in);
            break;
        case LC::Stop::Id:
        case LC::StopWithOnOff::Id:
            in.kind = MB_STOP;
            break;
        default:
            ok = false;
        }
        if (ok && in.kind == MB_MOVE) {
            /* Where the sweep starts from is where the controller last saw the
             * light, and that is CurrentLevel before CHIP's handler moves it. */
            esp_matter_attr_val_t v = esp_matter_invalid(nullptr);
            if (attribute::get_val(path.mEndpointId, LevelControl::Id,
                                   LevelControl::Attributes::CurrentLevel::Id, &v) == ESP_OK &&
                v.val.u8 >= 1 && v.val.u8 <= 254) {
                in.level = v.val.u8;
            }
        }
        if (ok && in.if_on) {
            /* Whether the controller sees the light lit - see mb_stays_dark. */
            esp_matter_attr_val_t v = esp_matter_invalid(nullptr);
            in.matter_on = attribute::get_val(path.mEndpointId, OnOff::Id, OnOff::Attributes::OnOff::Id, &v) ==
                               ESP_OK && v.val.b;
        }
    } else if (path.mClusterId == ModeSelect::Id && path.mCommandId == ModeSelect::Commands::ChangeToMode::Id) {
        ModeSelect::Commands::ChangeToMode::DecodableType cmd;
        const int want = chip::app::DataModel::Decode(tlv, cmd) == CHIP_NO_ERROR ? mb_al_intent_for_mode(cmd.newMode) : -1;
        ok      = want >= 0;
        in.kind = want == 1 ? MB_AL_START : MB_AL_STOP;
    } else if (path.mClusterId == ColorControl::Id && slot->kind == MB_SLOT_LAMP) {
        ok = decode_color(path, tlv, in);
    } else {
        ok = false;
    }

    /* Synchronous dispatch, so the handler can say who is asking. */
    const auto   *handler = static_cast<chip::app::CommandHandler *>(opaque);
    const uint8_t fabric  = handler ? handler->GetAccessingFabricIndex() : chip::kUndefinedFabricIndex;
    note_command(path, fabric, ok ? in : mb_intent_t{});
    if (!ok) {
        return ESP_OK;
    }
    /* After it is noted: the log is of what the controller sent. */
    in.transition_ds = mb_transition_ds(in.timed, in.transition_ds, fade_ds);

    /* The mode has a quiet window of its own. Holding the light's OnOff and
     * CurrentLevel sync for a mode change would freeze a dimming that has nothing
     * to do with it, and the other way round. */
    if (in.kind == MB_AL_START || in.kind == MB_AL_STOP || in.kind == MB_AL_TOGGLE) {
        slot->al_quiet_until_ms = now_ms() + MB_QUIET_MS;
    } else if (in.kind == MB_CT || in.kind == MB_HS || in.kind == MB_XY) {
        /* The colour's own, for the same reason: a colour change must not freeze
         * the brightness the controller sees, nor a dimming the colour. */
        slot->color_quiet_until_ms = now_ms() + in.transition_ds * 100u + MB_QUIET_MS;
    } else {
        /* An On is carried out only once its wait is over. */
        const uint32_t held_ms = in.kind == MB_ON ? MB_ON_HOLD_US / 1000 : 0;
        slot->quiet_until_ms   = now_ms() + held_ms + in.transition_ds * 100u + MB_QUIET_MS;
    }
    if (xQueueSend(s_queue, &in, 0) != pdTRUE) {
        /* CHIP's own handler still runs and moves the attributes, so the
         * controller sees a change the lamps never got; the sync pass puts the
         * attributes back once the quiet window ends. Rate-limited: a scene
         * that overflows the queue does so with every command in it. */
        static uint32_t last_said_ms;
        const uint32_t  t = now_ms();
        if (t - last_said_ms > 5000 || last_said_ms == 0) {
            last_said_ms = t;
            ESP_LOGW(TAG, "Command for \"%s\" dropped: the bridge queue is full", slot->name);
        }
    }
    return ESP_OK;
}

static void move_tick(mb_slot_t *slot, int64_t now_us)
{
    uint16_t ds     = 0;
    int      target = mb_move_tick(&slot->move, now_us, &ds);
    if (target) {
        const lighting_target_t t = slot_target(*slot);
        lighting_target_ramp_to(&t, zbc_level_to_pct(target), ds);
        slot->quiet_until_ms = now_ms() + ds * 100u + MB_QUIET_MS;
    }
}

/* A "turn on", as the group's setting asks - see mb_turn_on. Adaptive Lighting
 * that will not start still switches the group on: that is what was asked. A lamp
 * shown on its own has no such setting, and comes back as it was. */
static esp_err_t turn_on(const mb_slot_t &slot, const mb_intent_t &in)
{
    const lighting_target_t t = slot_target(slot);
    if (slot.kind != MB_SLOT_ROOM) {
        return lighting_target_set_onoff(&t, ZB_ONOFF_ON, in.transition_ds);
    }
    const uint16_t group_id = slot.group_id;
    const uint8_t  choice   = al_db_turn_on(group_id);
    switch (mb_turn_on(choice, al_db_expose(group_id) != ZBC_AL_EXPOSE_OFF)) {
    case MB_ON_ADAPTIVE: {
        const esp_err_t err = lighting_group_start_adaptive(group_id, in.transition_ds);
        if (err == ESP_OK) {
            return err;
        }
        ESP_LOGW(TAG, "Group %u: Adaptive Lighting did not start (%s); switching on as it was",
                 group_id, esp_err_to_name(err));
        break;
    }
    case MB_ON_FULL:
        /* A switch-on: a member that can only switch comes on too. */
        return lighting_group_switch_on_to(group_id, 100, in.transition_ds);
    default:
        if (choice == ZBC_TURN_ON_ADAPTIVE) {
            ESP_LOGI(TAG, "Group %u: turn on means Adaptive Lighting, which this room does not offer",
                     group_id);
        }
        break;
    }
    return lighting_group_set_onoff(group_id, ZB_ONOFF_ON, in.transition_ds);
}

/* A room and a lamp shown on its own take the same calls - lighting_target_* is
 * the group call for the one and the light's for the other - so curves, fades, the
 * gate and the settle reads apply to both. Adaptive Lighting and the turn-on
 * choice are a room's alone, and colour a lamp's. */
static void carry_out(const mb_intent_t &in)
{
    heap_watch_mark("matter_cmd", in.kind);
    mb_slot_t *slot = slot_by_endpoint(in.endpoint_id);
    if (!slot) {
        return;
    }
    const lighting_target_t t   = slot_target(*slot);
    const bool              room = slot->kind == MB_SLOT_ROOM;
    int64_t                 now  = esp_timer_get_time();

    /* Anything newer than a sweep ends it - but a Stop has to see it first, and
     * neither ending the Adaptive Lighting mode nor a colour touches the level. */
    const bool color = in.kind == MB_CT || in.kind == MB_HS || in.kind == MB_XY;
    if (in.kind != MB_STOP && in.kind != MB_AL_STOP && !color) {
        slot->move.active = false;
    }
    bool lamps_on = false;
    if (in.if_on && !in.matter_on) {
        lighting_target_state(&t, &lamps_on, nullptr);
    }
    if (mb_stays_dark(in.if_on, in.matter_on, lamps_on)) {
        return;
    }

    esp_err_t err = ESP_OK;
    switch (in.kind) {
    case MB_ON:
        err = turn_on(*slot, in);
        break;
    case MB_OFF:
        err = lighting_target_set_onoff(&t, ZB_ONOFF_OFF, in.transition_ds);
        break;
    case MB_TOGGLE: {
        /* Resolved here rather than by the on/off call, by the same rule (any
         * member lit), so that a toggle switching on is a turn on. */
        bool lit = false;
        lighting_target_state(&t, &lit, nullptr);
        err = lit ? lighting_target_set_onoff(&t, ZB_ONOFF_OFF, in.transition_ds) : turn_on(*slot, in);
        break;
    }
    case MB_LEVEL:
        err = in.switch_on ? lighting_target_switch_on_to(&t, zbc_level_to_pct(in.level), in.transition_ds)
                           : lighting_target_set_brightness(&t, zbc_level_to_pct(in.level), in.transition_ds);
        break;
    case MB_STEP:
        err = lighting_target_nudge(&t, in.dir * mb_step_pct(in.step), in.transition_ds, false, nullptr);
        break;
    case MB_MOVE: {
        int from = in.level;
        if (from < 1 || from > 254) {
            bool on  = false;
            int  pct = ZBC_UNKNOWN;
            lighting_target_state(&t, &on, &pct);
            from = pct == ZBC_UNKNOWN ? (in.dir > 0 ? 1 : 254) : zbc_pct_to_level(pct);
        }
        slot->move.active   = true;
        slot->move.dir      = in.dir;
        slot->move.from     = (uint8_t)from;
        slot->move.rate     = in.rate;
        slot->move.since_us = now;
        slot->move.next_us  = now;
        move_tick(slot, now);
        break;
    }
    case MB_STOP:
        if (slot->move.active) {
            int ended = mb_move_stop(&slot->move, now);
            err = lighting_target_stop_ramp(&t, zbc_level_to_pct(ended));
        }
        break;
    case MB_AL_START:
        if (room) {
            err = lighting_group_start_adaptive(slot->group_id, settings_transition_ds());
        }
        break;
    case MB_AL_STOP:
        if (room) {
            lighting_group_stop_adaptive(slot->group_id);
        }
        break;
    case MB_AL_TOGGLE: {
        if (!room) {
            break;
        }
        lighting_al_status_t st;
        const bool running = lighting_group_al_status(slot->group_id, &st) && st.active;
        if (mb_al_toggle_starts(running)) {
            err = lighting_group_start_adaptive(slot->group_id, settings_transition_ds());
        } else {
            lighting_group_stop_adaptive(slot->group_id);
        }
        break;
    }
    case MB_CT:
        if (!room) {
            err = lighting_set_color_temp(slot->ieee, slot->ep, in.a, in.transition_ds);
        }
        break;
    case MB_HS:
        if (!room) {
            err = lighting_set_color_hs(slot->ieee, slot->ep, (uint8_t)in.a, (uint8_t)in.b, in.transition_ds);
        }
        break;
    case MB_XY:
        if (!room) {
            err = lighting_set_color_xy(slot->ieee, slot->ep, in.a, in.b, in.transition_ds);
        }
        break;
    default:
        break;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "\"%s\" refused a Matter command: %s", slot->name, esp_err_to_name(err));
    }
}

static void note_write(mb_slot_t &s, const char *what, esp_err_t err);

/* A button's gesture, as the switch it is shown as reports it: the events, and
 * CurrentPosition moved before each one, as a switch's would. */
static void button_event(const mb_intent_t &in)
{
    mb_slot_t *slot = switch_by_button(in.button_id);
    if (!slot) {
        return;
    }
    mb_sw_step_t steps[MB_SW_MAX_STEPS];
    const int    n = mb_switch_events(in.emit, steps);
    if (n == 0) {
        return;
    }
    lock::ScopedChipStackLock lock(portMAX_DELAY);
    SwitchCluster *c = Switch::FindClusterOnEndpoint(slot->endpoint_id);
    if (!c) {
        if (slot->diag.last_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "\"%s\": Matter endpoint %u has no Switch cluster", slot->name, slot->endpoint_id);
        }
        slot->diag.last_err = ESP_ERR_INVALID_STATE;
        return;
    }
    /* An event the cluster drops - a feature it was not built with - comes back
     * without a number. */
    bool sent = true;
    for (int i = 0; i < n; i++) {
        const uint8_t pos = mb_sw_position(steps[i].event);
        if (c->GetCurrentPosition() != pos && c->SetCurrentPosition(pos) != CHIP_NO_ERROR) {
            sent = false;
        }
        std::optional<chip::EventNumber> number;
        switch (steps[i].event) {
        case MB_SW_INITIAL_PRESS:  number = c->OnInitialPress(1); break;
        case MB_SW_SHORT_RELEASE:  number = c->OnShortRelease(1); break;
        case MB_SW_LONG_PRESS:     number = c->OnLongPress(1); break;
        case MB_SW_LONG_RELEASE:   number = c->OnLongRelease(1); break;
        case MB_SW_MULTI_ONGOING:  number = c->OnMultiPressOngoing(1, steps[i].count); break;
        case MB_SW_MULTI_COMPLETE: number = c->OnMultiPressComplete(1, steps[i].count); break;
        default: break;
        }
        sent = sent && number.has_value();
    }
    note_write(*slot, "a switch event", sent ? ESP_OK : ESP_FAIL);
}

/* A command from the queue. An On waits here for what may follow it (see
 * MB_ON_HOLD_US); everything else is carried out, after the On or instead of it. */
static void run_intent(const mb_intent_t &in)
{
    if (in.kind == MB_SWITCH) {
        button_event(in);
        return;
    }
    mb_slot_t *slot = slot_by_endpoint(in.endpoint_id);
    if (!slot) {
        return;
    }
    if (in.kind == MB_ON) {
        /* Anything newer than a sweep ends it, and the On is newer now. */
        slot->move.active = false;
    }
    const bool  was_held = slot->on_hold.held;
    mb_intent_t out[2];
    const int   n = mb_on_hold_take(&slot->on_hold, &in, esp_timer_get_time(), out);
    if (was_held && n == 1) {
        ESP_LOGI(TAG, "\"%s\": the On was replaced by the %s after it", slot->name,
                 in.kind == MB_LEVEL ? "brightness" : "Off");
    }
    if (in.kind == MB_ON && !slot->on_hold.held) {
        ESP_LOGI(TAG, "\"%s\": the On went with the brightness before it", slot->name);
    }
    for (int i = 0; i < n; i++) {
        carry_out(out[i]);
    }
}

/* Carries out every held On whose wait is over, and says how long the bridge
 * task may sleep before the next one is due. */
static TickType_t run_held_ons(int64_t now_us, TickType_t longest)
{
    int64_t wait_us = (int64_t)longest * portTICK_PERIOD_MS * 1000;
    for (auto &s : s_slots) {
        mb_intent_t on;
        if (s.used && mb_on_hold_due(&s.on_hold, now_us, &on)) {
            carry_out(on);
        }
        if (s.used && s.on_hold.held && s.on_hold.due_us - now_us < wait_us) {
            wait_us = s.on_hold.due_us - now_us;
        }
    }
    const int64_t tick_us = (int64_t)portTICK_PERIOD_MS * 1000;
    return (TickType_t)((wait_us + tick_us - 1) / tick_us);
}

/* ------------------------------------------------------ lights -> Matter */

/* Deliberately does nothing - see the top of the file. Every step of a CHIP
 * transition lands here, and so does every write the bridge makes itself. */
static esp_err_t attribute_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                              uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv)
{
    (void)type; (void)endpoint_id; (void)cluster_id; (void)attribute_id; (void)val; (void)priv;
    return ESP_OK;
}

static void note_write(mb_slot_t &s, const char *what, esp_err_t err)
{
    if (err == ESP_OK) {
        s.diag.writes++;
        return;
    }
    if (s.diag.last_err != err) {
        ESP_LOGW(TAG, "\"%s\": writing %s to Matter failed: %s", s.name, what, esp_err_to_name(err));
    }
    s.diag.last_err = err;
}

/* Writes an attribute only when Matter does not already have the value. */
static void put_u8(mb_slot_t &s, uint32_t cluster, uint32_t attr, esp_matter_attr_val_t nv, const char *what)
{
    esp_matter_attr_val_t v = esp_matter_invalid(nullptr);
    if (attribute::get_val(s.endpoint_id, cluster, attr, &v) == ESP_OK && v.val.u8 == nv.val.u8) {
        return;
    }
    note_write(s, what, attribute::update(s.endpoint_id, cluster, attr, &nv));
}

static void put_u16(mb_slot_t &s, uint32_t cluster, uint32_t attr, uint16_t value, const char *what)
{
    esp_matter_attr_val_t v = esp_matter_invalid(nullptr);
    if (attribute::get_val(s.endpoint_id, cluster, attr, &v) == ESP_OK && v.val.u16 == value) {
        return;
    }
    esp_matter_attr_val_t nv = esp_matter_uint16(value);
    note_write(s, what, attribute::update(s.endpoint_id, cluster, attr, &nv));
}

/* What each slot shows, gathered before the CHIP lock is taken: these walk the
 * group and device registries under their own mutexes. -1 is unknown throughout. */
typedef struct {
    bool    known, on, on_known, held;
    int     pct;
    bool    al_known, al_active, al_held;
    int8_t  reachable;
    bool    color_known, color_held;
    int32_t ct;
    int16_t hue, sat;
    int8_t  mode;
    int8_t  occupied;
} mb_seen_t;

static void see_room_or_lamp(const mb_slot_t &s, mb_seen_t &g, uint32_t t)
{
    const lighting_target_t target = slot_target(s);
    g.pct   = ZBC_UNKNOWN;
    g.known = lighting_target_state(&target, &g.on, &g.pct);
    /* "Any member on" is false while no member's state is known yet, in the
     * first seconds after a reboot. Written as Off, it would tell every
     * controller the lights were off and then flip them on; while nothing is
     * known, nothing is written. */
    g.on_known = g.on || lighting_target_known_off(&target);
    g.held     = s.move.active || (int32_t)(t - s.quiet_until_ms) < 0;
    if (s.kind != MB_SLOT_ROOM) {
        return;
    }
    lighting_al_status_t al;
    g.al_known  = lighting_group_al_status(s.group_id, &al);
    g.al_active = g.al_known && al.active;
    g.al_held   = (int32_t)(t - s.al_quiet_until_ms) < 0;
}

/* A device's side: whether it is there, and for a lamp its colour as it last
 * reported it, for a sensor what its rules make of it. */
static void see_device(const mb_slot_t &s, mb_seen_t &g, uint32_t t)
{
    if (!device_db_get(s.ieee, &s_dev)) {
        return;
    }
    /* A lamp is offline to Matter as it is to the web page. `online` is not kept
     * across a restart: until the device is heard from, it is false. A lamp is
     * read at every boot and answers within seconds; a remote or a sensor sleeps
     * and says nothing until it is used, and one not heard from since the restart
     * is not known to be gone - only one heard and then marked gone is. */
    g.reachable = (int8_t)(s.kind == MB_SLOT_LAMP ? device_db_is_fresh(&s_dev, ZBC_OFFLINE_AFTER_US)
                                                  : s_dev.online || s_dev.last_seen_us == 0);
    if (s.kind == MB_SLOT_OCCUPANCY) {
        bool occupied = false;
        g.occupied = buttons_sensor_state(s.button_id, &occupied, nullptr) ? (int8_t)occupied : -1;
        return;
    }
    if (s.kind != MB_SLOT_LAMP) {
        return;
    }
    const zbc_endpoint_t *e = lighting_find_endpoint(&s_dev, s.ep);
    if (!e) {
        return;
    }
    g.color_known = true;
    g.color_held  = (int32_t)(t - s.color_quiet_until_ms) < 0;
    g.ct          = e->ct_mireds;
    g.hue         = e->hue;
    g.sat         = e->saturation;
    g.mode        = e->color_mode;
}

/* Whether the slot's endpoint has Level Control: a room's does, and a lamp's
 * unless it can only switch. */
static bool slot_dims(const mb_slot_t &s)
{
    return s.kind == MB_SLOT_ROOM || (s.kind == MB_SLOT_LAMP && mb_shape_type(s.built_shape) != MB_LAMP_ONOFF);
}

/* OnOff and CurrentLevel, for a room and a lamp alike. */
static void sync_light(mb_slot_t &s, const mb_seen_t &g)
{
    /* Read both every pass, held or not, so the diagnostics always show what
     * Matter has, and a failed read shows as "unknown" rather than silently
     * skipping the write. */
    esp_matter_attr_val_t v = esp_matter_invalid(nullptr);
    s.diag.matter_on = attribute::get_val(s.endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &v) == ESP_OK
                           ? (int8_t)v.val.b : -1;
    s.diag.matter_level = -1;
    if (slot_dims(s)) {
        v = esp_matter_invalid(nullptr);
        s.diag.matter_level =
            attribute::get_val(s.endpoint_id, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id, &v) ==
                    ESP_OK && v.val.u8 >= 1 && v.val.u8 <= 254
                ? v.val.u8 : -1;
    }
    s.diag.group_on  = g.known && g.on_known ? (int8_t)g.on : -1;
    s.diag.group_pct = g.known && g.pct != ZBC_UNKNOWN ? g.pct : -1;
    s.diag.held      = g.held;

    if (!g.known || g.held) {
        return;
    }
    if (g.on_known && s.diag.matter_on != (int8_t)g.on) {
        esp_matter_attr_val_t nv = esp_matter_bool(g.on);
        note_write(s, "OnOff", attribute::update(s.endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &nv));
    }
    if (slot_dims(s) && g.pct != ZBC_UNKNOWN && !mb_level_shows(s.diag.matter_level, g.pct)) {
        esp_matter_attr_val_t nv = esp_matter_nullable_uint8(zbc_pct_to_level(g.pct));
        note_write(s, "CurrentLevel",
                   attribute::update(s.endpoint_id, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id, &nv));
    }
}

/* Adaptive Lighting, on whichever exposure the group has. Ahead of the light's
 * own hold: the mode does not wait for a dimming to settle, and a dimming by hand
 * is exactly what ends it. */
static void sync_adaptive(mb_slot_t &s, const mb_seen_t &g)
{
    esp_matter_attr_val_t v = esp_matter_invalid(nullptr);
    s.diag.al_active    = g.al_known ? (int8_t)g.al_active : -1;
    s.diag.al_matter_on = -1;
    if (s.al_endpoint_id) {
        s.diag.al_matter_on =
            attribute::get_val(s.al_endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &v) == ESP_OK
                ? (int8_t)v.val.b : -1;
    }
    s.diag.matter_mode = -1;
    if (s.mode_select_live) {
        v = esp_matter_invalid(nullptr);
        s.diag.matter_mode =
            attribute::get_val(s.endpoint_id, ModeSelect::Id, ModeSelect::Attributes::CurrentMode::Id, &v) == ESP_OK
                ? (int8_t)v.val.u8 : -1;
    }
    if (!g.al_known || g.al_held) {
        return;
    }
    if (s.al_endpoint_id && s.diag.al_matter_on != (int8_t)g.al_active) {
        esp_matter_attr_val_t nv = esp_matter_bool(g.al_active);
        note_write(s, "Adaptive OnOff", attribute::update(s.al_endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &nv));
    }
    const int8_t mode = g.al_active ? MB_MODE_ADAPTIVE : MB_MODE_NORMAL;
    if (s.mode_select_live && s.diag.matter_mode != mode) {
        esp_matter_attr_val_t nv = esp_matter_uint8((uint8_t)mode);
        note_write(s, "CurrentMode",
                   attribute::update(s.endpoint_id, ModeSelect::Id, ModeSelect::Attributes::CurrentMode::Id, &nv));
    }
}

/* A lamp's colour as it reported it, on the attributes its light type has. Its
 * ColorMode is ZCL's: 0 hue and saturation, 1 x and y, 2 colour temperature, the
 * values Matter's ColorMode and EnhancedColorMode give them. CurrentX and CurrentY
 * are not tracked here, so they stay what the controller last set. */
static void sync_color(mb_slot_t &s, const mb_seen_t &g)
{
    if (!g.color_known || g.color_held) {
        return;
    }
    const mb_lamp_t type = mb_shape_type(s.built_shape);
    const bool      hs   = (s.built_shape & MB_SHAPE_HS) != 0;
    if (type != MB_LAMP_CT && type != MB_LAMP_COLOR) {
        return;
    }
    namespace CA = ColorControl::Attributes;
    if (g.ct > 0) {
        int32_t ct = g.ct;
        ct = ct < s.ct_min ? s.ct_min : (ct > s.ct_max ? s.ct_max : ct);
        put_u16(s, ColorControl::Id, CA::ColorTemperatureMireds::Id, (uint16_t)ct, "ColorTemperatureMireds");
    }
    if (type != MB_LAMP_COLOR) {
        return;
    }
    if (hs && g.hue >= 0 && g.hue <= 254 && g.sat >= 0 && g.sat <= 254) {
        put_u8(s, ColorControl::Id, CA::CurrentHue::Id, esp_matter_uint8((uint8_t)g.hue), "CurrentHue");
        put_u8(s, ColorControl::Id, CA::CurrentSaturation::Id, esp_matter_uint8((uint8_t)g.sat), "CurrentSaturation");
    }
    /* A lamp without hue and saturation was built without them, so a report of
     * that mode has nothing to show it with. */
    if (g.mode >= 0 && g.mode <= 2 && (g.mode != 0 || hs)) {
        put_u8(s, ColorControl::Id, CA::ColorMode::Id, esp_matter_enum8((uint8_t)g.mode), "ColorMode");
        put_u8(s, ColorControl::Id, CA::EnhancedColorMode::Id, esp_matter_enum8((uint8_t)g.mode), "EnhancedColorMode");
    }
}

/* Whether a controller should show the device as there at all. Writing Reachable
 * is what raises the ReachableChanged event. Nothing but this writes it, so the
 * slot's copy is what Matter has, and is compared without a read. */
static void sync_reachable(mb_slot_t &s, const mb_seen_t &g)
{
    if (g.reachable < 0 || s.reachable == g.reachable) {
        return;
    }
    esp_matter_attr_val_t nv  = esp_matter_bool(g.reachable != 0);
    const esp_err_t       err = attribute::update(s.endpoint_id, BridgedDeviceBasicInformation::Id,
                                                  BridgedDeviceBasicInformation::Attributes::Reachable::Id, &nv);
    note_write(s, "Reachable", err);
    if (err == ESP_OK) {
        s.reachable = g.reachable;
    }
}

/* A sensor's occupancy follows what its rules make of its reports - the same
 * occupied and clear, the clear coming its no-motion time after the last motion.
 * The cluster keeps the value itself, so this is its own setter, not an attribute
 * write. */
static void sync_occupancy(mb_slot_t &s, const mb_seen_t &g)
{
    OccupancySensingCluster *c = OccupancySensing::FindClusterOnEndpoint(s.endpoint_id);
    if (!c) {
        s.diag.matter_on = -1;
        return;
    }
    if (g.occupied >= 0 && c->IsOccupied() != (g.occupied != 0)) {
        c->SetOccupancy(g.occupied != 0);
        s.diag.writes++;
    }
    s.diag.matter_on = (int8_t)c->IsOccupied();
}

/* Copies what each room and device is showing into its Matter attributes. Those a
 * Matter command has just touched, or that are mid-sweep, are left to CHIP until
 * they settle. A room's `on` and `pct` are lighting_group_state()'s: any member
 * lit, and the last commanded percentage; a lamp's the same for the one light. */
static void sync_attributes(void)
{
    const uint32_t t = now_ms();
    static mb_seen_t g[MB_SLOTS];       /* bridge task only */
    for (size_t i = 0; i < MB_SLOTS; i++) {
        const mb_slot_t &s = s_slots[i];
        g[i]           = mb_seen_t{};
        g[i].reachable = -1;
        g[i].mode      = -1;
        g[i].occupied  = -1;
        g[i].ct        = ZBC_UNKNOWN;
        g[i].hue       = ZBC_UNKNOWN;
        g[i].sat       = ZBC_UNKNOWN;
        if (!s.used) {
            continue;
        }
        if (s.kind == MB_SLOT_ROOM || s.kind == MB_SLOT_LAMP) {
            see_room_or_lamp(s, g[i], t);
        }
        /* A button has no Bridged Device Basic Information of its own: whether
         * it is there is its remote's. */
        if (s.kind != MB_SLOT_ROOM && s.kind != MB_SLOT_SWITCH) {
            see_device(s, g[i], t);
        }
    }

    /* Then Matter's side, under the CHIP lock once for the whole pass rather
     * than once per slot - this runs four times a second. */
    lock::ScopedChipStackLock lock(portMAX_DELAY);
    for (size_t i = 0; i < MB_SLOTS; i++) {
        mb_slot_t &s = s_slots[i];
        if (!s.used) {
            continue;
        }
        switch (s.kind) {
        case MB_SLOT_ROOM:
            sync_adaptive(s, g[i]);
            sync_light(s, g[i]);
            break;
        case MB_SLOT_LAMP:
            sync_light(s, g[i]);
            sync_color(s, g[i]);
            break;
        case MB_SLOT_OCCUPANCY:
            sync_occupancy(s, g[i]);
            break;
        default:
            break;
        }
        if (s.kind != MB_SLOT_ROOM && s.kind != MB_SLOT_SWITCH) {
            sync_reachable(s, g[i]);
        }
    }
}

/* ------------------------------------------------------ groups -> endpoints */

/* What an endpoint is built as: a group's light or its "<name> Adaptive" switch,
 * or a device endpoint as its slot kind says - a lamp, a sensor, or a remote,
 * which is a bridged node and nothing else, its buttons below it. */
typedef enum {
    MB_EP_LIGHT = 0,
    MB_EP_ADAPTIVE,
    MB_EP_DEVICE,
} mb_ep_kind_t;

typedef struct {
    mb_ep_kind_t kind;
    bool         mode_select;    /* a light with the Adaptive Lighting Mode Select cluster */
    uint8_t      slot;           /* MB_EP_DEVICE: mb_slot_kind_t */
    uint8_t      shape;          /* a lamp: mb_lamp_shape() */
    uint16_t     ct_min, ct_max; /* a lamp: its range in mireds, 0 unknown */
} mb_ep_spec_t;

/* What a colour-temperature range the lamp never reported is taken as: 6500 K to
 * 2000 K, what most tunable whites can do. */
#define MB_CT_MIN_DEFAULT 153
#define MB_CT_MAX_DEFAULT 500

static void ct_range(uint16_t min, uint16_t max, uint16_t *lo, uint16_t *hi)
{
    *lo = min ? min : MB_CT_MIN_DEFAULT;
    *hi = max > *lo ? max : (MB_CT_MAX_DEFAULT > *lo ? MB_CT_MAX_DEFAULT : *lo + 1);
}

static void ct_config(cluster::color_control::feature::color_temperature::config_t &c, const mb_ep_spec_t &spec)
{
    uint16_t lo, hi;
    ct_range(spec.ct_min, spec.ct_max, &lo, &hi);
    const uint16_t mid = 370 < lo ? lo : (370 > hi ? hi : 370);   /* 2700 K, where a lamp usually is */
    c.color_temp_physical_min_mireds        = lo;
    c.color_temp_physical_max_mireds        = hi;
    c.couple_color_temp_to_level_min_mireds = lo;
    c.color_temperature_mireds              = mid;
    c.start_up_color_temperature_mireds     = nullable<uint16_t>(mid);
}

/* A lamp's light, by the most it can do. Hue and saturation are a feature of
 * their own on top of the Extended Color Light, added only for a lamp that has
 * them - an xy-only lamp would be told hues it converts badly or not at all. */
static esp_err_t add_lamp(endpoint_t *ep, const mb_ep_spec_t &spec)
{
    switch (mb_shape_type(spec.shape)) {
    case MB_LAMP_ONOFF: {
        endpoint::on_off_light::config_t config;
        return endpoint::on_off_light::add(ep, &config);
    }
    case MB_LAMP_DIMMABLE: {
        endpoint::dimmable_light::config_t config;
        return endpoint::dimmable_light::add(ep, &config);
    }
    case MB_LAMP_CT: {
        endpoint::color_temperature_light::config_t config;
        config.color_control.color_mode          = 2;   /* colour temperature, all it has */
        config.color_control.enhanced_color_mode = 2;
        ct_config(config.color_control_color_temperature, spec);
        return endpoint::color_temperature_light::add(ep, &config);
    }
    default: {
        endpoint::extended_color_light::config_t config;
        ct_config(config.color_control_color_temperature, spec);
        esp_err_t err = endpoint::extended_color_light::add(ep, &config);
        if (err != ESP_OK || !(spec.shape & MB_SHAPE_HS)) {
            return err;
        }
        cluster_t *cc = cluster::get(ep, ColorControl::Id);
        cluster::color_control::feature::hue_saturation::config_t hs;
        return cc ? cluster::color_control::feature::hue_saturation::add(cc, &hs) : ESP_FAIL;
    }
    }
}

/* A remote's button: a momentary switch that reports releases, long presses and
 * up to MB_SW_MULTI_PRESS_MAX presses in a row. */
static void switch_config(endpoint::generic_switch::config_t &config)
{
    namespace SF = cluster::switch_cluster::feature;
    config.switch_cluster.number_of_positions = 2;
    config.switch_cluster.current_position    = 0;
    config.switch_cluster.features.momentary_switch_multi_press.multi_press_max = MB_SW_MULTI_PRESS_MAX;
    config.switch_cluster.feature_flags = SF::momentary_switch::get_id() | SF::momentary_switch_release::get_id() |
                                          SF::momentary_switch_long_press::get_id() |
                                          SF::momentary_switch_multi_press::get_id();
}

/* A motion sensor. Without a sensing feature the cluster is left out silently, so
 * it says passive infrared, which is what nearly every Zigbee one is. */
static esp_err_t add_occupancy(endpoint_t *ep)
{
    endpoint::occupancy_sensor::config_t config;
    config.occupancy_sensing.occupancy_sensor_type        = 0;      /* PIR */
    config.occupancy_sensing.occupancy_sensor_type_bitmap = 0x01;   /* PIR */
    config.occupancy_sensing.feature_flags = cluster::occupancy_sensing::feature::passive_infrared::get_id();
    return endpoint::occupancy_sensor::add(ep, &config);
}

/* The Mode Select cluster's two modes, the same for every group. The cluster asks a
 * delegate for them rather than storing them, and esp-matter hands the delegate
 * given to cluster::mode_select::create() to CHIP (ModeSelectDelegateInitCB). */
static ModeSelect::Structs::ModeOptionStruct::Type s_modes[2];

class mb_modes_manager_t : public ModeSelect::SupportedModesManager
{
public:
    ModeOptionsProvider getModeOptionsProvider(chip::EndpointId) const override
    {
        return ModeOptionsProvider(s_modes, s_modes + 2);
    }

    chip::Protocols::InteractionModel::Status
    getModeOptionByMode(chip::EndpointId, uint8_t mode,
                        const ModeSelect::Structs::ModeOptionStruct::Type **data) const override
    {
        for (const auto &m : s_modes) {
            if (m.mode == mode) {
                *data = &m;
                return chip::Protocols::InteractionModel::Status::Success;
            }
        }
        return chip::Protocols::InteractionModel::Status::InvalidCommand;
    }
};

static mb_modes_manager_t s_modes_manager;

static void init_modes(void)
{
    static const char normal[]   = "Normal";
    static const char adaptive[] = "Adaptive";
    s_modes[0].label = chip::CharSpan(normal, sizeof(normal) - 1);
    s_modes[0].mode  = MB_MODE_NORMAL;
    s_modes[1].label = chip::CharSpan(adaptive, sizeof(adaptive) - 1);
    s_modes[1].mode  = MB_MODE_ADAPTIVE;
}

/* `priv` is an mb_ep_spec_t, and it decides what an endpoint is built as - not
 * the device type esp_matter_bridge stored for it. So a stored endpoint of
 * another light type comes back a Dimmable Light, and a light gains or loses
 * Mode Select at a restart, all on the same id. */
static esp_err_t device_type_cb(endpoint_t *ep, uint32_t device_type_id, void *priv)
{
    const mb_ep_spec_t *spec = static_cast<const mb_ep_spec_t *>(priv);
    if (spec && spec->kind == MB_EP_DEVICE) {
        switch (spec->slot) {
        case MB_SLOT_LAMP:      return add_lamp(ep, *spec);
        case MB_SLOT_OCCUPANCY: return add_occupancy(ep);
        case MB_SLOT_REMOTE:    return ESP_OK;   /* esp_matter_bridge made it a bridged node already */
        default:                return ESP_ERR_INVALID_ARG;   /* a button is never a bridged device */
        }
    }
    const mb_ep_kind_t kind = spec ? spec->kind
                                   : (device_type_id == ESP_MATTER_ON_OFF_PLUG_IN_UNIT_DEVICE_TYPE_ID ? MB_EP_ADAPTIVE
                                                                                                        : MB_EP_LIGHT);
    if (kind == MB_EP_ADAPTIVE) {
        endpoint::on_off_plug_in_unit::config_t config;
        return endpoint::on_off_plug_in_unit::add(ep, &config);
    }
    endpoint::dimmable_light::config_t config;
    esp_err_t err = endpoint::dimmable_light::add(ep, &config);
    if (err != ESP_OK || !spec || !spec->mode_select) {
        return err;
    }
    cluster::mode_select::config_t ms;
    strlcpy(ms.description, "Adaptive Lighting", sizeof(ms.description));
    ms.current_mode = MB_MODE_NORMAL;
    ms.delegate     = &s_modes_manager;
    return cluster::mode_select::create(ep, &ms, CLUSTER_FLAG_SERVER) ? ESP_OK : ESP_FAIL;
}

static void hook(cluster_t *cluster, const uint32_t *ids, size_t n)
{
    if (!cluster) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        command_t *cmd = command::get(cluster, ids[i], COMMAND_FLAG_ACCEPTED);
        if (cmd) {
            command::set_user_callback(cmd, on_command);
        }
    }
}

static void hook_commands(endpoint_t *ep)
{
    static const uint32_t onoff[] = {
        OnOff::Commands::Off::Id,           OnOff::Commands::On::Id,
        OnOff::Commands::Toggle::Id,        OnOff::Commands::OffWithEffect::Id,
        OnOff::Commands::OnWithRecallGlobalScene::Id, OnOff::Commands::OnWithTimedOff::Id,
    };
    static const uint32_t level[] = {
        LevelControl::Commands::MoveToLevel::Id,          LevelControl::Commands::Move::Id,
        LevelControl::Commands::Step::Id,                 LevelControl::Commands::Stop::Id,
        LevelControl::Commands::MoveToLevelWithOnOff::Id, LevelControl::Commands::MoveWithOnOff::Id,
        LevelControl::Commands::StepWithOnOff::Id,        LevelControl::Commands::StopWithOnOff::Id,
    };
    /* Used by the same function for the light and for the "<name> Adaptive" switch:
     * hook() skips a cluster the endpoint does not have - Level Control on the
     * switch, Mode Select on a light built without it. */
    static const uint32_t mode[] = {
        ModeSelect::Commands::ChangeToMode::Id,
    };
    /* A lamp shown on its own; hue and saturation only where it was built with
     * them, which hook() sees as commands the cluster does not have. */
    static const uint32_t color[] = {
        ColorControl::Commands::MoveToColorTemperature::Id, ColorControl::Commands::MoveToHueAndSaturation::Id,
        ColorControl::Commands::MoveToHue::Id,              ColorControl::Commands::MoveToSaturation::Id,
        ColorControl::Commands::MoveToColor::Id,
    };
    hook(cluster::get(ep, OnOff::Id), onoff, sizeof(onoff) / sizeof(onoff[0]));
    hook(cluster::get(ep, LevelControl::Id), level, sizeof(level) / sizeof(level[0]));
    hook(cluster::get(ep, ModeSelect::Id), mode, sizeof(mode) / sizeof(mode[0]));
    hook(cluster::get(ep, ColorControl::Id), color, sizeof(color) / sizeof(color[0]));
}

/* What a controller calls an endpoint: the group's name, or "<name> Adaptive" for
 * its switch. NodeLabel is at most 32 characters, so the suffix is what gives way. */
static void make_label(char *out, size_t cap, const char *name, bool adaptive)
{
    snprintf(out, cap, adaptive ? "%s Adaptive" : "%s", name);
}

/* The cluster's config makes the attribute, empty, so it is usually there to set. */
static void set_unique_id(cluster_t *c, char *uid)
{
    attribute_t *a = attribute::get(c, BridgedDeviceBasicInformation::Attributes::UniqueID::Id);
    if (a) {
        esp_matter_attr_val_t v = esp_matter_char_str(uid, strlen(uid));
        attribute::set_val(a, &v);
    } else {
        cluster::bridged_device_basic_information::attribute::create_unique_id(c, uid, strlen(uid));
    }
}

/* The group's name is what a controller shows the light as, and the group id is
 * what tells it that this is the same light it saw last time. The switch has an
 * id of its own, so no controller can mistake it for the light. */
static void add_identity(endpoint_t *ep, uint16_t group_id, const char *name, bool adaptive)
{
    cluster_t *c = cluster::get(ep, BridgedDeviceBasicInformation::Id);
    if (!c) {
        return;
    }
    char label[33];
    make_label(label, sizeof(label), name, adaptive);
    cluster::bridged_device_basic_information::attribute::create_node_label(c, label, strlen(label));

    char uid[33];
    snprintf(uid, sizeof(uid), adaptive ? "elixcon-light-hub-al-%u" : "elixcon-light-hub-group-%u", group_id);
    set_unique_id(c, uid);
}

/* A slot of `kind` on `endpoint_id`, its diagnostics all "unknown" until the first
 * pass reads them. */
static void slot_init(mb_slot_t *slot, uint8_t kind, uint16_t endpoint_id, const char *name)
{
    memset(slot, 0, sizeof(*slot));
    slot->used        = true;
    slot->kind        = kind;
    slot->endpoint_id = endpoint_id;
    strlcpy(slot->name, name, sizeof(slot->name));
    slot->diag.endpoint_id  = endpoint_id;
    slot->diag.group_on     = -1;
    slot->diag.group_pct    = -1;
    slot->diag.matter_on    = -1;
    slot->diag.matter_level = -1;
    slot->diag.al_active    = -1;
    slot->diag.al_matter_on = -1;
    slot->diag.matter_mode  = -1;
}

static bool adopt(mb_slot_t *slot, esp_matter_bridge::device_t *dev, uint16_t group_id, const char *name,
                  bool mode_select)
{
    add_identity(dev->endpoint, group_id, name, false);
    hook_commands(dev->endpoint);
    if (endpoint::enable(dev->endpoint) != ESP_OK) {
        ESP_LOGE(TAG, "Could not enable the endpoint for group %u", group_id);
        esp_matter_bridge::remove_device(dev);
        return false;
    }
    slot_init(slot, MB_SLOT_ROOM, endpoint::get_id(dev->endpoint), name);
    slot->group_id         = group_id;
    slot->dev              = dev;
    slot->mode_select_live = mode_select;
    slot->diag.group_id    = group_id;
    ESP_LOGI(TAG, "Group %u \"%s\" is Matter endpoint %u%s", group_id, name, slot->endpoint_id,
             mode_select ? ", with an Adaptive Lighting Mode Select" : "");
    return true;
}

/* The group's "<name> Adaptive" switch, on a slot its light already has. */
static bool adopt_al(mb_slot_t *slot, esp_matter_bridge::device_t *dev)
{
    add_identity(dev->endpoint, slot->group_id, slot->name, true);
    hook_commands(dev->endpoint);
    if (endpoint::enable(dev->endpoint) != ESP_OK) {
        ESP_LOGE(TAG, "Could not enable the Adaptive Lighting endpoint for group %u", slot->group_id);
        esp_matter_bridge::remove_device(dev);
        return false;
    }
    slot->al_endpoint_id   = endpoint::get_id(dev->endpoint);
    slot->al_dev           = dev;
    slot->diag.al_endpoint = slot->al_endpoint_id;
    ESP_LOGI(TAG, "Group %u \"%s\" has its Adaptive Lighting switch on Matter endpoint %u", slot->group_id,
             slot->name, slot->al_endpoint_id);
    return true;
}

/* ------------------------------------------------------ devices -> endpoints */

static const char *device_title(const zbc_device_t &d)
{
    return d.name[0] ? d.name : (d.model[0] ? d.model : "Zigbee device");
}

/* How many of a device's endpoints are lamps: one of several gets its endpoint
 * in its name, or a two-gang relay would be two lights of one name. */
static int lamp_endpoints(const zbc_device_t &d)
{
    int n = 0;
    for (int i = 0; i < d.ep_count; i++) {
        n += (d.eps[i].features & ZBC_FEAT_ONOFF) != 0;
    }
    return n;
}

static void want_push(const mb_want_t &w, size_t *n, unsigned *missed)
{
    if (*n < MB_DEV_SLOTS) {
        s_want[(*n)++] = w;
    } else {
        (*missed)++;
    }
}

/* The device endpoints mexp_db asks for, as they should be shown, into s_want;
 * returns how many. One a device no longer has is left out, and so is a sensor
 * nobody has set up: what its occupancy is, and when it clears, are its rules'.
 * Those and the ones that do not fit are counted in `skipped`. */
static size_t want_devices(uint8_t *skipped)
{
    mexp_entry_t list[MB_DEV_SLOTS];
    const size_t entries = mexp_db_list(list, MB_DEV_SLOTS);
    size_t       n       = 0;
    unsigned     missed  = 0;
    for (size_t i = 0; i < entries; i++) {
        if (!device_db_get(list[i].ieee, &s_dev)) {
            continue;
        }
        const zbc_endpoint_t *e = lighting_find_endpoint(&s_dev, list[i].ep);
        if (!e) {
            continue;
        }
        mb_want_t w = {};
        memcpy(w.ieee, list[i].ieee, ZBC_IEEE_LEN);
        w.ep = list[i].ep;

        zbc_button_ref_t refs[CONFIG_ZBC_MAX_BUTTONS];
        if (e->features & ZBC_FEAT_MOTION) {
            const size_t nr = button_db_list_on(w.ieee, w.ep, refs, CONFIG_ZBC_MAX_BUTTONS);
            for (size_t k = 0; k < nr; k++) {
                if (refs[k].src == ZBC_BTN_SRC_SENSOR) {
                    w.button_id = refs[k].id;
                }
            }
            if (!w.button_id) {
                missed++;
                continue;
            }
            w.kind = MB_SLOT_OCCUPANCY;
            strlcpy(w.label, device_title(s_dev), sizeof(w.label));
            want_push(w, &n, &missed);
        } else if ((e->features & ZBC_FEAT_SWITCH) || !(e->features & ZBC_FEAT_ONOFF)) {
            /* A remote, as the web page has it: an endpoint that sends presses is
             * one even where it lists On/Off as well (the older Aqara switch
             * does), and so is any endpoint buttons have been set up on. One
             * switch per button, in the order the buttons were set up: the page
             * sets a known remote up top to bottom (remotes.ts), which its key
             * numbers are not - a Hue dimmer's run On, Down, Up, Off. Apple Home
             * numbers a remote's buttons and shows no names, so this order -
             * button_db_list_on's - is what tells them apart there.
             *
             * The remote first, as the accessory its buttons go under: a want
             * list is made in order, and a button is made only below a remote
             * that has its endpoint. A remote with no button set up is nothing. */
            const size_t nr       = button_db_list_on(w.ieee, w.ep, refs, CONFIG_ZBC_MAX_BUTTONS);
            uint8_t      position = 0;
            for (size_t k = 0; k < nr; k++) {
                if (refs[k].src != ZBC_BTN_SRC_ZIGBEE) {
                    continue;
                }
                if (position == 0) {
                    mb_want_t r = w;
                    r.kind      = MB_SLOT_REMOTE;
                    strlcpy(r.label, device_title(s_dev), sizeof(r.label));
                    want_push(r, &n, &missed);
                }
                w.kind      = MB_SLOT_SWITCH;
                w.key       = refs[k].key;
                w.button_id = refs[k].id;
                w.position  = ++position;
                if (!button_db_name(refs[k].id, w.label) || !w.label[0]) {
                    snprintf(w.label, sizeof(w.label), "Button %u", (unsigned)w.position);
                }
                want_push(w, &n, &missed);
            }
        } else {
            w.kind     = MB_SLOT_LAMP;
            w.shape    = mb_lamp_shape(e->features & ZBC_FEAT_LEVEL, e->features & ZBC_FEAT_COLOR_TEMP,
                                       e->features & ZBC_FEAT_ANY_COLOR, e->features & ZBC_FEAT_COLOR_HS);
            w.ct_min   = e->ct_min_mireds > 0 && e->ct_min_mireds < 0xFFFF ? (uint16_t)e->ct_min_mireds : 0;
            w.ct_max   = e->ct_max_mireds > 0 && e->ct_max_mireds < 0xFFFF ? (uint16_t)e->ct_max_mireds : 0;
            if (lamp_endpoints(s_dev) > 1) {
                snprintf(w.label, sizeof(w.label), "%.28s %u", device_title(s_dev), (unsigned)w.ep);
            } else {
                strlcpy(w.label, device_title(s_dev), sizeof(w.label));
            }
            want_push(w, &n, &missed);
        }
    }
    *skipped = (uint8_t)(missed > 255 ? 255 : missed);
    return n;
}

static const mb_want_t *want_find(size_t n, uint8_t kind, const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint8_t key)
{
    for (size_t i = 0; i < n; i++) {
        const mb_want_t &w = s_want[i];
        if (w.kind == kind && w.ep == ep && w.key == key && memcmp(w.ieee, ieee, ZBC_IEEE_LEN) == 0) {
            return &w;
        }
    }
    return nullptr;
}

static mb_ep_spec_t want_spec(const mb_want_t &w)
{
    mb_ep_spec_t spec = {};
    spec.kind   = MB_EP_DEVICE;
    spec.slot   = w.kind;
    spec.shape  = w.shape;
    spec.ct_min = w.ct_min;
    spec.ct_max = w.ct_max;
    return spec;
}

/* The device type esp_matter_bridge stores for it, kept true to what it is. What
 * the endpoint is built as is the spec's (device_type_cb). */
static uint32_t want_device_type(const mb_want_t &w)
{
    if (w.kind == MB_SLOT_REMOTE) {
        return ESP_MATTER_BRIDGED_NODE_DEVICE_TYPE_ID;
    }
    if (w.kind == MB_SLOT_OCCUPANCY) {
        return ESP_MATTER_OCCUPANCY_SENSOR_DEVICE_TYPE_ID;
    }
    switch (mb_shape_type(w.shape)) {
    case MB_LAMP_ONOFF:    return ESP_MATTER_ON_OFF_LIGHT_DEVICE_TYPE_ID;
    case MB_LAMP_DIMMABLE: return ESP_MATTER_DIMMABLE_LIGHT_DEVICE_TYPE_ID;
    case MB_LAMP_CT:       return ESP_MATTER_COLOR_TEMPERATURE_LIGHT_DEVICE_TYPE_ID;
    default:               return ESP_MATTER_EXTENDED_COLOR_LIGHT_DEVICE_TYPE_ID;
    }
}

static void set_label(uint16_t endpoint_id, const char *label)
{
    char buf[MB_LABEL_LEN];
    strlcpy(buf, label, sizeof(buf));
    esp_matter_attr_val_t v = esp_matter_char_str(buf, strlen(buf));
    attribute::update(endpoint_id, BridgedDeviceBasicInformation::Id,
                      BridgedDeviceBasicInformation::Attributes::NodeLabel::Id, &v);
}

/* The label is what a controller shows; the UniqueID what tells it the endpoint is
 * the one it saw before; vendor and product are the device's own, read here once
 * rather than carried in every want. */
static void add_device_identity(endpoint_t *ep, const mb_want_t &w)
{
    namespace BA = cluster::bridged_device_basic_information::attribute;
    cluster_t *c = cluster::get(ep, BridgedDeviceBasicInformation::Id);
    if (!c) {
        return;
    }
    char buf[MB_LABEL_LEN];
    strlcpy(buf, w.label, sizeof(buf));
    BA::create_node_label(c, buf, strlen(buf));
    char uid[33];
    mb_device_uid(uid, sizeof(uid), w.ieee, w.ep);
    set_unique_id(c, uid);
    if (!device_db_get(w.ieee, &s_dev)) {
        return;
    }
    if (s_dev.manufacturer[0]) {
        strlcpy(buf, s_dev.manufacturer, sizeof(buf));
        BA::create_vendor_name(c, buf, strlen(buf));
    }
    if (s_dev.model[0]) {
        strlcpy(buf, s_dev.model, sizeof(buf));
        BA::create_product_name(c, buf, strlen(buf));
    }
}

static const char *slot_kind_name(uint8_t kind)
{
    return kind == MB_SLOT_LAMP     ? "light"
         : kind == MB_SLOT_SWITCH   ? "switch"
         : kind == MB_SLOT_REMOTE   ? "remote"
                                    : "occupancy sensor";
}

/* A slot as a want describes it, on endpoint `ep`. */
static void fill_slot(mb_slot_t *slot, const mb_want_t &w, endpoint_t *ep)
{
    slot_init(slot, w.kind, endpoint::get_id(ep), w.label);
    memcpy(slot->ieee, w.ieee, ZBC_IEEE_LEN);
    slot->ep          = w.ep;
    slot->key         = w.key;
    slot->button_id   = w.button_id;
    slot->built_shape = w.shape;
    ct_range(w.ct_min, w.ct_max, &slot->ct_min, &slot->ct_max);
    slot->reachable = -1;
    char ieee[19];
    zbc_ieee_to_str(w.ieee, ieee);
    ESP_LOGI(TAG, "\"%s\" (%s ep %u%s) is Matter endpoint %u, a %s", w.label, ieee, w.ep,
             w.kind == MB_SLOT_SWITCH ? ", a button" : "", slot->endpoint_id, slot_kind_name(w.kind));
}

/* A lamp, a sensor or a remote: a bridged device of its own. */
static bool adopt_device(mb_slot_t *slot, esp_matter_bridge::device_t *dev, const mb_want_t &w)
{
    add_device_identity(dev->endpoint, w);
    if (w.kind == MB_SLOT_LAMP) {
        hook_commands(dev->endpoint);
    }
    if (endpoint::enable(dev->endpoint) != ESP_OK) {
        ESP_LOGE(TAG, "Could not enable the endpoint for \"%s\"", w.label);
        esp_matter_bridge::remove_device(dev);
        return false;
    }
    fill_slot(slot, w, dev->endpoint);
    slot->dev = dev;
    return true;
}

/* A remote's button: a Generic Switch endpoint below the remote's, made anew
 * (`resume_id` 0) or brought back on the id it had. Brought back, it is a bare
 * endpoint, and gets the Descriptor that a new one is made with. */
static endpoint_t *make_switch(uint16_t resume_id)
{
    endpoint::generic_switch::config_t config;
    switch_config(config);
    if (!resume_id) {
        return endpoint::generic_switch::create(s_node, &config, ENDPOINT_FLAG_DESTROYABLE, nullptr);
    }
    endpoint_t *ep = endpoint::resume(s_node, ENDPOINT_FLAG_DESTROYABLE, resume_id, nullptr);
    if (!ep) {
        return nullptr;
    }
    cluster::descriptor::config_t descriptor;
    if (!cluster::descriptor::create(ep, &descriptor, CLUSTER_FLAG_SERVER) ||
        endpoint::generic_switch::add(ep, &config) != ESP_OK) {
        endpoint::destroy(s_node, ep);
        return nullptr;
    }
    return ep;
}

/* The button under its remote, which is what makes a controller show the remote as
 * one accessory with its buttons in it. Each is told apart by a Descriptor tag:
 * its place on the remote, in Matter's Common Number namespace, labelled with the
 * button's name. */
#define MB_NS_COMMON_NUMBER 0x07

static bool adopt_switch(mb_slot_t *slot, endpoint_t *ep, const mb_want_t &w, const mb_slot_t &remote)
{
    endpoint_t *parent = endpoint::get(s_node, remote.endpoint_id);
    if (!parent || endpoint::set_parent_endpoint(ep, parent) != ESP_OK) {
        endpoint::destroy(s_node, ep);
        return false;
    }
    fill_slot(slot, w, ep);
    slot->ep_handle = ep;
    chip::app::DataModel::Provider::SemanticTag tag;
    tag.namespaceID = MB_NS_COMMON_NUMBER;
    tag.tag         = w.position;
    tag.label.SetValue(chip::app::DataModel::MakeNullable(chip::CharSpan::fromCharString(slot->name)));
    endpoint::set_semantic_tags(ep, &tag, 1);
    if (endpoint::enable(ep) != ESP_OK) {
        ESP_LOGE(TAG, "Could not enable the endpoint for \"%s\"", w.label);
        endpoint::destroy(s_node, ep);
        slot->used = false;
        return false;
    }
    return true;
}

static void remove_slot(mb_slot_t &s)
{
    if (s.kind == MB_SLOT_SWITCH) {
        endpoint::destroy(s_node, s.ep_handle);
        s.used = false;
        return;
    }
    if (s.al_dev) {
        esp_matter_bridge::remove_device(s.al_dev);
    }
    esp_matter_bridge::remove_device(s.dev);
    s.used = false;
}

/* The remote a button goes under, when it has its endpoint. */
static const mb_slot_t *remote_of(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    return slot_by_device(MB_SLOT_REMOTE, ieee, ep, 0);
}

/* The device endpoints: every slot that mexp_db no longer asks for goes, every one
 * it does gets an endpoint, and names follow the devices and buttons. A lamp
 * whose clusters now say another light type is rebuilt at the next restart on the
 * same id, as a room's Mode Select is. Called with the CHIP lock held. */
static bool reconcile_devices(size_t wanted)
{
    bool changed = false;
    /* A remote's buttons first: an endpoint goes before the one it hangs off. */
    for (int pass = 0; pass < 2; pass++) {
    for (int i = CONFIG_ZBC_MAX_GROUPS; i < MB_SLOTS; i++) {
        mb_slot_t &s = s_slots[i];
        if (!s.used || (pass == 0) != (s.kind == MB_SLOT_SWITCH)) {
            continue;
        }
        const mb_want_t *w = want_find(wanted, s.kind, s.ieee, s.ep, s.key);
        /* A button goes with its remote, whatever the list says. */
        if (!w || (s.kind == MB_SLOT_SWITCH && !remote_of(s.ieee, s.ep))) {
            ESP_LOGI(TAG, "\"%s\" is no longer shown in Matter, removing endpoint %u", s.name, s.endpoint_id);
            remove_slot(s);
            changed = true;
            continue;
        }
        s.button_id = w->button_id;
        /* A button's name is its tag's label, which stays until a restart. */
        if (s.kind != MB_SLOT_SWITCH && strcmp(s.name, w->label) != 0) {
            strlcpy(s.name, w->label, sizeof(s.name));
            set_label(s.endpoint_id, s.name);
        }
        if (s.kind == MB_SLOT_LAMP) {
            const bool rebuild = w->shape != s.built_shape;
            if (rebuild && !s.rebuild) {
                ESP_LOGI(TAG, "\"%s\" can do something else now; its Matter light is rebuilt at the next restart",
                         s.name);
            }
            s.rebuild = rebuild;
        }
    }
    }
    bool heap_short = false;
    for (size_t k = 0; k < wanted; k++) {
        const mb_want_t &w = s_want[k];
        if (slot_by_device(w.kind, w.ieee, w.ep, w.key)) {
            continue;
        }
        mb_slot_t *slot = slot_free(true);
        if (!slot) {
            s_skipped++;
            continue;
        }
        const size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (free_heap < MB_DEV_HEAP_MIN) {
            if (!s_heap_short) {
                ESP_LOGW(TAG, "\"%s\" waits for a Matter endpoint: %u bytes of heap free, %u needed", w.label,
                         (unsigned)free_heap, (unsigned)MB_DEV_HEAP_MIN);
            }
            heap_short = true;
            s_skipped++;
            continue;
        }
        if (w.kind == MB_SLOT_SWITCH) {
            const mb_slot_t *remote = remote_of(w.ieee, w.ep);
            endpoint_t      *ep     = remote ? make_switch(0) : nullptr;
            if (!ep) {
                if (remote) {
                    ESP_LOGE(TAG, "Could not create a Matter endpoint for \"%s\"", w.label);
                }
                s_skipped++;
                continue;
            }
            changed |= adopt_switch(slot, ep, w, *remote);
            continue;
        }
        mb_ep_spec_t spec = want_spec(w);
        esp_matter_bridge::device_t *dev =
            esp_matter_bridge::create_device(s_node, s_aggregator_ep, want_device_type(w), &spec);
        if (!dev) {
            ESP_LOGE(TAG, "Could not create a Matter endpoint for \"%s\"", w.label);
            s_skipped++;
            continue;
        }
        changed |= adopt_device(slot, dev, w);
    }
    s_heap_short = heap_short;
    return changed;
}

/* After a reboot: bring back every bridged endpoint whose group or device still
 * wants it, on the endpoint id it had, and forget the rest. Lights first, because a
 * switch belongs to the slot its light makes - and esp_matter_bridge lists the ids
 * in no particular order. Called with the CHIP lock held.
 *
 * An id no map claims is erased in the first pass. Besides an endpoint whose map
 * was never saved, that is the 0s esp_matter_bridge loads into the new tail of its
 * id array when CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT has grown: its free
 * entries are kInvalidEndpointId, so until erase_bridged_device_info(0) turns the
 * 0s into that, there would be no room for a new endpoint. */
static void resume_bridged(void)
{
    mb_map_t     lights, switches;
    mb_dev_map_t devices;
    load_map(MB_NVS_KEY, &lights);
    load_map(MB_NVS_KEY_AL, &switches);
    load_dev_map(&devices);
    size_t       n      = group_db_snapshot(s_groups, CONFIG_ZBC_MAX_GROUPS);
    const size_t wanted = want_devices(&s_skipped);

    uint16_t ids[MAX_BRIDGED_DEVICE_COUNT];
    if (esp_matter_bridge::get_bridged_endpoint_ids(ids) != ESP_OK) {
        return;
    }
    bool zeros_freed = false;
    for (int pass = 0; pass < 3; pass++) {
        for (uint16_t id : ids) {
            if (id == chip::kInvalidEndpointId) {
                continue;
            }
            const uint16_t        light_group = map_group(&lights, id);
            const uint16_t        al_group    = light_group ? 0 : map_group(&switches, id);
            const mb_dev_entry_t *de          = light_group || al_group ? nullptr : map_device(&devices, id);
            const int             owner       = al_group ? 1 : (de ? 2 : 0);
            if (owner != pass) {
                continue;
            }
            if (pass == 0 && !light_group) {
                if (id == 0) {
                    if (!zeros_freed) {
                        zeros_freed = true;
                        esp_matter_bridge::erase_bridged_device_info(0);
                    }
                    continue;
                }
                ESP_LOGI(TAG, "Endpoint %u no longer stands for anything, removing it", id);
                esp_matter_bridge::erase_bridged_device_info(id);
                continue;
            }
            if (pass == 0) {
                const zbc_group_t *g    = group_in(s_groups, n, light_group);
                mb_slot_t         *slot = slot_free(false);
                if (!g || !slot || slot_by_group(g->id)) {
                    ESP_LOGI(TAG, "Endpoint %u no longer stands for a group, removing it", id);
                    esp_matter_bridge::erase_bridged_device_info(id);
                    continue;
                }
                mb_ep_spec_t spec = {};
                spec.kind         = MB_EP_LIGHT;
                spec.mode_select  = al_db_expose(g->id) == ZBC_AL_EXPOSE_MODE_SELECT;
                esp_matter_bridge::device_t *dev = esp_matter_bridge::resume_device(s_node, id, &spec);
                if (dev) {
                    adopt(slot, dev, g->id, g->name, spec.mode_select);
                }
                continue;
            }
            if (pass == 1) {
                mb_slot_t *slot = slot_by_group(al_group);
                if (!slot || slot->al_endpoint_id || al_db_expose(al_group) != ZBC_AL_EXPOSE_ENDPOINT) {
                    ESP_LOGI(TAG, "Endpoint %u no longer stands for a group, removing it", id);
                    esp_matter_bridge::erase_bridged_device_info(id);
                    continue;
                }
                mb_ep_spec_t spec = {};
                spec.kind         = MB_EP_ADAPTIVE;
                esp_matter_bridge::device_t *dev = esp_matter_bridge::resume_device(s_node, id, &spec);
                if (dev) {
                    adopt_al(slot, dev);
                }
                continue;
            }
            /* A lamp comes back built from what it can do now, which is how one
             * that gained colour gets it - on the id controllers know. A button
             * is never a bridged device; one that is was made by a build that
             * had no remotes, and goes. */
            const mb_want_t *w    = de->kind == MB_SLOT_SWITCH ? nullptr
                                                               : want_find(wanted, de->kind, de->ieee, de->ep, de->key);
            mb_slot_t       *slot = slot_free(true);
            if (!w || !slot || slot_by_device(w->kind, w->ieee, w->ep, w->key)) {
                ESP_LOGI(TAG, "Endpoint %u no longer stands for a device, removing it", id);
                esp_matter_bridge::erase_bridged_device_info(id);
                continue;
            }
            mb_ep_spec_t spec = want_spec(*w);
            esp_matter_bridge::device_t *dev = esp_matter_bridge::resume_device(s_node, id, &spec);
            if (dev) {
                adopt_device(slot, dev, *w);
            }
        }
    }
    /* Then the remotes' buttons, below the remotes that came back. They are not in
     * esp_matter_bridge's list, only in the device map; one not wanted any more
     * is simply not brought back, and its id is never given out again. */
    for (int i = 0; i < devices.count; i++) {
        const mb_dev_entry_t &de = devices.e[i];
        if (de.kind != MB_SLOT_SWITCH) {
            continue;
        }
        const mb_want_t *w      = want_find(wanted, MB_SLOT_SWITCH, de.ieee, de.ep, de.key);
        const mb_slot_t *remote = remote_of(de.ieee, de.ep);
        mb_slot_t       *slot   = slot_free(true);
        if (!w || !remote || !slot || slot_by_device(MB_SLOT_SWITCH, de.ieee, de.ep, de.key)) {
            continue;
        }
        endpoint_t *ep = make_switch(de.endpoint_id);
        if (!ep) {
            ESP_LOGE(TAG, "Could not bring back endpoint %u for \"%s\"", de.endpoint_id, w->label);
            continue;
        }
        adopt_switch(slot, ep, *w, *remote);
    }
    save_maps();
}

/* Once a second: a light for every group, none for a group that is gone, the
 * Adaptive Lighting switch wherever a group asks for one, and the names current. */
static void reconcile(void)
{
    size_t       n       = group_db_snapshot(s_groups, CONFIG_ZBC_MAX_GROUPS);
    /* Before the CHIP lock, like the snapshot: this reads the device, button and
     * exposure tables under their own mutexes. */
    const size_t wanted  = want_devices(&s_skipped);
    bool         changed = false;

    lock::ScopedChipStackLock lock(portMAX_DELAY);
    for (auto &s : s_slots) {
        if (s.used && s.kind == MB_SLOT_ROOM && !group_in(s_groups, n, s.group_id)) {
            ESP_LOGI(TAG, "Group %u is gone, removing Matter endpoint %u", s.group_id, s.endpoint_id);
            remove_slot(s);
            changed = true;
        }
    }
    for (size_t i = 0; i < n; i++) {
        const zbc_group_t *g      = &s_groups[i];
        const uint8_t      expose = al_db_expose(g->id);
        mb_slot_t         *slot   = slot_by_group(g->id);
        if (!slot) {
            slot = slot_free(false);
            if (!slot) {
                break;
            }
            mb_ep_spec_t spec = {};
            spec.kind         = MB_EP_LIGHT;
            spec.mode_select  = expose == ZBC_AL_EXPOSE_MODE_SELECT;
            esp_matter_bridge::device_t *dev = esp_matter_bridge::create_device(
                s_node, s_aggregator_ep, ESP_MATTER_DIMMABLE_LIGHT_DEVICE_TYPE_ID, &spec);
            if (!dev) {
                ESP_LOGE(TAG, "Could not create a Matter endpoint for group %u", g->id);
                break;
            }
            if (!adopt(slot, dev, g->id, g->name, spec.mode_select)) {
                continue;
            }
            changed = true;
        } else if (strcmp(slot->name, g->name) != 0) {
            strlcpy(slot->name, g->name, sizeof(slot->name));
            set_label(slot->endpoint_id, g->name);
            if (slot->al_endpoint_id) {
                char label[MB_LABEL_LEN];
                make_label(label, sizeof(label), g->name, true);
                set_label(slot->al_endpoint_id, label);
            }
        }

        /* The switch follows the preference at once. Turning it off and on again
         * gives it a new id, which is fine: somebody removed it on purpose. */
        if (expose == ZBC_AL_EXPOSE_ENDPOINT && !slot->al_endpoint_id) {
            mb_ep_spec_t spec = {};
            spec.kind         = MB_EP_ADAPTIVE;
            esp_matter_bridge::device_t *dev = esp_matter_bridge::create_device(
                s_node, s_aggregator_ep, ESP_MATTER_ON_OFF_PLUG_IN_UNIT_DEVICE_TYPE_ID, &spec);
            if (!dev) {
                ESP_LOGE(TAG, "Could not create the Adaptive Lighting endpoint for group %u", g->id);
            } else {
                changed |= adopt_al(slot, dev);
            }
        } else if (expose != ZBC_AL_EXPOSE_ENDPOINT && slot->al_endpoint_id) {
            ESP_LOGI(TAG, "Group %u no longer has an Adaptive Lighting switch, removing Matter endpoint %u", g->id,
                     slot->al_endpoint_id);
            esp_matter_bridge::remove_device(slot->al_dev);
            slot->al_endpoint_id   = 0;
            slot->al_dev           = nullptr;
            slot->diag.al_endpoint = 0;
            changed                = true;
        }

        /* Mode Select cannot be added to or taken from a light that is running, so
         * the light is rebuilt on its own id at the next restart. Said once. */
        const bool want_mode_select = expose == ZBC_AL_EXPOSE_MODE_SELECT;
        if (want_mode_select != slot->mode_select_live) {
            if (!slot->expose_logged) {
                slot->expose_logged = true;
                ESP_LOGI(TAG, "Group %u %s Adaptive Lighting Mode Select at the next restart", g->id,
                         want_mode_select ? "gains its" : "loses its");
            }
        } else {
            slot->expose_logged = false;
        }
    }
    changed |= reconcile_devices(wanted);
    if (changed) {
        save_maps();
    }
}

/* ------------------------------------------------------------ plumbing */

/* CHIP publishes its services under an operational host name of its own, and in
 * doing so renames the mDNS host, after which `zigbee.local` stops resolving.
 * The name comes back as a delegated host pointing at our own address, kept
 * current across a new lease. */
static void keep_hostname(void)
{
    wifi_mgr_status_t wifi;
    wifi_manager_get_status(&wifi);
    char host[64] = "";
    if (!wifi.connected || wifi.hostname[0] == '\0' || mdns_hostname_get(host) != ESP_OK ||
        strcmp(host, wifi.hostname) == 0) {
        return;
    }
    esp_netif_ip_info_t ip;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta || esp_netif_get_ip_info(sta, &ip) != ESP_OK || ip.ip.addr == 0 || ip.ip.addr == s_delegate_ip) {
        return;
    }
    mdns_ip_addr_t addr = {};
    addr.addr.type              = ESP_IPADDR_TYPE_V4;
    addr.addr.u_addr.ip4.addr   = ip.ip.addr;
    esp_err_t err = s_delegate_ip ? mdns_delegate_hostname_set_address(wifi.hostname, &addr)
                                  : mdns_delegate_hostname_add(wifi.hostname, &addr);
    if (err == ESP_OK) {
        s_delegate_ip = ip.ip.addr;
        ESP_LOGI(TAG, "mDNS host is now \"%s\"; %s.local kept as a delegated name", host, wifi.hostname);
    }
}

/* Called with the CHIP lock held. What CHIP resumes after a restart is what it
 * wrote here: this tells a resumption that keeps failing (`tries`) from one with
 * nothing to resume. */
static void read_kept(void)
{
    using chip::app::SubscriptionResumptionStorage;
    SubscriptionResumptionStorage *storage = chip::app::InteractionModelEngine::GetInstance()->GetSubscriptionResumptionStorage();
    SubscriptionResumptionStorage::SubscriptionInfoIterator *it = storage ? storage->IterateSubscriptions() : nullptr;
    if (!it) {
        return;
    }
    mb_kept_t kept[MATTER_MAX_LISTED_FABRICS] = {};
    SubscriptionResumptionStorage::SubscriptionInfo info;
    while (it->Next(info)) {
        mb_kept_t *k = nullptr;
        for (auto &e : kept) {
            if (e.count == 0 || e.index == info.mFabricIndex) {
                k = &e;
                break;
            }
        }
        if (!k) {
            continue;
        }
        k->index = info.mFabricIndex;
        k->count++;
        k->min_s = info.mMinInterval > k->min_s ? info.mMinInterval : k->min_s;
        k->max_s = info.mMaxInterval > k->max_s ? info.mMaxInterval : k->max_s;
#if CHIP_CONFIG_SUBSCRIPTION_TIMEOUT_RESUMPTION
        const uint8_t tries = info.mResumptionRetries > UINT8_MAX ? UINT8_MAX : (uint8_t)info.mResumptionRetries;
        k->tries = tries > k->tries ? tries : k->tries;
#endif
    }
    it->Release();
    memcpy(s_kept, kept, sizeof(kept));
}

static void refresh_status(void)
{
    static uint32_t refreshes;
    uint8_t         fabrics = 0;
    bool            window  = false;
    matter_fabric_t list[MATTER_MAX_LISTED_FABRICS] = {};
    uint8_t         listed  = 0;
    {
        lock::ScopedChipStackLock lock(portMAX_DELAY);
        chip::Server &server = chip::Server::GetInstance();
        chip::app::InteractionModelEngine *im = chip::app::InteractionModelEngine::GetInstance();
        fabrics = server.GetFabricTable().FabricCount();
        window  = server.GetCommissioningWindowManager().IsCommissioningWindowOpen();
        if (refreshes++ % MB_KEPT_EVERY == 0) {
            read_kept();
        }
        for (const chip::FabricInfo &fi : server.GetFabricTable()) {
            if (listed >= MATTER_MAX_LISTED_FABRICS) {
                break;
            }
            matter_fabric_t &f = list[listed++];
            f.index            = fi.GetFabricIndex();
            f.ours             = f.index == mctl_fabric_index();
            f.vendor_id        = static_cast<uint16_t>(fi.GetVendorId());
            chip::CharSpan label = fi.GetFabricLabel();
            size_t n = label.size() < sizeof(f.label) - 1 ? label.size() : sizeof(f.label) - 1;
            memcpy(f.label, label.data(), n);
            f.label[n] = '\0';
            f.subs_live = (uint8_t)im->GetNumActiveReadHandlers(chip::app::ReadHandler::InteractionType::Subscribe,
                                                                f.index);
            for (const mb_kept_t &k : s_kept) {
                if (k.count && k.index == f.index) {
                    f.subs_kept    = k.count;
                    f.sub_min_s    = k.min_s;
                    f.sub_max_s    = k.max_s;
                    f.resume_tries = k.tries;
                }
            }
            f.live_after_s = -1;
        }
    }
    /* The rooms before the status lock, as al_db_expose takes a lock of its own. */
    uint8_t        groups = 0;
    matter_group_t diag[CONFIG_ZBC_MAX_GROUPS] = {};
    for (const auto &s : s_slots) {
        if (s.used && s.kind == MB_SLOT_ROOM && groups < CONFIG_ZBC_MAX_GROUPS) {
            matter_group_t &d  = diag[groups++];
            d                  = s.diag;
            d.expose           = al_db_expose(s.group_id);
            d.mode_select_live = s.mode_select_live;
            d.al_endpoint      = s.al_endpoint_id;
        }
    }
    const int32_t up_s = (int32_t)(esp_timer_get_time() / 1000000);
    xSemaphoreTake(s_status_mtx, portMAX_DELAY);
    for (uint8_t i = 0; i < listed; i++) {
        /* When a fabric first had a subscription is only known here, from one
         * refresh to the next, by its index. */
        for (uint8_t j = 0; j < s_status.fabric_listed; j++) {
            if (s_status.fabrics[j].index == list[i].index) {
                list[i].live_after_s = s_status.fabrics[j].live_after_s;
            }
        }
        if (list[i].live_after_s < 0 && list[i].subs_live) {
            list[i].live_after_s = up_s;
        }
    }
    s_status.fabric_count  = fabrics;
    s_status.fabric_listed = listed;
    memcpy(s_status.fabrics, list, sizeof(list));
    s_status.window_open   = window;
    s_status.groups        = groups;
    memcpy(s_status.group_list, diag, sizeof(diag));
    /* The devices straight into the status, as nothing here takes a lock. */
    uint8_t devices = 0;
    memset(s_status.device_list, 0, sizeof(s_status.device_list));
    for (const auto &s : s_slots) {
        if (!s.used || s.kind == MB_SLOT_ROOM || devices >= MB_DEV_SLOTS) {
            continue;
        }
        matter_device_t &d = s_status.device_list[devices++];
        memcpy(d.ieee, s.ieee, ZBC_IEEE_LEN);
        d.ep          = s.ep;
        d.key         = s.key;
        d.kind        = s.kind;
        d.endpoint_id = s.endpoint_id;
        d.button_id   = s.button_id;
        d.reachable   = s.reachable;
        if (s.kind == MB_SLOT_SWITCH) {
            const mb_slot_t *remote = remote_of(s.ieee, s.ep);
            d.parent_id = remote ? remote->endpoint_id : 0;
            d.reachable = remote ? remote->reachable : -1;
        }
        d.matter_on    = s.diag.matter_on;
        d.matter_level = s.diag.matter_level;
        d.held         = s.diag.held;
        d.writes       = s.diag.writes;
        d.last_err     = s.diag.last_err;
        d.rebuild      = s.rebuild;
    }
    s_status.devices            = devices;
    s_status.devices_skipped    = s_skipped;
    s_status.devices_heap_short = s_heap_short;
    xSemaphoreGive(s_status_mtx);
}

/* Called with the CHIP lock held. The codes come from the test commissionable
 * data (passcode 20202021, discriminator 3840) and never change, so once is
 * enough. */
static void read_codes(void)
{
    const chip::RendezvousInformationFlags flags(chip::RendezvousInformationFlag::kOnNetwork);
    char manual[sizeof(s_status.manual_code)] = "";
    char qr[sizeof(s_status.qr_code)]         = "";
    chip::MutableCharSpan m(manual, sizeof(manual) - 1);
    chip::MutableCharSpan q(qr, sizeof(qr) - 1);
    if (GetManualPairingCode(m, flags) == CHIP_NO_ERROR) {
        manual[m.size()] = '\0';
    }
    if (GetQRCode(q, flags) == CHIP_NO_ERROR) {
        qr[q.size()] = '\0';
    }
    xSemaphoreTake(s_status_mtx, portMAX_DELAY);
    strlcpy(s_status.manual_code, manual, sizeof(s_status.manual_code));
    strlcpy(s_status.qr_code, qr, sizeof(s_status.qr_code));
    xSemaphoreGive(s_status_mtx);
    ESP_LOGI(TAG, "Pair with manual code %s, or QR payload %s", manual, qr);
}

#if CONFIG_ZBC_HEAP_WATCH
/* A controller's subscriptions as heap_watch marks: asked for, established - its
 * first full report sent and acknowledged, which is where a wildcard subscription
 * costs the most - and ended. A request carries its session, so it is marked with
 * the fabric (/api/matter's fabric_list says whose); a ReadHandler keeps its
 * fabric private, so the other two carry the negotiated max interval, which
 * fabric_list[].subscriptions shows per controller. A subscription CHIP resumes by
 * itself is never requested: it shows as established only. The interaction model
 * holds one such callback, and nothing else here or in esp_matter uses it; with
 * the watch off it stays free. */
class SubscriptionMarks : public chip::app::ReadHandler::ApplicationCallback
{
    static uint32_t max_interval(const chip::app::ReadHandler &handler)
    {
        uint16_t min_s = 0, max_s = 0;
        handler.GetReportingIntervals(min_s, max_s);
        return max_s;
    }
    CHIP_ERROR OnSubscriptionRequested(chip::app::ReadHandler &, chip::Transport::SecureSession &session) override
    {
        heap_watch_mark("mtr_sub_req", session.GetFabricIndex());
        return CHIP_NO_ERROR;
    }
    void OnSubscriptionEstablished(chip::app::ReadHandler &handler) override
    {
        heap_watch_mark("mtr_sub_up", max_interval(handler));
    }
    void OnSubscriptionTerminated(chip::app::ReadHandler &handler) override
    {
        heap_watch_mark("mtr_sub_end", max_interval(handler));
    }
};
static SubscriptionMarks s_subscription_marks;
#endif

static void event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    (void)arg;
    using namespace chip::DeviceLayer;
    /* A secure session - a controller (or the bridge, to a lamp) setting up CASE -
     * by name and fabric; everything else by its CHIP event number, except CHIP's
     * repost of a Wi-Fi or IP event, which heap_watch has marked already. */
    if (event->Type == DeviceEventType::kSecureSessionEstablished) {
        heap_watch_mark("mtr_session", event->SecureSessionEstablished.FabricIndex);
    } else if (event->Type != DeviceEventType::kESPSystemEvent) {
        heap_watch_mark("chip_event", event->Type);
    }
    switch (event->Type) {
    case DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioned by a controller");
        break;
    case DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGW(TAG, "Commissioning failed: the fail-safe timer expired");
        break;
    case DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "Commissioning window open");
        break;
    case DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Commissioning window closed");
        break;
    default:
        break;
    }
}

static void bridge_task(void *arg)
{
    (void)arg;
    int64_t    next_sync  = 0;
    int64_t    next_state = 0;
    TickType_t wait       = pdMS_TO_TICKS(100);
    for (;;) {
        mb_intent_t in;
        if (xQueueReceive(s_queue, &in, wait) == pdTRUE) {
            run_intent(in);
        }
        int64_t now = esp_timer_get_time();
        wait        = run_held_ons(now, pdMS_TO_TICKS(100));
        for (auto &s : s_slots) {
            if (s.used) {
                move_tick(&s, now);
            }
        }
        if (now >= next_sync) {
            reconcile();
            refresh_status();
            keep_hostname();
            next_sync = now + MB_SYNC_US;
        }
        if (now >= next_state) {
            sync_attributes();
            next_state = now + MB_STATE_US;
        }
    }
}

esp_err_t matter_bridge_start(void)
{
    esp_err_t err = nvs_flash_init_partition(MB_NVS_PART);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase_partition(MB_NVS_PART);
        err = nvs_flash_init_partition(MB_NVS_PART);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No usable '%s' partition (%s): the partition table has to be flashed over USB",
                 MB_NVS_PART, esp_err_to_name(err));
        return err;
    }

    s_status_mtx = xSemaphoreCreateMutex();
    /* Deep enough for a scene: a controller sends one command per light in
     * it, all at once, and each is a few bytes here. */
    s_queue      = xQueueCreate(32, sizeof(mb_intent_t));
    if (!s_status_mtx || !s_queue) {
        return ESP_ERR_NO_MEM;
    }

    node::config_t node_config;
    s_node = node::create(&node_config, attribute_cb, nullptr);
    if (!s_node) {
        ESP_LOGE(TAG, "Could not create the Matter node");
        return ESP_FAIL;
    }
    endpoint::aggregator::config_t aggregator_config;
    endpoint_t *aggregator = endpoint::aggregator::create(s_node, &aggregator_config, ENDPOINT_FLAG_NONE, nullptr);
    if (!aggregator) {
        ESP_LOGE(TAG, "Could not create the aggregator endpoint");
        return ESP_FAIL;
    }
    s_aggregator_ep = endpoint::get_id(aggregator);

    err = esp_matter::start(event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Matter did not start: %s", esp_err_to_name(err));
        return err;
    }

    {
        lock::ScopedChipStackLock lock(portMAX_DELAY);
#if CONFIG_ZBC_HEAP_WATCH
        /* Before any subscription can be established: resuming one takes a
         * network round trip, and this is under the lock start() returned with. */
        chip::app::InteractionModelEngine::GetInstance()->RegisterReadHandlerAppCallback(&s_subscription_marks);
#endif
        CHIP_ERROR mode = chip::DeviceLayer::ConnectivityMgr().SetWiFiStationMode(
            chip::DeviceLayer::ConnectivityManager::kWiFiStationMode_ApplicationControlled);
        if (mode != CHIP_NO_ERROR) {
            /* Then CHIP goes on reconnecting in competition with wifi_manager,
             * which would show up as a Wi-Fi link that drops and flaps. */
            ESP_LOGE(TAG, "Could not take the Wi-Fi station away from CHIP: %" CHIP_ERROR_FORMAT, mode.Format());
        }
        /* What a controller calls the bridge itself, as opposed to the lights
         * hanging off it. Asserted once here rather than in the reconcile pass:
         * NodeLabel is writable and non-volatile, so a controller may rename it
         * and re-asserting every second would stomp that for nothing. Our
         * setting wins at each boot - which is immediately after a rename, since
         * renaming restarts the box. */
        char label[ZBC_COORD_NAME_LEN];
        strlcpy(label, settings_name(), sizeof(label));
        esp_matter_attr_val_t node_label = esp_matter_char_str(label, strlen(label));
        attribute::update(MB_ROOT_ENDPOINT, BasicInformation::Id,
                          BasicInformation::Attributes::NodeLabel::Id, &node_label);

        read_codes();
        init_modes();
        err = esp_matter_bridge::initialize(s_node, device_type_cb);
        if (err == ESP_OK) {
            resume_bridged();
        } else {
            ESP_LOGE(TAG, "Bridge did not initialise: %s", esp_err_to_name(err));
        }
    }

    xSemaphoreTake(s_status_mtx, portMAX_DELAY);
    s_status.running = true;
    xSemaphoreGive(s_status_mtx);

    if (xTaskCreate(bridge_task, "matter_br", 6144, nullptr, 4, nullptr) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void matter_bridge_get_status(matter_status_t *out)
{
    if (!s_status_mtx) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_status_mtx, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_status_mtx);
}

void matter_bridge_button_event(uint16_t button_id, uint8_t emit)
{
    /* Only what a switch reports (mb_switch_events); the rest would only fill the
     * queue. */
    mb_sw_step_t steps[MB_SW_MAX_STEPS];
    if (!s_queue || mb_switch_events(emit, steps) == 0) {
        return;
    }
    mb_intent_t in = {};
    in.kind        = MB_SWITCH;
    in.button_id   = button_id;
    in.emit        = emit;
    xQueueSend(s_queue, &in, 0);
}

esp_err_t matter_bridge_open_window(void)
{
    if (!s_node) {
        return ESP_ERR_INVALID_STATE;
    }
    lock::ScopedChipStackLock lock(portMAX_DELAY);
    chip::CommissioningWindowManager &mgr = chip::Server::GetInstance().GetCommissioningWindowManager();
    if (mgr.IsCommissioningWindowOpen()) {
        return ESP_OK;
    }
    CHIP_ERROR err = mgr.OpenBasicCommissioningWindow(chip::System::Clock::Seconds32(15 * 60),
                                                      chip::CommissioningWindowAdvertisement::kDnssdOnly);
    return err == CHIP_NO_ERROR ? ESP_OK : ESP_FAIL;
}

/* Not esp_matter::factory_reset(): that ends in CHIP's own factory reset, which
 * also calls esp_wifi_restore() and reboots on a schedule of its own. Everything
 * Matter keeps is in its own partition, so erasing that is the whole of it. */
esp_err_t matter_bridge_erase(void)
{
    if (s_node) {
        lock::ScopedChipStackLock lock(portMAX_DELAY);
        nvs_flash_deinit_partition(MB_NVS_PART);
        return nvs_flash_erase_partition(MB_NVS_PART);
    }
    nvs_flash_deinit_partition(MB_NVS_PART);
    return nvs_flash_erase_partition(MB_NVS_PART);
}
