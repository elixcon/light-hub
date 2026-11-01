#include "node_zb.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_coexist.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_zigbee.h"
#include "ezbee/zcl/cluster/custom.h"
#include "ezbee/zha.h"
#include "ezbee/zdo.h"
#include "nvs.h"

#include "light_db.h"
#include "local_lights.h"
#include "node_buttons.h"
#include "node_main.h"
#include "node_proto.h"
#include "node_store.h"
#include "node_zcl.h"
#include "radio_coex.h"

static const char *TAG = "node_zb";

#define NODE_STORAGE_PARTITION "nvs"
/* The namespace esp_zigbee_plat_datasets keeps the stack's data in. Its own reset
 * is nvs_erase_all on this handle, followed by a restart we cannot afford on a
 * trial image: a restart there is a rollback. */
#define NODE_STORAGE_NAMESPACE "zigbee"

/* ZCL strings, length first. */
#define NODE_MANUFACTURER "\x07""Elixcon"
#define NODE_MODEL        "\x09""LightNode"

#define ALL_CHANNELS 0x07FFF800u
/* How long the parent keeps a silent child, and how often it is told otherwise.
 * The receiver is on, so nothing has to be fetched: this is only the keep-alive. */
#define ED_TIMEOUT    EZB_NWK_ED_TIMEOUT_64MIN
#define KEEP_ALIVE_MS (30 * 1000)
#define JOIN_RETRY_US (30 * 1000000LL)
#define INIT_RETRY_US (1 * 1000000LL)
#define REJOIN_RETRY_US (10 * 1000000LL)
/* A frame the stack's retries bring round again within this long is the same
 * frame. A repeated Toggle would otherwise undo itself. */
#define DUP_WINDOW_US (1000 * 1000)

typedef enum {
    EV_SYNC = 0,      /* the attributes should follow the lights */
    EV_COMMISSION,    /* start commissioning, s_commission_mode */
    EV_COMMAND,
    EV_ENTER_WIFI,    /* the hub asked: cmd.level holds the minutes */
    EV_LEFT,          /* left the network for good */
    EV_ANNOUNCE,      /* the endpoints changed since the hub last looked */
} ev_type_t;

typedef struct {
    uint8_t        type;
    uint8_t        dst_ep;
    uint8_t        src_ep;
    uint8_t        tsn;
    uint8_t        cmd_id;
    bool           want_rsp;
    uint16_t       src;
    uint16_t       cluster;
    node_zcl_cmd_t cmd;
} node_ev_t;

static QueueHandle_t      s_queue;
static esp_timer_handle_t s_commission_timer;
static volatile uint8_t   s_commission_mode;
static bool               s_sync_wanted;
static bool               s_started;
static bool               s_with_wifi;
static volatile int64_t   s_joined_at;
static uint32_t           s_endpoints_digest;

static node_zb_stats_t    s_stats;
static portMUX_TYPE       s_log_mux = portMUX_INITIALIZER_UNLOCKED;
static node_zb_frame_t    s_frames[16];
static int                s_frames_n;
static node_zb_signal_t   s_signals[12];
static int                s_signals_n;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void note_signal(const char *name, int status)
{
    node_zb_signal_t e = { .at_ms = now_ms(), .status = status };
    strlcpy(e.name, name, sizeof(e.name));
    portENTER_CRITICAL(&s_log_mux);
    if (s_signals_n == (int)(sizeof(s_signals) / sizeof(s_signals[0]))) {
        memmove(&s_signals[0], &s_signals[1], sizeof(s_signals) - sizeof(s_signals[0]));
        s_signals_n--;
    }
    s_signals[s_signals_n++] = e;
    portEXIT_CRITICAL(&s_log_mux);
}

static void note_frame(const ezb_zcl_cmd_hdr_t *h, uint16_t src, bool handled)
{
    const node_zb_frame_t e = {
        .at_ms = now_ms(), .src = src, .cluster = h->cluster_id, .dst_ep = h->dst_ep,
        .cmd = h->cmd_id, .fc = h->fc, .handled = handled,
    };
    portENTER_CRITICAL(&s_log_mux);
    if (s_frames_n == (int)(sizeof(s_frames) / sizeof(s_frames[0]))) {
        memmove(&s_frames[0], &s_frames[1], sizeof(s_frames) - sizeof(s_frames[0]));
        s_frames_n--;
    }
    s_frames[s_frames_n++] = e;
    portEXIT_CRITICAL(&s_log_mux);
}

int node_zb_frames(node_zb_frame_t *out, int max)
{
    portENTER_CRITICAL(&s_log_mux);
    const int n = s_frames_n < max ? s_frames_n : max;
    memcpy(out, &s_frames[s_frames_n - n], (size_t)n * sizeof(*out));
    portEXIT_CRITICAL(&s_log_mux);
    return n;
}

int node_zb_signals(node_zb_signal_t *out, int max)
{
    portENTER_CRITICAL(&s_log_mux);
    const int n = s_signals_n < max ? s_signals_n : max;
    memcpy(out, &s_signals[s_signals_n - n], (size_t)n * sizeof(*out));
    portEXIT_CRITICAL(&s_log_mux);
    return n;
}

void node_zb_stats(node_zb_stats_t *out)
{
    *out = s_stats;
    out->started = s_started;
    if (s_started && esp_zigbee_lock_acquire(pdMS_TO_TICKS(200))) {
        out->joined     = ezb_bdb_dev_joined();
        out->rx_on      = ezb_get_rx_on_when_idle();
        out->short_addr = ezb_nwk_get_short_address();
        out->pan_id     = ezb_nwk_get_panid();
        out->channel    = ezb_nwk_get_current_channel();
        out->parent_short = 0xFFFF;
        ezb_nwk_info_iterator_t it = EZB_NWK_INFO_ITERATOR_INIT;
        ezb_nwk_neighbor_info_t nb;
        while (ezb_nwk_get_next_neighbor(&it, &nb) == EZB_ERR_NONE) {
            out->neighbours++;
            if (nb.relationship == EZB_NWK_RELATIONSHIP_PARENT) {
                out->parent_short = nb.short_addr;
                out->parent_lqi   = nb.lqi;
                out->parent_rssi  = nb.rssi;
                out->parent_type  = nb.device_type;
                memcpy(out->parent_ieee, &nb.ieee_addr, 8);
            }
        }
        esp_zigbee_lock_release();
    }
}

esp_err_t node_zb_forget_network(void)
{
    nvs_handle_t nvs;
    esp_err_t    err = nvs_open_from_partition(NODE_STORAGE_PARTITION, NODE_STORAGE_NAMESPACE,
                                               NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(nvs);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

/* ---------------------------------------------------------- the attributes */

static void set_attr(uint8_t ep, uint16_t cluster, uint16_t attr, void *value)
{
    ezb_zcl_set_attr_value(ep, cluster, EZB_ZCL_CLUSTER_SERVER, attr, EZB_ZCL_STD_MANUF_CODE, value, false);
}

/* What the lights are doing, whatever was asked of them: a command the overheat
 * interlock refused leaves the lamp reading off, which is what the hub's settle
 * read should find (CLAUDE.md, State is never assumed). */
static void sync_attrs(void)
{
    zbc_light_t lights[CONFIG_ZBC_MAX_LIGHTS];
    const int   n = (int)light_db_snapshot(lights, CONFIG_ZBC_MAX_LIGHTS);
    for (int i = 0; i < n; i++) {
        zbc_local_state_t st;
        if (lights[i].id > NODE_LIGHT_EP_MAX || !local_light_get(lights[i].id, &st)) {
            continue;
        }
        const uint8_t ep = (uint8_t)lights[i].id;
        esp_zigbee_lock_acquire(portMAX_DELAY);
        if (st.on >= 0) {
            bool on = st.on > 0;
            set_attr(ep, EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, &on);
        }
        if (st.level >= 0) {
            uint8_t level = (uint8_t)st.level;
            set_attr(ep, EZB_ZCL_CLUSTER_ID_LEVEL, EZB_ZCL_ATTR_LEVEL_CURRENT_LEVEL_ID, &level);
        }
        if (st.mireds > 0) {
            uint16_t mireds = (uint16_t)st.mireds;
            set_attr(ep, EZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                     EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_MIREDS_ID, &mireds);
        }
        esp_zigbee_lock_release();
    }
    s_stats.attr_syncs++;
}

void node_zb_state_changed(void)
{
    if (!s_queue) {
        return;
    }
    if (!__atomic_exchange_n(&s_sync_wanted, true, __ATOMIC_ACQ_REL)) {
        const node_ev_t ev = { .type = EV_SYNC };
        if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
            /* The queue is busy with commands, each of which syncs afterwards. */
            __atomic_store_n(&s_sync_wanted, false, __ATOMIC_RELEASE);
        }
    }
}

/* ------------------------------------------------------------ the commands */

/* The fade a frame leaves to OnOffTransitionTime, which the hub writes. */
static uint16_t transition_of(uint8_t ep, uint16_t asked)
{
    if (asked != NODE_ZCL_TT_DEFAULT) {
        return asked;
    }
    uint16_t ds = 0;
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_zcl_attr_desc_t desc = ezb_zcl_get_attr_desc(ep, EZB_ZCL_CLUSTER_ID_LEVEL, EZB_ZCL_CLUSTER_SERVER,
                                                     EZB_ZCL_ATTR_LEVEL_ON_OFF_TRANSITION_TIME_ID,
                                                     EZB_ZCL_STD_MANUF_CODE);
    if (desc) {
        ezb_zcl_attr_desc_get_value(desc, &ds);
    }
    esp_zigbee_lock_release();
    return ds;
}

static esp_err_t run_command(const node_ev_t *ev)
{
    const uint16_t id = ev->dst_ep;
    zbc_light_t    light;
    if (!light_db_get(id, &light)) {
        return ESP_ERR_NOT_FOUND;
    }
    const bool dims = zbc_light_pwm_channels(light.kind) > 0;
    const uint16_t ds = transition_of(ev->dst_ep, ev->cmd.transition_ds);

    zbc_local_state_t st = { .on = -1, .level = -1, .mireds = -1 };
    local_light_get(id, &st);

    switch (ev->cmd.kind) {
    case NODE_ZCL_ON:
        if (!dims || ds == 0) {
            return local_light_set(id, ZB_ONOFF_ON);
        }
        return local_light_fade_in(id, st.level > 0 ? (uint8_t)st.level : 254, ds);
    case NODE_ZCL_OFF:
    case NODE_ZCL_OFF_EFFECT:
        if (!dims) {
            return local_light_set(id, ZB_ONOFF_OFF);
        }
        return local_light_fade_off(id, ds);
    case NODE_ZCL_TOGGLE:
        return local_light_set(id, ZB_ONOFF_TOGGLE);
    case NODE_ZCL_LEVEL:
        if (st.on <= 0) {
            return ESP_OK;   /* ZCL: without "with on/off", a lamp that is off stays so */
        }
        return local_light_set_level(id, ev->cmd.level, ds);
    case NODE_ZCL_LEVEL_ONOFF:
        return local_light_set_level(id, ev->cmd.level, ds);
    case NODE_ZCL_STOP:
        return local_light_stop(id);
    case NODE_ZCL_CT:
        return local_light_set_ct(id, ev->cmd.mireds, ds);
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static void send_default_rsp(const node_ev_t *ev, uint8_t status)
{
    ezb_zcl_default_rsp_cmd_t rsp = {
        .cmd_ctrl = {
            .dst_addr   = { .addr_mode = EZB_ADDR_MODE_SHORT, .u.short_addr = ev->src },
            .dst_ep     = ev->src_ep,
            .src_ep     = ev->dst_ep,
            .cluster_id = ev->cluster,
            .manuf_code = EZB_ZCL_STD_MANUF_CODE,
            .fc         = { .direction = 1, .dis_default_rsp = 1 },
        },
        .payload = { .tsn = ev->tsn, .rsp_to_cmd = ev->cmd_id, .status_code = status },
    };
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_zcl_default_rsp_cmd_req(&rsp);
    esp_zigbee_lock_release();
}

/* Zigbee callbacks hold the stack lock and must not block or send, so the frame
 * becomes a queue item; everything is then done on node_task. */
static bool frame_is_duplicate(uint16_t src, const ezb_zcl_cmd_hdr_t *h)
{
    static struct {
        int64_t  at;
        uint16_t src, cluster;
        uint8_t  ep, cmd, tsn;
        bool     used;
    } recent[6];
    const int64_t now = esp_timer_get_time();
    int oldest = 0;
    for (int i = 0; i < (int)(sizeof(recent) / sizeof(recent[0])); i++) {
        if (recent[i].used && recent[i].src == src && recent[i].ep == h->dst_ep &&
            recent[i].cluster == h->cluster_id && recent[i].cmd == h->cmd_id && recent[i].tsn == h->tsn &&
            now - recent[i].at < DUP_WINDOW_US) {
            return true;
        }
        if (recent[i].at < recent[oldest].at) {
            oldest = i;
        }
    }
    recent[oldest].at = now;
    recent[oldest].src = src;
    recent[oldest].cluster = h->cluster_id;
    recent[oldest].ep = h->dst_ep;
    recent[oldest].cmd = h->cmd_id;
    recent[oldest].tsn = h->tsn;
    recent[oldest].used = true;
    return false;
}

/* Taken here rather than left to the stack's cluster servers, which step a level
 * themselves: local_lights.c does the fade, at the transition the frame asked for,
 * and the attributes follow it. True drops the frame from the stack. */
static bool raw_frame(const ezb_zcl_raw_frame_t *raw)
{
    if (!raw || !raw->header) {
        return false;
    }
    const ezb_zcl_cmd_hdr_t *h = raw->header;
    const uint16_t src = h->src_addr.addr_mode == EZB_ADDR_MODE_SHORT ? h->src_addr.u.short_addr : 0xFFFF;
    s_stats.frames++;

    /* The hub, and only the hub: anything on the network can address this cluster. */
    if (h->cluster_id == NODE_PROTO_CLUSTER) {
        note_frame(h, src, true);
        if (src == 0x0000 && h->dst_ep == NODE_CONTROL_EP && h->cmd_id == NODE_CMD_ENTER_WIFI &&
            !EZB_ZCL_CMD_FC_IS_TO_CLI_DIRECTION(h->fc)) {
            const node_ev_t ev = {
                .type = EV_ENTER_WIFI,
                .cmd  = { .level = raw->payload_length >= 1 ? raw->payload[0] : 0 },
            };
            xQueueSend(s_queue, &ev, 0);
        }
        return true;
    }

    node_zcl_cmd_t cmd;
    const bool mine = src != 0xFFFF &&
                      h->dst_ep >= 1 && h->dst_ep <= NODE_LIGHT_EP_MAX &&
                      EZB_ZCL_CMD_FC_GET_FRAME_TYPE(h->fc) == EZB_ZCL_FRAME_TYPE_CLUSTER_SPECIFIC &&
                      !EZB_ZCL_CMD_FC_IS_MANUF_SPEC(h->fc) && !EZB_ZCL_CMD_FC_IS_TO_CLI_DIRECTION(h->fc) &&
                      node_zcl_decode(h->cluster_id, h->cmd_id, raw->payload, raw->payload_length, &cmd);
    note_frame(h, src, mine);
    if (!mine) {
        return false;
    }
    if (frame_is_duplicate(src, h)) {
        s_stats.duplicates++;
        return true;
    }
    const node_ev_t ev = {
        .type = EV_COMMAND, .dst_ep = h->dst_ep, .src_ep = h->src_ep, .tsn = h->tsn, .cmd_id = h->cmd_id,
        .want_rsp = !EZB_ZCL_CMD_FC_IS_DIS_DEFAULT_RSP(h->fc), .src = src, .cluster = h->cluster_id, .cmd = cmd,
    };
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        s_stats.refused++;
    }
    return true;
}

/* ---------------------------------------------------------------- the task */

static void commission_timer_cb(void *arg)
{
    (void)arg;
    const node_ev_t ev = { .type = EV_COMMISSION };
    xQueueSend(s_queue, &ev, 0);
}

static void schedule_commission(uint8_t mode, int64_t delay_us)
{
    s_commission_mode = mode;
    esp_timer_stop(s_commission_timer);
    esp_timer_start_once(s_commission_timer, (uint64_t)delay_us);
}

/* The attributes follow the lights once commands have gone quiet, not after each
 * one, so a burst costs one report. The hub sends a switch-off as two frames, a
 * colour and then OffWithEffect. SYNC_MAX_WAIT_US so a slider's stream of commands
 * still reports as it goes. */
#define SYNC_QUIET_MS    100
#define SYNC_MAX_WAIT_US (500 * 1000)

static void node_task(void *arg)
{
    (void)arg;
    int64_t sync_since = 0;   /* when the oldest unreported change happened; 0 when none */
    for (;;) {
        node_ev_t ev;
        const TickType_t wait = sync_since ? pdMS_TO_TICKS(SYNC_QUIET_MS) : portMAX_DELAY;
        const bool got = xQueueReceive(s_queue, &ev, wait) == pdTRUE;
        if (got) {
            switch (ev.type) {
            case EV_COMMISSION:
                esp_zigbee_lock_acquire(portMAX_DELAY);
                if (s_commission_mode == EZB_BDB_MODE_NETWORK_STEERING) {
                    s_stats.steer_attempts++;
                    /* Set before each attempt: it goes to the parent in the association.
                     * Between attempts the receiver has nothing to hear, and left on it
                     * would hold the antenna against the setup hotspot. */
                    ezb_set_rx_on_when_idle(true);
                }
                ezb_bdb_start_top_level_commissioning(s_commission_mode);
                esp_zigbee_lock_release();
                break;
            case EV_COMMAND: {
                const esp_err_t err = run_command(&ev);
                if (err == ESP_OK) {
                    s_stats.handled++;
                } else {
                    s_stats.refused++;
                    ESP_LOGW(TAG, "ep %d cluster 0x%04x cmd 0x%02x: %s", ev.dst_ep, ev.cluster, ev.cmd_id,
                             esp_err_to_name(err));
                }
                if (ev.want_rsp) {
                    send_default_rsp(&ev, err == ESP_OK ? EZB_ZCL_STATUS_SUCCESS : EZB_ZCL_STATUS_FAIL);
                }
                __atomic_store_n(&s_sync_wanted, true, __ATOMIC_RELEASE);
                break;
            }
            case EV_ENTER_WIFI:
                ESP_LOGW(TAG, "The hub asks for Wi-Fi, %u min", ev.cmd.level);
                /* Long enough for the APS ack to have gone. */
                vTaskDelay(pdMS_TO_TICKS(300));
                node_restart_into(NODE_ASK_WIFI, ev.cmd.level);
                break;
            case EV_LEFT:
                ESP_LOGW(TAG, "Left the network for good: back to looking for one");
                node_store_set_paired(false);
                vTaskDelay(pdMS_TO_TICKS(300));
                node_restart_into(NODE_ASK_NONE, 0);
                break;
            case EV_ANNOUNCE: {
                /* The hub interrogates on an announce, and that is how it hears of a
                 * light added or removed. */
                ezb_zdo_device_annce_req_t req = { 0 };
                esp_zigbee_lock_acquire(portMAX_DELAY);
                ezb_zdo_device_annce_req(&req);
                esp_zigbee_lock_release();
                node_store_set_endpoints(s_endpoints_digest);
                break;
            }
            default:
                break;
            }
        }
        const int64_t now = esp_timer_get_time();
        if (__atomic_exchange_n(&s_sync_wanted, false, __ATOMIC_ACQ_REL) && !sync_since) {
            sync_since = now;
        }
        if (sync_since && s_started && (!got || now - sync_since >= SYNC_MAX_WAIT_US)) {
            sync_since = 0;
            sync_attrs();
        }
    }
}

/* ------------------------------------------------------------- the signals */

static void post_event(uint8_t type)
{
    const node_ev_t ev = { .type = type };
    xQueueSend(s_queue, &ev, 0);
}

static void on_network(const char *how)
{
    ESP_LOGI(TAG, "%s: network 0x%04x, channel %d, short 0x%04x", how, ezb_nwk_get_panid(),
             ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
    s_joined_at = esp_timer_get_time();
    /* A short flash write, in the stack's callback: this happens once per boot. */
    if (!node_store_paired()) {
        node_store_set_paired(true);
    }
    node_zb_state_changed();
    if (node_store_endpoints() != s_endpoints_digest) {
        post_event(EV_ANNOUNCE);
    }
}

static bool app_signal_handler(const ezb_app_signal_t *app_signal)
{
    const ezb_app_signal_type_t type = ezb_app_signal_get_type(app_signal);
    const char *name = ezb_app_signal_to_string(type);
    const bool  bdb  = type == EZB_BDB_SIGNAL_DEVICE_FIRST_START || type == EZB_BDB_SIGNAL_DEVICE_REBOOT ||
                       type == EZB_BDB_SIGNAL_STEERING;
    const void *params = ezb_app_signal_get_params(app_signal);
    const int   status = bdb && params ? *(const ezb_bdb_comm_status_t *)params : -1;
    note_signal(name ? name : "?", status);

    switch (type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        schedule_commission(EZB_BDB_MODE_INITIALIZATION, 1000);
        break;

    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (status != EZB_BDB_STATUS_SUCCESS && !ezb_bdb_is_factory_new()) {
            /* A rejoin that found nobody, which is what a parent that is off looks
             * like. The network is still ours: keep trying, more slowly. */
            ESP_LOGW(TAG, "%s failed (0x%02x), retrying", name, status);
            schedule_commission(EZB_BDB_MODE_INITIALIZATION, REJOIN_RETRY_US);
        } else if (status != EZB_BDB_STATUS_SUCCESS) {
            schedule_commission(EZB_BDB_MODE_INITIALIZATION, INIT_RETRY_US);
        } else if (ezb_bdb_is_factory_new()) {
            if (node_store_paired()) {
                /* Told it is paired, with no network stored: the store was lost. */
                ESP_LOGW(TAG, "Paired, with no network stored");
                post_event(EV_LEFT);
            } else {
                ESP_LOGI(TAG, "No network yet, looking for one");
                schedule_commission(EZB_BDB_MODE_NETWORK_STEERING, 1000);
            }
        } else {
            on_network(name);
        }
        break;

    case EZB_BDB_SIGNAL_STEERING:
        if (status == EZB_BDB_STATUS_SUCCESS) {
            on_network(name);
        } else {
            /* NO_NETWORK, and TCLK_EX_FAILURE: a key exchange that failed makes the
             * stack leave again by itself, and the next attempt usually succeeds. */
            ezb_set_rx_on_when_idle(false);
            schedule_commission(EZB_BDB_MODE_NETWORK_STEERING, JOIN_RETRY_US);
        }
        break;

    case EZB_ZDO_SIGNAL_LEAVE: {
        const ezb_zdo_signal_leave_params_t *p = params;
        ESP_LOGW(TAG, "Left the network (type %d)", p ? p->leave_type : -1);
        /* A join that failed its key exchange leaves too, and is not a leave from a
         * network this node belonged to: the steering signal is already retrying. */
        if (node_store_paired() && p && p->leave_type == EZB_ZDO_LEAVE_TYPE_RESET) {
            post_event(EV_LEFT);
        }
        s_joined_at = 0;
        break;
    }

    default:
        break;
    }
    return true;
}

/* ---------------------------------------------------------- the endpoints */

static void add_basic_strings(ezb_af_ep_desc_t ep)
{
    ezb_zcl_cluster_desc_t basic = ezb_af_endpoint_get_cluster_desc(ep, EZB_ZCL_CLUSTER_ID_BASIC,
                                                                    EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)NODE_MANUFACTURER);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)NODE_MODEL);
}

static ezb_af_ep_desc_t light_endpoint(const zbc_light_t *light)
{
    const uint8_t ep = (uint8_t)light->id;
    ezb_af_ep_desc_t desc;

    if (zbc_light_tunable(light->kind)) {
        ezb_zha_color_dimmable_light_config_t cfg = EZB_ZHA_COLOR_DIMMABLE_LIGHT_CONFIG();
        cfg.basic_cfg.power_source          = EZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE;
        cfg.color_cfg.color_mode            = EZB_ZCL_COLOR_CONTROL_COLOR_MODE_COLOR_TEMPERATURE_MIREDS;
        cfg.color_cfg.enhanced_color_mode   = EZB_ZCL_COLOR_CONTROL_ENHANCED_COLOR_MODE_COLOR_TEMPERATURE_MIREDS;
        cfg.color_cfg.color_capabilities    = 0x0010u;   /* colour temperature, nothing else */
        desc = ezb_zha_create_color_dimmable_light(ep, &cfg);

        ezb_zcl_cluster_desc_t color = ezb_af_endpoint_get_cluster_desc(desc, EZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                                                                        EZB_ZCL_CLUSTER_SERVER);
        uint16_t coolest = (uint16_t)(1000000u / (light->cold_k ? light->cold_k : ZBC_LIGHT_DEFAULT_COLD_K));
        uint16_t warmest = (uint16_t)(1000000u / (light->warm_k ? light->warm_k : ZBC_LIGHT_DEFAULT_WARM_K));
        uint16_t mireds  = warmest;
        ezb_zcl_color_control_cluster_desc_add_attr(color, EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_MIREDS_ID, &mireds);
        ezb_zcl_color_control_cluster_desc_add_attr(color, EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MIN_MIREDS_ID, &coolest);
        ezb_zcl_color_control_cluster_desc_add_attr(color, EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MAX_MIREDS_ID, &warmest);
    } else if (zbc_light_pwm_channels(light->kind) > 0) {
        ezb_zha_dimmable_light_config_t cfg = EZB_ZHA_DIMMABLE_LIGHT_CONFIG();
        cfg.basic_cfg.power_source = EZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE;
        desc = ezb_zha_create_dimmable_light(ep, &cfg);
    } else {
        ezb_zha_on_off_light_config_t cfg = EZB_ZHA_ON_OFF_LIGHT_CONFIG();
        cfg.basic_cfg.power_source = EZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE;
        desc = ezb_zha_create_on_off_light(ep, &cfg);
        add_basic_strings(desc);
        return desc;
    }

    /* The hub writes its fade time here, and a bare On or Off then takes it. */
    ezb_zcl_cluster_desc_t level = ezb_af_endpoint_get_cluster_desc(desc, EZB_ZCL_CLUSTER_ID_LEVEL,
                                                                    EZB_ZCL_CLUSTER_SERVER);
    uint16_t onoff_ds = 0;
    ezb_zcl_level_cluster_desc_add_attr(level, EZB_ZCL_ATTR_LEVEL_ON_OFF_TRANSITION_TIME_ID, &onoff_ds);
    add_basic_strings(desc);
    return desc;
}

static esp_err_t create_endpoints(void)
{
    ezb_af_device_desc_t dev = ezb_af_create_device_desc();
    uint32_t digest = 2166136261u;

    zbc_light_t lights[CONFIG_ZBC_MAX_LIGHTS];
    const int   n = (int)light_db_snapshot(lights, CONFIG_ZBC_MAX_LIGHTS);
    for (int i = 0; i < n; i++) {
        if (lights[i].id < 1 || lights[i].id > NODE_LIGHT_EP_MAX) {
            ESP_LOGW(TAG, "Light %u has no endpoint number", lights[i].id);
            continue;
        }
        ESP_RETURN_ON_ERROR(ezb_af_device_add_endpoint_desc(dev, light_endpoint(&lights[i])), TAG, "light endpoint");
        ESP_LOGI(TAG, "Endpoint %u: %s", lights[i].id, lights[i].name);
        /* FNV-1a over what the simple descriptor says. */
        const uint32_t part = ((uint32_t)lights[i].id << 8) | lights[i].kind;
        digest = (digest ^ part) * 16777619u;
    }
    s_endpoints_digest = digest;

    ezb_zha_configuration_tool_config_t cfg = EZB_ZHA_CONFIGURATION_TOOL_CONFIG();
    cfg.basic_cfg.power_source = EZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE;
    ezb_af_ep_desc_t control = ezb_zha_create_configuration_tool(NODE_CONTROL_EP, &cfg);
    add_basic_strings(control);
    /* What tells a hub this is a node: see node_proto.h. The frames themselves go
     * through raw_frame, so the cluster needs no handlers, only to be listed. */
    const ezb_zcl_custom_cluster_config_t proto = { .cluster_id = NODE_PROTO_CLUSTER };
    ESP_RETURN_ON_ERROR(ezb_af_endpoint_add_cluster_desc(
                            control, ezb_zcl_custom_create_cluster_desc(&proto, EZB_ZCL_CLUSTER_SERVER)),
                        TAG, "control cluster");
    ESP_RETURN_ON_ERROR(ezb_af_device_add_endpoint_desc(dev, control), TAG, "control endpoint");

    ESP_RETURN_ON_ERROR(ezb_af_device_desc_register(dev), TAG, "register device");
    ezb_zcl_raw_command_handler_register(raw_frame);
    return ESP_OK;
}

static void zb_main_task(void *arg)
{
    (void)arg;
    esp_zigbee_config_t config = {
        .device_config = {
            .device_type         = EZB_NWK_DEVICE_TYPE_END_DEVICE,
            .install_code_policy = false,
            .zed_config          = { .ed_timeout = ED_TIMEOUT, .keep_alive = KEEP_ALIVE_MS },
        },
        .platform_config = {
            .storage_partition_name = NODE_STORAGE_PARTITION,
            .radio_config           = { .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE },
        },
    };

    if (s_with_wifi) {
        /* Without this, whichever of Wi-Fi and 802.15.4 starts first keeps the antenna. */
        ESP_ERROR_CHECK(esp_coex_wifi_i154_enable());
    }
    ESP_ERROR_CHECK(esp_zigbee_init(&config));

    /* Before joining: the flag goes to the parent in the association, and is what
     * makes it send to this child at once rather than hold frames for a poll. */
    ezb_set_rx_on_when_idle(true);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(ALL_CHANNELS));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(0));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(app_signal_handler));
    ESP_ERROR_CHECK(create_endpoints());

    ESP_ERROR_CHECK(esp_zigbee_start(false));
    s_started = true;
    radio_coex_start();
    esp_zigbee_launch_mainloop();

    radio_coex_stop();
    s_started = false;
    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

int64_t node_zb_joined_at(void)
{
    return s_joined_at;
}

static void button_sent(ezb_af_user_cnf_t *cnf, void *user_ctx)
{
    node_buttons_delivered((uint8_t)(uintptr_t)user_ctx, cnf && cnf->status == 0);
}

/* From the node's own tasks: takes the stack lock. */
static esp_err_t send_to_hub(uint8_t cmd_id, uint8_t *data, uint16_t len, ezb_af_user_cnf_callback_t cb, void *ctx)
{
    if (!s_started || !s_joined_at) {
        return ESP_FAIL;
    }
    /* Not ezb_zcl_custom_cluster_cmd_req, which wants a client of the cluster on this
     * endpoint before it will send to a server. */
    ezb_zcl_custom_cmd_t cmd = {
        .cmd_ctrl = {
            .dst_addr   = { .addr_mode = EZB_ADDR_MODE_SHORT, .u.short_addr = 0x0000 },
            .dst_ep     = NODE_HUB_EP,
            .src_ep     = NODE_CONTROL_EP,
            .cluster_id = NODE_PROTO_CLUSTER,
            .manuf_code = NODE_PROTO_MANUF,
            .fc         = { .manuf_specific = 1, .direction = 0, .dis_default_rsp = 1 },
            .cnf_ctx    = { .cb = cb, .user_ctx = ctx },
        },
        .cmd_id      = cmd_id,
        .data_length = len,
        .data        = data,
    };
    esp_zigbee_lock_acquire(portMAX_DELAY);
    const ezb_err_t err = ezb_zcl_custom_cmd_req(&cmd);
    esp_zigbee_lock_release();
    return err == EZB_ERR_NONE ? ESP_OK : ESP_FAIL;
}

esp_err_t node_zb_send_button(uint8_t key, uint8_t event, uint8_t seq)
{
    uint8_t body[2] = { key, event };
    return send_to_hub(NODE_CMD_BUTTON, body, sizeof(body), button_sent, (void *)(uintptr_t)seq);
}

void node_zb_send_wifi_state(uint8_t state, const uint8_t ipv4[4])
{
    uint8_t body[5] = { state, 0, 0, 0, 0 };
    if (ipv4) {
        memcpy(&body[1], ipv4, 4);
    }
    send_to_hub(NODE_CMD_WIFI_STATE, body, sizeof(body), NULL, NULL);
}

esp_err_t node_zb_start(bool with_wifi)
{
    s_with_wifi = with_wifi;
    s_queue = xQueueCreate(12, sizeof(node_ev_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t args = { .callback = commission_timer_cb, .name = "zb_commission" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_commission_timer), TAG, "timer");
    if (xTaskCreate(node_task, "node", 4096, NULL, 4, NULL) != pdPASS ||
        xTaskCreate(zb_main_task, "zigbee_main", 6144, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* wifi_manager.c stops Zigbee before it opens the setup hotspot, because a
 * listening 802.15.4 receiver would hold the antenna. */
#include "zb_coordinator.h"

esp_err_t zb_coordinator_stop(void)
{
    /* Unjoined, it listens only while it looks for a network, and that goes on: the
     * hotspot is how somebody configures it while the hub is opened for it. */
    if (!s_started || !esp_zigbee_is_started() || !s_joined_at) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Stopping the Zigbee stack to free the radio for Wi-Fi");
    esp_timer_stop(s_commission_timer);
    return esp_zigbee_stop() == ESP_OK ? ESP_OK : ESP_FAIL;
}

/* radio_coex.c asks whether the hub's energy-scan watchdog (radio_ed.c) is alive. An
 * end device forms no network, so it has no energy scans to watch. */
#include "esp_attr.h"
#include "radio_ed.h"

IRAM_ATTR bool radio_ed_live(void)
{
    return false;
}
