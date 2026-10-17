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
 */
#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

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
#include <setup_payload/OnboardingCodesUtil.h>

#include "group_db.h"
#include "heap_watch.h"
#include "lighting.h"
#include "matter_bridge.h"
#include "matter_ctl.h"
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

static_assert(MAX_BRIDGED_DEVICE_COUNT >= 2 * CONFIG_ZBC_MAX_GROUPS,
              "CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT is too small: every group needs an endpoint for "
              "its light and one for its Adaptive Lighting switch, plus one for the root and one for the "
              "aggregator");

/* >>> host-test region: matter <<< */

/* What a Matter command asks of a group. Decoded in the CHIP task, carried out in
 * the bridge task - plain data, so that posting one is all the CHIP task does. */
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
} mb_kind_t;

typedef struct {
    uint8_t  kind;
    bool     if_on;          /* the variants without OnOff leave a dark group dark */
    int8_t   dir;
    uint8_t  level;          /* MB_LEVEL: target. MB_MOVE: start, 0 when unknown */
    uint8_t  step;
    uint8_t  rate;
    uint16_t group_id;
    uint16_t transition_ds;
    bool     timed;          /* the controller named the transition, as decoded */
    bool     matter_on;      /* if_on: the light's Matter OnOff when the command arrived */
    bool     switch_on;      /* MB_LEVEL: it replaced a held On, so it switches the group on too */
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
 * Only from dark. "Turn on the basement" said to a lit basement changes nothing
 * today, and a group that jumped to full - or out of the brightness somebody
 * had just set - because a voice assistant was asked for what is already so
 * would be a light that does something nobody asked it to. Adaptive Lighting
 * only where the group offers it; elsewhere the On is the plain one. */
typedef enum {
    MB_ON_AS_WAS = 0,     /* the group's last brightness, as an On always was */
    MB_ON_ADAPTIVE,
    MB_ON_FULL,
} mb_on_t;

static mb_on_t mb_turn_on(uint8_t choice, bool lit, bool al_offered)
{
    if (lit) {
        return MB_ON_AS_WAS;
    }
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
 * Every On waits, lit group or not: on a lit group an On changes nothing anybody
 * can see, so the wait only ever shows as a dark group coming on that much later. */
#define MB_ON_HOLD_US (500 * 1000)

typedef struct {
    bool        held;
    mb_intent_t on;
    int64_t     due_us;
} mb_on_hold_t;

/* What to carry out now that `in` has arrived, in order, into `out`; returns how
 * many. A brightness or an off replaces a held On: each says where the group ends
 * up, and the On would only have lit it on the way there. The brightness then
 * lights the group whatever variant it was - the On said to. Anything else starts
 * from what the On leaves (a toggle, a step, a sweep), so the On goes first. A
 * second On while one waits is the same request, and the first keeps its time. */
static int mb_on_hold_take(mb_on_hold_t *h, const mb_intent_t *in, int64_t now_us, mb_intent_t out[2])
{
    if (in->kind == MB_ON) {
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

/* >>> end host-test region: matter <<< */

static_assert((int)MB_ON_AS_WAS == (int)ZBC_TURN_ON_LAST && (int)MB_ON_ADAPTIVE == (int)ZBC_TURN_ON_ADAPTIVE &&
                  (int)MB_ON_FULL == (int)ZBC_TURN_ON_FULL,
              "mb_turn_on reads the stored zbc_turn_on_t values");

typedef struct {
    bool     used;
    uint16_t group_id;
    uint16_t endpoint_id;
    esp_matter_bridge::device_t *dev;
    char     name[ZBC_GROUP_NAME_LEN];
    /* Written by the CHIP task, read by the bridge task: 32 bits so a read can
     * never be torn, compared with a signed difference so it survives the wrap. */
    volatile uint32_t quiet_until_ms;
    mb_move_t move;          /* bridge task only */
    mb_on_hold_t on_hold;    /* bridge task only */
    matter_group_t diag;     /* bridge task only; copied out by refresh_status */
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

static node_t           *s_node;
static uint16_t          s_aggregator_ep;
static mb_slot_t         s_slots[CONFIG_ZBC_MAX_GROUPS];
static QueueHandle_t     s_queue;
static SemaphoreHandle_t s_status_mtx;
static matter_status_t   s_status;
static uint32_t          s_delegate_ip;
/* Too big for the bridge task's stack; only that task touches it. */
static zbc_group_t       s_groups[CONFIG_ZBC_MAX_GROUPS];

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
        if (s.used && s.group_id == group_id) {
            return &s;
        }
    }
    return nullptr;
}

static mb_slot_t *slot_free(void)
{
    for (auto &s : s_slots) {
        if (!s.used) {
            return &s;
        }
    }
    return nullptr;
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

static void load_map(const char *key, mb_map_t *map)
{
    memset(map, 0, sizeof(*map));
    nvs_handle_t h;
    if (nvs_open_from_partition(MB_NVS_PART, MB_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(*map);
    if (nvs_get_blob(h, key, map, &len) != ESP_OK || len != sizeof(*map) ||
        map->version != MB_MAP_VERSION || map->count > CONFIG_ZBC_MAX_GROUPS) {
        memset(map, 0, sizeof(*map));
    }
    nvs_close(h);
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
        if (s.used && (!adaptive || s.al_endpoint_id)) {
            map.e[map.count].group_id    = s.group_id;
            map.e[map.count].endpoint_id = adaptive ? s.al_endpoint_id : s.endpoint_id;
            map.count++;
        }
    }
    nvs_handle_t h;
    if (nvs_open_from_partition(MB_NVS_PART, MB_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "Could not open %s to save the group map", MB_NVS_PART);
        return;
    }
    if (nvs_set_blob(h, key, &map, sizeof(map)) != ESP_OK || nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "Could not save the group map \"%s\"", key);
    }
    nvs_close(h);
}

static void save_maps(void)
{
    save_map(MB_NVS_KEY, false);
    save_map(MB_NVS_KEY_AL, true);
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
    if (!slot) {
        return ESP_OK;
    }

    const uint16_t fade_ds = settings_transition_ds();
    mb_intent_t    in      = {};
    in.group_id            = slot->group_id;
    in.transition_ds       = fade_ds;

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
            ESP_LOGW(TAG, "Command for group %u dropped: the bridge queue is full", in.group_id);
        }
    }
    return ESP_OK;
}

static void move_tick(mb_slot_t *slot, int64_t now_us)
{
    uint16_t ds     = 0;
    int      target = mb_move_tick(&slot->move, now_us, &ds);
    if (target) {
        lighting_group_ramp_to(slot->group_id, zbc_level_to_pct(target), ds);
        slot->quiet_until_ms = now_ms() + ds * 100u + MB_QUIET_MS;
    }
}

/* A "turn on", as the group's setting asks - see mb_turn_on. Adaptive Lighting
 * that will not start still switches the group on: that is what was asked.
 * `lit` is "not known to be dark" (lighting_group_known_off), so a room nobody
 * has heard from yet is left as it was. */
static esp_err_t turn_on(const mb_intent_t &in, bool lit)
{
    const uint8_t choice = al_db_turn_on(in.group_id);
    switch (mb_turn_on(choice, lit, al_db_expose(in.group_id) != ZBC_AL_EXPOSE_OFF)) {
    case MB_ON_ADAPTIVE: {
        const esp_err_t err = lighting_group_start_adaptive(in.group_id, in.transition_ds);
        if (err == ESP_OK) {
            return err;
        }
        ESP_LOGW(TAG, "Group %u: Adaptive Lighting did not start (%s); switching on as it was",
                 in.group_id, esp_err_to_name(err));
        break;
    }
    case MB_ON_FULL:
        /* A switch-on: a member that can only switch comes on too. */
        return lighting_group_switch_on_to(in.group_id, 100, in.transition_ds);
    default:
        if (!lit && choice == ZBC_TURN_ON_ADAPTIVE) {
            ESP_LOGI(TAG, "Group %u: turn on means Adaptive Lighting, which this room does not offer",
                     in.group_id);
        }
        break;
    }
    return lighting_group_set_onoff(in.group_id, ZB_ONOFF_ON, in.transition_ds);
}

static void carry_out(const mb_intent_t &in)
{
    heap_watch_mark("matter_cmd", in.kind);
    mb_slot_t *slot = slot_by_group(in.group_id);
    if (!slot) {
        return;
    }
    int64_t now = esp_timer_get_time();

    /* Anything newer than a sweep ends it - but a Stop has to see it first, and
     * ending the Adaptive Lighting mode does not touch the lights. */
    if (in.kind != MB_STOP && in.kind != MB_AL_STOP) {
        slot->move.active = false;
    }
    bool lamps_on = false;
    if (in.if_on && !in.matter_on) {
        lighting_group_state(in.group_id, &lamps_on, nullptr);
    }
    if (mb_stays_dark(in.if_on, in.matter_on, lamps_on)) {
        return;
    }

    esp_err_t err = ESP_OK;
    switch (in.kind) {
    case MB_ON:
        /* Only a room known to be dark gets the setting: one whose members have
         * not said yet (after a restart, or lamps that do not report) may well be
         * lit, and must not be jumped to full or into a mode. */
        err = turn_on(in, !lighting_group_known_off(in.group_id));
        break;
    case MB_OFF:
        err = lighting_group_set_onoff(in.group_id, ZB_ONOFF_OFF, in.transition_ds);
        break;
    case MB_TOGGLE: {
        /* Resolved here rather than by lighting_group_set_onoff, by the same
         * rule (any member lit), so that a toggle switching on is a turn on -
         * which, as for an On, takes the setting only where the room is known
         * to be dark. */
        bool lit = false;
        lighting_group_state(in.group_id, &lit, nullptr);
        err = lit ? lighting_group_set_onoff(in.group_id, ZB_ONOFF_OFF, in.transition_ds)
                  : turn_on(in, !lighting_group_known_off(in.group_id));
        break;
    }
    case MB_LEVEL:
        err = in.switch_on
                  ? lighting_group_switch_on_to(in.group_id, zbc_level_to_pct(in.level), in.transition_ds)
                  : lighting_group_set_brightness(in.group_id, zbc_level_to_pct(in.level), in.transition_ds);
        break;
    case MB_STEP:
        err = lighting_group_nudge(in.group_id, in.dir * mb_step_pct(in.step), in.transition_ds, false, nullptr);
        break;
    case MB_MOVE: {
        int from = in.level;
        if (from < 1 || from > 254) {
            bool on  = false;
            int  pct = ZBC_UNKNOWN;
            lighting_group_state(in.group_id, &on, &pct);
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
            err = lighting_group_stop_ramp(in.group_id, zbc_level_to_pct(ended));
        }
        break;
    case MB_AL_START:
        err = lighting_group_start_adaptive(in.group_id, settings_transition_ds());
        break;
    case MB_AL_STOP:
        lighting_group_stop_adaptive(in.group_id);
        break;
    case MB_AL_TOGGLE: {
        lighting_al_status_t st;
        const bool running = lighting_group_al_status(in.group_id, &st) && st.active;
        if (mb_al_toggle_starts(running)) {
            err = lighting_group_start_adaptive(in.group_id, settings_transition_ds());
        } else {
            lighting_group_stop_adaptive(in.group_id);
        }
        break;
    }
    default:
        break;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Group %u refused a Matter command: %s", in.group_id, esp_err_to_name(err));
    }
}

/* A command from the queue. An On waits here for what may follow it (see
 * MB_ON_HOLD_US); everything else is carried out, after the On or instead of it. */
static void run_intent(const mb_intent_t &in)
{
    mb_slot_t *slot = slot_by_group(in.group_id);
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
        ESP_LOGI(TAG, "Group %u: the On was replaced by the %s after it", in.group_id,
                 in.kind == MB_LEVEL ? "brightness" : "Off");
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

/* Copies what each group is showing into its Matter attributes. Groups a Matter
 * command has just touched, or that are mid-sweep, are left to CHIP until they
 * settle. `on` and `pct` are lighting_group_state()'s: any member lit, and the
 * last commanded percentage. */
static void note_write(mb_slot_t &s, const char *what, esp_err_t err)
{
    if (err == ESP_OK) {
        s.diag.writes++;
        return;
    }
    if (s.diag.last_err != err) {
        ESP_LOGW(TAG, "Group %u: writing %s to Matter failed: %s", s.group_id, what, esp_err_to_name(err));
    }
    s.diag.last_err = err;
}

static void sync_attributes(void)
{
    const uint32_t t = now_ms();

    /* The groups' side first, without the CHIP lock: these walk the group and
     * device registries under their own mutexes. */
    struct {
        bool known, on, on_known, held;
        int  pct;
        bool al_known, al_active, al_held;
    } g[CONFIG_ZBC_MAX_GROUPS] = {};
    for (size_t i = 0; i < CONFIG_ZBC_MAX_GROUPS; i++) {
        const mb_slot_t &s = s_slots[i];
        if (!s.used) {
            continue;
        }
        g[i].pct   = ZBC_UNKNOWN;
        g[i].known = lighting_group_state(s.group_id, &g[i].on, &g[i].pct);
        /* "Any member on" is false while no member's state is known yet, in the
         * first seconds after a reboot. Written as Off, it would tell every
         * controller the lights were off and then flip them on; while nothing is
         * known, nothing is written. */
        g[i].on_known = g[i].on || lighting_group_known_off(s.group_id);
        g[i].held     = s.move.active || (int32_t)(t - s.quiet_until_ms) < 0;

        lighting_al_status_t al;
        g[i].al_known  = lighting_group_al_status(s.group_id, &al);
        g[i].al_active = g[i].al_known && al.active;
        g[i].al_held   = (int32_t)(t - s.al_quiet_until_ms) < 0;
    }

    /* Then Matter's side, under the CHIP lock once for the whole pass rather
     * than once per group - this runs four times a second. */
    lock::ScopedChipStackLock lock(portMAX_DELAY);
    for (size_t i = 0; i < CONFIG_ZBC_MAX_GROUPS; i++) {
        mb_slot_t &s = s_slots[i];
        if (!s.used) {
            continue;
        }
        /* Read both every pass, held or not, so the diagnostics always show what
         * Matter has, and a failed read shows as "unknown" rather than silently
         * skipping the write. */
        esp_matter_attr_val_t v = esp_matter_invalid(nullptr);
        s.diag.matter_on = attribute::get_val(s.endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &v) == ESP_OK
                               ? (int8_t)v.val.b : -1;
        v = esp_matter_invalid(nullptr);
        s.diag.matter_level =
            attribute::get_val(s.endpoint_id, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id, &v) ==
                    ESP_OK && v.val.u8 >= 1 && v.val.u8 <= 254
                ? v.val.u8 : -1;
        s.diag.group_on  = g[i].known && g[i].on_known ? (int8_t)g[i].on : -1;
        s.diag.group_pct = g[i].known && g[i].pct != ZBC_UNKNOWN ? g[i].pct : -1;
        s.diag.held      = g[i].held;

        /* Adaptive Lighting, on whichever exposure the group has. Ahead of the
         * light's own hold below: the mode does not wait for a dimming to settle,
         * and a dimming by hand is exactly what ends it. */
        s.diag.al_active    = g[i].al_known ? (int8_t)g[i].al_active : -1;
        s.diag.al_matter_on = -1;
        if (s.al_endpoint_id) {
            v = esp_matter_invalid(nullptr);
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
        if (g[i].al_known && !g[i].al_held) {
            if (s.al_endpoint_id && s.diag.al_matter_on != (int8_t)g[i].al_active) {
                esp_matter_attr_val_t nv = esp_matter_bool(g[i].al_active);
                note_write(s, "Adaptive OnOff",
                           attribute::update(s.al_endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &nv));
            }
            const int8_t mode = g[i].al_active ? MB_MODE_ADAPTIVE : MB_MODE_NORMAL;
            if (s.mode_select_live && s.diag.matter_mode != mode) {
                esp_matter_attr_val_t nv = esp_matter_uint8((uint8_t)mode);
                note_write(s, "CurrentMode",
                           attribute::update(s.endpoint_id, ModeSelect::Id, ModeSelect::Attributes::CurrentMode::Id,
                                             &nv));
            }
        }

        if (!g[i].known || g[i].held) {
            continue;
        }

        if (g[i].on_known && s.diag.matter_on != (int8_t)g[i].on) {
            esp_matter_attr_val_t nv = esp_matter_bool(g[i].on);
            note_write(s, "OnOff",
                       attribute::update(s.endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &nv));
        }
        if (g[i].pct != ZBC_UNKNOWN && !mb_level_shows(s.diag.matter_level, g[i].pct)) {
            esp_matter_attr_val_t nv = esp_matter_nullable_uint8(zbc_pct_to_level(g[i].pct));
            note_write(s, "CurrentLevel",
                       attribute::update(s.endpoint_id, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id,
                                         &nv));
        }
    }
}

/* ------------------------------------------------------ groups -> endpoints */

/* What a group's endpoint is built as: its light, or its "<name> Adaptive" switch. */
typedef enum {
    MB_EP_LIGHT = 0,
    MB_EP_ADAPTIVE,
} mb_ep_kind_t;

typedef struct {
    uint8_t kind;           /* mb_ep_kind_t */
    bool    mode_select;    /* a light with the Adaptive Lighting Mode Select cluster */
} mb_ep_spec_t;

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
    const uint8_t kind = spec ? spec->kind
                              : static_cast<uint8_t>(device_type_id == ESP_MATTER_ON_OFF_PLUG_IN_UNIT_DEVICE_TYPE_ID
                                                         ? MB_EP_ADAPTIVE : MB_EP_LIGHT);
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
    hook(cluster::get(ep, OnOff::Id), onoff, sizeof(onoff) / sizeof(onoff[0]));
    hook(cluster::get(ep, LevelControl::Id), level, sizeof(level) / sizeof(level[0]));
    hook(cluster::get(ep, ModeSelect::Id), mode, sizeof(mode) / sizeof(mode[0]));
}

/* What a controller calls an endpoint: the group's name, or "<name> Adaptive" for
 * its switch. NodeLabel is at most 32 characters, so the suffix is what gives way. */
static void make_label(char *out, size_t cap, const char *name, bool adaptive)
{
    snprintf(out, cap, adaptive ? "%s Adaptive" : "%s", name);
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
    attribute_t *a = attribute::get(c, BridgedDeviceBasicInformation::Attributes::UniqueID::Id);
    if (a) {
        esp_matter_attr_val_t v = esp_matter_char_str(uid, strlen(uid));
        attribute::set_val(a, &v);
    } else {
        cluster::bridged_device_basic_information::attribute::create_unique_id(c, uid, strlen(uid));
    }
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
    memset(slot, 0, sizeof(*slot));
    slot->used        = true;
    slot->group_id    = group_id;
    slot->endpoint_id = endpoint::get_id(dev->endpoint);
    slot->dev         = dev;
    strlcpy(slot->name, name, sizeof(slot->name));
    slot->mode_select_live  = mode_select;
    slot->diag.group_id     = group_id;
    slot->diag.endpoint_id  = slot->endpoint_id;
    slot->diag.group_on     = -1;
    slot->diag.group_pct    = -1;
    slot->diag.matter_on    = -1;
    slot->diag.matter_level = -1;
    slot->diag.al_active    = -1;
    slot->diag.al_matter_on = -1;
    slot->diag.matter_mode  = -1;
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

/* After a reboot: bring back every bridged endpoint whose group still wants it, on
 * the endpoint id it had, and forget the rest. Lights first, because a switch
 * belongs to the slot its light makes - and esp_matter_bridge lists the ids in no
 * particular order. Called with the CHIP lock held. */
static void resume_bridged(void)
{
    mb_map_t lights, switches;
    load_map(MB_NVS_KEY, &lights);
    load_map(MB_NVS_KEY_AL, &switches);
    size_t n = group_db_snapshot(s_groups, CONFIG_ZBC_MAX_GROUPS);

    uint16_t ids[MAX_BRIDGED_DEVICE_COUNT];
    if (esp_matter_bridge::get_bridged_endpoint_ids(ids) != ESP_OK) {
        return;
    }
    for (int pass = 0; pass < 2; pass++) {
        for (uint16_t id : ids) {
            if (id == chip::kInvalidEndpointId) {
                continue;
            }
            const uint16_t light_group = map_group(&lights, id);
            if ((pass == 0) != (light_group != 0)) {
                continue;
            }
            if (pass == 0) {
                const zbc_group_t *g    = group_in(s_groups, n, light_group);
                mb_slot_t         *slot = slot_free();
                if (!g || !slot || slot_by_group(g->id)) {
                    ESP_LOGI(TAG, "Endpoint %u no longer stands for a group, removing it", id);
                    esp_matter_bridge::erase_bridged_device_info(id);
                    continue;
                }
                mb_ep_spec_t spec = { MB_EP_LIGHT, al_db_expose(g->id) == ZBC_AL_EXPOSE_MODE_SELECT };
                esp_matter_bridge::device_t *dev = esp_matter_bridge::resume_device(s_node, id, &spec);
                if (dev) {
                    adopt(slot, dev, g->id, g->name, spec.mode_select);
                }
                continue;
            }
            const uint16_t al_group = map_group(&switches, id);
            mb_slot_t     *slot     = al_group ? slot_by_group(al_group) : nullptr;
            if (!slot || slot->al_endpoint_id || al_db_expose(al_group) != ZBC_AL_EXPOSE_ENDPOINT) {
                ESP_LOGI(TAG, "Endpoint %u no longer stands for a group, removing it", id);
                esp_matter_bridge::erase_bridged_device_info(id);
                continue;
            }
            mb_ep_spec_t spec = { MB_EP_ADAPTIVE, false };
            esp_matter_bridge::device_t *dev = esp_matter_bridge::resume_device(s_node, id, &spec);
            if (dev) {
                adopt_al(slot, dev);
            }
        }
    }
    save_maps();
}

/* Once a second: a light for every group, none for a group that is gone, the
 * Adaptive Lighting switch wherever a group asks for one, and the names current. */
static void reconcile(void)
{
    size_t n       = group_db_snapshot(s_groups, CONFIG_ZBC_MAX_GROUPS);
    bool   changed = false;

    lock::ScopedChipStackLock lock(portMAX_DELAY);
    for (auto &s : s_slots) {
        if (s.used && !group_in(s_groups, n, s.group_id)) {
            ESP_LOGI(TAG, "Group %u is gone, removing Matter endpoint %u", s.group_id, s.endpoint_id);
            if (s.al_dev) {
                esp_matter_bridge::remove_device(s.al_dev);
            }
            esp_matter_bridge::remove_device(s.dev);
            s.used  = false;
            changed = true;
        }
    }
    for (size_t i = 0; i < n; i++) {
        const zbc_group_t *g      = &s_groups[i];
        const uint8_t      expose = al_db_expose(g->id);
        mb_slot_t         *slot   = slot_by_group(g->id);
        if (!slot) {
            slot = slot_free();
            if (!slot) {
                break;
            }
            mb_ep_spec_t spec = { MB_EP_LIGHT, expose == ZBC_AL_EXPOSE_MODE_SELECT };
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
            char label[33];
            make_label(label, sizeof(label), g->name, false);
            esp_matter_attr_val_t v = esp_matter_char_str(label, strlen(label));
            attribute::update(slot->endpoint_id, BridgedDeviceBasicInformation::Id,
                              BridgedDeviceBasicInformation::Attributes::NodeLabel::Id, &v);
            if (slot->al_endpoint_id) {
                make_label(label, sizeof(label), g->name, true);
                v = esp_matter_char_str(label, strlen(label));
                attribute::update(slot->al_endpoint_id, BridgedDeviceBasicInformation::Id,
                                  BridgedDeviceBasicInformation::Attributes::NodeLabel::Id, &v);
            }
        }

        /* The switch follows the preference at once. Turning it off and on again
         * gives it a new id, which is fine: somebody removed it on purpose. */
        if (expose == ZBC_AL_EXPOSE_ENDPOINT && !slot->al_endpoint_id) {
            mb_ep_spec_t spec = { MB_EP_ADAPTIVE, false };
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
    uint8_t        groups = 0;
    matter_group_t diag[CONFIG_ZBC_MAX_GROUPS] = {};
    for (const auto &s : s_slots) {
        if (s.used) {
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
