/* Matter lamps: control and state. See matter_ctl.h for what this is, and
 * matter_fabric.cpp for why it runs on the Matter server's own stack.
 *
 * Two tables, both fixed and static. A lamp is one row of mlight_db: its state
 * as last reported and its command gate. A node is one Matter device - a bulb,
 * or a hub with twenty lamps behind it - and holds the one CASE session and the
 * one subscription all of its lamps share.
 *
 * Threading. The C API runs on any task and only ever writes a lamp's gate under
 * s_mux and wakes the CHIP task; everything that touches CHIP - sessions,
 * invokes, the subscriptions - runs on the CHIP task. So a caller never blocks,
 * and no lock is ever held across a call into CHIP.
 *
 * State is never assumed: a command is fire-and-forget, and the lamp's own
 * report through the subscription is what updates it. */
#include <inttypes.h>
#include <math.h>
#include <new>
#include <string.h>
#include <utility>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <esp_matter.h>

#include <app-common/zap-generated/cluster-objects.h>
#include <app/InteractionModelEngine.h>
#include <app/ReadClient.h>
#include <app/server/Server.h>
#include <controller/InvokeInteraction.h>
#include <platform/CHIPDeviceLayer.h>

#include "colorspace.h"
#include "heap_watch.h"
#include "matter_priv.h"
#include "mlight_db.h"
#include "state_rev.h"
#include "wifi_manager.h"

static const char *TAG = "mctl";

/* >>> host-test region: mctl-gate <<< */
/* The per-lamp command gate, and when a device counts as gone. The Zigbee gate's
 * design (zb_coordinator.c), for the Zigbee gate's reason: a command whose answer
 * is late can still be on its way when the next one goes, and arrive after it -
 * a light that fades off and comes back. So one command is on its way per lamp,
 * and the next goes when it has been answered. Plain C, so test/test_mctl_gate.c
 * can drive it. */

typedef enum {
    MCTL_CMD_NONE = 0,
    MCTL_CMD_ONOFF,     /* a: zb_onoff_cmd_t */
    MCTL_CMD_FADE_OFF,  /* OffWithEffect: fade out and off, decided by the lamp */
    MCTL_CMD_LEVEL,     /* a: level 1-254 */
    MCTL_CMD_FADE_IN,   /* a: level; two steps - to the bottom at once, then the ramp */
    MCTL_CMD_CT,        /* a: mireds */
    MCTL_CMD_WARM_OFF,  /* a: mireds, b: 1 for a plain Off; two steps - the colour, then the off */
    MCTL_CMD_HS,        /* a: hue 0-254, b: saturation 0-254 */
    MCTL_CMD_XY,        /* a: CIE x, b: CIE y, both /65536 */
} mctl_cmd_kind_t;

typedef struct {
    uint8_t  kind;           /* mctl_cmd_kind_t */
    uint8_t  step;           /* which step of a two-step command is next */
    uint16_t a, b;
    uint16_t transition_ds;
} mctl_cmd_t;

/* Which of a lamp's two queues a command waits in: a curve's level and colour are
 * two halves of one intent, and one replacing the other would drop the colour. */
#define MCTL_CH_LEVEL 0
#define MCTL_CH_COLOR 1
#define MCTL_CH_COUNT 2

/* A command whose answer has not come after this long is given up on and the next
 * let out. CHIP's own invoke gives up well before, when the session's
 * retransmissions run out, so this only guards against an answer that never comes. */
#define MCTL_GATE_TIMEOUT_US (20LL * 1000000)
/* A command still waiting for its device's session after this long is dropped
 * rather than sent late: a lamp that comes on half a minute after the press is
 * worse than one that did not. */
#define MCTL_STALE_US        (15LL * 1000000)
/* How long a device's subscription may be down before its commands are refused
 * rather than held. */
#define MCTL_UNREACH_US      (30LL * 1000000)

typedef struct {
    bool       busy;          /* a command holds the slot: on its way, or taken and waiting to go */
    bool       sent;          /* ...and it is on its way */
    int64_t    since_us;      /* when the command in the slot was asked for */
    int64_t    sent_us;
    mctl_cmd_t inflight;
    mctl_cmd_t pending[MCTL_CH_COUNT];
    int64_t    queued_us[MCTL_CH_COUNT];
} mctl_gate_t;

static int mctl_channel(uint8_t kind)
{
    return kind == MCTL_CMD_CT || kind == MCTL_CMD_HS || kind == MCTL_CMD_XY ? MCTL_CH_COLOR : MCTL_CH_LEVEL;
}

static bool mctl_two_step(uint8_t kind)
{
    return kind == MCTL_CMD_FADE_IN || kind == MCTL_CMD_WARM_OFF;
}

/* A new command. A free slot is taken at once - as the Zigbee gate sends at once -
 * so that a caller's second command queues behind its first instead of replacing
 * it: the warm-off's dim, then its colour-and-off, are two calls. Otherwise it
 * replaces whatever waits on its channel. */
static void mctl_gate_admit(mctl_gate_t *g, const mctl_cmd_t *c, int64_t now_us)
{
    if (!g->busy) {
        g->busy          = true;
        g->sent          = false;
        g->since_us      = now_us;
        g->inflight      = *c;
        g->inflight.step = 0;
        return;
    }
    const int ch        = mctl_channel(c->kind);
    g->pending[ch]      = *c;
    g->pending[ch].step = 0;
    g->queued_us[ch]    = now_us;
}

/* Whatever waits moves into the slot: level before colour, so a brightness goes
 * out ahead of the colour that belongs to it. */
static void mctl_gate_promote(mctl_gate_t *g)
{
    g->busy          = false;
    g->sent          = false;
    g->inflight.kind = MCTL_CMD_NONE;
    for (int ch = 0; ch < MCTL_CH_COUNT; ch++) {
        if (g->pending[ch].kind != MCTL_CMD_NONE) {
            g->inflight           = g->pending[ch];
            g->since_us           = g->queued_us[ch];
            g->pending[ch].kind   = MCTL_CMD_NONE;
            g->busy               = true;
            return;
        }
    }
}

/* What to put on air now, if anything. With `can_send` false - the device's
 * session is not up yet - nothing is taken, but a command that has waited too
 * long is still dropped, and one whose answer never came still given up on.
 * Adds to *dropped and *timed_out what it gave up. */
static bool mctl_gate_next(mctl_gate_t *g, int64_t now_us, bool can_send, mctl_cmd_t *out,
                           int *dropped, int *timed_out)
{
    for (;;) {
        if (!g->busy) {
            return false;
        }
        if (g->sent) {
            if (now_us - g->sent_us < MCTL_GATE_TIMEOUT_US) {
                return false;
            }
            (*timed_out)++;
            mctl_gate_promote(g);
            continue;
        }
        if (now_us - g->since_us >= MCTL_STALE_US) {
            (*dropped)++;
            mctl_gate_promote(g);
            continue;
        }
        if (!can_send) {
            return false;
        }
        g->sent    = true;
        g->sent_us = now_us;
        *out       = g->inflight;
        return true;
    }
}

/* The command on its way was answered, or failed. The first step of a two-step
 * command lets the second out whether or not it arrived - the second is the part
 * that matters, the first only how it looks (FADE_IN) or what the lamp is left at
 * (WARM_OFF); otherwise the slot passes to whatever waits. */
static void mctl_gate_done(mctl_gate_t *g, int64_t now_us)
{
    if (!g->busy || !g->sent) {
        return;
    }
    if (mctl_two_step(g->inflight.kind) && g->inflight.step == 0) {
        g->inflight.step = 1;
        g->sent          = false;
        g->since_us      = now_us;
        return;
    }
    mctl_gate_promote(g);
}

/* Whether a command to a device whose subscription is down is refused rather
 * than held. A device still connecting - at boot, or just after it dropped - is
 * waited for: a press in the first seconds after a restart must not be refused.
 * `down_since_us` is when it went down, or when we started trying. */
static bool mctl_refuses(bool up, int64_t down_since_us, int64_t now_us)
{
    return !up && now_us - down_since_us > MCTL_UNREACH_US;
}
/* >>> end host-test region: mctl-gate <<< */

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;

namespace {

constexpr int      kLamps = CONFIG_ZBC_MAX_MATTER_LIGHTS;
constexpr int      kNodes = CONFIG_ZBC_MAX_MATTER_NODES;
/* A subscription's max interval: the device reports at least this often, and
 * silence for a little longer is how a device that has gone is noticed. */
constexpr uint16_t kMaxIntervalS = 60;
/* Subscriptions set up at once. Each needs CASE, and the Server has two CASE
 * clients for the bridge's own subscription resumption and ours together. */
constexpr int      kConnectAtOnce = 2;
constexpr uint32_t kTickMs = 500;
/* A command that could not get a session waits this long before trying again. */
constexpr int64_t  kConnectRetryUs = 5LL * 1000000;
/* How long the station has to have had an address before anything is looked
 * up. CHIP's operational lookup waits up to 45 s for an answer, and the ESP32
 * DNS-SD backend asks once: a query that goes out before Wi-Fi has associated is
 * lost, and the first subscription after every boot then starts 45 s late. */
constexpr int64_t  kNetSettleUs = 5LL * 1000000;
int64_t            s_net_since_us;   /* when the station was first seen with an address, 0 while not */

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
bool         s_running;
TaskHandle_t s_chip_task;   /* seen from the tick, for its stack's high-water mark */

struct lamp_t {
    bool        used;
    uint16_t    id;
    uint8_t     node;          /* index into s_nodes */
    uint16_t    ep;
    uint16_t    features;
    /* As reported, ZBC_UNKNOWN until then. */
    int8_t      on_off;
    int16_t     level;
    int32_t     ct;
    int16_t     hue, sat;
    int32_t     x, y;
    int8_t      mode;
    mctl_gate_t gate;
    bool        stop_wanted;   /* Stop is not gated - see mctl_level_stop */
    bool        no_off_effect; /* refused OffWithEffect once; a plain Off from then on */
    uint32_t    gen;           /* bumps when the slot changes hands: a late answer is dropped */
};

void conn_ok(void * ctx, Messaging::ExchangeManager &, const SessionHandle & session);
void conn_fail(void * ctx, const ScopedNodeId &, CHIP_ERROR err);

class NodeSub;

struct node_t {
    node_t() : on_conn(conn_ok, this), on_fail(conn_fail, this) {}

    bool     used = false;
    uint64_t node_id = 0;
    uint8_t  lamp_fabric = 0;
    /* Shared, under s_mux. */
    bool     up = false;
    int64_t  down_since_us = 0;
    int64_t  last_seen_us = 0;
    uint32_t drops = 0;
    uint32_t reports = 0;
    uint32_t last_error = 0;
    uint32_t heap_low_up = 0;
    /* CHIP task only. */
    NodeSub *sub = nullptr;
    bool     sub_pending = false;  /* sent, neither established nor failed yet */
    uint8_t  retries = 0;
    uint8_t  lamps = 0;
    bool     colour = false;       /* any of its lamps does colour: subscribe Color Control */
    SessionHolder session;
    bool     connecting = false;
    bool     rescan = false;       /* run the discovery once the session is up */
    int64_t  next_connect_us = 0;
    uint32_t gen = 0;
    Callback::Callback<OnDeviceConnected>         on_conn;
    Callback::Callback<OnDeviceConnectionFailure> on_fail;
};

lamp_t s_lamps[kLamps];
node_t s_nodes[kNodes];

struct {
    uint32_t sent, failed, dropped, timeouts;
} s_stats;

bool s_kick_posted;
void kick();

int64_t now_us()
{
    return esp_timer_get_time();
}

int node_index(const node_t * n)
{
    return (int)(n - s_nodes);
}

/* CHIP task: nothing else changes which rows are used. */
int lamp_at(int node, EndpointId ep)
{
    for (int i = 0; i < kLamps; i++) {
        if (s_lamps[i].used && s_lamps[i].node == node && s_lamps[i].ep == ep) {
            return i;
        }
    }
    return -1;
}

void note_error(node_t & n, CHIP_ERROR err)
{
    portENTER_CRITICAL(&s_mux);
    n.last_error = err.AsInteger();
    portEXIT_CRITICAL(&s_mux);
}

void set_up(node_t & n, bool up)
{
    bool changed;
    portENTER_CRITICAL(&s_mux);
    changed = n.up != up;
    n.up    = up;
    if (up) {
        n.last_seen_us = now_us();
        n.heap_low_up  = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    } else if (changed) {
        n.down_since_us = now_us();
        n.drops++;
    }
    portEXIT_CRITICAL(&s_mux);
    if (changed) {
        heap_watch_mark(up ? "mctl_up" : "mctl_down", (uint32_t)n.node_id);
        zbc_state_bump();      /* the page shows online */
        ESP_LOGI(TAG, "Node 0x%016" PRIx64 " %s", n.node_id, up ? "is up" : "went down");
    }
}

/* ------------------------------------------------------------ subscriptions */

/* The wait before the next resubscription: fast at first - a lamp that was only
 * restarting is back in seconds - then at most a minute. CHIP's own policy backs
 * off to an hour and a half, and "unplugged, plugged back in" must not wait that
 * long to come back. */
uint32_t resubscribe_wait_ms(uint8_t retries)
{
    static const uint32_t waits[] = { 1000, 3000, 10000, 20000, 30000, 60000 };
    return waits[retries < sizeof(waits) / sizeof(waits[0]) ? retries : sizeof(waits) / sizeof(waits[0]) - 1];
}

template <typename T>
bool decode(TLV::TLVReader & data, T & out)
{
    return DataModel::Decode(data, out) == CHIP_NO_ERROR;
}

class NodeSub : public ReadClient::Callback
{
public:
    NodeSub(int node, uint32_t gen) : node_(node), gen_(gen) {}

    ReadClient *client = nullptr;
    bool        dead   = false;     /* OnDone came: the tick replaces it */

    void OnReportEnd() override
    {
        node_t & n = s_nodes[node_];
        portENTER_CRITICAL(&s_mux);
        n.last_seen_us = now_us();
        n.reports++;
        portEXIT_CRITICAL(&s_mux);
        if (changed_) {
            changed_ = false;
            zbc_state_bump();
        }
    }

    void NotifySubscriptionStillActive(const ReadClient &) override
    {
        node_t & n = s_nodes[node_];
        portENTER_CRITICAL(&s_mux);
        n.last_seen_us = now_us();
        portEXIT_CRITICAL(&s_mux);
    }

    void OnSubscriptionEstablished(SubscriptionId) override
    {
        node_t & n   = s_nodes[node_];
        n.sub_pending = false;
        n.retries     = 0;
        set_up(n, true);
        kick();                  /* commands held for the session go now */
    }

    CHIP_ERROR OnResubscriptionNeeded(ReadClient * c, CHIP_ERROR cause) override
    {
        node_t & n    = s_nodes[node_];
        n.sub_pending = false;
        note_error(n, cause);
        set_up(n, false);
        const uint32_t wait = resubscribe_wait_ms(n.retries);
        if (n.retries < 255) {
            n.retries++;
        }
        /* A liveness timeout means the session is as dead as the subscription:
         * set a new one up rather than resubscribe over it. */
        return c->ScheduleResubscription(wait, NullOptional, cause == CHIP_ERROR_TIMEOUT);
    }

    void OnError(CHIP_ERROR err) override { note_error(s_nodes[node_], err); }

    void OnCASESessionEstablished(const SessionHandle & session, ReadPrepareParams &) override
    {
        /* The session the subscription set up serves the commands too. */
        s_nodes[node_].session.Grab(session);
    }

    void OnDeallocatePaths(ReadPrepareParams && params) override
    {
        delete[] params.mpAttributePathParamsList;
        params.mpAttributePathParamsList    = nullptr;
        params.mAttributePathParamsListSize = 0;
    }

    void OnDone(ReadClient *) override
    {
        /* Only when resubscribing itself failed. Deleted from the tick, not here:
         * the ReadClient is still on the stack. */
        dead = true;
        node_t & n    = s_nodes[node_];
        n.sub_pending = false;
        set_up(n, false);
    }

    void OnAttributeData(const ConcreteDataAttributePath & path, TLV::TLVReader * data, const StatusIB & status) override
    {
        if (!data || !status.IsSuccess() || s_nodes[node_].gen != gen_) {
            return;
        }
        const int li = lamp_at(node_, path.mEndpointId);
        if (li < 0) {
            return;          /* a hub endpoint that is not one of our lamps */
        }
        lamp_t & l = s_lamps[li];
        int32_t  v = ZBC_UNKNOWN;
        int      field = -1;

        if (path.mClusterId == OnOff::Id && path.mAttributeId == OnOff::Attributes::OnOff::Id) {
            bool b;
            if (decode(*data, b)) {
                v     = b ? 1 : 0;
                field = 0;
            }
        } else if (path.mClusterId == LevelControl::Id && path.mAttributeId == LevelControl::Attributes::CurrentLevel::Id) {
            DataModel::Nullable<uint8_t> lvl;
            if (decode(*data, lvl)) {
                v     = lvl.IsNull() ? ZBC_UNKNOWN : lvl.Value();
                field = 1;
            }
        } else if (path.mClusterId == ColorControl::Id) {
            namespace CA = ColorControl::Attributes;
            uint16_t u16;
            uint8_t  u8;
            switch (path.mAttributeId) {
            case CA::ColorMode::Id: {
                ColorControl::ColorModeEnum m;
                if (decode(*data, m)) {
                    v     = (int32_t)m;
                    field = 2;
                }
                break;
            }
            case CA::ColorTemperatureMireds::Id:
                if (decode(*data, u16)) {
                    v     = u16;
                    field = 3;
                }
                break;
            case CA::CurrentHue::Id:
                if (decode(*data, u8)) {
                    v     = u8;
                    field = 4;
                }
                break;
            case CA::CurrentSaturation::Id:
                if (decode(*data, u8)) {
                    v     = u8;
                    field = 5;
                }
                break;
            case CA::CurrentX::Id:
                if (decode(*data, u16)) {
                    v     = u16;
                    field = 6;
                }
                break;
            case CA::CurrentY::Id:
                if (decode(*data, u16)) {
                    v     = u16;
                    field = 7;
                }
                break;
            default:
                break;
            }
        }
        if (field < 0) {
            return;
        }
        bool changed = false;
        portENTER_CRITICAL(&s_mux);
        switch (field) {
        case 0: changed = l.on_off != v; l.on_off = (int8_t)v; break;
        case 1: changed = l.level != v; l.level = (int16_t)v; break;
        case 2: changed = l.mode != v; l.mode = (int8_t)v; break;
        case 3: changed = l.ct != v; l.ct = v; break;
        case 4: changed = l.hue != v; l.hue = (int16_t)v; break;
        case 5: changed = l.sat != v; l.sat = (int16_t)v; break;
        case 6: changed = l.x != v; l.x = v; break;
        case 7: changed = l.y != v; l.y = v; break;
        default: break;
        }
        portEXIT_CRITICAL(&s_mux);
        changed_ |= changed;
    }

private:
    int      node_;
    uint32_t gen_;
    bool     changed_ = false;
};

void stop_sub(node_t & n)
{
    if (n.sub) {
        /* The client first: its destructor still calls into the callback. */
        if (n.sub->client) {
            Platform::Delete(n.sub->client);
        }
        Platform::Delete(n.sub);
        n.sub = nullptr;
    }
    n.sub_pending = false;
}

/* OnOff and CurrentLevel on every endpoint, and Color Control whole when any lamp
 * of the device does colour: three paths at most, the most a device is promised
 * to take in one subscription. A hub's endpoints that are not our lamps report
 * too, and are ignored; a wildcard costs less than a path per lamp would. */
CHIP_ERROR start_sub(node_t & n)
{
    const size_t count = n.colour ? 3 : 2;
    AttributePathParams *paths = new (std::nothrow) AttributePathParams[count];
    VerifyOrReturnError(paths, CHIP_ERROR_NO_MEMORY);
    paths[0] = AttributePathParams(OnOff::Id, OnOff::Attributes::OnOff::Id);
    paths[1] = AttributePathParams(LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id);
    if (n.colour) {
        paths[2] = AttributePathParams(ColorControl::Id, kInvalidAttributeId);
    }

    NodeSub *sub = Platform::New<NodeSub>(node_index(&n), n.gen);
    if (!sub) {
        delete[] paths;
        return CHIP_ERROR_NO_MEMORY;
    }
    sub->client = Platform::New<ReadClient>(InteractionModelEngine::GetInstance(),
                                            &Server::GetInstance().GetExchangeManager(), *sub,
                                            ReadClient::InteractionType::Subscribe);
    if (!sub->client) {
        Platform::Delete(sub);
        delete[] paths;
        return CHIP_ERROR_NO_MEMORY;
    }
    ReadPrepareParams params;
    params.mpAttributePathParamsList    = paths;
    params.mAttributePathParamsListSize = count;
    params.mMinIntervalFloorSeconds     = 0;
    params.mMaxIntervalCeilingSeconds   = kMaxIntervalS;
    /* false: the device drops whatever an earlier boot of ours left subscribed. */
    params.mKeepSubscriptions           = false;
    n.sub         = sub;
    n.sub_pending = true;
    /* The ReadClient sets CASE up itself, and again on every resubscription. On
     * failure it has already handed the paths back through OnDeallocatePaths. */
    CHIP_ERROR err = sub->client->SendAutoResubscribeRequest(ScopedNodeId(n.node_id, mctl::fabric_index()),
                                                             std::move(params));
    if (err != CHIP_NO_ERROR) {
        stop_sub(n);
    }
    return err;
}

/* -------------------------------------------------------------- sessions */

void rescan_now(node_t & n);

void conn_ok(void * ctx, Messaging::ExchangeManager &, const SessionHandle & session)
{
    node_t & n   = *static_cast<node_t *>(ctx);
    n.connecting = false;
    n.session.Grab(session);
    /* The lamp is back before the subscription's own retry: bring that forward. */
    if (n.sub && n.sub->client && !n.up) {
        n.sub->client->TriggerResubscribeIfScheduled("session up");
    }
    if (n.rescan) {
        n.rescan = false;
        rescan_now(n);
    }
    kick();
}

void conn_fail(void * ctx, const ScopedNodeId &, CHIP_ERROR err)
{
    node_t & n        = *static_cast<node_t *>(ctx);
    n.connecting      = false;
    n.rescan          = false;
    n.next_connect_us = now_us() + kConnectRetryUs;
    note_error(n, err);
    ESP_LOGW(TAG, "No session to node 0x%016" PRIx64 ": %" CHIP_ERROR_FORMAT, n.node_id, err.Format());
}

bool session_ready(const node_t & n)
{
    return n.session && n.session->IsActiveSession();
}

void connect(node_t & n)
{
    if (n.connecting || now_us() < n.next_connect_us || mctl::fabric_index() == kUndefinedFabricIndex) {
        return;
    }
    n.connecting = true;
    heap_watch_mark("mctl_connect", (uint32_t)n.node_id);
    Server::GetInstance().GetCASESessionManager()->FindOrEstablishSession(ScopedNodeId(n.node_id, mctl::fabric_index()),
                                                                          &n.on_conn, &n.on_fail);
}

/* -------------------------------------------------------------- commands */

void answered(int li, uint32_t gen, bool gated, uint8_t kind, CHIP_ERROR err);

template <typename Req>
CHIP_ERROR invoke(int li, const SessionHandle & session, const Req & req, bool gated, uint8_t kind)
{
    const lamp_t & l   = s_lamps[li];
    const uint32_t gen = l.gen;
    return Controller::InvokeCommandRequest(
        &Server::GetInstance().GetExchangeManager(), session, l.ep, req,
        [li, gen, gated, kind](const ConcreteCommandPath &, const StatusIB &, const typename Req::ResponseType &) {
            answered(li, gen, gated, kind, CHIP_NO_ERROR);
        },
        [li, gen, gated, kind](CHIP_ERROR e) { answered(li, gen, gated, kind, e); });
}

CHIP_ERROR send_off(int li, const SessionHandle & s, bool plain, uint8_t kind)
{
    if (plain || s_lamps[li].no_off_effect) {
        return invoke(li, s, OnOff::Commands::Off::Type(), true, kind);
    }
    /* Effect 0, variant 0: the lamp fades out over 0.8 s and switches off, one
     * command - the same as zb_fade_off, and never a ramp to level 0. */
    OnOff::Commands::OffWithEffect::Type req;
    req.effectIdentifier = OnOff::EffectIdentifierEnum::kDelayedAllOff;
    req.effectVariant    = 0;
    return invoke(li, s, req, true, kind);
}

CHIP_ERROR send_level(int li, const SessionHandle & s, uint8_t level, uint16_t ds)
{
    /* WithOnOff, as zb_set_level: a level from dark switches the lamp on. */
    LevelControl::Commands::MoveToLevelWithOnOff::Type req;
    req.level = level;
    req.transitionTime.SetNonNull(ds);
    return invoke(li, s, req, true, MCTL_CMD_LEVEL);
}

CHIP_ERROR send_ct(int li, const SessionHandle & s, uint16_t mireds, uint16_t ds)
{
    ColorControl::Commands::MoveToColorTemperature::Type req;
    req.colorTemperatureMireds = mireds;
    req.transitionTime         = ds;
    return invoke(li, s, req, true, MCTL_CMD_CT);
}

CHIP_ERROR send_cmd(int li, const mctl_cmd_t & c, const SessionHandle & s)
{
    switch (c.kind) {
    case MCTL_CMD_ONOFF:
        switch ((zb_onoff_cmd_t)c.a) {
        case ZB_ONOFF_ON:
            return invoke(li, s, OnOff::Commands::On::Type(), true, c.kind);
        case ZB_ONOFF_OFF:
            return invoke(li, s, OnOff::Commands::Off::Type(), true, c.kind);
        default:
            return invoke(li, s, OnOff::Commands::Toggle::Type(), true, c.kind);
        }
    case MCTL_CMD_FADE_OFF:
        return send_off(li, s, false, c.kind);
    case MCTL_CMD_LEVEL:
        return send_level(li, s, (uint8_t)c.a, c.transition_ds);
    case MCTL_CMD_FADE_IN:
        /* The bottom of the range at once, then the ramp: see zb_fade_in. */
        return c.step == 0 ? send_level(li, s, 1, 0) : send_level(li, s, (uint8_t)c.a, c.transition_ds);
    case MCTL_CMD_CT:
        return send_ct(li, s, c.a, c.transition_ds);
    case MCTL_CMD_WARM_OFF:
        /* Transition 0 on the colour: the off follows as soon as it is answered,
         * which would cut a transition short. See zb_warm_off. */
        return c.step == 0 ? send_ct(li, s, c.a, 0) : send_off(li, s, c.b != 0, c.kind);
    case MCTL_CMD_HS: {
        ColorControl::Commands::MoveToHueAndSaturation::Type req;
        req.hue            = (uint8_t)c.a;
        req.saturation     = (uint8_t)c.b;
        req.transitionTime = c.transition_ds;
        return invoke(li, s, req, true, c.kind);
    }
    case MCTL_CMD_XY: {
        ColorControl::Commands::MoveToColor::Type req;
        req.colorX         = c.a;
        req.colorY         = c.b;
        req.transitionTime = c.transition_ds;
        return invoke(li, s, req, true, c.kind);
    }
    default:
        return CHIP_ERROR_INVALID_ARGUMENT;
    }
}

bool is_unsupported_command(CHIP_ERROR err)
{
    return StatusIB(err).mStatus == Protocols::InteractionModel::Status::UnsupportedCommand;
}

void answered(int li, uint32_t gen, bool gated, uint8_t kind, CHIP_ERROR err)
{
    lamp_t & l = s_lamps[li];
    if (!l.used || l.gen != gen) {
        return;      /* removed meanwhile */
    }
    portENTER_CRITICAL(&s_mux);
    if (err == CHIP_NO_ERROR) {
        s_stats.sent++;
    } else {
        s_stats.failed++;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!gated) {
        return;
    }
    node_t & n = s_nodes[l.node];
    if (err != CHIP_NO_ERROR) {
        note_error(n, err);
        ESP_LOGW(TAG, "Lamp %u: command %u failed: %" CHIP_ERROR_FORMAT, l.id, kind, err.Format());
        if (err == CHIP_ERROR_TIMEOUT) {
            /* Nothing came back over this session; the next command sets a new
             * one up rather than retransmitting into it. */
            n.session.Release();
        }
    }

    const int64_t now = now_us();
    portENTER_CRITICAL(&s_mux);
    const bool off_step = (l.gate.inflight.kind == MCTL_CMD_FADE_OFF) ||
                          (l.gate.inflight.kind == MCTL_CMD_WARM_OFF && l.gate.inflight.step == 1 &&
                           l.gate.inflight.b == 0);
    if (err != CHIP_NO_ERROR && is_unsupported_command(err) && off_step && !l.no_off_effect) {
        /* A lamp without the Lighting feature refuses OffWithEffect. It gets a
         * plain Off now, and from then on - switching off is what was asked for. */
        l.no_off_effect      = true;
        l.gate.inflight.kind = MCTL_CMD_ONOFF;
        l.gate.inflight.a    = ZB_ONOFF_OFF;
        l.gate.inflight.step = 0;
        l.gate.sent          = false;
        l.gate.since_us      = now;
    } else {
        mctl_gate_done(&l.gate, now);
    }
    portEXIT_CRITICAL(&s_mux);
    kick();
}

/* ------------------------------------------------------------ the service */

bool network_ready(int64_t now)
{
    wifi_mgr_status_t wifi;
    wifi_manager_get_status(&wifi);
    if (!wifi.connected) {
        s_net_since_us = 0;
        return false;
    }
    if (s_net_since_us == 0) {
        s_net_since_us = now;
    }
    return now - s_net_since_us >= kNetSettleUs;
}

/* A device that has never been up and is not being tried yet - Wi-Fi not settled,
 * or its turn among kConnectAtOnce not come - is still "connecting" as
 * mctl_refuses means it: its half minute counts from when it is first tried, not
 * from when its row was loaded, or a slow association at boot would have its
 * lamps refused before anything was ever sent to them. */
void hold_untried(int64_t now)
{
    portENTER_CRITICAL(&s_mux);
    for (auto & n : s_nodes) {
        if (n.used && !n.sub && !n.up && n.last_seen_us == 0) {
            n.down_since_us = now;
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

/* Everything that sends, on the CHIP task: subscriptions to start, sessions to set
 * up, commands to put on air. Run from the tick and from every kick. */
void service()
{
    const int64_t now = now_us();
    if (mctl::fabric_index() == kUndefinedFabricIndex || !network_ready(now)) {
        /* Commands wait in their gates meanwhile, and go stale there as they
         * would waiting for a session. */
        hold_untried(now);
        return;
    }

    /* Subscriptions, a few at a time. */
    int pending = 0;
    for (auto & n : s_nodes) {
        if (n.used && n.sub && n.sub->dead) {
            stop_sub(n);
        }
        pending += n.used && n.sub_pending;
    }
    for (auto & n : s_nodes) {
        if (pending >= kConnectAtOnce) {
            break;
        }
        if (n.used && !n.sub && n.lamps > 0) {
            CHIP_ERROR err = start_sub(n);
            if (err == CHIP_NO_ERROR) {
                pending++;
            } else {
                note_error(n, err);
            }
        }
    }
    /* Those still waiting for their turn. */
    hold_untried(now);

    int dropped = 0, timed_out = 0;
    for (int li = 0; li < kLamps; li++) {
        lamp_t & l = s_lamps[li];
        if (!l.used) {
            continue;
        }
        node_t &   n     = s_nodes[l.node];
        const bool ready = session_ready(n);
        mctl_cmd_t c;
        bool       stop;
        bool       go;
        bool       waiting;
        portENTER_CRITICAL(&s_mux);
        stop = ready && l.stop_wanted;
        if (stop) {
            l.stop_wanted = false;
        }
        go      = mctl_gate_next(&l.gate, now, ready, &c, &dropped, &timed_out);
        waiting = l.stop_wanted || (l.gate.busy && !l.gate.sent);
        portEXIT_CRITICAL(&s_mux);

        if (!ready) {
            if (waiting) {
                connect(n);
            }
            continue;
        }
        const auto            held = n.session.Get();
        const SessionHandle & s    = held.Value();
        if (stop) {
            /* Ungated, as zb_level_stop is: gated, the level command that follows
             * would replace it and no Stop would ever go. */
            CHIP_ERROR err = invoke(li, s, LevelControl::Commands::Stop::Type(), false, MCTL_CMD_NONE);
            if (err != CHIP_NO_ERROR) {
                ESP_LOGW(TAG, "Lamp %u: Stop not sent: %" CHIP_ERROR_FORMAT, l.id, err.Format());
            }
        }
        if (go) {
            CHIP_ERROR err = send_cmd(li, c, s);
            if (err != CHIP_NO_ERROR) {
                /* Not sent at all, so no answer is coming: open the gate here. */
                answered(li, l.gen, true, c.kind, err);
            }
        }
    }
    if (dropped || timed_out) {
        portENTER_CRITICAL(&s_mux);
        s_stats.dropped += dropped;
        s_stats.timeouts += timed_out;
        portEXIT_CRITICAL(&s_mux);
        if (dropped) {
            ESP_LOGW(TAG, "%d command(s) dropped: no session to the lamp within %lld s", dropped,
                     (long long)(MCTL_STALE_US / 1000000));
        }
    }
}

void kick_work(intptr_t)
{
    __atomic_store_n(&s_kick_posted, false, __ATOMIC_RELEASE);
    service();
}

/* Any task. One wake-up however many commands arrive before it runs. */
void kick()
{
    if (__atomic_exchange_n(&s_kick_posted, true, __ATOMIC_ACQ_REL)) {
        return;
    }
    if (DeviceLayer::PlatformMgr().ScheduleWork(kick_work, 0) != CHIP_NO_ERROR) {
        __atomic_store_n(&s_kick_posted, false, __ATOMIC_RELEASE);   /* the tick picks it up */
    }
}

void tick_cb(System::Layer *, void *)
{
    s_chip_task = xTaskGetCurrentTaskHandle();
    service();
    DeviceLayer::SystemLayer().StartTimer(System::Clock::Milliseconds32(kTickMs), tick_cb, nullptr);
}

/* ------------------------------------------------------------- the tables */

void clear_state(lamp_t & l)
{
    l.on_off = ZBC_UNKNOWN;
    l.level  = ZBC_UNKNOWN;
    l.ct     = ZBC_UNKNOWN;
    l.hue    = ZBC_UNKNOWN;
    l.sat    = ZBC_UNKNOWN;
    l.x      = ZBC_UNKNOWN;
    l.y      = ZBC_UNKNOWN;
    l.mode   = ZBC_UNKNOWN;
}

void drop_node(node_t & n)
{
    stop_sub(n);
    n.on_conn.Cancel();
    n.on_fail.Cancel();
    n.session.Release();
    n.connecting = false;
    n.rescan     = false;
    n.gen++;
    portENTER_CRITICAL(&s_mux);
    n.used = false;
    n.up   = false;
    portEXIT_CRITICAL(&s_mux);
}

int node_for(uint64_t node_id, uint8_t lamp_fabric)
{
    int spare = -1;
    for (int i = 0; i < kNodes; i++) {
        if (s_nodes[i].used && s_nodes[i].node_id == node_id) {
            return i;
        }
        if (spare < 0 && !s_nodes[i].used) {
            spare = i;
        }
    }
    if (spare < 0) {
        return -1;
    }
    node_t & n = s_nodes[spare];
    stop_sub(n);
    n.session.Release();
    n.gen++;
    n.connecting      = false;
    n.rescan          = false;
    n.retries         = 0;
    n.next_connect_us = 0;
    n.lamps           = 0;
    n.colour          = false;
    portENTER_CRITICAL(&s_mux);
    n.used          = true;
    n.node_id       = node_id;
    n.lamp_fabric   = lamp_fabric;
    n.up            = false;
    n.down_since_us = now_us();   /* connecting: see mctl_refuses */
    n.last_seen_us  = 0;
    n.drops         = 0;
    n.reports       = 0;
    n.last_error    = 0;
    portEXIT_CRITICAL(&s_mux);
    return spare;
}

void rescan_done(NodeId node, CHIP_ERROR err, int lamps)
{
    if (err != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "Rescan of node 0x%016" PRIx64 " failed: %" CHIP_ERROR_FORMAT, node, err.Format());
        return;
    }
    ESP_LOGI(TAG, "Rescan of node 0x%016" PRIx64 ": %d lamp(s)", node, lamps);
    mctl::ctl_reload_now();
}

void rescan_now(node_t & n)
{
    auto session = n.session.Get();
    CHIP_ERROR err = session.HasValue() ? mctl::discover_start(n.node_id, n.lamp_fabric, session.Value(), rescan_done)
                                        : CHIP_ERROR_INCORRECT_STATE;
    if (err != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "Rescan of node 0x%016" PRIx64 " did not start: %" CHIP_ERROR_FORMAT, n.node_id, err.Format());
    }
}

} // namespace

namespace mctl {

void ctl_reload_now()
{
    zbc_mlight_t *rows = static_cast<zbc_mlight_t *>(Platform::MemoryCalloc(kLamps, sizeof(zbc_mlight_t)));
    if (!rows) {
        ESP_LOGE(TAG, "No memory to reload the Matter lamps");
        return;
    }
    const size_t count = mlight_db_snapshot(rows, kLamps);

    /* Devices first: one per node id that still has rows. */
    bool    keep[kNodes]       = {};
    uint8_t lamps[kNodes]      = {};
    bool    colour[kNodes]     = {};
    int     node_of[kLamps];
    for (size_t r = 0; r < count; r++) {
        const int ni = node_for(rows[r].node_id, rows[r].lamp_fabric);
        node_of[r]   = ni;
        if (ni < 0) {
            ESP_LOGW(TAG, "Too many Matter devices: lamp %u is left out", rows[r].id);
            continue;
        }
        keep[ni] = true;
        lamps[ni]++;
        colour[ni] |= (rows[r].features & (ZBC_FEAT_ANY_COLOR | ZBC_FEAT_COLOR_TEMP)) != 0;
    }
    for (int i = 0; i < kNodes; i++) {
        node_t & n = s_nodes[i];
        if (n.used && !keep[i]) {
            drop_node(n);
            continue;
        }
        if (n.used && (n.lamps != lamps[i] || n.colour != colour[i])) {
            /* Lamps came or went (a rescan found more behind a hub): subscribe
             * again, so the new ones get a first report. */
            stop_sub(n);
            n.lamps  = lamps[i];
            n.colour = colour[i];
        }
    }

    /* Then the lamps: a lamp that stays keeps its state and its gate. */
    portENTER_CRITICAL(&s_mux);
    for (auto & l : s_lamps) {
        bool still = false;
        for (size_t r = 0; r < count && !still; r++) {
            still = l.used && rows[r].id == l.id && node_of[r] >= 0 && node_of[r] == l.node &&
                    rows[r].remote_ep == l.ep;
        }
        if (l.used && !still) {
            l.used = false;
            l.gen++;
        }
    }
    for (size_t r = 0; r < count; r++) {
        if (node_of[r] < 0) {
            continue;
        }
        lamp_t *slot = nullptr, *spare = nullptr;
        for (auto & l : s_lamps) {
            if (l.used && l.id == rows[r].id) {
                slot = &l;
                break;
            }
            if (!spare && !l.used) {
                spare = &l;
            }
        }
        if (!slot && spare) {
            slot = spare;
            const uint32_t gen = slot->gen + 1;
            memset(slot, 0, sizeof(*slot));
            slot->gen = gen;
            clear_state(*slot);
            slot->used = true;
            slot->id   = rows[r].id;
            slot->node = (uint8_t)node_of[r];
            slot->ep   = rows[r].remote_ep;
        }
        if (slot) {
            slot->features = rows[r].features;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    Platform::MemoryFree(rows);
    zbc_state_bump();
    kick();
}

} // namespace mctl

/* ------------------------------------------------------ leaving a device */

namespace {

/* RemoveFabric, to a device that is being forgotten: its rows are gone, so it has
 * no node slot any more, and this holds what it needs until the answer. */
struct leaver_t {
    leaver_t() : on_conn(leave_conn, this), on_fail(leave_fail, this) {}
    bool     used = false;
    uint64_t node_id = 0;
    uint8_t  lamp_fabric = 0;
    int64_t  since_us = 0;
    Callback::Callback<OnDeviceConnected>         on_conn;
    Callback::Callback<OnDeviceConnectionFailure> on_fail;

    static void leave_conn(void * ctx, Messaging::ExchangeManager &, const SessionHandle & session);
    static void leave_fail(void * ctx, const ScopedNodeId &, CHIP_ERROR err);
};

leaver_t s_leavers[kNodes];
/* How many RemoveFabrics mctl_forget_all is still waiting for. */
volatile int s_leaving;

void leaver_done(leaver_t & lv)
{
    if (lv.used) {
        lv.used = false;
        __atomic_sub_fetch(&s_leaving, 1, __ATOMIC_ACQ_REL);
    }
}

void leaver_t::leave_conn(void * ctx, Messaging::ExchangeManager &, const SessionHandle & session)
{
    leaver_t & lv = *static_cast<leaver_t *>(ctx);
    OperationalCredentials::Commands::RemoveFabric::Type req;
    req.fabricIndex = lv.lamp_fabric;
    const uint64_t node = lv.node_id;
    /* The answer may never come: the device drops the session as it forgets us.
     * Either way it is done with. */
    CHIP_ERROR err = Controller::InvokeCommandRequest(
        &Server::GetInstance().GetExchangeManager(), session, kRootEndpointId, req,
        [&lv, node](const ConcreteCommandPath &, const StatusIB &,
                    const OperationalCredentials::Commands::NOCResponse::DecodableType &) {
            ESP_LOGI(TAG, "Node 0x%016" PRIx64 " forgot us", node);
            leaver_done(lv);
        },
        [&lv, node](CHIP_ERROR e) {
            ESP_LOGI(TAG, "Node 0x%016" PRIx64 ": RemoveFabric ended with %" CHIP_ERROR_FORMAT, node, e.Format());
            leaver_done(lv);
        });
    if (err != CHIP_NO_ERROR) {
        leaver_done(lv);
    }
}

void leaver_t::leave_fail(void * ctx, const ScopedNodeId &, CHIP_ERROR err)
{
    leaver_t & lv = *static_cast<leaver_t *>(ctx);
    ESP_LOGW(TAG, "Node 0x%016" PRIx64 " not reachable to forget us (%" CHIP_ERROR_FORMAT
                  "); remove the lamp in its own app too",
             lv.node_id, err.Format());
    leaver_done(lv);
}

void leave(uint64_t node_id, uint8_t lamp_fabric)
{
    if (lamp_fabric == 0 || mctl::fabric_index() == kUndefinedFabricIndex) {
        return;
    }
    for (auto & lv : s_leavers) {
        if (!lv.used) {
            lv.used        = true;
            lv.node_id     = node_id;
            lv.lamp_fabric = lamp_fabric;
            lv.since_us    = now_us();
            __atomic_add_fetch(&s_leaving, 1, __ATOMIC_ACQ_REL);
            Server::GetInstance().GetCASESessionManager()->FindOrEstablishSession(
                ScopedNodeId(node_id, mctl::fabric_index()), &lv.on_conn, &lv.on_fail);
            return;
        }
    }
    ESP_LOGW(TAG, "Node 0x%016" PRIx64 " not told to forget us: too many at once", node_id);
}

struct leave_req_t {
    uint64_t node_id;
    uint8_t  lamp_fabric;
};

void leave_work(intptr_t arg)
{
    leave_req_t *req = reinterpret_cast<leave_req_t *>(arg);
    leave(req->node_id, req->lamp_fabric);
    Platform::Delete(req);
}

void forget_all_work(intptr_t)
{
    for (auto & n : s_nodes) {
        if (n.used) {
            leave(n.node_id, n.lamp_fabric);
        }
    }
}

void reload_work(intptr_t)
{
    mctl::ctl_reload_now();
}

struct rescan_req_t {
    uint64_t node_id;
};

void rescan_work(intptr_t arg)
{
    rescan_req_t *req = reinterpret_cast<rescan_req_t *>(arg);
    for (auto & n : s_nodes) {
        if (n.used && n.node_id == req->node_id) {
            if (session_ready(n)) {
                rescan_now(n);
            } else {
                n.rescan          = true;
                n.next_connect_us = 0;
                connect(n);
            }
        }
    }
    Platform::Delete(req);
}

/* ------------------------------------------------------------- the C API */

esp_err_t submit(uint16_t id, mctl_cmd_t c, uint16_t needs)
{
    if (!s_running || mctl::fabric_index() == kUndefinedFabricIndex) {
        return ESP_ERR_INVALID_STATE;
    }
    const int64_t now = now_us();
    esp_err_t     err = ESP_ERR_NOT_FOUND;
    portENTER_CRITICAL(&s_mux);
    for (auto & l : s_lamps) {
        if (!l.used || l.id != id) {
            continue;
        }
        const node_t & n = s_nodes[l.node];
        if (needs && !(l.features & needs)) {
            err = ESP_ERR_NOT_SUPPORTED;
        } else if (mctl_refuses(n.up, n.down_since_us, now)) {
            err = ZB_ERR_UNREACHABLE;
        } else {
            mctl_gate_admit(&l.gate, &c, now);
            err = ESP_OK;
        }
        break;
    }
    portEXIT_CRITICAL(&s_mux);
    if (err == ESP_OK) {
        kick();
    }
    return err;
}

mctl_cmd_t cmd(uint8_t kind, uint16_t a = 0, uint16_t b = 0, uint16_t ds = 0)
{
    mctl_cmd_t c    = {};
    c.kind          = kind;
    c.a             = a;
    c.b             = b;
    c.transition_ds = ds;
    return c;
}

/* A lamp's colour features, read without the row. */
uint16_t features_of(uint16_t id)
{
    uint16_t f = 0;
    portENTER_CRITICAL(&s_mux);
    for (const auto & l : s_lamps) {
        if (l.used && l.id == id) {
            f = l.features;
            break;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return f;
}

} // namespace

extern "C" {

esp_err_t mctl_start(void)
{
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        mctl::fabric_load();
        s_running = true;
        mctl::ctl_reload_now();
        DeviceLayer::SystemLayer().StartTimer(System::Clock::Milliseconds32(kTickMs), tick_cb, nullptr);
    }
    ESP_LOGI(TAG, "Matter lamps: %u stored", (unsigned)mlight_db_count());
    return ESP_OK;
}

bool mctl_running(void)
{
    return s_running;
}

void mctl_reload(void)
{
    if (s_running && DeviceLayer::PlatformMgr().ScheduleWork(reload_work, 0) != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "Reload not scheduled; the lamp table is stale until the next change");
    }
}

esp_err_t mctl_set_onoff(uint16_t id, zb_onoff_cmd_t c)
{
    return submit(id, cmd(MCTL_CMD_ONOFF, (uint16_t)c), 0);
}

esp_err_t mctl_set_level(uint16_t id, uint8_t level, uint16_t transition_ds)
{
    return submit(id, cmd(MCTL_CMD_LEVEL, level < 1 ? 1 : level, 0, transition_ds), ZBC_FEAT_LEVEL);
}

esp_err_t mctl_fade_in(uint16_t id, uint8_t level, uint16_t transition_ds)
{
    return submit(id, cmd(MCTL_CMD_FADE_IN, level < 1 ? 1 : level, 0, transition_ds), ZBC_FEAT_LEVEL);
}

esp_err_t mctl_fade_off(uint16_t id)
{
    return submit(id, cmd(MCTL_CMD_FADE_OFF), 0);
}

esp_err_t mctl_warm_off(uint16_t id, uint16_t mireds, bool plain_off)
{
    return submit(id, cmd(MCTL_CMD_WARM_OFF, mireds, plain_off ? 1 : 0), ZBC_FEAT_COLOR_TEMP);
}

esp_err_t mctl_set_ct(uint16_t id, uint16_t mireds, uint16_t transition_ds)
{
    return submit(id, cmd(MCTL_CMD_CT, mireds, 0, transition_ds), ZBC_FEAT_COLOR_TEMP);
}

/* Hue and saturation as 0-254; a lamp that only speaks xy gets the same colour
 * converted, as zb_set_color_hs does. */
esp_err_t mctl_set_hs(uint16_t id, uint8_t hue, uint8_t saturation, uint16_t transition_ds)
{
    const uint16_t f = features_of(id);
    if (!(f & ZBC_FEAT_ANY_COLOR)) {
        return f ? ESP_ERR_NOT_SUPPORTED : submit(id, cmd(MCTL_CMD_HS), ZBC_FEAT_ANY_COLOR);
    }
    if (f & ZBC_FEAT_COLOR_HS) {
        return submit(id, cmd(MCTL_CMD_HS, hue, saturation, transition_ds), ZBC_FEAT_ANY_COLOR);
    }
    float x, y;
    zbc_hs_to_xy((int)lroundf((float)hue * 360.0f / 254.0f), (int)lroundf((float)saturation * 100.0f / 254.0f), &x, &y);
    return submit(id, cmd(MCTL_CMD_XY, (uint16_t)(x * 65535.0f), (uint16_t)(y * 65535.0f), transition_ds),
                  ZBC_FEAT_ANY_COLOR);
}

/* The mirror, for a lamp that only speaks hue and saturation. */
esp_err_t mctl_set_xy(uint16_t id, uint16_t x, uint16_t y, uint16_t transition_ds)
{
    const uint16_t f = features_of(id);
    if (!(f & ZBC_FEAT_ANY_COLOR)) {
        return f ? ESP_ERR_NOT_SUPPORTED : submit(id, cmd(MCTL_CMD_XY), ZBC_FEAT_ANY_COLOR);
    }
    if (f & ZBC_FEAT_COLOR_XY) {
        return submit(id, cmd(MCTL_CMD_XY, x, y, transition_ds), ZBC_FEAT_ANY_COLOR);
    }
    float h, s;
    zbc_xy_to_hs((float)x / 65535.0f, (float)y / 65535.0f, &h, &s);
    return submit(id, cmd(MCTL_CMD_HS, (uint16_t)lroundf(h * 254.0f / 360.0f), (uint16_t)lroundf(s * 254.0f / 100.0f),
                          transition_ds),
                  ZBC_FEAT_ANY_COLOR);
}

esp_err_t mctl_level_stop(uint16_t id)
{
    if (!s_running || mctl::fabric_index() == kUndefinedFabricIndex) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    portENTER_CRITICAL(&s_mux);
    for (auto & l : s_lamps) {
        if (l.used && l.id == id) {
            const node_t & n = s_nodes[l.node];
            if (!(l.features & ZBC_FEAT_LEVEL)) {
                err = ESP_ERR_NOT_SUPPORTED;
            } else if (mctl_refuses(n.up, n.down_since_us, now_us())) {
                err = ZB_ERR_UNREACHABLE;
            } else {
                l.stop_wanted = true;
                err           = ESP_OK;
            }
            break;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    if (err == ESP_OK) {
        kick();
    }
    return err;
}

bool mctl_get(uint16_t id, mctl_state_t *out)
{
    bool   found = false;
    lamp_t copy;
    node_t *node = nullptr;
    bool    up   = false;
    int64_t seen = 0;
    portENTER_CRITICAL(&s_mux);
    for (const auto & l : s_lamps) {
        if (l.used && l.id == id) {
            copy  = l;
            node  = &s_nodes[l.node];
            up    = node->up;
            seen  = node->last_seen_us;
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    if (!found || !out) {
        return found;
    }
    out->on_off       = copy.on_off;
    out->level        = copy.level;
    out->ct_mireds    = copy.ct;
    out->hue          = copy.hue;
    out->saturation   = copy.sat;
    out->color_mode   = copy.mode;
    out->online       = up;
    out->last_seen_us = seen;
    /* A lamp in xy mode shows its colour as hue and saturation, which is what the
     * page draws; the conversion is the one a command in the other direction uses. */
    if (copy.mode == 1 && copy.x != ZBC_UNKNOWN && copy.y != ZBC_UNKNOWN) {
        float h, s;
        zbc_xy_to_hs((float)copy.x / 65535.0f, (float)copy.y / 65535.0f, &h, &s);
        out->hue        = (int16_t)lroundf(h * 254.0f / 360.0f);
        out->saturation = (int16_t)lroundf(s * 254.0f / 100.0f);
    }
    return true;
}

bool mctl_has_fabric(void)
{
    return s_running && mctl::fabric_index() != kUndefinedFabricIndex;
}

uint8_t mctl_fabric_index(void)
{
    return s_running ? mctl::fabric_index() : 0;
}

esp_err_t mctl_rescan_node(uint64_t node_id)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mlight_db_node_rows(node_id) == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    rescan_req_t *req = Platform::New<rescan_req_t>();
    if (!req) {
        return ESP_ERR_NO_MEM;
    }
    req->node_id = node_id;
    if (DeviceLayer::PlatformMgr().ScheduleWork(rescan_work, reinterpret_cast<intptr_t>(req)) != CHIP_NO_ERROR) {
        Platform::Delete(req);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t mctl_remove_node(uint64_t node_id, uint8_t lamp_fabric)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    leave_req_t *req = Platform::New<leave_req_t>();
    if (!req) {
        return ESP_ERR_NO_MEM;
    }
    req->node_id     = node_id;
    req->lamp_fabric = lamp_fabric;
    if (DeviceLayer::PlatformMgr().ScheduleWork(leave_work, reinterpret_cast<intptr_t>(req)) != CHIP_NO_ERROR) {
        Platform::Delete(req);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void mctl_forget_all(uint32_t timeout_ms)
{
    if (!s_running || mctl::fabric_index() == kUndefinedFabricIndex) {
        return;
    }
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        forget_all_work(0);
    }
    /* Polled: the answers arrive on the CHIP task, and a semaphore given after
     * the timeout would outlive its waiter. */
    const int64_t until = now_us() + (int64_t)timeout_ms * 1000;
    while (__atomic_load_n(&s_leaving, __ATOMIC_ACQUIRE) > 0 && now_us() < until) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void mctl_get_status(mctl_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->running      = s_running;
    out->fabric_index = mctl_fabric_index();
    mctl::commission_status(out);
    portENTER_CRITICAL(&s_mux);
    out->own_node_id      = s_running ? mctl::fabric_own_node() : 0;
    strlcpy(out->fabric_error, mctl::fabric_error(), sizeof(out->fabric_error));
    out->commands_sent    = s_stats.sent;
    out->commands_failed  = s_stats.failed;
    out->commands_dropped = s_stats.dropped;
    out->gate_timeouts    = s_stats.timeouts;
    for (const auto & n : s_nodes) {
        if (!n.used || out->nodes >= MCTL_STATUS_NODES) {
            continue;
        }
        mctl_node_status_t & o = out->node[out->nodes++];
        o.node_id       = n.node_id;
        o.lamps         = n.lamps;
        o.online        = n.up;
        o.subscribed    = n.sub != nullptr;
        o.drops         = n.drops;
        o.reports       = n.reports;
        o.last_seen_us  = n.last_seen_us;
        o.down_since_us = n.up ? 0 : n.down_since_us;
        o.last_error    = n.last_error;
        o.heap_low_up   = n.heap_low_up;
    }
    portEXIT_CRITICAL(&s_mux);
    /* Outside the spinlock: it walks the stack. */
    out->chip_stack_unused = s_chip_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_chip_task) : 0;
}

} // extern "C"
