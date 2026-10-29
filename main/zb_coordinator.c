#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_coexist.h"

#include "esp_zigbee.h"
#include "ezbee/aps.h"
#if CONFIG_ZBC_WLED
#include "ezbee/test_utils.h"   /* ezb_nwk_set_neighbor_info - see ent_keep_neighbour_locked */
#endif
#include "ezbee/zha.h"
#include "ezbee/zcl/cluster/groups.h"
#include "ezbee/zcl/cluster/ota_upgrade.h"

#include "colorspace.h"
#include "device_db.h"
#include "remote.h"
#include "settings.h"
#include "state_rev.h"
#include "wifi_manager.h"
#include "zb_coordinator.h"
#include "zb_ota.h"
#include "radio_ed.h"
#include "radio_coex.h"
#include "heap_watch.h"

static const char *TAG = "zigbee";

/* esp_err_to_name() for what a command returns, which may be our own
 * ZB_ERR_UNREACHABLE - a code ESP-IDF has no name for. */
static const char *zb_err_name(esp_err_t err)
{
    return err == ZB_ERR_UNREACHABLE ? "not answering" : esp_err_to_name(err);
}

/* Our own endpoint. A "custom gateway" endpoint carries the client side of the
 * usual HA clusters, which is what lets us drive other people's lights. */
#define ZBC_GATEWAY_EP_ID 1

#define ZBC_STORAGE_PARTITION    "nvs"

#define ZBC_MANUFACTURER_NAME "\x09""ESPRESSIF"
#define ZBC_MODEL_IDENTIFIER  "\x0a""ZBCoordC6"

/* Reporting intervals. An unchanged value is repeated every 5 min so live can be
 * told from stale; the minimum interval sets how closely a change is followed.
 *
 * On/Off changes are rare and go out at once. CurrentLevel gets 1 s: every
 * command here is a ramp, and with a zero minimum every member of a group fade
 * reports each step, on air just as the next command is trying to get out, with
 * the answers arriving stale and out of order. The settle read catches the final
 * value, not the report stream. */
#define ZBC_REPORT_MIN_ONOFF_S    0
#define ZBC_REPORT_MIN_LEVEL_S    1
#define ZBC_REPORT_MAX_INTERVAL_S 300

/* The stack occasionally refuses a command with EZB_ERR_FAIL and accepts it a
 * moment later. Retry, releasing the lock in between so the stack task can make
 * progress. Only from a task that may block, which is every caller of the
 * public zb_* functions. */
#define ZBC_SEND_ATTEMPTS 3

#define ZBC_SEND_RETRY(err_var, call)                                            \
    do {                                                                         \
        for (int zbc_try_ = 0; zbc_try_ < ZBC_SEND_ATTEMPTS; zbc_try_++) {       \
            esp_zigbee_lock_acquire(portMAX_DELAY);                              \
            (err_var) = (call);                                                  \
            esp_zigbee_lock_release();                                           \
            if ((err_var) == EZB_ERR_NONE) {                                     \
                break;                                                           \
            }                                                                    \
            ESP_LOGW(TAG, "Command refused (0x%04x), attempt %d of %d",          \
                     (err_var), zbc_try_ + 1, ZBC_SEND_ATTEMPTS);                \
            vTaskDelay(pdMS_TO_TICKS(40));                                       \
        }                                                                        \
    } while (0)

/* APS delivery confirmation of a command. The slot holds what the command asked
 * for, so an undelivered command is re-sent from there rather than waiting for
 * the settle read.
 *
 * Runs in the Zigbee task with the stack lock held, so it must not take it. */
static void data_confirm_cb(ezb_af_user_cnf_t *cnf, void *user_ctx);

/* How many endpoints can have a command in flight at once: one settle slot and
 * one gate slot each. Sized for every device, not one group - a Matter scene or
 * a button rule touches several groups at once, and with too few slots settle
 * steals one from a lamp mid-settle and the gate sends unsequenced. */
#define ZBC_COMMAND_SLOTS (CONFIG_ZBC_MAX_DEVICES > CONFIG_ZBC_MAX_GROUP_MEMBERS \
                               ? CONFIG_ZBC_MAX_DEVICES : CONFIG_ZBC_MAX_GROUP_MEMBERS)

/* Settle-read slots, keyed by (short address, endpoint) and reused, so a slider
 * drag costs one re-read per lamp; only the last command decides where it ends
 * up. A group command needs one slot per member. */
#define ZBC_SETTLE_SLOTS ZBC_COMMAND_SLOTS

/* A colour command says nothing about on/off, but a curve sends a level command
 * and then a colour command to the same endpoint: the colour command keeps the
 * intent the level command set instead of clearing it. */
#define ZBC_WANT_KEEP ((int8_t)-2)

/* The level a fade in starts from, and so the only level a lamp has any
 * business sitting at when it was asked for something much brighter. */
#define ZBC_FADE_FLOOR_LEVEL 1

/* How long OffWithEffect 0x00/0x00 takes ("fade to off over 0.8 s"). The off
 * commands carry it as their transition, although the frame has no such field,
 * because the settle window is armed from it: a lamp reports off only once the
 * effect has run. */
#define ZBC_OFF_EFFECT_DS 8

/* The `host-test region` blocks below are extracted verbatim by `test/Makefile`
 * and compiled against stubs, so the state machines can be driven through
 * orderings impractical to produce on hardware. Extracted, not copied, so the
 * two cannot drift. Moving or deleting a marker breaks `make -C test`. */
/* >>> host-test region: settle-types <<< */
/* Which colour attribute came back. Our own names, not the ZCL ids, so the
 * decision that uses them (color_off_target) can be driven from a host test. */
enum { ZBC_COLOR_PART_CT, ZBC_COLOR_PART_HUE, ZBC_COLOR_PART_SAT };

static struct {
    uint16_t short_addr;
    uint8_t  ep;
    uint16_t features;
    uint8_t  reads_left;
    /* What the command asked for, so the read that follows can tell whether the
     * lamp ended up somewhere else. -1: nothing to check (a colour change). */
    int8_t   want_on;
    uint8_t  want_level;
    uint8_t  fixes_left;
    /* What the last colour command asked for, and its own correction budget -
     * separate from the level intent's, because a curve turns one request into a
     * level command and a colour command sharing this slot, and one must not
     * spend the other's tries. ZBC_CMD_NONE: no colour to check. */
    uint8_t  want_color;       /* zbc_cmd_kind_t: CT, HS, or NONE */
    uint16_t want_color_a;     /* mireds, or hue */
    uint16_t want_color_b;     /* saturation */
    uint8_t  color_fixes_left;
    /* Only the last read of a command is judged. Earlier reads, and answers still
     * in flight from the previous command, report the old state and would
     * provoke a correction that undoes the new one. */
    bool     judge;
    /* Whether the lamp answered the last read. Without this a delivered command
     * followed by silence leaves the old state stored, and the correction, which
     * runs only on a report, never fires. */
    bool     answered;
    bool     audit;
    /* Whether the stack confirmed delivery of the command. Silence after that is a
     * lost read, not a reason to send the command again. See settle_work. */
    bool     delivered;
    /* How many more times a lamp that has gone quiet is worth asking again.
     * Reads only - never a command; see the audit branch in settle_work. */
    uint8_t  recover_left;
    /* When this slot's next read is due. One timer serves every slot and is armed
     * for the earliest, so a command to one lamp cannot cut short another
     * lamp's settle window. */
    int64_t  due_us;
} s_settle[ZBC_SETTLE_SLOTS];
/* >>> end host-test region: settle-types <<< */
static esp_timer_handle_t s_settle_timer;

/* Counters for faults that look like software and are usually the radio,
 * readable over the API because opening the serial port resets the board.
 * Free-running since boot: sample `GET /api/status` twice and divide. */
static struct {
    uint32_t cmd_undelivered;   /* the stack gave up on a command */
    uint32_t read_undelivered;  /* ...or on a read */
    uint32_t cmd_slow;          /* a command got there, but only after ZBC_SLOW_DELIVERY_US */
    uint32_t cmd_slow_ms;       /* ...and how long those took, added up */
    uint32_t went_quiet;        /* a lamp stopped answering; state marked unknown */
    uint32_t corrections;       /* a lamp was somewhere it was not asked to be */
    uint32_t gate_timeouts;     /* a confirmation never arrived at all */
    uint32_t slots_short;       /* more endpoints in flight than slots; see ZBC_COMMAND_SLOTS */
    uint32_t frames_heard;      /* frames devices sent of their own accord; see note_link */
    uint32_t frames_missed;     /* ...and the sequence numbers between them that never arrived */
    uint32_t table_entries;     /* the coordinator's own neighbour table, at the last walk */
    uint32_t table_children;    /* ...and how many of those are its children */
    uint32_t sends_skipped;     /* not put on air: the device has stopped answering */
    uint32_t router_tables_asked;      /* router neighbour-table pages asked for (ask_router_table) */
    uint32_t router_tables_answered;   /* ...answered, a refusal included */
    uint32_t router_tables_unanswered; /* ...ended by the stack with no answer */
} s_radio;

/* Endpoints that refused OffWithEffect. The off path is built on it, but it is
 * optional, and a lamp without it would never switch off; these get a plain Off.
 * Runtime only: re-learning it costs one command. Keyed by IEEE, which survives
 * a rejoin; a short address can pass to another device. */
static struct {
    bool     used;
    uint8_t  ieee[ZBC_IEEE_LEN];
    uint8_t  ep;
} s_no_off_effect[CONFIG_ZBC_MAX_DEVICES];

static bool     s_stack_started;
/* The image zb_ota.c holds, as the stack's OTA server knows it. Touched only
 * with the stack lock held - in the Zigbee task or through zb_ota_server_*. */
static ezb_zcl_ota_file_handle_t s_ota_file;
static bool     s_network_up;
/* PAN, channel, short and long address, copied once at network-up. They do not
 * change while the network is up, and the status poll must not wait on the
 * stack lock for them. */
static zb_netinfo_t s_net;

/* Where a new network is formed: the channel the last one was moved to, which a
 * Zigbee-only reset keeps, else the build's default. */
static uint8_t formation_channel(void)
{
    const uint8_t ch = settings_zigbee_channel();
    return ch ? ch : CONFIG_ZBC_ZIGBEE_CHANNEL;
}
static int64_t  s_permit_deadline_us;
static esp_timer_handle_t s_retry_timer;
static esp_timer_handle_t s_refresh_timer;
static ezb_bdb_comm_mode_mask_t s_retry_mode;
/* A formation the stack reported as failed, to be started again at once rather
 * than counted as another quiet wait - see retry_work. */
static bool s_formation_failed;

/* The formation log for /api/status - see zb_forming_t. Written in the Zigbee
 * task and zb_work_task, read by the HTTP task. */
static zb_forming_t s_forming = { .bdb_status = -1 };
static portMUX_TYPE s_forming_mux = portMUX_INITIALIZER_UNLOCKED;

static void forming_note(const char *what, int code, uint8_t mode, uint32_t *count)
{
    portENTER_CRITICAL(&s_forming_mux);
    if (count) {
        (*count)++;
    }
    if (s_forming.count == ZB_FORMING_NOTES) {
        memmove(&s_forming.notes[0], &s_forming.notes[1], sizeof(s_forming.notes[0]) * (ZB_FORMING_NOTES - 1));
        s_forming.count--;
    }
    s_forming.notes[s_forming.count++] = (zb_forming_note_t){
        .at_us = esp_timer_get_time(), .what = what, .code = code, .mode = mode,
    };
    portEXIT_CRITICAL(&s_forming_mux);
}

/* Call with the stack lock held, or from the Zigbee task. */
static void forming_sample_locked(void)
{
    const int     status = ezb_bdb_get_commissioning_status();
    const uint8_t mode   = ezb_bdb_get_commissioning_mode();
    portENTER_CRITICAL(&s_forming_mux);
    s_forming.bdb_status = status;
    s_forming.bdb_mode   = mode;
    s_forming.sampled_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_forming_mux);
}

/* Every commissioning start goes through here, so the log shows each one and
 * what the stack answered. */
static ezb_err_t start_commissioning(ezb_bdb_comm_mode_mask_t mode)
{
    const ezb_err_t err = ezb_bdb_start_top_level_commissioning(mode);
    forming_note("start", err, mode, &s_forming.starts);
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Commissioning start (mode 0x%02x) refused: %d", mode, err);
    }
    return err;
}

/* A short burst of re-interrogation after an announce - see announce_work. */
static esp_timer_handle_t s_announce_timer;
/* Asking a device what it runs after it finished downloading - ota_recheck_work. */
static esp_timer_handle_t s_ota_recheck_timer;
static uint8_t            s_announce_step;

/* ------------------------------------------------------------------ helpers */

static void zcl_string_copy(char *dst, size_t dst_size, const void *zcl_value)
{
    /* ZCL character strings are length-prefixed and not NUL-terminated. */
    const uint8_t *raw = zcl_value;
    size_t         len = raw[0];
    if (len == 0xFF) {  /* invalid string */
        len = 0;
    }
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, raw + 1, len);
    dst[len] = '\0';
}

static bool ieee_of(uint16_t short_addr, uint8_t out[ZBC_IEEE_LEN])
{
    return device_db_ieee_by_short(short_addr, out);
}

/* --------------------------------------------------- device interrogation */

static void interrogate_device(uint16_t short_addr);
static esp_err_t lookup_endpoint(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint16_t *short_addr, uint16_t *features);
static void read_basic_info(uint16_t short_addr, uint8_t ep);
static void configure_reporting(uint16_t short_addr, uint8_t ep, uint16_t cluster_id, uint16_t attr_id,
                                uint8_t attr_type, uint16_t min_interval_s);
static void bind_for_reports(uint16_t short_addr, uint8_t ep, uint16_t cluster_id);
static void ensure_reports_bound(uint16_t short_addr, const zbc_device_t *dev);
static bool bind_scan_take(const uint8_t ieee[ZBC_IEEE_LEN]);
static void ensure_color_info(uint16_t short_addr, const zbc_device_t *dev);
static void read_attributes(uint16_t short_addr, uint8_t ep, uint16_t cluster_id, uint16_t *attrs, uint8_t count);
static void read_onoff_state(uint16_t short_addr, uint8_t ep);
static void read_light_state(uint16_t short_addr, uint8_t ep, uint16_t features);
static void read_static_color_info(uint16_t short_addr, uint8_t ep);
static void read_ota_client_info(uint16_t short_addr, uint8_t ep);
static void ota_wake_post(void);
static void hue_bind_post(uint16_t short_addr, bool force);
static void hue_note_notify(uint16_t short_addr);
static void ask_when_heard(uint16_t short_addr);
static void write_onoff_transition(uint16_t short_addr, uint8_t ep, uint16_t ds, bool verify);

/* One command to a lamp, in the form the ordering gate below stores and replays.
 * Every state-changing command this coordinator sends is one of these; the
 * arguments are named per kind rather than in a union because two 16-bit fields
 * cover all of them and a flat struct copies by assignment. */
/* >>> host-test region: gate-types <<< */
typedef enum {
    ZBC_CMD_NONE = 0,
    ZBC_CMD_ONOFF,     /* a: zb_onoff_cmd_t */
    ZBC_CMD_FADE_OFF,  /* OffWithEffect - fade out and off, decided by the lamp */
    ZBC_CMD_LEVEL,     /* a: level 0-254 */
    ZBC_CMD_FADE_IN,   /* a: level 0-254; two frames, see zb_fade_in */
    ZBC_CMD_CT,        /* a: mireds */
    ZBC_CMD_WARM_OFF,  /* a: mireds, b: 1 for a plain Off; two frames, see zb_warm_off */
    ZBC_CMD_HS,        /* a: hue 0-254, b: saturation 0-254 */
    ZBC_CMD_XY,        /* a: CIE x, b: CIE y, both /65536 */
} zbc_cmd_kind_t;

typedef struct {
    uint8_t  kind;           /* zbc_cmd_kind_t */
    uint8_t  step;           /* which frame of a multi-frame command is next */
    uint16_t a, b;
    uint16_t transition_ds;
} zbc_cmd_t;

/* Which of an endpoint's two queues a command belongs in - see the gate. */
#define ZBC_CH_LEVEL 0   /* on/off, level, stop: how bright, and lit at all */
#define ZBC_CH_COLOR 1   /* colour temperature, hue/saturation, xy */
#define ZBC_CH_COUNT 2
/* >>> end host-test region: gate-types <<< */

static bool      no_off_effect(uint16_t short_addr, uint8_t ep);
static void      mark_no_off_effect(uint16_t short_addr, uint8_t ep);
static esp_err_t gate_send(uint16_t short_addr, uint8_t ep, const zbc_cmd_t *cmd, int ch);
static void      reach_note(uint16_t short_addr, bool delivered);
static bool      reach_admits(uint16_t short_addr, bool command);
static bool      reach_is_gone(uint16_t short_addr);
static void      gate_send_locked(uint16_t short_addr, uint8_t ep, const zbc_cmd_t *cmd, int ch);
static bool      gate_advance(uint16_t short_addr, uint8_t ep);
static bool      gate_confirms_preset(uint16_t short_addr, uint8_t ep);
static int64_t   gate_inflight_age_us(uint16_t short_addr, uint8_t ep);
static bool      gate_flush_stale(void);
static void      resend_intent_locked(uint16_t short_addr, uint8_t ep, int8_t want_on,
                                      uint8_t want_level);
static void      color_correct_locked(uint16_t short_addr, uint8_t ep, int part, uint16_t got);
static void      settle_rearm_locked(bool want_gate_tick);

/* Whether a device runs on a battery with its receiver off between messages -
 * the only thing that separates an Aqara wireless switch from a lamp. A
 * lumi.sensor_switch.aq2 advertises On/Off as an input cluster and nothing as an
 * output, which is the simple descriptor of an On/Off light, and its device id
 * is manufacturer-specific. The MAC capability flags are the device's own claim
 * about itself: something that cannot hear a command unless it happens to be
 * awake is not a lamp, whatever clusters it lists.
 *
 * The flags arrive in the announce and in the neighbour table, so nothing has
 * to be asked for. */
#define ZBC_MAC_CAP_MAINS_POWER (1u << 2)
#define ZBC_MAC_CAP_RX_ON_IDLE  (1u << 3)

static struct {
    uint8_t  ieee[ZBC_IEEE_LEN];
    bool     battery;
    bool     known;
} s_power[CONFIG_ZBC_MAX_DEVICES];

/* Keyed by IEEE: the short address changes on every rejoin, and a table keyed by
 * it fills with stale entries. Announce and neighbour table both carry the IEEE. */
static void note_power_source(const uint8_t ieee[ZBC_IEEE_LEN], bool battery)
{
    for (size_t i = 0; i < sizeof(s_power) / sizeof(s_power[0]); i++) {
        if (s_power[i].known && memcmp(s_power[i].ieee, ieee, ZBC_IEEE_LEN) == 0) {
            s_power[i].battery = battery;
            return;
        }
    }
    for (size_t i = 0; i < sizeof(s_power) / sizeof(s_power[0]); i++) {
        if (!s_power[i].known) {
            memcpy(s_power[i].ieee, ieee, ZBC_IEEE_LEN);
            s_power[i].battery = battery;
            s_power[i].known   = true;
            return;
        }
    }
}

/* Unknown means "never told", the safe way round: the endpoint is then
 * classified from its clusters alone. */
static bool runs_on_a_battery(const uint8_t ieee[ZBC_IEEE_LEN])
{
    for (size_t i = 0; i < sizeof(s_power) / sizeof(s_power[0]); i++) {
        if (s_power[i].known && memcmp(s_power[i].ieee, ieee, ZBC_IEEE_LEN) == 0) {
            return s_power[i].battery;
        }
    }
    return false;
}

/* An endpoint that sends presses and has nothing of a lamp: a remote's. */
static bool ep_presses_only(uint16_t features)
{
    return (features & ZBC_FEAT_SWITCH) && !(features & ZBC_FEAT_ONOFF);
}

static bool senses_motion(const zbc_device_t *dev)
{
    for (int i = 0; i < dev->ep_count; i++) {
        if (dev->eps[i].features & ZBC_FEAT_MOTION) {
            return true;
        }
    }
    return false;
}

/* A device that senses motion on one endpoint is a sensor on the others too. A
 * Hue SML lists On/Off, Level and Scenes as outputs on its ZLL endpoint 1, for
 * a bridge to bind lamps to it directly, and nothing on it can be pressed. As a
 * remote that endpoint would be bound, and sought on every neighbour scan, for
 * frames that never come. Its descriptor usually arrives before the sensor's,
 * so the sensor's takes it out of the record, and one that arrives after is not
 * put in (see simple_desc_result). Zigbee task. */
static void drop_presses_beside_motion(const uint8_t ieee[ZBC_IEEE_LEN], const zbc_device_t *dev)
{
    for (int i = 0; i < dev->ep_count; i++) {
        if (ep_presses_only(dev->eps[i].features)) {
            ESP_LOGI(TAG, "Endpoint %d of a motion sensor is not a remote", dev->eps[i].ep);
            device_db_remove_endpoint(ieee, dev->eps[i].ep);
        }
    }
}

static void simple_desc_result(const ezb_zdo_simple_desc_req_result_t *result, void *user_ctx)
{
    (void)user_ctx;
    if (!result || result->error != EZB_ERR_NONE || !result->rsp ||
        result->rsp->status != EZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "Simple descriptor request failed");
        return;
    }

    const ezb_af_simple_desc_t *desc  = &result->rsp->desc;
    uint16_t                    saddr = result->rsp->nwk_addr_of_interest;

    /* An endpoint is a light if it can be switched; dimming and colour are extras
     * found the same way. Whether colour means hue/saturation or CIE xy comes
     * from ColorCapabilities, read further down.
     *
     * A remote cannot be recognised from input clusters alone. A standard one
     * (IKEA rocker, Hue dimmer) advertises On/Off and Level as outputs, because
     * it sends those commands. An Aqara wireless switch advertises neither: the
     * newer revision reports clicks on Multistate Input, and the older one
     * advertises On/Off as an input, like a lamp. Both bits can be set on one
     * endpoint; the On/Off binding is what makes the older one report. */
    uint16_t features = 0;
    bool     has_multistate = false, has_onoff = false, has_level = false, has_ota = false;
    bool     has_scenes = false;
    for (int i = 0; i < desc->app_input_cluster_count; i++) {
        switch (desc->app_cluster_list[i]) {
        case EZB_ZCL_CLUSTER_ID_ON_OFF:        features |= ZBC_FEAT_ONOFF; has_onoff = true; break;
        case EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING: features |= ZBC_FEAT_MOTION; break;
        case EZB_ZCL_CLUSTER_ID_LEVEL:         features |= ZBC_FEAT_LEVEL; has_level = true; break;
        case EZB_ZCL_CLUSTER_ID_COLOR_CONTROL: features |= ZBC_FEAT_COLOR_TEMP; break;
        case EZB_ZCL_CLUSTER_ID_MULTISTATE_INPUT:
            features |= ZBC_FEAT_SWITCH; has_multistate = true; break;
        default: break;
        }
    }
    /* Output clusters sit in the tail of the same array. A remote advertises
     * On/Off and Level there because it sends those commands; either direction
     * is worth binding, since the bind gives the device somewhere to send. */
    for (int i = desc->app_input_cluster_count;
         i < desc->app_input_cluster_count + desc->app_output_cluster_count; i++) {
        switch (desc->app_cluster_list[i]) {
        case EZB_ZCL_CLUSTER_ID_ON_OFF: has_onoff = true; features |= ZBC_FEAT_SWITCH; break;
        case EZB_ZCL_CLUSTER_ID_LEVEL:  has_level = true; features |= ZBC_FEAT_SWITCH; break;
        case EZB_ZCL_CLUSTER_ID_SCENES: has_scenes = true; features |= ZBC_FEAT_SWITCH; break;
        /* An OTA Upgrade client: this endpoint is where the device asks for
         * firmware, and so where an Image Notify has to go. Not a switch. */
        case EZB_ZCL_CLUSTER_ID_OTA_UPGRADE: has_ota = true; break;
        default: break;
        }
    }
    uint8_t ieee[ZBC_IEEE_LEN];
    if (!ieee_of(saddr, ieee)) {
        ESP_LOGW(TAG, "Simple descriptor from unknown device 0x%04x", saddr);
        return;
    }

    /* A switch-only endpoint on a battery device with its receiver off is a
     * remote, whatever its cluster list says. A battery device that also dims or
     * does colour stays a lamp.
     *
     * Only an endpoint with a cluster a press can arrive on counts: a Hue dimmer
     * (RWL021) has a second endpoint with only Basic, Power, Identify, Binary
     * Input and 0xFC00, which is not a second remote. */
    const bool press_clusters = has_onoff || has_level || has_multistate || has_scenes;
    if (runs_on_a_battery(ieee) && press_clusters &&
        !(features & (ZBC_FEAT_LEVEL | ZBC_FEAT_COLOR_TEMP | ZBC_FEAT_ANY_COLOR))) {
        features |= ZBC_FEAT_SWITCH;
        features &= (uint16_t)~ZBC_FEAT_ONOFF;
    }

    /* An endpoint with Occupancy Sensing is a motion sensor and nothing else.
     * Some list On/Off or Level as outputs too, to switch a lamp bound to them
     * directly, which would make them remotes above; their motion is read here
     * from the occupancy report instead. */
    const bool is_sensor = (features & ZBC_FEAT_MOTION) != 0;
    if (is_sensor) {
        features = ZBC_FEAT_MOTION;
    }

    if (has_ota) {
        zb_ota_note_client(saddr, desc->ep_id);
    }

    const bool is_light  = (features & ZBC_FEAT_ONOFF) != 0;
    const bool is_switch = (features & ZBC_FEAT_SWITCH) != 0;
    if (!is_light && !is_switch && !is_sensor) {
        /* The simple descriptor is the authority, so any record of it goes (lamps
         * excepted - see device_db_remove_endpoint). */
        device_db_remove_endpoint(ieee, desc->ep_id);
        return;
    }

    /* What the registry already holds about this endpoint. Interrogation runs on
     * every announce; model, colour capabilities and mired limits never change,
     * so they are read only when missing. Bindings are redone every time: a
     * factory-reset lamp loses them. */
    zbc_device_t known;
    bool     have_model = false, have_color = false;
    uint16_t recorded   = features;
    const bool have_known = device_db_get(ieee, &known);
    if (have_known) {
        have_model = known.model[0] != '\0';
        for (int i = 0; i < known.ep_count; i++) {
            if (known.eps[i].ep == desc->ep_id) {
                have_color = known.eps[i].ct_min_mireds != ZBC_UNKNOWN;
                /* A colour-only lamp whose ColorCapabilities already said so keeps
                 * that answer: the provisional colour-temperature bit the cluster
                 * list implies is not put back on every announce. */
                const uint16_t f = known.eps[i].features;
                if ((f & ZBC_FEAT_ANY_COLOR) && !(f & ZBC_FEAT_COLOR_TEMP)) {
                    recorded &= (uint16_t)~ZBC_FEAT_COLOR_TEMP;
                }
                break;
            }
        }
    }

    if (have_known && ep_presses_only(features) && senses_motion(&known)) {
        /* See drop_presses_beside_motion; a record of it from before goes. */
        ESP_LOGI(TAG, "Device 0x%04x endpoint %d: a motion sensor's, not a remote", saddr, desc->ep_id);
        device_db_remove_endpoint(ieee, desc->ep_id);
        return;
    }

    ESP_LOGI(TAG, "Device 0x%04x endpoint %d is a %s (device id 0x%04x, features 0x%02x)",
             saddr, desc->ep_id,
             is_sensor ? "motion sensor"
                       : (is_light && is_switch ? "light and a switch" : (is_light ? "light" : "switch")),
             desc->app_device_id, features);
    device_db_add_endpoint(ieee, desc->ep_id, desc->app_device_id, recorded);
    if (is_sensor) {
        /* The simple descriptor is the authority here too: whatever the endpoint
         * was recorded as goes, but not what its reports taught us. */
        device_db_clear_features(ieee, desc->ep_id,
                                 (uint16_t)~(ZBC_FEAT_MOTION | ZBC_FEAT_MOTION_CLEARS));
        if (have_known) {
            drop_presses_beside_motion(ieee, &known);
        }
    } else if (is_switch && !is_light) {
        /* device_db_add_endpoint only adds bits, so clear them first: the simple
         * descriptor is the authority on what an endpoint is. The colour bits,
         * learned later and separately, are left alone. */
        device_db_clear_features(ieee, desc->ep_id, ZBC_FEAT_ONOFF);
    }

    /* Basic info lives on the first endpoint that has it; asking the one we are
     * going to control is good enough and keeps the traffic down. */
    if (!have_model) {
        read_basic_info(saddr, desc->ep_id);
    }

    if (is_switch) {
        /* A remote sends to whatever is in its own binding table, so without these it
         * is silent. Aqara reports unbound and refuses some of this; refusals are
         * logged, not treated as failures. A sleepy device is reliably awake only
         * now; one that misses this is bound on whichever neighbour scan catches
         * it listening. */
        if (has_multistate) {
            bind_for_reports(saddr, desc->ep_id, EZB_ZCL_CLUSTER_ID_MULTISTATE_INPUT);
            configure_reporting(saddr, desc->ep_id, EZB_ZCL_CLUSTER_ID_MULTISTATE_INPUT,
                                EZB_ZCL_ATTR_MULTISTATE_INPUT_PRESENT_VALUE_ID,
                                EZB_ZCL_ATTR_TYPE_UINT16, ZBC_REPORT_MIN_ONOFF_S);
        }
        if (has_onoff && !is_light) {
            bind_for_reports(saddr, desc->ep_id, EZB_ZCL_CLUSTER_ID_ON_OFF);
        }
        if (has_level && !is_light) {
            bind_for_reports(saddr, desc->ep_id, EZB_ZCL_CLUSTER_ID_LEVEL);
        }
    }

    if (is_sensor) {
        /* Most sensors report occupancy only to a binding, and a configured
         * maximum interval repeats the last value, which mends a lost
         * "unoccupied". A Xiaomi reports to the coordinator unbound and ignores
         * both; it is asleep again by the time an answer would be due. */
        bind_for_reports(saddr, desc->ep_id, EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING);
        configure_reporting(saddr, desc->ep_id, EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING,
                            EZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID, EZB_ZCL_ATTR_TYPE_MAP8,
                            ZBC_REPORT_MIN_ONOFF_S);
        return;
    }

    if (!is_light) {
        /* Nothing below applies to a remote, and a sleepy device would not answer it:
         * airtime spent for delivery failures that read as a bad link. */
        return;
    }

    bind_for_reports(saddr, desc->ep_id, EZB_ZCL_CLUSTER_ID_ON_OFF);
    configure_reporting(saddr, desc->ep_id, EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
                        EZB_ZCL_ATTR_TYPE_BOOL, ZBC_REPORT_MIN_ONOFF_S);
    if (features & ZBC_FEAT_LEVEL) {
        bind_for_reports(saddr, desc->ep_id, EZB_ZCL_CLUSTER_ID_LEVEL);
        configure_reporting(saddr, desc->ep_id, EZB_ZCL_CLUSTER_ID_LEVEL,
                            EZB_ZCL_ATTR_LEVEL_CURRENT_LEVEL_ID, EZB_ZCL_ATTR_TYPE_UINT8,
                            ZBC_REPORT_MIN_LEVEL_S);
        /* Interrogation runs again every time a device announces itself, so a
         * lamp that rejoins or is factory reset gets the fade time back. */
        write_onoff_transition(saddr, desc->ep_id, settings_transition_ds(), false);
    }
    if (!have_color) {
        read_static_color_info(saddr, desc->ep_id);
    }
    read_light_state(saddr, desc->ep_id, features);
    if (has_ota) {
        read_ota_client_info(saddr, desc->ep_id);
    }
}

static void active_ep_result(const ezb_zdo_active_ep_req_result_t *result, void *user_ctx)
{
    (void)user_ctx;
    if (!result || result->error != EZB_ERR_NONE || !result->rsp ||
        result->rsp->status != EZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "Active endpoint request failed");
        return;
    }

    uint16_t saddr = result->rsp->nwk_addr_of_interest;
    ESP_LOGI(TAG, "Device 0x%04x has %d endpoint(s)", saddr, result->rsp->active_ep_count);

    for (int i = 0; i < result->rsp->active_ep_count; i++) {
        ezb_zdo_simple_desc_req_t req = {
            .dst_nwk_addr = saddr,
            .field = {
                .nwk_addr_of_interest = saddr,
                .endpoint             = result->rsp->active_ep_list[i],
            },
            .cb = simple_desc_result,
        };
        ezb_zdo_simple_desc_req(&req);
    }
}

/* Rebuilds the registry from the coordinator's own neighbour table. Devices stay
 * joined when the stored registry is discarded, and would otherwise reappear
 * only after being power-cycled. Also catches a device never recorded. */
static void neighbour_table_result(const ezb_zdo_nwk_mgmt_lqi_req_result_t *result, void *user_ctx)
{
    (void)user_ctx;
    if (!result || result->error != EZB_ERR_NONE || !result->rsp ||
        result->rsp->status != EZB_ZDP_STATUS_SUCCESS) {
        /* Logged: this is what rebuilds the registry after the stored blob is
         * discarded. */
        ESP_LOGW(TAG, "Neighbour table request failed (error 0x%04x, status 0x%02x)",
                 result ? result->error : 0xffff,
                 (result && result->rsp) ? result->rsp->status : 0xff);
        return;
    }

    ESP_LOGI(TAG, "Neighbour table: %u entr%s", result->rsp->neighbor_table_entries,
             result->rsp->neighbor_table_entries == 1 ? "y" : "ies");
    for (int i = 0; i < result->rsp->neighbor_table_list_count; i++) {
        const ezb_zdp_nwk_mgmt_lqi_neighbor_table_entry_t *n = &result->rsp->neighbor_table_list[i];
        if (n->nwk_addr == 0x0000 || n->nwk_addr == ezb_nwk_get_short_address()) {
            continue;  /* ourselves */
        }
        uint8_t ieee[ZBC_IEEE_LEN];
        for (int b = 0; b < ZBC_IEEE_LEN; b++) {
            ieee[b] = n->extended_addr.u8[ZBC_IEEE_LEN - 1 - b];
        }

        /* A neighbour entry whose IEEE the router never learned comes through as
         * all ones. Registered, it would be a permanent offline phantom row that
         * every such neighbour maps to. */
        bool have_ieee = false;
        for (int b = 0; b < ZBC_IEEE_LEN && !have_ieee; b++) {
            have_ieee = ieee[b] != 0xff && ieee[b] != 0x00;
        }
        if (!have_ieee) {
            ESP_LOGD(TAG, "Neighbour 0x%04x has no IEEE address yet, skipping", n->nwk_addr);
            continue;
        }

        zbc_device_t existing;
        bool known = device_db_get(ieee, &existing) && existing.ep_count > 0;
        bool created = false;
        device_db_upsert(ieee, n->nwk_addr, &created);

        /* Link quality: a lamp that answers only sometimes is usually a radio
         * fault, not a software one. */
        ESP_LOGI(TAG, "Neighbour 0x%04x lqa %u/255%s", n->nwk_addr, n->lqa,
                 n->lqa < 100 ? " - weak, expect dropped commands" : "");
        /* Also on the device row, so the API can report it. */
        device_db_set_lqa(ieee, n->lqa);
        /* The same fact the announcement carries, for a device that joined
         * before this coordinator last rebooted and so never announced itself
         * to this run. */
        note_power_source(ieee, n->device_type == EZB_NWK_DEVICE_TYPE_END_DEVICE &&
                                    n->rx_on_when_idle == 0);
        if (!known) {
            ESP_LOGI(TAG, "Recovering device 0x%04x from the neighbour table", n->nwk_addr);
            interrogate_device(n->nwk_addr);
        } else {
            /* Bindings live on the lamp and cannot be queried, and interrogation is
             * skipped for a known device - after a restart, every device. So
             * reporting is re-established once per boot, on the first scan that
             * finds the device. */
            ensure_reports_bound(n->nwk_addr, &existing);
            ensure_color_info(n->nwk_addr, &existing);
        }
    }

    /* The table can span several responses. An empty page asks nothing more,
     * as in router_table_result: asking again from the same index would loop. */
    const unsigned next = (unsigned)result->rsp->start_index + result->rsp->neighbor_table_list_count;
    if (result->rsp->neighbor_table_list_count > 0 && next < result->rsp->neighbor_table_entries) {
        ezb_zdo_nwk_mgmt_lqi_req_t req = {
            .dst_nwk_addr = 0x0000,
            .field        = { .start_index = (uint8_t)next },
            .cb           = neighbour_table_result,
        };
        ezb_zdo_nwk_mgmt_lqi_req(&req);
    }
}

static void scan_neighbours(void)
{
    ezb_zdo_nwk_mgmt_lqi_req_t req = {
        .dst_nwk_addr = 0x0000,
        .field        = { .start_index = 0 },
        .cb           = neighbour_table_result,
    };
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_zdo_nwk_mgmt_lqi_req(&req);
    esp_zigbee_lock_release();
}

/* Runs in the Zigbee task (signal handler), so the stack lock is already held. */
static void interrogate_device(uint16_t short_addr)
{
    ezb_zdo_active_ep_req_t req = {
        .dst_nwk_addr = short_addr,
        .field        = { .nwk_addr_of_interest = short_addr },
        .cb           = active_ep_result,
    };
    if (ezb_zdo_active_ep_req(&req) != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Could not ask device 0x%04x for its endpoints", short_addr);
    }
}

static void read_basic_info(uint16_t short_addr, uint8_t ep)
{
    uint16_t attrs[] = {
        EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
        EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
    };
    ezb_zcl_read_attr_cmd_t cmd = {
        .cmd_ctrl = {
            .dst_addr   = { .addr_mode = EZB_ADDR_MODE_SHORT, .u.short_addr = short_addr },
            .dst_ep     = ep,
            .src_ep     = ZBC_GATEWAY_EP_ID,
            .cluster_id = EZB_ZCL_CLUSTER_ID_BASIC,
            .manuf_code = EZB_ZCL_STD_MANUF_CODE,
        },
        .payload = {
            .attr_number = sizeof(attrs) / sizeof(attrs[0]),
            .attr_field  = attrs,
        },
    };
    ezb_zcl_read_attr_cmd_req(&cmd);
}

/* Plain On/Off commands carry no transition time - the lamp uses its own Level
 * Control attributes instead. OnOffTransitionTime (0x0010) is the general one,
 * but OnTransitionTime (0x0012) and OffTransitionTime (0x0013) override it per
 * direction when they are set to anything other than 0xffff, so all three have
 * to be written or a lamp that carries the more specific pair keeps snapping.
 * The pair is optional, and a lamp that does not have it refuses just those two.
 * Caller holds the stack lock. */
static void write_onoff_transition(uint16_t short_addr, uint8_t ep, uint16_t ds, bool verify)
{
    uint16_t            value  = ds;
    ezb_zcl_attribute_t attrs[3] = {
        { .id   = EZB_ZCL_ATTR_LEVEL_ON_OFF_TRANSITION_TIME_ID,
          .data = { .type = EZB_ZCL_ATTR_TYPE_UINT16, .size = sizeof(value), .value = &value } },
        { .id   = EZB_ZCL_ATTR_LEVEL_ON_TRANSITION_TIME_ID,
          .data = { .type = EZB_ZCL_ATTR_TYPE_UINT16, .size = sizeof(value), .value = &value } },
        { .id   = EZB_ZCL_ATTR_LEVEL_OFF_TRANSITION_TIME_ID,
          .data = { .type = EZB_ZCL_ATTR_TYPE_UINT16, .size = sizeof(value), .value = &value } },
    };
    ezb_zcl_write_attr_cmd_t cmd = {
        .cmd_ctrl = {
            .dst_addr   = { .addr_mode = EZB_ADDR_MODE_SHORT, .u.short_addr = short_addr },
            .dst_ep     = ep,
            .src_ep     = ZBC_GATEWAY_EP_ID,
            .cluster_id = EZB_ZCL_CLUSTER_ID_LEVEL,
            .manuf_code = EZB_ZCL_STD_MANUF_CODE,
        },
        .payload = { .attr_number = 3, .attr_field = attrs },
    };
    ezb_err_t err = ezb_zcl_write_attr_cmd_req(&cmd);
    ESP_LOGI(TAG, "Set on/off fade %u.%us on 0x%04x ep %d (error 0x%04x)",
             ds / 10, ds % 10, short_addr, ep, err);

    /* Read them back: a write response says the command was accepted, not that
     * the value is in use. Only when asked (the settings path), not on every
     * join: an OSRAM PAR16 stores all three, reads them back correctly and
     * still snaps, which is why lighting.c ramps the level instead. */
    if (!verify) {
        return;
    }
    uint16_t which[3] = { EZB_ZCL_ATTR_LEVEL_ON_OFF_TRANSITION_TIME_ID,
                          EZB_ZCL_ATTR_LEVEL_ON_TRANSITION_TIME_ID,
                          EZB_ZCL_ATTR_LEVEL_OFF_TRANSITION_TIME_ID };
    read_attributes(short_addr, ep, EZB_ZCL_CLUSTER_ID_LEVEL, which, 3);
}

/* An undelivered read and a read the lamp ignored look the same from here: no
 * answer, and `audit` eventually calls the state unknown. Reads are sent with a
 * confirmation so the radio can be told from the device.
 *
 * Log-only, unlike `data_confirm_cb`: re-sending a command restates the user's
 * intent, re-sending a read only adds traffic to a link that is struggling. */
static void read_confirm_cb(ezb_af_user_cnf_t *cnf, void *user_ctx)
{
    (void)user_ctx;
    if (!cnf) {
        return;
    }
    reach_note(cnf->dst_addr.u.short_addr, cnf->status == 0);
    if (cnf->status == 0) {
        return;
    }
    s_radio.read_undelivered++;
    ESP_LOGW(TAG, "Read of 0x%04x ep %d was not delivered (status 0x%02x)",
             cnf->dst_addr.u.short_addr, cnf->dst_ep, cnf->status);
}

/* Confirmation of the ungated Stop. Log-only, and not data_confirm_cb: that one
 * advances the gate, which a command that never occupied it must not do - it
 * would let a queued command out while another is in flight. Not re-sent
 * either: the move-to-level sent on release is what puts the lamp right. */
static void stop_confirm_cb(ezb_af_user_cnf_t *cnf, void *user_ctx)
{
    (void)user_ctx;
    if (!cnf) {
        return;
    }
    reach_note(cnf->dst_addr.u.short_addr, cnf->status == 0);
    if (cnf->status == 0) {
        return;
    }
    s_radio.cmd_undelivered++;
    ESP_LOGW(TAG, "Stop for 0x%04x ep %d was not delivered (status 0x%02x)",
             cnf->dst_addr.u.short_addr, cnf->dst_ep, cnf->status);
}

static void read_attributes(uint16_t short_addr, uint8_t ep, uint16_t cluster_id, uint16_t *attrs, uint8_t count)
{
    /* Every read - settle, recovery, the refresh passes - comes through here,
     * and none of them is worth sending to a device that is not there. */
    if (!reach_admits(short_addr, false)) {
        return;
    }
    ezb_zcl_read_attr_cmd_t cmd = {
        .cmd_ctrl = {
            .dst_addr   = { .addr_mode = EZB_ADDR_MODE_SHORT, .u.short_addr = short_addr },
            .dst_ep     = ep,
            .src_ep     = ZBC_GATEWAY_EP_ID,
            .cluster_id = cluster_id,
            .manuf_code = EZB_ZCL_STD_MANUF_CODE,
            .cnf_ctx    = { .cb = read_confirm_cb },
        },
        .payload = { .attr_number = count, .attr_field = attrs },
    };
    /* A refused read never reaches the air and gets no confirmation. The stack
     * sometimes refuses without a reason, most often under a burst, which is
     * why refresh_all_states paces itself. */
    ezb_err_t err = ezb_zcl_read_attr_cmd_req(&cmd);
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Read of 0x%04x ep %d cluster 0x%04x refused by the stack (0x%04x)",
                 short_addr, ep, cluster_id, err);
    }
}

static void read_onoff_state(uint16_t short_addr, uint8_t ep)
{
    uint16_t attrs[] = { EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID };
    read_attributes(short_addr, ep, EZB_ZCL_CLUSTER_ID_ON_OFF, attrs, 1);
}

/* The firmware a lamp runs, from the attributes of its OTA Upgrade client:
 * CurrentFileVersion, ManufacturerID and ImageTypeID, which an image is matched
 * on. The client side is the device's, so the read goes "to client". Asked at
 * every interrogation. A sleepy remote is not asked: its Query Next Image says
 * the same when it wakes. */
static void read_ota_client_info(uint16_t short_addr, uint8_t ep)
{
    uint16_t                attrs[] = { 0x0002, 0x0007, 0x0008 };
    ezb_zcl_read_attr_cmd_t cmd     = {
        .cmd_ctrl = {
            .dst_addr   = { .addr_mode = EZB_ADDR_MODE_SHORT, .u.short_addr = short_addr },
            .dst_ep     = ep,
            .src_ep     = ZBC_GATEWAY_EP_ID,
            .cluster_id = EZB_ZCL_CLUSTER_ID_OTA_UPGRADE,
            .manuf_code = EZB_ZCL_STD_MANUF_CODE,
            .fc         = { .direction = 1 },
            .cnf_ctx    = { .cb = read_confirm_cb },
        },
        .payload = { .attr_number = sizeof(attrs) / sizeof(attrs[0]), .attr_field = attrs },
    };
    ezb_err_t err = ezb_zcl_read_attr_cmd_req(&cmd);
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Read of 0x%04x ep %d OTA client refused by the stack (0x%04x)", short_addr, ep, err);
    }
}

/* What a lamp will never change on its own: its capabilities and the physical
 * limits of its colour temperature. Asked for once, when the device is
 * interrogated, and persisted from there. */
static void read_static_color_info(uint16_t short_addr, uint8_t ep)
{
    uint16_t attrs[] = {
        EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_CAPABILITIES_ID,
        EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MIN_MIREDS_ID,
        EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MAX_MIREDS_ID,
    };
    read_attributes(short_addr, ep, EZB_ZCL_CLUSTER_ID_COLOR_CONTROL, attrs,
                    sizeof(attrs) / sizeof(attrs[0]));
}

/* Read back only what the command could have moved. This runs twice after
 * every command for every lamp involved, both radios share one antenna, and
 * reading everything every time makes lamps miss answers. */
/* Where the lamp's colour is, in one frame. Its own function because the
 * periodic re-read wants exactly this and none of the rest: on/off and level
 * are reported, colour is not. One list of attributes, so the two callers
 * cannot come to disagree about what "the colour" is. */
static void read_color_state(uint16_t short_addr, uint8_t ep, uint16_t features)
{
    if (!(features & (ZBC_FEAT_COLOR_TEMP | ZBC_FEAT_ANY_COLOR))) {
        return;
    }
    uint16_t attrs[] = {
        EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID,
        EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_MIREDS_ID,
        EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID,
        EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID,
    };
    read_attributes(short_addr, ep, EZB_ZCL_CLUSTER_ID_COLOR_CONTROL, attrs,
                    sizeof(attrs) / sizeof(attrs[0]));
}

static void read_light_state(uint16_t short_addr, uint8_t ep, uint16_t features)
{
    /* A remote has no light state and is asleep: a read would never be answered
     * and would only count in read_undelivered. */
    if (!(features & ZBC_FEAT_ONOFF)) {
        return;
    }

    read_onoff_state(short_addr, ep);

    if (features & ZBC_FEAT_LEVEL) {
        uint16_t attrs[] = { EZB_ZCL_ATTR_LEVEL_CURRENT_LEVEL_ID };
        read_attributes(short_addr, ep, EZB_ZCL_CLUSTER_ID_LEVEL, attrs, 1);
    }

    read_color_state(short_addr, ep, features);
}

static const uint16_t ZBC_BINDABLE[] = {
    EZB_ZCL_CLUSTER_ID_ON_OFF,
    EZB_ZCL_CLUSTER_ID_LEVEL,
    EZB_ZCL_CLUSTER_ID_MULTISTATE_INPUT,
    EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING,
};
#define ZBC_BINDABLE_CLUSTERS ((int)(sizeof(ZBC_BINDABLE) / sizeof(ZBC_BINDABLE[0])))
/* The most of them one endpoint wants (wanted_reports): a remote's three. A
 * lamp wants two and a sensor one, so the answer tables below are sized by this
 * rather than by every cluster there is - each cluster more would cost 3.5 KB
 * of RAM that no endpoint could ever use. */
#define ZBC_WANTED_PER_EP 3
/* zb_report_state() hands one endpoint's out over the API, so the two sizes are
 * one number; an endpoint wanting more without widening that struct would have
 * the rest dropped from the status silently. */
_Static_assert(ZBC_WANTED_PER_EP <= ZB_REPORT_MAX_CLUSTERS,
               "ZB_REPORT_MAX_CLUSTERS is smaller than ZBC_WANTED_PER_EP");

static uint32_t bind_cluster_index(uint16_t cluster_id)
{
    for (int i = 0; i < ZBC_BINDABLE_CLUSTERS; i++) {
        if (ZBC_BINDABLE[i] == cluster_id) {
            return (uint32_t)i;
        }
    }
    return 0;
}

/* Bindings and reporting configurations already answered this boot, so the
 * five-minute neighbour scan does not re-send them. Recorded when the device
 * answers (SUCCESS or a refusal), never on a send with no reply: silence is
 * worth asking again, a definite "no" is not. Runtime only; the lamp's own
 * tables are the record.
 *
 * Two tables because they are two answers: a Configure Reporting lost after a
 * successful bind leaves the lamp bound but silent, and must be asked again.
 *
 * Keyed by IEEE, which survives a rejoin; a short address can pass to another
 * device. `refused` keeps which answer it was, for the API: a refusal stops the
 * asking just the same. */
typedef struct {
    bool     used;
    bool     refused;
    uint8_t  ieee[ZBC_IEEE_LEN];
    uint8_t  ep;
    uint16_t cluster_id;
} zbc_done_t;

#define ZBC_DONE_ENTRIES (CONFIG_ZBC_MAX_DEVICES * CONFIG_ZBC_MAX_ENDPOINTS_PER_DEVICE * ZBC_WANTED_PER_EP)
static zbc_done_t s_bound[ZBC_DONE_ENTRIES];
static zbc_done_t s_configured[ZBC_DONE_ENTRIES];

/* The row holding this answer, or -1. done_mark never makes a second. */
static int done_find(const zbc_done_t *t, const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep,
                     uint16_t cluster_id)
{
    for (size_t i = 0; i < ZBC_DONE_ENTRIES; i++) {
        if (t[i].used && t[i].ep == ep && t[i].cluster_id == cluster_id &&
            memcmp(t[i].ieee, ieee, ZBC_IEEE_LEN) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool done_has_ieee(const zbc_done_t *t, const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep,
                         uint16_t cluster_id)
{
    return done_find(t, ieee, ep, cluster_id) >= 0;
}

static bool done_has(const zbc_done_t *t, uint16_t short_addr, uint8_t ep, uint16_t cluster_id)
{
    uint8_t ieee[ZBC_IEEE_LEN];
    return ieee_of(short_addr, ieee) && done_has_ieee(t, ieee, ep, cluster_id);
}

/* Whether the answer recorded for this endpoint and cluster was a refusal;
 * false while there is none. */
static bool done_refused_ieee(const zbc_done_t *t, const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep,
                              uint16_t cluster_id)
{
    const int i = done_find(t, ieee, ep, cluster_id);
    return i >= 0 && t[i].refused;
}

static void done_mark(zbc_done_t *t, uint16_t short_addr, uint8_t ep, uint16_t cluster_id,
                      bool refused)
{
    uint8_t ieee[ZBC_IEEE_LEN];
    if (done_has(t, short_addr, ep, cluster_id) || !ieee_of(short_addr, ieee)) {
        return;
    }
    for (size_t i = 0; i < ZBC_DONE_ENTRIES; i++) {
        if (!t[i].used) {
            t[i].refused    = refused;
            memcpy(t[i].ieee, ieee, ZBC_IEEE_LEN);
            t[i].ep         = ep;
            t[i].cluster_id = cluster_id;
            t[i].used       = true;      /* last: zb_report_state reads without a lock */
            return;
        }
    }
}

/* For an answer the device has since lost: a Hue dimmer that announces has been
 * reset or re-paired, and its binding table with it. */
static void done_forget(zbc_done_t *t, const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep,
                        uint16_t cluster_id)
{
    const int i = done_find(t, ieee, ep, cluster_id);
    if (i >= 0) {
        t[i].used = false;
    }
}

static bool bind_done(uint16_t short_addr, uint8_t ep, uint16_t cluster_id)
{
    return done_has(s_bound, short_addr, ep, cluster_id);
}

static void mark_bound(uint16_t short_addr, uint8_t ep, uint16_t cluster_id, bool refused)
{
    done_mark(s_bound, short_addr, ep, cluster_id, refused);
}

static bool config_done(uint16_t short_addr, uint8_t ep, uint16_t cluster_id)
{
    return done_has(s_configured, short_addr, ep, cluster_id);
}

static void mark_configured(uint16_t short_addr, uint8_t ep, uint16_t cluster_id, bool refused)
{
    done_mark(s_configured, short_addr, ep, cluster_id, refused);
}

/* Short address, endpoint and cluster packed into the ZDO callback's user_ctx,
 * which is all it carries - the result does not say who answered. The cluster
 * is an index into this list. */
#define BIND_CTX(addr, ep, cl) ((void *)(uintptr_t)((addr) | ((uint32_t)(ep) << 16) | \
                                                    (bind_cluster_index(cl) << 24)))

/* Asked for only when a bind is refused: "no table space" is a claim about the
 * device's own binding table, and the useful question is what is in it. */
static void dump_binding_table_cb(const ezb_zdo_nwk_mgmt_bind_req_result_t *result, void *user_ctx)
{
    const uint16_t addr = (uint16_t)(uintptr_t)user_ctx;
    if (!result || result->error != EZB_ERR_NONE || !result->rsp) {
        ESP_LOGW(TAG, "Could not read the binding table of 0x%04x", addr);
        return;
    }
    const ezb_zdp_nwk_mgmt_bind_rsp_field_t *r = result->rsp;
    ESP_LOGW(TAG, "Binding table of 0x%04x: %u entries total, showing %u from index %u",
             addr, r->binding_table_entries, r->binding_table_list_count, r->start_index);
    for (int i = 0; i < r->binding_table_list_count; i++) {
        const ezb_zdp_nwk_mgmt_bind_table_entry_t *e = &r->binding_table_list[i];
        uint8_t src[ZBC_IEEE_LEN], dst[ZBC_IEEE_LEN];
        for (int b = 0; b < ZBC_IEEE_LEN; b++) {
            src[b] = e->src_addr.u8[ZBC_IEEE_LEN - 1 - b];
            dst[b] = e->dst_addr.extended_addr.u8[ZBC_IEEE_LEN - 1 - b];
        }
        char ssrc[19], sdst[19];
        zbc_ieee_to_str(src, ssrc);
        zbc_ieee_to_str(dst, sdst);
        ESP_LOGW(TAG, "  [%d] %s ep %d cluster 0x%04x -> %s ep %d (mode %d)",
                 r->start_index + i, ssrc, e->src_ep, e->cluster_id, sdst, e->dst_ep,
                 e->dst_addr_mode);
    }
}

static void dump_binding_table(uint16_t short_addr)
{
    ezb_zdo_nwk_mgmt_bind_req_t req = {
        .dst_nwk_addr = short_addr,
        .field        = { .start_index = 0 },
        .cb           = dump_binding_table_cb,
        .user_ctx     = (void *)(uintptr_t)short_addr,
    };
    ezb_zdo_nwk_mgmt_bind_req(&req);
}

static void bind_result_cb(const ezb_zdp_bind_req_result_t *result, void *user_ctx)
{
    const uint32_t ctx        = (uint32_t)(uintptr_t)user_ctx;
    const uint16_t short_addr = (uint16_t)(ctx & 0xffff);
    const uint8_t  ep         = (uint8_t)((ctx >> 16) & 0xff);
    const uint32_t ci         = (ctx >> 24) & 0xff;
    const uint16_t cluster_id = ZBC_BINDABLE[ci < (uint32_t)ZBC_BINDABLE_CLUSTERS ? ci : 0];

    if (!result || result->error != EZB_ERR_NONE || !result->rsp) {
        ESP_LOGW(TAG, "Bind of 0x%04x ep %d cluster 0x%04x did not get through; will retry",
                 short_addr, ep, cluster_id);
        return;
    }
    if (result->rsp->status != EZB_ZDP_STATUS_SUCCESS) {
        /* An answered refusal is final for this boot. Retrying it every scan would
         * put a request on air every five minutes for something already declined.
         *
         * A Hue LST002 holds one binding: its table has only the On/Off binding to
         * us and it refuses the Level bind with TABLE_FULL, factory reset or not.
         * On/Off is bound first and so gets the slot, which is the right way
         * round: it is the state tracking depends on. */
        ESP_LOGW(TAG, "Device 0x%04x ep %d refused the bind for cluster 0x%04x (status 0x%02x)"
                      " - it will not report that cluster", short_addr, ep, cluster_id,
                 result->rsp->status);
        if (result->rsp->status == EZB_ZDP_STATUS_TABLE_FULL ||
            result->rsp->status == EZB_ZDP_STATUS_BINDING_TABLE_FULL) {
            dump_binding_table(short_addr);
        }
        mark_bound(short_addr, ep, cluster_id, true);
        return;
    }
    mark_bound(short_addr, ep, cluster_id, false);
    ESP_LOGI(TAG, "Bound 0x%04x ep %d cluster 0x%04x for reporting", short_addr, ep, cluster_id);
}

/* Configure Reporting is only half of it: a device sends its reports to the
 * entries in its own binding table, so without one pointing here nothing
 * arrives. Without reports a lamp changed by anything else - a wall switch,
 * another controller - goes unnoticed until the next command.
 *
 * The stack keeps EUI-64s little-endian; device_db stores them byte-reversed,
 * so both addresses are reversed back here. */
/* A ZDO bind of one cluster of one endpoint of a device to our endpoint, with
 * the answer going to `cb`. */
static void bind_to_us(uint16_t short_addr, uint8_t ep, uint16_t cluster_id,
                       ezb_zdo_bind_req_callback_t cb, void *ctx)
{
    uint8_t ieee[ZBC_IEEE_LEN];
    if (!ieee_of(short_addr, ieee)) {
        ESP_LOGW(TAG, "No IEEE address for 0x%04x yet; cannot bind", short_addr);
        return;
    }

    ezb_zdo_bind_req_t req = {
        .dst_nwk_addr = short_addr,
        .field = {
            .src_ep        = ep,
            .cluster_id    = cluster_id,
            .dst_addr_mode = 0x03,                  /* 64-bit, so dst_ep is present */
            .dst_ep        = ZBC_GATEWAY_EP_ID,
        },
        .cb       = cb,
        .user_ctx = ctx,
    };
    for (int i = 0; i < 8; i++) {
        req.field.src_addr.u8[i] = ieee[7 - i];
    }
    ezb_nwk_get_extended_address(&req.field.dst_addr.extended_addr);

    ezb_err_t err = ezb_zdo_bind_req(&req);
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Bind request for cluster 0x%04x could not be sent (0x%04x)", cluster_id, err);
    }
}

static void bind_for_reports(uint16_t short_addr, uint8_t ep, uint16_t cluster_id)
{
    bind_to_us(short_addr, ep, cluster_id, bind_result_cb, BIND_CTX(short_addr, ep, cluster_id));
}

/* ColorCapabilities and the physical mired limits are read once, at
 * interrogation. A lost answer leaves only the provisional colour bit implied by
 * the cluster list: no real capabilities (hue and saturation vanish from the
 * UI) and no limits for clamp_kelvin. So they are asked again on every scan
 * until answered, one read per scan. */
static void ensure_color_info(uint16_t short_addr, const zbc_device_t *dev)
{
    for (int i = 0; i < dev->ep_count; i++) {
        const zbc_endpoint_t *e = &dev->eps[i];
        if (!(e->features & (ZBC_FEAT_COLOR_TEMP | ZBC_FEAT_ANY_COLOR))) {
            continue;
        }
        if (e->ct_min_mireds != ZBC_UNKNOWN) {
            continue;                      /* already answered, once is enough */
        }
        ESP_LOGI(TAG, "Colour capabilities of 0x%04x ep %d never arrived; asking again",
                 short_addr, e->ep);
        read_static_color_info(short_addr, e->ep);
    }
}

/* Re-establish reporting for a device we already knew about, without dragging
 * the whole interrogation chain along - the endpoints, model and capabilities
 * are already in the registry; only the bindings are missing. */
/* One cluster of one endpoint: bind it unless the bind has been answered, and
 * configure its reporting unless that has - two answers, tracked apart, so a
 * configure lost after a successful bind is asked again on the next scan. */
static void ensure_cluster_reports(uint16_t short_addr, uint8_t ep, uint16_t cluster_id,
                                   uint16_t attr_id, uint8_t attr_type, uint16_t min_interval_s,
                                   bool configure, bool may_bind)
{
    if (may_bind && !bind_done(short_addr, ep, cluster_id)) {
        ESP_LOGI(TAG, "Re-establishing the binding of 0x%04x ep %d cluster 0x%04x",
                 short_addr, ep, cluster_id);
        bind_for_reports(short_addr, ep, cluster_id);
    }
    if (configure && !config_done(short_addr, ep, cluster_id)) {
        ESP_LOGI(TAG, "Re-establishing reporting for 0x%04x ep %d cluster 0x%04x",
                 short_addr, ep, cluster_id);
        configure_reporting(short_addr, ep, cluster_id, attr_id, attr_type, min_interval_s);
    }
}

/* What an endpoint wants established, from its features alone. The one
 * statement of the rule, shared by the retry that establishes bindings and
 * reporting and by `zb_report_state()`, so the two cannot disagree. At most
 * ZBC_WANTED_PER_EP entries, which sizes the caller's array: a remote wants
 * three, a lamp two, a sensor one. */
typedef struct {
    uint16_t    cluster_id;
    const char *name;
    uint16_t    attr_id;
    uint8_t     attr_type;
    uint16_t    min_interval_s;
    bool        configure;    /* false: bound only, nothing to report */
} zbc_report_want_t;

static int wanted_reports(uint16_t features, zbc_report_want_t *out)
{
    int n = 0;
    /* A sensor reports occupancy and is asked nothing else. */
    if (features & ZBC_FEAT_MOTION) {
        out[n++] = (zbc_report_want_t){ EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING, "occupancy",
                                        EZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID,
                                        EZB_ZCL_ATTR_TYPE_MAP8, ZBC_REPORT_MIN_ONOFF_S, true };
        return n;
    }
    /* A remote's bindings are what make it send at all, and a battery one is
     * asleep most of the time: the repeating scan is what catches it awake. */
    if (features & ZBC_FEAT_SWITCH) {
        out[n++] = (zbc_report_want_t){ EZB_ZCL_CLUSTER_ID_MULTISTATE_INPUT, "switch",
                                        EZB_ZCL_ATTR_MULTISTATE_INPUT_PRESENT_VALUE_ID,
                                        EZB_ZCL_ATTR_TYPE_UINT16, ZBC_REPORT_MIN_ONOFF_S, true };
    }
    if (ep_presses_only(features)) {
        /* Nothing else applies to a remote, and a sleepy device would not answer
         * it. */
        out[n++] = (zbc_report_want_t){ EZB_ZCL_CLUSTER_ID_ON_OFF, "onoff", 0, 0, 0, false };
        out[n++] = (zbc_report_want_t){ EZB_ZCL_CLUSTER_ID_LEVEL,  "level", 0, 0, 0, false };
        return n;
    }
    out[n++] = (zbc_report_want_t){ EZB_ZCL_CLUSTER_ID_ON_OFF, "onoff",
                                    EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, EZB_ZCL_ATTR_TYPE_BOOL,
                                    ZBC_REPORT_MIN_ONOFF_S, true };
    if (features & ZBC_FEAT_LEVEL) {
        out[n++] = (zbc_report_want_t){ EZB_ZCL_CLUSTER_ID_LEVEL, "level",
                                        EZB_ZCL_ATTR_LEVEL_CURRENT_LEVEL_ID,
                                        EZB_ZCL_ATTR_TYPE_UINT8, ZBC_REPORT_MIN_LEVEL_S, true };
    }
    return n;
}

static void ensure_reports_bound(uint16_t short_addr, const zbc_device_t *dev)
{
    const bool may_bind = bind_scan_take(dev->ieee);
    for (int i = 0; i < dev->ep_count; i++) {
        zbc_report_want_t want[ZBC_WANTED_PER_EP];
        const int         n = wanted_reports(dev->eps[i].features, want);
        for (int w = 0; w < n; w++) {
            ensure_cluster_reports(short_addr, dev->eps[i].ep, want[w].cluster_id,
                                   want[w].attr_id, want[w].attr_type,
                                   want[w].min_interval_s, want[w].configure, may_bind);
        }
    }
}

static void configure_reporting(uint16_t short_addr, uint8_t ep, uint16_t cluster_id, uint16_t attr_id,
                                uint8_t attr_type, uint16_t min_interval_s)
{
    ezb_zcl_config_report_record_t record = {
        .direction = EZB_ZCL_REPORTING_SEND,
        .attr_id   = attr_id,
        .client = {
            .attr_type    = attr_type,
            .min_interval = min_interval_s,
            .max_interval = ZBC_REPORT_MAX_INTERVAL_S,
            /* Analog attributes need a reportable change; 1 step is fine and
             * discrete types ignore it. */
            .reportable_change = { .u8 = 1 },
        },
    };
    ezb_zcl_config_report_cmd_t cmd = {
        .cmd_ctrl = {
            .dst_addr   = { .addr_mode = EZB_ADDR_MODE_SHORT, .u.short_addr = short_addr },
            .dst_ep     = ep,
            .src_ep     = ZBC_GATEWAY_EP_ID,
            .cluster_id = cluster_id,
            .manuf_code = EZB_ZCL_STD_MANUF_CODE,
        },
        .payload = { .record_number = 1, .record_field = &record },
    };
    ezb_zcl_config_report_cmd_req(&cmd);
}

/* ---------------------------------------------------------------- remotes */

static zb_remote_cb_t   s_remote_cb;
static zb_remote_log_t  s_remote_log[ZBC_REMOTE_LOG_LEN];
static uint8_t          s_remote_next;   /* where the next entry goes */
static uint32_t         s_remote_seq;
static portMUX_TYPE     s_remote_mux = portMUX_INITIALIZER_UNLOCKED;

void zb_set_remote_handler(zb_remote_cb_t cb)
{
    s_remote_cb = cb;
}

static zb_remote_frame_t s_frames[ZBC_REMOTE_FRAMES_LEN];
static uint8_t           s_frames_next;
static uint8_t           s_frames_used;

/* Zigbee task; the copy-out is from the HTTP task, hence the spinlock. */
static void trace_remote_frame(uint16_t short_addr, const ezb_zcl_raw_frame_t *raw)
{
    const ezb_zcl_cmd_hdr_t *h = raw->header;
    portENTER_CRITICAL(&s_remote_mux);
    zb_remote_frame_t *f = &s_frames[s_frames_next];
    memset(f, 0, sizeof(*f));
    f->at_us       = esp_timer_get_time();
    f->short_addr  = short_addr;
    f->ep          = h->src_ep;
    f->cluster     = h->cluster_id;
    f->cmd         = h->cmd_id;
    f->tsn         = h->tsn;
    f->payload_len = raw->payload_length > 255 ? 255 : (uint8_t)raw->payload_length;
    if (raw->payload) {
        memcpy(f->payload, raw->payload,
               raw->payload_length < sizeof(f->payload) ? raw->payload_length : sizeof(f->payload));
    }
    s_frames_next = (uint8_t)((s_frames_next + 1) % ZBC_REMOTE_FRAMES_LEN);
    if (s_frames_used < ZBC_REMOTE_FRAMES_LEN) {
        s_frames_used++;
    }
    portEXIT_CRITICAL(&s_remote_mux);
}

size_t zb_recent_remote_frames(zb_remote_frame_t *out, size_t max)
{
    if (!out || max == 0) {
        return 0;
    }
    size_t n = 0;
    portENTER_CRITICAL(&s_remote_mux);
    const int used  = s_frames_used;
    const int first = (s_frames_next - used + ZBC_REMOTE_FRAMES_LEN) % ZBC_REMOTE_FRAMES_LEN;
    for (int i = 0; i < used && n < max; i++) {
        out[n++] = s_frames[(first + i) % ZBC_REMOTE_FRAMES_LEN];
    }
    portEXIT_CRITICAL(&s_remote_mux);
    return n;
}

size_t zb_recent_remote_events(zb_remote_log_t *out, size_t max)
{
    if (!out || max == 0) {
        return 0;
    }
    size_t n = 0;
    portENTER_CRITICAL(&s_remote_mux);
    for (int i = 0; i < ZBC_REMOTE_LOG_LEN && n < max; i++) {
        /* Backwards from the newest, wrapping. */
        const int idx = (s_remote_next - 1 - i + 2 * ZBC_REMOTE_LOG_LEN) % ZBC_REMOTE_LOG_LEN;
        if (s_remote_log[idx].seq == 0) {
            break;
        }
        out[n++] = s_remote_log[idx];
    }
    portEXIT_CRITICAL(&s_remote_mux);
    return n;
}

/* Runs in the Zigbee task with the stack lock held; everything it calls is
 * spinlock-only for that reason.
 *
 * `quiet` skips the log line and the capture ring and only passes the event on:
 * a turning wheel sends ten frames a second, which would bury the log and push
 * every other press out of the eight-entry ring the editor learns keys from. */
static void note_remote_event(uint16_t short_addr, uint8_t ep, uint8_t key, zb_remote_ev_t ev,
                              bool quiet, const ezb_zcl_raw_frame_t *raw)
{
    const ezb_zcl_cmd_hdr_t *h = raw ? raw->header : NULL;
    uint8_t ieee[ZBC_IEEE_LEN];
    if (!ieee_of(short_addr, ieee)) {
        ESP_LOGW(TAG, "Press from 0x%04x, which is not a device we know", short_addr);
        return;
    }
    if (quiet) {
        if (s_remote_cb) {
            s_remote_cb(ieee, ep, key, ev);
        }
        return;
    }

    /* A release names no key (see ZB_REMOTE_KEY_ANY), so it stays out of the
     * capture log, where it would overwrite the hold being captured. */
    if (ev != ZB_REMOTE_RELEASE) {
        portENTER_CRITICAL(&s_remote_mux);
        zb_remote_log_t *e = &s_remote_log[s_remote_next];
        e->seq = ++s_remote_seq;
        memcpy(e->ieee, ieee, ZBC_IEEE_LEN);
        e->ep    = ep;
        e->key   = key;
        e->ev    = (uint8_t)ev;
        e->at_us = esp_timer_get_time();
        e->cluster = h ? h->cluster_id : 0xFFFF;
        e->cmd     = h ? h->cmd_id : 0;
        e->tsn     = h ? h->tsn : 0;
        e->group   = (h && h->dst_addr.addr_mode == EZB_ADDR_MODE_GROUP)
                         ? h->dst_addr.u.group_addr.group : 0xFFFF;
        e->payload_len = 0;
        memset(e->payload, 0, sizeof(e->payload));
        if (raw && raw->payload) {
            e->payload_len = raw->payload_length > 255 ? 255 : (uint8_t)raw->payload_length;
            memcpy(e->payload, raw->payload,
                   raw->payload_length < sizeof(e->payload) ? raw->payload_length
                                                            : sizeof(e->payload));
        }
        s_remote_next = (uint8_t)((s_remote_next + 1) % ZBC_REMOTE_LOG_LEN);
        portEXIT_CRITICAL(&s_remote_mux);
        zbc_state_bump();             /* the editor is watching this ring */
    }

    ESP_LOGI(TAG, "Remote 0x%04x ep %d key %d: %s", short_addr, ep, key,
             remote_event_name(ev));

    if (s_remote_cb) {
        s_remote_cb(ieee, ep, key, ev);
    }
}

/* True when we believe this endpoint sends button presses, so that a lamp
 * reporting On/Off is never mistaken for somebody pressing something. Asked once
 * per attribute of every report, which is why it is the cheap lookup and not
 * device_db_get - that copies the whole device row onto this task's stack. */
static bool endpoint_is_switch(uint16_t short_addr, uint8_t ep)
{
    return (device_db_features_by_short(short_addr, ep) & ZBC_FEAT_SWITCH) != 0;
}

/* A Hue dimmer's two endpoints: the one its standard commands come from, where
 * its buttons are filed, and the one Philips' button cluster is on. See
 * hue_bind_post. */
#define HUE_DIMMER_BUTTON_EP 1
#define HUE_DIMMER_NOTIFY_EP 2
#define HUE_MANUF_CODE       0x100B     /* Signify */
static zb_hue_stats_t s_hue = { .bind_status = -1 };

/* Which remote this is, for remote.c: some commands mean one thing from one
 * model and another from the next. A device not interrogated yet has no model,
 * and decodes by command alone. */
static zb_remote_model_t remote_model_by_short(uint16_t short_addr)
{
    char model[ZBC_STRING_LEN];
    device_db_model_by_short(short_addr, model);
    return remote_model(model);
}

/* A frame on Philips' button cluster from a Hue dimmer. The model is looked up
 * only for that cluster, and must be: 0xFC00 is the first manufacturer-specific
 * cluster number, so another maker's frame on it is something else entirely. */
static bool hue_dimmer_frame(uint16_t short_addr, uint16_t cluster_id)
{
    return cluster_id == ZB_REMOTE_CL_HUE &&
           remote_model_by_short(short_addr) == ZB_REMOTE_MODEL_HUE_DIMMER;
}

/* Whether a dimmer's presses are read from Philips' cluster now - see
 * hue_note_notify. */
static bool hue_buttons_on(uint16_t short_addr)
{
    return (device_db_features_by_short(short_addr, HUE_DIMMER_BUTTON_EP) & ZBC_FEAT_HUE_BUTTONS) != 0;
}

/* The groups an IKEA BILRESA (E2490) sends to in Zigbee mode, one per channel
 * of its channel button: 21658, 21659, 21660. They are fixed in the remote, and
 * a bind request has no effect on it, so the only way to hear it is to be in
 * all three. Nothing else here uses groups - lamps are fanned out to, never
 * group-cast at - so membership costs nothing but the frames it lets in. */
static const uint16_t ZBC_REMOTE_GROUPS[] = { 0x549A, 0x549B, 0x549C };

/* Membership is asked for, at every network-up, with an AddGroup to our own
 * endpoint. AddGroup is idempotent, and the answer is logged by
 * zcl_core_action_handler, which is how to tell that it took. */
static void join_remote_groups(void)
{
    for (size_t i = 0; i < sizeof(ZBC_REMOTE_GROUPS) / sizeof(ZBC_REMOTE_GROUPS[0]); i++) {
        ezb_zcl_groups_add_group_cmd_t cmd = {
            .cmd_ctrl = {
                .dst_addr   = { .addr_mode = EZB_ADDR_MODE_SHORT,
                                .u.short_addr = ezb_nwk_get_short_address() },
                .dst_ep     = ZBC_GATEWAY_EP_ID,
                .src_ep     = ZBC_GATEWAY_EP_ID,
            },
            .payload = { .group_id = ZBC_REMOTE_GROUPS[i], .group_name = NULL },
        };
        ezb_err_t err = ezb_zcl_groups_add_group_cmd_req(&cmd);
        if (err != EZB_ERR_NONE) {
            ESP_LOGW(TAG, "Could not ask to join group 0x%04x (0x%04x) - a BILRESA on that"
                          " channel will not be heard", ZBC_REMOTE_GROUPS[i], err);
        }
    }
}

/* One per scroll wheel heard from. Written only from the Zigbee task, so no
 * lock. A slot is only a direction and a last level; one taken over from an
 * idle remote costs that remote a tenth of a second. */
static struct {
    uint16_t   short_addr;
    uint8_t    ep;
    uint8_t    used;
    int64_t    seen_us;
    zb_wheel_t w;
} s_wheels[4];

static int wheel_slot(uint16_t short_addr, uint8_t ep, bool create)
{
    int free_i = -1, oldest = 0;
    for (int i = 0; i < (int)(sizeof(s_wheels) / sizeof(s_wheels[0])); i++) {
        if (s_wheels[i].used && s_wheels[i].short_addr == short_addr && s_wheels[i].ep == ep) {
            return i;
        }
        if (!s_wheels[i].used && free_i < 0) {
            free_i = i;
        }
        if (s_wheels[i].seen_us < s_wheels[oldest].seen_us) {
            oldest = i;
        }
    }
    if (!create) {
        return -1;
    }
    const int i = free_i >= 0 ? free_i : oldest;
    memset(&s_wheels[i], 0, sizeof(s_wheels[i]));
    s_wheels[i].used       = 1;
    s_wheels[i].short_addr = short_addr;
    s_wheels[i].ep         = ep;
    return i;
}

/* When each Tuya scene switch last sent its own count of a gesture, for
 * remote_is_echo. Zigbee task only, so no lock; a slot taken over from an idle
 * remote costs it nothing but one echo, decoded as a press. */
static struct {
    uint16_t short_addr;
    uint8_t  ep;
    uint8_t  used;
    int64_t  at_us;
} s_tuya_actions[4];

static void tuya_action_note(uint16_t short_addr, uint8_t ep, int64_t now)
{
    int pick = 0;
    for (int i = 0; i < (int)(sizeof(s_tuya_actions) / sizeof(s_tuya_actions[0])); i++) {
        if (s_tuya_actions[i].used && s_tuya_actions[i].short_addr == short_addr &&
            s_tuya_actions[i].ep == ep) {
            pick = i;
            break;
        }
        if (s_tuya_actions[i].at_us < s_tuya_actions[pick].at_us) {
            pick = i;   /* unused slots are 0, so they go first */
        }
    }
    s_tuya_actions[pick].used       = 1;
    s_tuya_actions[pick].short_addr = short_addr;
    s_tuya_actions[pick].ep         = ep;
    s_tuya_actions[pick].at_us      = now;
}

/* How long ago, or -1 for not heard. */
static int64_t tuya_action_age(uint16_t short_addr, uint8_t ep, int64_t now)
{
    for (int i = 0; i < (int)(sizeof(s_tuya_actions) / sizeof(s_tuya_actions[0])); i++) {
        if (s_tuya_actions[i].used && s_tuya_actions[i].short_addr == short_addr &&
            s_tuya_actions[i].ep == ep) {
            return now - s_tuya_actions[i].at_us;
        }
    }
    return -1;
}

/* --------------------------------------------- what the radio hears from each device */

/* >>> host-test region: linkgap <<< */

/* How many frames a device numbered between the last one heard from it and this
 * one. A device gives every frame it sends of its own accord the next ZCL
 * sequence number, wrapping at 256, so a step of two means one frame lost. This
 * is the only measure of the receiving side, which a remote's press (one frame,
 * never repeated) depends on entirely.
 *
 * A step beyond LINK_GAP_MAX + 1 counts as nothing: a device that rebooted,
 * rejoined or restarted its counter, or one that also sends elsewhere (a remote
 * bound to a lamp too). A step of zero is a retransmission let through twice. */
#define LINK_GAP_MAX 15

static unsigned link_gap(uint8_t last_tsn, uint8_t tsn)
{
    const unsigned step = (uint8_t)(tsn - last_tsn);
    if (step == 0 || step > LINK_GAP_MAX + 1) {
        return 0;
    }
    return step - 1;
}

/* >>> end host-test region: linkgap <<< */

/* >>> host-test region: reach <<< */

/* Whether a device is still there to be talked to.
 *
 * A command to an unplugged lamp is retried by MAC and APS and route-searched
 * with a network-wide broadcast, then followed by settle reads, re-sends and
 * recovery reads, each handled the same way - airtime the radio needs to hear
 * the next remote press.
 *
 * So a device whose last ZBC_UNREACH_FAILS frames all went undelivered, with
 * nothing heard from it since the first, is left alone: no reads, and a command
 * at most once every ZBC_UNREACH_PROBE_US. That command is the probe; its
 * delivery, or anything heard from the device (an announce above all), brings
 * it back. Three, because the stack has already retried each frame: a poor link
 * loses single frames, not three in a row with nothing heard in between.
 *
 * Failures closer together than ZBC_UNREACH_BURST_US count once: a settle pass
 * sends three reads back to back, and one bad moment fails all three.
 *
 * The probe is one intent, not one frame: commands within ZBC_UNREACH_INTENT_US
 * of it go too (a level and its colour; the warm-off's dim, colour and off), or
 * a lamp that is back is left half-changed. */
#define ZBC_UNREACH_FAILS     3
#define ZBC_UNREACH_PROBE_US  (30LL * 1000 * 1000)
#define ZBC_UNREACH_BURST_US  (500LL * 1000)
#define ZBC_UNREACH_INTENT_US (100LL * 1000)

typedef struct {
    uint8_t fails;           /* consecutive frames undelivered */
    int64_t last_fail_us;    /* the last failure that counted */
    int64_t last_probe_us;   /* the last command let through while gone */
} zbc_reach_t;

/* A frame the stack gave up on. `heard_us` is when the device was last heard:
 * heard since the previous failure, and the run starts over. */
static void reach_fail(zbc_reach_t *r, int64_t now_us, int64_t heard_us)
{
    if (heard_us > r->last_fail_us) {
        r->fails = 0;
    } else if (r->fails > 0 && now_us - r->last_fail_us < ZBC_UNREACH_BURST_US) {
        return;              /* the same moment of the same outage */
    }
    if (r->fails < 255) {
        r->fails++;
    }
    r->last_fail_us = now_us;
}

/* A frame the stack delivered: whatever the run was, it is over. */
static void reach_ok(zbc_reach_t *r)
{
    r->fails = 0;
}

static bool reach_gone(const zbc_reach_t *r, int64_t heard_us)
{
    return r->fails >= ZBC_UNREACH_FAILS && heard_us <= r->last_fail_us;
}

/* Whether a command may go on air now, noting it as the probe when the device
 * is gone. Reads never probe: a read to a device that is not there is all cost. */
static bool reach_may_command(zbc_reach_t *r, int64_t now_us, int64_t heard_us)
{
    if (!reach_gone(r, heard_us)) {
        return true;
    }
    if (r->last_probe_us != 0 && now_us - r->last_probe_us < ZBC_UNREACH_PROBE_US) {
        /* The rest of the probe's own request goes with it. */
        return now_us - r->last_probe_us < ZBC_UNREACH_INTENT_US;
    }
    r->last_probe_us = now_us;
    return true;
}

/* >>> end host-test region: reach <<< */

/* One row per device a frame has been sent to, keyed by short address (what a
 * confirmation carries). Written from the Zigbee task and from whichever task
 * sends (gate_send runs in the caller's task and takes the stack lock only
 * around the send), so every access is under s_reach_mux, and nothing that
 * blocks or logs runs inside it. A device that rejoins with a new short address
 * starts a fresh row, and its announce counts as heard. */
static struct {
    uint16_t    short_addr;
    bool        used;
    bool        said_gone;    /* the log has said so, and must say when it is back */
    zbc_reach_t r;
} s_reach[ZBC_COMMAND_SLOTS];
static portMUX_TYPE s_reach_mux = portMUX_INITIALIZER_UNLOCKED;

/* Under s_reach_mux. -1 when there is no row and `create` is false. */
static int reach_row_locked(uint16_t short_addr, bool create)
{
    int spare = -1;
    for (int i = 0; i < ZBC_COMMAND_SLOTS; i++) {
        if (s_reach[i].used && s_reach[i].short_addr == short_addr) {
            return i;
        }
        if (spare < 0 && !s_reach[i].used) {
            spare = i;
        }
    }
    if (!create) {
        return -1;
    }
    if (spare < 0) {
        /* Full: a row with the shortest run of failures is the one to forget. */
        spare = 0;
        for (int i = 1; i < ZBC_COMMAND_SLOTS; i++) {
            if (s_reach[i].r.fails < s_reach[spare].r.fails) {
                spare = i;
            }
        }
    }
    memset(&s_reach[spare], 0, sizeof(s_reach[spare]));
    s_reach[spare].used       = true;
    s_reach[spare].short_addr = short_addr;
    return spare;
}

/* Under s_reach_mux: records whether row i is gone, and returns +1 when it has
 * just become so, -1 when it has just come back, 0 otherwise - for reach_tell,
 * which says it outside the lock. */
static int reach_changed_locked(int i, int64_t seen_us)
{
    const bool gone = reach_gone(&s_reach[i].r, seen_us);
    if (gone == s_reach[i].said_gone) {
        return 0;
    }
    s_reach[i].said_gone = gone;
    return gone ? 1 : -1;
}

/* The log line and the registry's `online`, once per episode each way. Back
 * online too: a delivered probe is the device answering, and a lamp that never
 * reports would otherwise stay offline until a neighbour scan listed it. */
static void reach_tell(uint16_t short_addr, int change)
{
    if (!change) {
        return;
    }
    if (change > 0) {
        ESP_LOGW(TAG, "Device 0x%04x has not taken its last %d frames and has not been heard;"
                      " it is sent a command at most every %lld s and not read until it answers",
                 short_addr, ZBC_UNREACH_FAILS, ZBC_UNREACH_PROBE_US / 1000000);
    } else {
        ESP_LOGI(TAG, "Device 0x%04x is answering again", short_addr);
    }
    uint8_t ieee[ZBC_IEEE_LEN];
    if (ieee_of(short_addr, ieee)) {
        device_db_set_online(ieee, change < 0);
    }
}

/* Every row dropped. What went undelivered while the coordinator was on another
 * channel - moving the network, or measuring the band - says nothing about the
 * devices, and kept, it would hold a healthy lamp to one frame in 30 s. A device
 * the log already called gone keeps its registry `online` until it is heard. */
static void reach_forget_all(void)
{
    portENTER_CRITICAL(&s_reach_mux);
    memset(s_reach, 0, sizeof(s_reach));
    portEXIT_CRITICAL(&s_reach_mux);
}

/* When the device was last heard, asked of the registry only when it matters:
 * reach_gone() reads it only for a row with a full run of failures, and a
 * healthy device has no row. 0 ("never") otherwise, which agrees with the row if
 * a failure completes the run in between: reach_note has just called it gone. */
static int64_t reach_heard_us(uint16_t short_addr)
{
    portENTER_CRITICAL(&s_reach_mux);
    const int  i    = reach_row_locked(short_addr, false);
    const bool asks = i >= 0 && s_reach[i].r.fails >= ZBC_UNREACH_FAILS;
    portEXIT_CRITICAL(&s_reach_mux);
    return asks ? device_db_last_seen_by_short(short_addr) : 0;
}

/* What a confirmation says about the device it was for. A delivery ends any
 * run of failures whatever was heard, so only a failure asks when that was. */
static void reach_note(uint16_t short_addr, bool delivered)
{
    const int64_t seen   = delivered ? 0 : device_db_last_seen_by_short(short_addr);
    int           change = 0;
    portENTER_CRITICAL(&s_reach_mux);
    const int i = reach_row_locked(short_addr, !delivered);
    if (i >= 0) {
        if (delivered) {
            reach_ok(&s_reach[i].r);
        } else {
            reach_fail(&s_reach[i].r, esp_timer_get_time(), seen);
        }
        change = reach_changed_locked(i, seen);
    }
    portEXIT_CRITICAL(&s_reach_mux);
    reach_tell(short_addr, change);
}

/* Whether anything may be sent to this device now: `command` true for a
 * command (let through as the probe once per interval), false for a read. */
static bool reach_admits(uint16_t short_addr, bool command)
{
    const int64_t seen   = reach_heard_us(short_addr);
    bool          ok     = true;
    int           change = 0;
    portENTER_CRITICAL(&s_reach_mux);
    const int i = reach_row_locked(short_addr, false);
    if (i >= 0) {
        ok = command ? reach_may_command(&s_reach[i].r, esp_timer_get_time(), seen)
                     : !reach_gone(&s_reach[i].r, seen);
        change = reach_changed_locked(i, seen);
    }
    if (!ok) {
        s_radio.sends_skipped++;
    }
    portEXIT_CRITICAL(&s_reach_mux);
    reach_tell(short_addr, change);
    return ok;
}

/* Whether the device has stopped answering, asking nothing and changing nothing. */
static bool reach_is_gone(uint16_t short_addr)
{
    const int64_t seen = reach_heard_us(short_addr);
    portENTER_CRITICAL(&s_reach_mux);
    const int  i    = reach_row_locked(short_addr, false);
    const bool gone = i >= 0 && reach_gone(&s_reach[i].r, seen);
    portEXIT_CRITICAL(&s_reach_mux);
    return gone;
}

/* >>> host-test region: parent <<< */

/* Which device an end device hangs off, and so which link its frames cross
 * last. A sleepy device picks its parent itself whenever it rejoins - after its
 * parent stopped answering it, for instance while this board restarted - and
 * then stays. For a remote that group-casts that choice is most of its
 * reliability: as our child every frame it sends us is acknowledged and
 * retried, as a router's child it reaches us as that router's one
 * unacknowledged broadcast. Nothing on air says when it moves, so it is looked
 * up: in our own neighbour table at every walk, and in each router's table
 * (Mgmt_Lqi) straight after.
 *
 * Our own table is the authority on whether a device is ours - it is where the
 * device polls - so a router that still lists it (a stale entry, until that
 * router's aging or a Parent Announce clears it) is not believed while it is.
 * A router's claim stands until another replaces it: an answer lost on the way
 * must not make a remote look orphaned for five minutes. Known only from TABLE
 * (then the parent is us) or ROUTER (then it is `addr`), so a zeroed row is an
 * unknown one. */
#define ZBC_PARENT_US      0x0000   /* the coordinator's short address, always */
#define ZBC_PARENT_UNKNOWN 0xFFFF   /* the broadcast address: never a parent */

typedef enum {
    ZBC_PARENT_SRC_NONE = 0,   /* never learned */
    ZBC_PARENT_SRC_TABLE,      /* our own table lists it as our child */
    ZBC_PARENT_SRC_ROUTER,     /* a router's table lists it as that router's child */
    ZBC_PARENT_SRC_GONE,       /* ours until a walk that no longer listed it */
    ZBC_PARENT_SRC_LEFT,       /* it said it was leaving */
} zbc_parent_src_t;

typedef struct {
    uint16_t addr;   /* the router, for ZBC_PARENT_SRC_ROUTER */
    uint8_t  src;    /* zbc_parent_src_t */
    bool     ours;   /* listed by our own table at its last walk */
} zbc_parent_t;

static uint16_t parent_addr(const zbc_parent_t *p)
{
    return p->src == ZBC_PARENT_SRC_TABLE    ? ZBC_PARENT_US
           : p->src == ZBC_PARENT_SRC_ROUTER ? p->addr
                                             : ZBC_PARENT_UNKNOWN;
}

/* What our own walk said: whether it listed the device as our child. True when
 * the parent changed. A device a router holds is left with it: our table not
 * listing it says nothing about theirs. */
static bool parent_walked(zbc_parent_t *p, bool listed)
{
    const uint16_t was = parent_addr(p);
    p->ours = listed;
    if (listed) {
        p->src  = ZBC_PARENT_SRC_TABLE;
        p->addr = ZBC_PARENT_US;
    } else if (p->src == ZBC_PARENT_SRC_TABLE) {
        p->src = ZBC_PARENT_SRC_GONE;
    }
    return parent_addr(p) != was;
}

/* A router's table lists the device as its child. True when the parent
 * changed; ignored while our own table lists it. */
static bool parent_claimed(zbc_parent_t *p, uint16_t router)
{
    if (p->ours || router == ZBC_PARENT_US || router == ZBC_PARENT_UNKNOWN) {
        return false;
    }
    const uint16_t was = parent_addr(p);
    p->src  = ZBC_PARENT_SRC_ROUTER;
    p->addr = router;
    return parent_addr(p) != was;
}

/* The device said it was leaving. It has no parent until a table says where
 * it went. True when that is a change. */
static bool parent_left(zbc_parent_t *p)
{
    const uint16_t was = parent_addr(p);
    p->ours = false;
    p->src  = ZBC_PARENT_SRC_LEFT;
    return parent_addr(p) != was;
}

/* >>> end host-test region: parent <<< */

/* The last changes of parent - see zb_parent_changes. Written from the Zigbee
 * task (signals, router answers) and the work task (our own walk). */
static zb_parent_note_t s_parent_log[ZB_PARENT_LOG_LEN];
static uint8_t          s_parent_log_next;
static uint8_t          s_parent_log_used;
static portMUX_TYPE     s_parent_log_mux = portMUX_INITIALIZER_UNLOCKED;

static void parent_log(const char *what, const uint8_t *ieee, uint16_t short_addr,
                       uint16_t parent, uint16_t was)
{
    zb_parent_note_t n = {
        .at_us      = esp_timer_get_time(),
        .what       = what,
        .have_ieee  = ieee != NULL,
        .short_addr = short_addr,
        .parent     = parent,
        .was        = was,
    };
    if (ieee) {
        memcpy(n.ieee, ieee, ZBC_IEEE_LEN);
    }
    portENTER_CRITICAL(&s_parent_log_mux);
    s_parent_log[s_parent_log_next] = n;
    s_parent_log_next = (uint8_t)((s_parent_log_next + 1) % ZB_PARENT_LOG_LEN);
    if (s_parent_log_used < ZB_PARENT_LOG_LEN) {
        s_parent_log_used++;
    }
    portEXIT_CRITICAL(&s_parent_log_mux);
    ESP_LOGI(TAG, "Device 0x%04x %s: parent 0x%04x, was 0x%04x", short_addr, what, parent, was);
}

size_t zb_parent_changes(zb_parent_note_t *out, size_t max)
{
    if (!out || max == 0) {
        return 0;
    }
    size_t n = 0;
    portENTER_CRITICAL(&s_parent_log_mux);
    const int used  = s_parent_log_used;
    const int first = (s_parent_log_next - used + ZB_PARENT_LOG_LEN) % ZB_PARENT_LOG_LEN;
    for (int i = 0; i < used && n < max; i++) {
        out[n++] = s_parent_log[(first + i) % ZB_PARENT_LOG_LEN];
    }
    portEXIT_CRITICAL(&s_parent_log_mux);
    return n;
}

/* One row per device heard from since boot, keyed by short address because that
 * is all a frame carries and a registry lookup per frame is not worth paying;
 * the API maps an IEEE to it. Written from the Zigbee task and the work task,
 * read from the HTTP task, so every touch is under the spinlock. A device that
 * rejoins with a new short address starts a fresh row; the old one is the first
 * to be reused. */
typedef struct {
    uint16_t short_addr;
    bool     used;
    bool     have_tsn;
    uint8_t  last_tsn;
    int8_t   rssi;
    int8_t   child;      /* 1 ours, 0 not ours, -1 not looked yet */
    bool     groupcast;  /* has sent to a group - see zb_link_info_t */
    bool     have_ieee;  /* from a neighbour table; frames carry only the short address */
    uint8_t  ieee[ZBC_IEEE_LEN];
    zbc_parent_t parent; /* an end device's; see the `parent` region */
    int16_t  parent_lqi; /* the parent's reading of the link, -1 unknown */
    uint32_t timeout_s;  /* our child's end-device timeout, 0 when not our child */
    uint32_t timeout_left_s; /* ...and how much of it is left - see ed_timeout_seconds */
    uint32_t frames;
    uint32_t missed;
    int64_t  seen_us;
    int64_t  parent_us;  /* when the parent last changed */
} zb_link_t;

static zb_link_t    s_link[CONFIG_ZBC_MAX_DEVICES];
static portMUX_TYPE s_link_mux = portMUX_INITIALIZER_UNLOCKED;
/* Whether note_children() has run yet. A row made after it is for a device the
 * walk did not list as a child, so it starts as "not ours" rather than "not
 * looked". A child that joins between walks reads the same until the next walk;
 * that is the cheaper error, since the walk follows within a minute of a boot. */
static bool s_table_walked;

/* Under s_link_mux. */
static zb_link_t *link_slot(uint16_t short_addr, bool create)
{
    int free_i = -1, oldest = 0;
    for (int i = 0; i < CONFIG_ZBC_MAX_DEVICES; i++) {
        if (s_link[i].used && s_link[i].short_addr == short_addr) {
            return &s_link[i];
        }
        if (!s_link[i].used && free_i < 0) {
            free_i = i;
        }
        if (s_link[i].seen_us < s_link[oldest].seen_us) {
            oldest = i;
        }
    }
    if (!create) {
        return NULL;
    }
    zb_link_t *l = &s_link[free_i >= 0 ? free_i : oldest];
    memset(l, 0, sizeof(*l));
    l->used       = true;
    l->short_addr = short_addr;
    l->child      = s_table_walked ? 0 : -1;
    l->parent_lqi = -1;
    return l;
}

/* Every ZCL frame that arrives, from the raw tap, in the Zigbee task. Only the
 * frames a device sent of its own accord carry *its* sequence numbers - a
 * report, or a cluster command from a client to us. A read response or a
 * default response carries ours back, and would read as a device skipping
 * about at random. The RSSI is taken from all of them. */
static void note_link(uint16_t saddr, const ezb_zcl_cmd_hdr_t *h)
{
    /* A server-to-client command is usually an answer carrying our sequence
     * number back, and is not counted. Philips' button notification is the
     * exception: the dimmer is the 0xFC00 server and sends it unasked, numbered
     * from its own sequence like its endpoint 1 commands, so its 2-3 frames per
     * press would otherwise read as frames lost. Only a Hue dimmer's: 0xFC00 is
     * the first manufacturer-specific cluster number and means something else
     * from other makers. The model is looked up only for such a frame, before
     * the spinlock below. */
    const bool own = EZB_ZCL_CMD_FC_GET_FRAME_TYPE(h->fc) == EZB_ZCL_FRAME_TYPE_CLUSTER_SPECIFIC
                         ? ((h->fc >> 3) & 1U) == EZB_ZCL_CMD_DIRECTION_TO_SRV ||
                               hue_dimmer_frame(saddr, h->cluster_id)
                         : h->cmd_id == EZB_ZCL_CMD_REPORT_ATTRIBUTES;
    unsigned missed  = 0;
    uint8_t  before  = 0;

    portENTER_CRITICAL(&s_link_mux);
    zb_link_t *l = link_slot(saddr, true);
    l->rssi    = h->rssi;
    l->seen_us = esp_timer_get_time();
    if (h->dst_addr.addr_mode == EZB_ADDR_MODE_GROUP) {
        l->groupcast = true;
    }
    if (own) {
        if (l->have_tsn) {
            before = l->last_tsn;
            missed = link_gap(l->last_tsn, h->tsn);
        }
        l->last_tsn = h->tsn;
        l->have_tsn = true;
        l->frames++;
        l->missed += missed;
    }
    portEXIT_CRITICAL(&s_link_mux);

    if (own) {
        s_radio.frames_heard++;
        s_radio.frames_missed += missed;
    }
    if (missed) {
        /* Rare enough to log every time; it tells a lost press from a remote that
         * never sent one. */
        ESP_LOGW(TAG, "Device 0x%04x sent %u frame(s) that never arrived (tsn %u, then %u; rssi %d dBm)",
                 saddr, missed, before, h->tsn, h->rssi);
    }
}

bool zb_device_link(const uint8_t ieee[ZBC_IEEE_LEN], zb_link_info_t *out)
{
    uint16_t short_addr;
    if (!out || !device_db_short_by_ieee(ieee, &short_addr)) {
        return false;
    }
    bool         found = false;
    zbc_parent_t parent = { 0 };
    memset(out, 0, sizeof(*out));
    portENTER_CRITICAL(&s_link_mux);
    const zb_link_t *l = link_slot(short_addr, false);
    if (l) {
        out->heard              = l->seen_us != 0;
        out->rssi               = l->rssi;
        out->child              = l->child;
        out->frames             = l->frames;
        out->missed             = l->missed;
        out->groupcast          = l->groupcast;
        out->parent_lqi         = l->parent_lqi;
        out->parent_us          = l->parent_us;
        out->timeout_s          = l->timeout_s;
        out->timeout_left_s     = l->timeout_left_s;
        parent                  = l->parent;
        found                   = true;
    }
    portEXIT_CRITICAL(&s_link_mux);
    if (!found) {
        return false;
    }

    /* The parent's long address, outside the spinlock: the registry has its own
     * mutex. Our own from the copy taken at network-up. */
    const uint16_t p = parent_addr(&parent);
    out->parent_known = p != ZBC_PARENT_UNKNOWN;
    out->parent_is_us = p == ZBC_PARENT_US;
    out->parent_short = p;
    out->parent_src   = parent.src == ZBC_PARENT_SRC_TABLE    ? "table"
                        : parent.src == ZBC_PARENT_SRC_ROUTER ? "router"
                                                              : NULL;
    if (out->parent_is_us) {
        memcpy(out->parent_ieee, s_net.ieee, ZBC_IEEE_LEN);
        out->parent_ieee_known = true;
    } else if (out->parent_known) {
        out->parent_ieee_known = device_db_ieee_by_short(p, out->parent_ieee);
    }
    if (!out->parent_known) {
        out->parent_lqi = -1;
    }
    return true;
}

/* How many routers each walk asks for their tables: every lamp in range, in any
 * house this is built for. */
#define ZBC_ROUTERS_ASKED 12

/* The routers are asked on the first two walks and then on every twelfth - an
 * hour - rather than every five minutes. esp-zigbee-lib 2.0.4 keeps a block
 * (~44-56 bytes) of about one router-table request in thirty, answered or not,
 * in whichever task sent it: page 0 here, the later pages in the Zigbee task.
 * On the Shelly, six routers and ~21 pages a walk, that was all of what the
 * two tasks still lost after ZBC_BIND_SCANS - ~10 blocks an hour with every
 * page answered (router_tables_* in zigbee.radio). Every sixth walk held it at
 * 1.35 an hour over two days (0.68 a walk), ~1.5 KB a day; every twelfth halves
 * that. A remote that moves between two lamps shows it up to an hour late; one
 * that joins or leaves us shows it at the next walk, as our own table is still
 * read every time. */
#define ZBC_ROUTER_WALK_EVERY 12

/* An ezb_nwk_ed_timeout_t in seconds: 10 s, then 2 minutes doubling with each
 * step up to 16384 minutes. The neighbour table's `device_timeout` is one of
 * these codes too, whatever its comment says - a child restored at boot read
 * 8 there with 15355 in `timeout_counter`, which then fell by 60 over a minute:
 * the code for 256 minutes, and the seconds left of them. */
static uint32_t ed_timeout_seconds(uint32_t code)
{
    if (code == EZB_NWK_ED_TIMEOUT_10SEC) {
        return 10;
    }
    return code <= EZB_NWK_ED_TIMEOUT_16384MIN ? 60U << code : code;
}

/* How far apart the boot read-back spaces its endpoints, and the walk its
 * router table requests. Sent as one burst - together with the first neighbour
 * scan, its binds and the sleepy-device retries, while lamps are rejoining -
 * some reads are refused silently and those lamps show "unknown" until their
 * five-minute report. Runs in zb_work_task, which may block, so the pacing is a
 * short sleep. */
#define ZBC_REFRESH_PACE_MS 60

static void ask_router_table(uint16_t router, uint8_t start);

/* A router's neighbour table, asked for after our own walk: the end devices it
 * lists as its children hang off it rather than us. Zigbee task, stack lock
 * held. A lost or refused answer changes nothing - see zbc_parent_t. */
static void router_table_result(const ezb_zdo_nwk_mgmt_lqi_req_result_t *result, void *user_ctx)
{
    const uint16_t router = (uint16_t)(uintptr_t)user_ctx;
    if (!result || result->error != EZB_ERR_NONE || !result->rsp) {
        /* Counted and marked: esp-zigbee-lib 2.0.4 keeps a ~44-byte block of a
         * bind request that ends like this (see ZBC_BIND_SCANS), and the Shelly
         * still loses ~44-byte blocks in this task and zb_work a few times an
         * hour. If these requests do the same, the count matches the blocks. */
        s_radio.router_tables_unanswered++;
        heap_watch_mark("rtable_lost", router);
        return;
    }
    s_radio.router_tables_answered++;
    if (result->rsp->status != EZB_ZDP_STATUS_SUCCESS) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    for (int i = 0; i < result->rsp->neighbor_table_list_count; i++) {
        const ezb_zdp_nwk_mgmt_lqi_neighbor_table_entry_t *e = &result->rsp->neighbor_table_list[i];
        if (e->affinity != EZB_NWK_RELATIONSHIP_CHILD ||
            e->device_type != EZB_NWK_DEVICE_TYPE_END_DEVICE) {
            continue;
        }
        uint8_t ieee[ZBC_IEEE_LEN];
        bool    have_ieee = false;
        for (int b = 0; b < ZBC_IEEE_LEN; b++) {
            ieee[b]    = e->extended_addr.u8[ZBC_IEEE_LEN - 1 - b];
            have_ieee |= ieee[b] != 0xff && ieee[b] != 0x00;
        }
        uint16_t was;
        bool     changed;
        portENTER_CRITICAL(&s_link_mux);
        zb_link_t *l = link_slot(e->nwk_addr, true);
        if (have_ieee) {
            memcpy(l->ieee, ieee, ZBC_IEEE_LEN);
            l->have_ieee = true;
        }
        was     = parent_addr(&l->parent);
        changed = parent_claimed(&l->parent, router);
        if (parent_addr(&l->parent) == router) {
            l->parent_lqi = e->lqa;
        }
        if (changed) {
            l->parent_us = now;
        }
        portEXIT_CRITICAL(&s_link_mux);
        if (changed) {
            parent_log("router", have_ieee ? ieee : NULL, e->nwk_addr, router, was);
            zbc_state_bump();
        }
    }

    /* The table can span several responses. */
    const unsigned next = (unsigned)result->rsp->start_index + result->rsp->neighbor_table_list_count;
    if (result->rsp->neighbor_table_list_count > 0 && next < result->rsp->neighbor_table_entries) {
        ask_router_table(router, (uint8_t)next);
    }
}

/* Stack lock held. */
static void ask_router_table(uint16_t router, uint8_t start)
{
    ezb_zdo_nwk_mgmt_lqi_req_t req = {
        .dst_nwk_addr = router,
        .field        = { .start_index = start },
        .cb           = router_table_result,
        .user_ctx     = (void *)(uintptr_t)router,
    };
    if (ezb_zdo_nwk_mgmt_lqi_req(&req) != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Could not ask router 0x%04x for its neighbour table", router);
    } else {
        s_radio.router_tables_asked++;
    }
}

/* The coordinator's own neighbour table, walked locally. The ZDO scan asks the
 * same table over the air and does not list sleepy remotes, so this is where a
 * remote's parent and lqa come from. Then, with `ask_routers`, each router in it
 * is asked for its own, which is where a remote that is not ours is found. Runs
 * in zb_work_task, which may block: the routers are asked a little apart. */
static void note_children(bool ask_routers)
{
    ezb_nwk_info_iterator_t it = EZB_NWK_INFO_ITERATOR_INIT;
    ezb_nwk_neighbor_info_t n;
    int      children = 0, entries = 0, routers_n = 0;
    uint16_t routers[ZBC_ROUTERS_ASKED];
    /* Which link rows the walk listed as our end-device children, by index. */
    bool     listed[CONFIG_ZBC_MAX_DEVICES] = { false };

    portENTER_CRITICAL(&s_link_mux);
    for (int i = 0; i < CONFIG_ZBC_MAX_DEVICES; i++) {
        if (s_link[i].used) {
            s_link[i].child = 0;
        }
    }
    portEXIT_CRITICAL(&s_link_mux);

    esp_zigbee_lock_acquire(portMAX_DELAY);
    while (ezb_nwk_get_next_neighbor(&it, &n) == EZB_ERR_NONE) {
        entries++;
        if (n.device_type == EZB_NWK_DEVICE_TYPE_ROUTER && routers_n < ZBC_ROUTERS_ASKED) {
            routers[routers_n++] = n.short_addr;
        }
        if (n.relationship != EZB_NWK_RELATIONSHIP_CHILD) {
            continue;
        }
        children++;
        uint8_t ieee[ZBC_IEEE_LEN];
        for (int b = 0; b < ZBC_IEEE_LEN; b++) {
            ieee[b] = n.ieee_addr.u8[ZBC_IEEE_LEN - 1 - b];
        }
        const bool ed     = n.device_type == EZB_NWK_DEVICE_TYPE_END_DEVICE;
        const bool sleepy = ed && !n.rx_on_when_idle;
        ESP_LOGI(TAG, "Child 0x%04x: %s, lqi %u/255, rssi %d dBm, timeout %lu s, %lu s left",
                 n.short_addr, sleepy ? "sleepy end device" : ed ? "end device" : "router",
                 n.lqi, n.rssi, (unsigned long)ed_timeout_seconds(n.device_timeout),
                 (unsigned long)n.timeout_counter);
        device_db_set_lqa(ieee, n.lqi);
        note_power_source(ieee, sleepy);
        portENTER_CRITICAL(&s_link_mux);
        zb_link_t *l = link_slot(n.short_addr, true);
        l->child = 1;
        /* A router that joined through us is listed as a child too, but once
         * joined it routes for itself: only an end device has a parent. */
        if (ed) {
            listed[l - s_link] = true;
            memcpy(l->ieee, ieee, ZBC_IEEE_LEN);
            l->have_ieee          = true;
            l->parent_lqi     = n.lqi;
            l->timeout_s      = ed_timeout_seconds(n.device_timeout);
            l->timeout_left_s = n.timeout_counter;
        }
        portEXIT_CRITICAL(&s_link_mux);
    }
    esp_zigbee_lock_release();

    /* What the walk says about every device on record, listed or not: one that
     * was ours and is no longer listed has gone to another parent or left. */
    const int64_t now = esp_timer_get_time();
    for (int i = 0; i < CONFIG_ZBC_MAX_DEVICES; i++) {
        bool     changed = false, have_ieee = false;
        uint8_t  ieee[ZBC_IEEE_LEN] = { 0 };
        uint16_t addr = 0, was = 0, parent = 0;
        portENTER_CRITICAL(&s_link_mux);
        if (s_link[i].used) {
            was     = parent_addr(&s_link[i].parent);
            changed = parent_walked(&s_link[i].parent, listed[i]);
            parent  = parent_addr(&s_link[i].parent);
            if (!listed[i]) {
                s_link[i].timeout_s          = 0;
                s_link[i].timeout_left_s = 0;
                if (parent == ZBC_PARENT_UNKNOWN) {
                    s_link[i].parent_lqi = -1;
                }
            }
            if (changed) {
                s_link[i].parent_us = now;
                addr      = s_link[i].short_addr;
                have_ieee = s_link[i].have_ieee;
                memcpy(ieee, s_link[i].ieee, ZBC_IEEE_LEN);
            }
        }
        portEXIT_CRITICAL(&s_link_mux);
        if (changed) {
            parent_log(listed[i] ? "table" : "gone", have_ieee ? ieee : NULL, addr, parent, was);
            zbc_state_bump();
        }
    }

    s_radio.table_entries  = (uint32_t)entries;
    s_radio.table_children = (uint32_t)children;
    s_table_walked         = true;
    ESP_LOGI(TAG, "Neighbour table holds %d entr%s, %d of them our children", entries,
             entries == 1 ? "y" : "ies", children);

    for (int r = 0; ask_routers && r < routers_n && s_stack_started; r++) {
        if (reach_is_gone(routers[r])) {
            continue;
        }
        if (r) {
            vTaskDelay(pdMS_TO_TICKS(ZBC_REFRESH_PACE_MS));
        }
        esp_zigbee_lock_acquire(portMAX_DELAY);
        ask_router_table(routers[r], 0);
        esp_zigbee_lock_release();
    }
}

/* One line per (device, endpoint, cluster, command) per boot: an unknown remote
 * must be visible at INFO, but a device repeating one frame must not bury the
 * log. */
static void log_unknown_frame(uint16_t saddr, const ezb_zcl_cmd_hdr_t *h,
                              const ezb_zcl_raw_frame_t *raw)
{
    static struct {
        uint16_t short_addr;
        uint8_t  ep;
        uint16_t cluster_id;
        uint8_t  cmd_id;
    } seen[12];
    static uint8_t seen_n;

    for (int i = 0; i < seen_n; i++) {
        if (seen[i].short_addr == saddr && seen[i].ep == h->src_ep &&
            seen[i].cluster_id == h->cluster_id && seen[i].cmd_id == h->cmd_id) {
            return;
        }
    }
    if (seen_n < (uint8_t)(sizeof(seen) / sizeof(seen[0]))) {
        seen[seen_n].short_addr = saddr;
        seen[seen_n].ep         = h->src_ep;
        seen[seen_n].cluster_id = h->cluster_id;
        seen[seen_n].cmd_id     = h->cmd_id;
        seen_n++;
    }

    char hex[3 * 16 + 1];
    size_t n = 0;
    const uint16_t len = raw->payload_length > 16 ? 16 : raw->payload_length;
    for (uint16_t i = 0; i < len && raw->payload; i++) {
        n += (size_t)snprintf(hex + n, sizeof(hex) - n, "%02x ", raw->payload[i]);
    }
    hex[n] = '\0';

    ESP_LOGI(TAG, "Device 0x%04x ep %d sent command 0x%02x on cluster 0x%04x"
                  " (manuf 0x%04x, %u byte(s): %s) - not a press this build understands",
             saddr, h->src_ep, h->cmd_id, h->cluster_id, h->manuf_code,
             (unsigned)raw->payload_length, hex);
}

/* The same frame twice. An IKEA SYMFONISK (E1744) sends every command once per
 * binding-table entry, and interrogation binds both On/Off and Level, so each
 * press and turn step arrives twice, 12-24 ms apart, with the same tsn. They are
 * two APS frames, so the stack's duplicate rejection passes both; decoded twice,
 * a click is two toggles and a turn restarts its sweep. zigbee2mqtt debounces
 * this device for the same reason.
 *
 * Only our own handling (link counters, decoding) is skipped; the stack still
 * gets the frame. A tsn repeats only after 256 frames. Zigbee task only, so no
 * lock. */
#define ZBC_DUP_WINDOW_US (1000 * 1000)

static struct {
    uint16_t short_addr;
    uint16_t cluster_id;
    int64_t  at_us;
    uint8_t  ep;
    uint8_t  cmd_id;
    uint8_t  tsn;
    uint8_t  fc;
    uint8_t  used;
} s_recent_frames[8];

static bool frame_is_duplicate(uint16_t saddr, const ezb_zcl_cmd_hdr_t *h)
{
    const int64_t now    = esp_timer_get_time();
    int           oldest = 0;
    for (int i = 0; i < (int)(sizeof(s_recent_frames) / sizeof(s_recent_frames[0])); i++) {
        if (s_recent_frames[i].used && s_recent_frames[i].short_addr == saddr &&
            s_recent_frames[i].ep == h->src_ep && s_recent_frames[i].cluster_id == h->cluster_id &&
            s_recent_frames[i].cmd_id == h->cmd_id && s_recent_frames[i].tsn == h->tsn &&
            s_recent_frames[i].fc == h->fc && now - s_recent_frames[i].at_us < ZBC_DUP_WINDOW_US) {
            return true;
        }
        if (s_recent_frames[i].at_us < s_recent_frames[oldest].at_us) {
            oldest = i;
        }
    }
    s_recent_frames[oldest].used       = 1;
    s_recent_frames[oldest].short_addr = saddr;
    s_recent_frames[oldest].ep         = h->src_ep;
    s_recent_frames[oldest].cluster_id = h->cluster_id;
    s_recent_frames[oldest].cmd_id     = h->cmd_id;
    s_recent_frames[oldest].tsn        = h->tsn;
    s_recent_frames[oldest].fc         = h->fc;
    s_recent_frames[oldest].at_us      = now;
    return false;
}

/* Every ZCL frame that reaches us, before the stack dispatches it. The only way
 * to see a cluster-specific command sent to the coordinator: ezbee has action
 * callbacks only for clusters we host, and a remote sends to clusters we do not.
 *
 * Always returns false. True drops the frame, and the read responses, reports
 * and default responses everything else depends on come through here too.
 *
 * A frame that decodes to nothing is logged once at INFO: ESP_LOGD is compiled
 * out, and an unknown remote would otherwise look like one not transmitting. */
static bool zcl_raw_frame_handler(const ezb_zcl_raw_frame_t *raw)
{
    if (!raw || !raw->header) {
        return false;
    }
    const ezb_zcl_cmd_hdr_t *h = raw->header;
    const uint16_t saddr = (h->src_addr.addr_mode == EZB_ADDR_MODE_SHORT)
                               ? h->src_addr.u.short_addr : 0xFFFF;
    if (saddr == 0xFFFF) {
        return false;
    }
    /* Every frame, whatever it is: the one place the receiving side of the radio
     * is measured. Not our own - join_remote_groups() sends AddGroup to this
     * very endpoint. */
    if (saddr != ezb_nwk_get_short_address()) {
        if (frame_is_duplicate(saddr, h)) {
            return false;   /* see frame_is_duplicate; the stack still gets it */
        }
        note_link(saddr, h);
        ask_when_heard(saddr);
        /* A sleepy device is awake right now and about to poll its parent, so
         * this is when a firmware notify waiting for it can reach it. Sent from
         * the work task, not from inside the stack's frame dispatch. */
        if (zb_ota_wake_heard(saddr)) {
            ota_wake_post();
        }
    }
    if (EZB_ZCL_CMD_FC_GET_FRAME_TYPE(h->fc) != EZB_ZCL_FRAME_TYPE_CLUSTER_SPECIFIC) {
        return false;   /* a read, a report, a default response - handled elsewhere */
    }

    /* A device asking for firmware. The stack answers; what is taken here is what
     * the device says it runs, which Query Next Image carries whether or not we
     * have an image - for a sleepy remote the only way to learn it. Block
     * requests are hundreds of frames, known and not logged as unknown. */
    if (h->cluster_id == EZB_ZCL_CLUSTER_ID_OTA_UPGRADE) {
        zbc_ota_qni_t q;
        if (h->cmd_id == EZB_ZCL_CMD_OTA_UPGRADE_QUERY_NEXT_IMAGE_REQUEST_ID &&
            zbc_ota_qni_parse(raw->payload, raw->payload_length, &q)) {
            zb_ota_note_query(saddr, h->src_ep, &q);
            device_db_touch_by_short(saddr);
        }
        return false;
    }

    const int64_t now = esp_timer_get_time();
    uint8_t       key = 0;

    /* A scroll wheel's stream of absolute levels. Only from an endpoint we
     * believe is a switch: a lamp has no business sending us MoveToLevel, and
     * one that did must not start dimming the house. */
    uint8_t level = 0;
    if (remote_wheel_frame(h->cluster_id, h->cmd_id, raw->payload, raw->payload_length, &level) &&
        endpoint_is_switch(saddr, h->src_ep)) {
        const int     slot       = wheel_slot(saddr, h->src_ep, true);
        const int8_t  dir_before = s_wheels[slot].w.dir;
        const int64_t gap        = now - s_wheels[slot].seen_us;
        s_wheels[slot].seen_us   = now;
        zb_remote_ev_t wev = remote_wheel_level(&s_wheels[slot].w, level, now, &key);
        if (wev == ZB_REMOTE_HOLD_TICK) {
            /* One line per turn, and one each time it changes direction. */
            const bool fresh = s_wheels[slot].w.dir != dir_before || gap > 500 * 1000;
            if (fresh) {
                ESP_LOGI(TAG, "Remote 0x%04x ep %d turned its wheel to level %u (tsn %u)",
                         saddr, h->src_ep, level, h->tsn);
                device_db_touch_by_short(saddr);
            }
            note_remote_event(saddr, h->src_ep, key, wev, !fresh, raw);
        }
        return false;
    }

    /* A wheel click drags a level-0 frame along in front of it; tell the wheel,
     * so that zero is not taken for a turn. */
    if (h->cluster_id == EZB_ZCL_CLUSTER_ID_ON_OFF) {
        const int slot = wheel_slot(saddr, h->src_ep, false);
        if (slot >= 0) {
            remote_wheel_note_onoff(&s_wheels[slot].w, now);
        }
    }

    const zb_remote_model_t model    = remote_model_by_short(saddr);
    uint8_t                 press_ep = h->src_ep;
    if (model == ZB_REMOTE_MODEL_HUE_DIMMER) {
        if (h->cluster_id == ZB_REMOTE_CL_HUE) {
            s_hue.zcl_frames++;
            /* Filed with its endpoint 1 buttons - see HUE_DIMMER_BUTTON_EP. */
            press_ep = HUE_DIMMER_BUTTON_EP;
            hue_note_notify(saddr);
        } else if (hue_buttons_on(saddr)) {
            /* The same press, on the cluster it is read from; acting on both
             * would switch twice. */
            if (h->cluster_id == EZB_ZCL_CLUSTER_ID_ON_OFF || h->cluster_id == EZB_ZCL_CLUSTER_ID_LEVEL) {
                return false;
            }
        } else {
            /* Awake right now, having just sent this: the moment to ask it to
             * speak its own cluster. */
            hue_bind_post(saddr, false);
        }
    }

    zb_remote_ev_t ev = remote_decode_cmd(model, h->cluster_id, h->cmd_id,
                                          raw->payload, raw->payload_length, &key);
    if (h->cluster_id == EZB_ZCL_CLUSTER_ID_ON_OFF && h->cmd_id == ZB_REMOTE_CMD_TUYA_ACTION &&
        ev != ZB_REMOTE_NONE) {
        tuya_action_note(saddr, h->src_ep, now);
    } else if (remote_is_echo(h->cluster_id, h->cmd_id, tuya_action_age(saddr, h->src_ep, now))) {
        ev = ZB_REMOTE_IGNORED;   /* the same gesture again, as a lamp command */
    }
    if (ev == ZB_REMOTE_IGNORED) {
        /* Half of a press counted from another frame (the Hue dimmer's press
         * edge). It names no key, so it must not reach the capture ring, where
         * it would teach "key 0" for whichever button was pressed. */
        trace_remote_frame(saddr, raw);
        device_db_touch_by_short(saddr);
        return false;
    }
    if (ev != ZB_REMOTE_NONE) {
        /* The command a press decoded from, not just what it decoded to: one Hue
         * Smart Button press is three frames (Level Move, Stop, OffWithEffect).
         * The tsn tells a retransmission from a second press, the destination
         * whether it came by binding or to a group. */
        ESP_LOGI(TAG, "Remote 0x%04x ep %d sent cluster 0x%04x command 0x%02x (%u byte%s, tsn %u,"
                      " to %s 0x%04x)",
                 saddr, h->src_ep, h->cluster_id, h->cmd_id,
                 (unsigned)raw->payload_length, raw->payload_length == 1 ? "" : "s", h->tsn,
                 h->dst_addr.addr_mode == EZB_ADDR_MODE_GROUP ? "group" : "us",
                 h->dst_addr.addr_mode == EZB_ADDR_MODE_GROUP ? h->dst_addr.u.group_addr.group
                                                              : h->dst_addr.u.short_addr);
        trace_remote_frame(saddr, raw);
        device_db_touch_by_short(saddr);
        note_remote_event(saddr, press_ep, key, ev, false, raw);
        return false;
    }

    log_unknown_frame(saddr, h, raw);
    return false;
}

/* A BILRESA's group-casts, taken in at APS before the stack's group delivery.
 * esp-zigbee-lib 2.0.4 leaks something per group-cast delivered to us: after
 * about 64 it refuses every send (EZB_ERR_FAIL) and hears nothing until a
 * restart, with the heap untouched; unicast frames do not do this. Consuming
 * the frame at the ZCL raw hook does not help, so the cost is below ZCL. The
 * frame is parsed here and handed to zcl_raw_frame_handler; anything that is
 * not a well-formed ZCL frame to one of those groups goes on to the stack. */
/* The ZCL header of an APS frame parsed here, and the frame handed to the
 * same raw handler the stack's own delivery uses. False when too short. */
static bool aps_feed_zcl(const ezb_apsde_data_ind_t *ind)
{
    const uint8_t *p   = ind->asdu;
    uint16_t       len = ind->asdu_length;
    ezb_zcl_cmd_hdr_t h = {
        .src_addr   = ind->src_address,
        .dst_addr   = ind->dst_address,
        .src_ep     = ind->src_endpoint,
        .dst_ep     = ind->dst_endpoint,
        .cluster_id = ind->cluster_id,
        .profile_id = ind->profile_id,
        .rssi       = ind->rssi,
    };
    if (len < 3) {
        return false;
    }
    h.fc = p[0];
    const bool manuf = (h.fc & 0x04U) != 0;
    if (manuf && len < 5) {
        return false;
    }
    if (manuf) {
        h.manuf_code = (uint16_t)(p[1] | (p[2] << 8));
    }
    const uint16_t hdr = manuf ? 5 : 3;
    h.tsn    = p[hdr - 2];
    h.cmd_id = p[hdr - 1];

    const ezb_zcl_raw_frame_t raw = {
        .header         = &h,
        .payload_length = (uint16_t)(len - hdr),
        .payload        = len > hdr ? (uint8_t *)&p[hdr] : NULL,
    };
    zcl_raw_frame_handler(&raw);
    return true;
}

#if CONFIG_ZBC_WLED
/* Philips' entertainment cluster - see zb_ent_begin. */
#define ZBC_ENT_CLUSTER     0xFC01
#define ZBC_ENT_MANUF       0x100B
#define ZBC_ENT_CMD_FRAME   0x01
#define ZBC_ENT_CMD_SYNC    0x03
#define ZBC_ENT_ATTR_BAL    0x0005

static bool ent_note_aps(const ezb_apsde_data_ind_t *ind);
#endif

static bool aps_group_frame_handler(const ezb_apsde_data_ind_t *ind)
{
    if (!ind || ind->status != 0 || ind->src_address.addr_mode != EZB_ADDR_MODE_SHORT || !ind->asdu) {
        return false;
    }

#if CONFIG_ZBC_WLED
    /* A Hue lamp answering on, or repeating, an entertainment frame. */
    if (ind->cluster_id == ZBC_ENT_CLUSTER) {
        return ent_note_aps(ind);
    }
#endif

    /* A Hue dimmer's button cluster. 0xFC00 is on no endpoint of ours and the
     * stack may not pass it up to the ZCL hook, so it is taken here like the
     * group-casts below, parsed and handed over. Counted either way, which
     * shows whether the dimmer sends it at all. */
    if (ind->dst_address.addr_mode != EZB_ADDR_MODE_GROUP &&
        hue_dimmer_frame(ind->src_address.u.short_addr, ind->cluster_id)) {
        s_hue.aps_frames++;
        return aps_feed_zcl(ind);
    }

    if (ind->dst_address.addr_mode != EZB_ADDR_MODE_GROUP) {
        return false;
    }
    bool ours = false;
    for (size_t i = 0; i < sizeof(ZBC_REMOTE_GROUPS) / sizeof(ZBC_REMOTE_GROUPS[0]); i++) {
        ours |= ind->dst_address.u.group_addr.group == ZBC_REMOTE_GROUPS[i];
    }
    if (!ours) {
        return false;
    }
    return aps_feed_zcl(ind);
}

/* ------------------------------------------------------------ ZCL handling */

/* One line per (device, endpoint, cluster, attribute) per boot - the attribute
 * half of log_unknown_frame: a remote reporting presses somewhere this build
 * does not look would otherwise look like one with a flat battery. */
static void log_unknown_attr(uint16_t saddr, uint8_t ep, uint16_t cluster_id, uint16_t attr_id,
                             uint32_t value)
{
    static struct {
        uint16_t short_addr;
        uint8_t  ep;
        uint16_t cluster_id;
        uint16_t attr_id;
    } seen[12];
    static uint8_t seen_n;

    for (int i = 0; i < seen_n; i++) {
        if (seen[i].short_addr == saddr && seen[i].ep == ep &&
            seen[i].cluster_id == cluster_id && seen[i].attr_id == attr_id) {
            return;
        }
    }
    if (seen_n < (uint8_t)(sizeof(seen) / sizeof(seen[0]))) {
        seen[seen_n].short_addr = saddr;
        seen[seen_n].ep         = ep;
        seen[seen_n].cluster_id = cluster_id;
        seen[seen_n].attr_id    = attr_id;
        seen_n++;
    }
    ESP_LOGI(TAG, "Remote 0x%04x ep %d reported attribute 0x%04x on cluster 0x%04x as %lu"
                  " - not a press this build understands", saddr, ep, attr_id, cluster_id,
             (unsigned long)value);
}

/* The lamp answered something. Any attribute counts: read_light_state asks for
 * on/off, level and colour in separate frames, and one lost answer does not make
 * the lamp quiet. */
static void note_answered(uint16_t short_addr, uint8_t ep)
{
    for (int i = 0; i < ZBC_SETTLE_SLOTS; i++) {
        if (s_settle[i].short_addr == short_addr && s_settle[i].ep == ep) {
            s_settle[i].answered = true;
            return;
        }
    }
}

/* ---------------------------------------------------------------- sensors */

static zb_sensor_cb_t s_sensor_cb;

void zb_set_sensor_handler(zb_sensor_cb_t cb)
{
    s_sensor_cb = cb;
}

/* An Occupancy report, bit 0 of which says whether anybody is there. The report
 * is what makes an endpoint a sensor as much as its simple descriptor does: a
 * Xiaomi lumi.sensor_motion is asleep again before most interrogations finish,
 * and reports its occupancy unbound and unasked, so an endpoint not on record
 * is recorded from it. One recorded as a lamp or a remote is left alone. Zigbee
 * task, stack lock held. */
static void note_occupancy(uint16_t short_addr, uint8_t ep, bool occupied)
{
    uint8_t ieee[ZBC_IEEE_LEN];
    if (!device_db_ieee_by_short(short_addr, ieee)) {
        return;
    }
    uint16_t features = device_db_features_by_short(short_addr, ep);
    if (features == 0) {
        ESP_LOGI(TAG, "Device 0x%04x endpoint %d reports occupancy: recording a motion sensor",
                 short_addr, ep);
        if (device_db_add_endpoint(ieee, ep, 0, ZBC_FEAT_MOTION) != ESP_OK) {
            return;
        }
        features = ZBC_FEAT_MOTION;
        zbc_device_t dev;
        if (device_db_get(ieee, &dev)) {
            drop_presses_beside_motion(ieee, &dev);
        }
    }
    if (!(features & ZBC_FEAT_MOTION)) {
        return;
    }
    if (!occupied && !(features & ZBC_FEAT_MOTION_CLEARS)) {
        ESP_LOGI(TAG, "Device 0x%04x endpoint %d says when it is clear itself", short_addr, ep);
        device_db_add_features(ieee, ep, ZBC_FEAT_MOTION_CLEARS);
        features |= ZBC_FEAT_MOTION_CLEARS;
    }
    ESP_LOGI(TAG, "Device 0x%04x endpoint %d: %s", short_addr, ep, occupied ? "motion" : "no motion");
    if (s_sensor_cb) {
        s_sensor_cb(ieee, ep, occupied, (features & ZBC_FEAT_MOTION_CLEARS) != 0);
    }
}

static void handle_attribute(uint16_t src_addr, uint8_t src_ep, uint16_t cluster_id,
                             uint16_t attr_id, uint8_t attr_type, const void *value)
{
    if (!value) {
        return;
    }

    /* read_ota_client_info's answers. Nothing else of a device's OTA client is
     * of interest, and none of it belongs in the lamp handling below. */
    if (cluster_id == EZB_ZCL_CLUSTER_ID_OTA_UPGRADE) {
        if (attr_id == 0x0002 && attr_type == EZB_ZCL_ATTR_TYPE_UINT32) {
            uint32_t v;
            memcpy(&v, value, sizeof(v));
            zb_ota_note_attr(src_addr, src_ep, attr_id, v);
        } else if ((attr_id == 0x0007 || attr_id == 0x0008) && attr_type == EZB_ZCL_ATTR_TYPE_UINT16) {
            uint16_t v;
            memcpy(&v, value, sizeof(v));
            zb_ota_note_attr(src_addr, src_ep, attr_id, v);
        }
        return;
    }

    if (cluster_id == EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING &&
        attr_id == EZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID) {
        note_occupancy(src_addr, src_ep, (*(const uint8_t *)value & 0x01) != 0);
        return;
    }

    /* An Aqara wireless switch reports its clicks rather than sending commands -
     * on Multistate Input in the newer revision, and on the On/Off cluster in
     * the older one, which advertises On/Off as an *input* and so looks like a
     * lamp all the way down to here. Gated on the endpoint being one we already
     * believe is a switch, or every lamp that switched off would read as
     * somebody pressing something. */
    /* Only these three clusters can carry a press. That keeps the Basic strings
     * (manufacturer, model) out of the decoder, which would read a length byte
     * as a value and log every string as an unknown frame. */
    const bool press_cluster = cluster_id == EZB_ZCL_CLUSTER_ID_ON_OFF ||
                               cluster_id == EZB_ZCL_CLUSTER_ID_LEVEL ||
                               cluster_id == EZB_ZCL_CLUSTER_ID_MULTISTATE_INPUT;
    if (press_cluster && endpoint_is_switch(src_addr, src_ep)) {
        uint32_t v = 0;
        switch (attr_type) {
        case EZB_ZCL_ATTR_TYPE_UINT16:
        case EZB_ZCL_ATTR_TYPE_MAP16:
        case EZB_ZCL_ATTR_TYPE_ENUM16:   v = *(const uint16_t *)value; break;
        default:                         v = *(const uint8_t *)value;  break;
        }
        uint8_t        key = 0;
        zb_remote_ev_t ev  = remote_decode_attr(remote_model_by_short(src_addr), cluster_id, attr_id,
                                                v, &key);
        if (ev == ZB_REMOTE_IGNORED) {
            return;   /* the other edge of a press already counted */
        }
        if (ev != ZB_REMOTE_NONE) {
            note_remote_event(src_addr, src_ep, key, ev, false, NULL);
        } else {
            /* An unknown remote must not be silent: same reason and rate limit as the
             * raw frame tap. */
            log_unknown_attr(src_addr, src_ep, cluster_id, attr_id, v);
        }
        /* A switch-only endpoint has no state worth recording: an Aqara remote's
         * On/Off attribute means "pressed", and recording it would arm settle
         * against a lamp that does not exist. Only the press clusters are
         * swallowed - the Basic strings below are how a remote gets its name. */
        if (!(device_db_features_by_short(src_addr, src_ep) & ZBC_FEAT_ONOFF)) {
            return;
        }
    }

    if (cluster_id == EZB_ZCL_CLUSTER_ID_ON_OFF && attr_id == EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
        int8_t state = *(const uint8_t *)value ? 1 : 0;
        device_db_set_attr_by_short(src_addr, src_ep, ZBC_ATTR_ONOFF, state);
        ESP_LOGI(TAG, "Device 0x%04x ep %d is %s", src_addr, src_ep, state ? "on" : "off");

        /* On a weak link an unacknowledged command is retransmitted, and a repeat of
         * the previous one can arrive after the newest, leaving the lamp at the
         * older intent. Nothing above this layer can order those, so the
         * coordinator re-asserts what it last asked for - bounded by fixes_left,
         * and only while the read after a command is still expected. */
        for (int i = 0; i < ZBC_SETTLE_SLOTS; i++) {
            if (s_settle[i].short_addr != src_addr || s_settle[i].ep != src_ep) {
                continue;
            }
            s_settle[i].answered = true;
            /* `reads_left` bounds this in time. Without it the slot stays armed after
             * the command settles, and a later disagreeing report (reporting
             * repeats every ZBC_REPORT_MAX_INTERVAL_S) would reverse a lamp switched
             * by a wall switch or another controller. Same rule as ZBC_WANT_KEEP in
             * schedule_settle_read: an intent counts only while its command
             * settles. */
            if (s_settle[i].reads_left == 0 || !s_settle[i].judge ||
                s_settle[i].want_on < 0 || s_settle[i].want_on == state ||
                s_settle[i].fixes_left == 0) {
                break;
            }
            s_settle[i].fixes_left--;
            s_settle[i].judge = false;
            s_radio.corrections++;
            ESP_LOGW(TAG, "Device 0x%04x ep %d is %s but was told %s - re-asserting",
                     src_addr, src_ep, state ? "on" : "off", s_settle[i].want_on ? "on" : "off");
            resend_intent_locked(src_addr, src_ep, s_settle[i].want_on, s_settle[i].want_level);
            /* Check that the correction took. The slot is on its audit pass, which reads
             * nothing, so that is cleared; and it gets its own deadline rather than
             * restarting the shared timer - see settle_rearm_locked. */
            s_settle[i].reads_left = 1;
            s_settle[i].audit      = false;
            s_settle[i].answered   = false;
            s_settle[i].delivered  = false;
            s_settle[i].due_us     = esp_timer_get_time() + 1500000LL;
            settle_rearm_locked(false);
            break;
        }
        return;
    }

    if (cluster_id == EZB_ZCL_CLUSTER_ID_LEVEL &&
        (attr_id == EZB_ZCL_ATTR_LEVEL_ON_OFF_TRANSITION_TIME_ID ||
         attr_id == EZB_ZCL_ATTR_LEVEL_ON_TRANSITION_TIME_ID ||
         attr_id == EZB_ZCL_ATTR_LEVEL_OFF_TRANSITION_TIME_ID)) {
        uint16_t v = *(const uint16_t *)value;
        const char *name = attr_id == EZB_ZCL_ATTR_LEVEL_ON_OFF_TRANSITION_TIME_ID ? "OnOff"
                         : (attr_id == EZB_ZCL_ATTR_LEVEL_ON_TRANSITION_TIME_ID ? "On" : "Off");
        if (v == 0xffff) {
            ESP_LOGI(TAG, "Device 0x%04x ep %d %sTransitionTime unset (defers to OnOff)",
                     src_addr, src_ep, name);
        } else {
            ESP_LOGI(TAG, "Device 0x%04x ep %d %sTransitionTime %u.%us",
                     src_addr, src_ep, name, v / 10, v % 10);
        }
        return;
    }

    if (cluster_id == EZB_ZCL_CLUSTER_ID_LEVEL && attr_id == EZB_ZCL_ATTR_LEVEL_CURRENT_LEVEL_ID) {
        uint8_t level = *(const uint8_t *)value;
        device_db_set_attr_by_short(src_addr, src_ep, ZBC_ATTR_LEVEL, level);
        ESP_LOGD(TAG, "Device 0x%04x ep %d level %d", src_addr, src_ep, level);
        note_answered(src_addr, src_ep);

        /* A fade in is two commands - preset to the bottom, ramp up - and a late
         * duplicate of the first leaves the lamp at the bottom after the ramp: on,
         * as asked, but nearly dark. Only a lamp at the very bottom when told to
         * be much brighter counts; a general "level differs from what was sent"
         * test would fight the lamp's rounding, clamping and curves. */
        for (int i = 0; i < ZBC_SETTLE_SLOTS; i++) {
            if (s_settle[i].short_addr != src_addr || s_settle[i].ep != src_ep) {
                continue;
            }
            if (s_settle[i].reads_left == 0 || !s_settle[i].judge ||
                s_settle[i].want_on != 1 || s_settle[i].fixes_left == 0 ||
                level > ZBC_FADE_FLOOR_LEVEL || s_settle[i].want_level <= ZBC_FADE_FLOOR_LEVEL * 8) {
                break;
            }
            s_settle[i].fixes_left--;
            s_settle[i].judge = false;
            s_radio.corrections++;
            ESP_LOGW(TAG, "Device 0x%04x ep %d is at level %d but was told %d - sending it again",
                     src_addr, src_ep, level, s_settle[i].want_level);
            resend_intent_locked(src_addr, src_ep, s_settle[i].want_on, s_settle[i].want_level);
            /* Same re-arm as the on/off correction above, for the same reasons. */
            s_settle[i].reads_left = 1;
            s_settle[i].audit      = false;
            s_settle[i].answered   = false;
            s_settle[i].delivered  = false;
            s_settle[i].due_us     = esp_timer_get_time() + 1500000LL;
            settle_rearm_locked(false);
            break;
        }
        return;
    }

    if (cluster_id == EZB_ZCL_CLUSTER_ID_COLOR_CONTROL) {
        uint16_t u16 = *(const uint16_t *)value;
        uint8_t  u8  = *(const uint8_t *)value;
        note_answered(src_addr, src_ep);
        switch (attr_id) {
        case EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_CAPABILITIES_ID: {
            /* The cluster's presence only promised "some colour"; this says which
             * kind, and whether the lamp really does colour temperature. */
            uint8_t ieee[ZBC_IEEE_LEN];
            if (!ieee_of(src_addr, ieee)) {
                return;
            }
            uint16_t features = 0;
            if (u16 & EZB_ZCL_COLOR_CONTROL_COLOR_CAPABILITIES_HUE_SATURATION_SUPPORTED) {
                features |= ZBC_FEAT_COLOR_HS;
            }
            if (u16 & EZB_ZCL_COLOR_CONTROL_COLOR_CAPABILITIES_XY_SUPPORTED) {
                features |= ZBC_FEAT_COLOR_XY;
            }
            if (u16 & EZB_ZCL_COLOR_CONTROL_COLOR_CAPABILITIES_COLOR_TEMPERATURE_SUPPORTED) {
                features |= ZBC_FEAT_COLOR_TEMP;
            }
            device_db_add_features(ieee, src_ep, features);
            /* The provisional bit the cluster list set goes when the lamp says it
             * has no colour temperature - a colour-only lamp is not tunable white.
             * Only on an answer that names something: a lamp reporting no
             * capability at all is believed less than its cluster list. */
            if (u16 != 0 && !(features & ZBC_FEAT_COLOR_TEMP)) {
                device_db_clear_features(ieee, src_ep, ZBC_FEAT_COLOR_TEMP);
            }
            ESP_LOGI(TAG, "Device 0x%04x ep %d colour capabilities 0x%04x", src_addr, src_ep, u16);
            return;
        }
        case EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID:
            device_db_set_attr_by_short(src_addr, src_ep, ZBC_ATTR_COLOR_MODE, u8);
            return;
        case EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_MIREDS_ID:
            device_db_set_attr_by_short(src_addr, src_ep, ZBC_ATTR_CT, u16);
            color_correct_locked(src_addr, src_ep, ZBC_COLOR_PART_CT, u16);
            return;
        case EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MIN_MIREDS_ID:
            device_db_set_attr_by_short(src_addr, src_ep, ZBC_ATTR_CT_MIN, u16);
            return;
        case EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MAX_MIREDS_ID:
            device_db_set_attr_by_short(src_addr, src_ep, ZBC_ATTR_CT_MAX, u16);
            return;
        case EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID:
            device_db_set_attr_by_short(src_addr, src_ep, ZBC_ATTR_HUE, u8);
            color_correct_locked(src_addr, src_ep, ZBC_COLOR_PART_HUE, u8);
            return;
        case EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID:
            device_db_set_attr_by_short(src_addr, src_ep, ZBC_ATTR_SATURATION, u8);
            color_correct_locked(src_addr, src_ep, ZBC_COLOR_PART_SAT, u8);
            return;
        default:
            return;
        }
    }

    if (cluster_id != EZB_ZCL_CLUSTER_ID_BASIC || attr_type != EZB_ZCL_ATTR_TYPE_STRING) {
        return;
    }
    uint8_t ieee[ZBC_IEEE_LEN];
    if (!ieee_of(src_addr, ieee)) {
        return;
    }
    char text[ZBC_STRING_LEN];
    zcl_string_copy(text, sizeof(text), value);
    if (attr_id == EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID) {
        device_db_set_identity(ieee, text, NULL);
        ESP_LOGI(TAG, "Device 0x%04x manufacturer: %s", src_addr, text);
    } else if (attr_id == EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID) {
        device_db_set_identity(ieee, NULL, text);
        ESP_LOGI(TAG, "Device 0x%04x model: %s", src_addr, text);
    }
}

static uint16_t src_short_addr(const ezb_zcl_cmd_hdr_t *hdr)
{
    if (!hdr || hdr->src_addr.addr_mode != EZB_ADDR_MODE_SHORT) {
        return 0xFFFF;
    }
    return hdr->src_addr.u.short_addr;
}

static void handle_read_attr_rsp(ezb_zcl_cmd_read_attr_rsp_message_t *msg)
{
    uint16_t saddr = src_short_addr(msg->in.header);
    if (saddr == 0xFFFF) {
        return;
    }
    device_db_touch_by_short(saddr);
    for (ezb_zcl_read_attr_rsp_variable_t *v = msg->in.variables; v; v = v->next) {
        if (v->status == EZB_ZCL_STATUS_SUCCESS) {
            handle_attribute(saddr, msg->in.header->src_ep, msg->info.cluster_id,
                             v->attr_id, v->attr_type, v->attr_value);
        }
    }
}

static void handle_report_attr(ezb_zcl_cmd_report_attr_message_t *msg)
{
    uint16_t saddr = src_short_addr(msg->in.header);
    if (saddr == 0xFFFF) {
        return;
    }
    device_db_touch_by_short(saddr);
    for (ezb_zcl_report_attr_variable_t *v = msg->in.variables; v; v = v->next) {
        handle_attribute(saddr, msg->in.header->src_ep, msg->info.cluster_id,
                         v->attr_id, v->attr_type, v->attr_value);
    }
}

static void schedule_ota_recheck(const uint8_t ieee[ZBC_IEEE_LEN], uint32_t from_version);
static void ota_recheck_work(void);
static void ota_wake_work(void);
static bool device_listens(const zbc_device_t *dev);
static uint8_t ota_client_ep(const zbc_device_t *dev);
static esp_err_t send_image_notify(uint16_t dst, uint8_t dst_ep);

/* The stack's OTA Upgrade server, which answers every request itself from the
 * image attached in ota_attach_locked, asking here at each step. What to answer
 * is zb_ota.c's to decide; this only carries it across. */
static void handle_ota_server_progress(ezb_zcl_ota_upgrade_server_progress_message_t *msg)
{
    const uint16_t hdr_addr = src_short_addr(msg->in.header);
    switch (msg->in.progress) {
    case EZB_ZCL_OTA_UPGRADE_PROGRESS_START: {
        heap_watch_mark("ota_query", msg->in.start.query.short_address);
        /* The identity was taken from the raw frame already, where it can be
         * told whether the hardware version was sent at all; here it cannot. */
        const zbc_ota_qni_t q = {
            .manuf_code   = msg->in.start.query.manuf_code,
            .image_type   = msg->in.start.query.image_type,
            .file_version = msg->in.start.query.file_version,
        };
        const uint16_t addr = msg->in.start.query.short_address;
        msg->out.result = zb_ota_admit(addr, msg->in.start.query.ep_id, &q) ? EZB_ZCL_STATUS_SUCCESS
                                                                              : EZB_ZCL_STATUS_NO_IMAGE_AVAILABLE;
        break;
    }
    case EZB_ZCL_OTA_UPGRADE_PROGRESS_SENDING:
        msg->out.result = zb_ota_note_block(hdr_addr, msg->in.sending.file_offset) ? EZB_ZCL_STATUS_SUCCESS
                                                                                   : EZB_ZCL_STATUS_NO_IMAGE_AVAILABLE;
        device_db_touch_by_short(hdr_addr);
        break;
    case EZB_ZCL_OTA_UPGRADE_PROGRESS_FINISH: {
        heap_watch_mark("ota_upgrade_end", hdr_addr);
        uint8_t  ieee[ZBC_IEEE_LEN];
        uint32_t from = 0;
        if (zb_ota_note_end(hdr_addr, msg->in.finish.status, &from) && msg->in.finish.status == 0x00 &&
            device_db_ieee_by_short(hdr_addr, ieee)) {
            schedule_ota_recheck(ieee, from);
        }
        msg->out.result = EZB_ZCL_STATUS_SUCCESS;
        /* Upgrade now. The response carries two UTC times, and a current time
         * of zero makes the upgrade time an offset from now - zero, at once.
         * The SDK declares both fields 16 bits wide where the frame has 32;
         * zero is the one value that reads the same either way. */
        msg->out.finish.current_time = 0;
        msg->out.finish.upgrade_time = 0;
        device_db_touch_by_short(hdr_addr);
        break;
    }
    default:
        msg->out.result = EZB_ZCL_STATUS_SUCCESS;
        break;
    }
}

static void zcl_core_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    switch (callback_id) {
    case EZB_ZCL_CORE_READ_ATTR_RSP_CB_ID:
        handle_read_attr_rsp((ezb_zcl_cmd_read_attr_rsp_message_t *)message);
        break;
    case EZB_ZCL_CORE_REPORT_ATTR_CB_ID:
        handle_report_attr((ezb_zcl_cmd_report_attr_message_t *)message);
        break;
    case EZB_ZCL_CORE_CONFIG_REPORT_RSP_CB_ID: {
        /* A refused Configure Reporting is logged; the silence that follows would
         * otherwise look like a radio problem. As with the write response: one
         * SUCCESS record with attr_id 0xFFFF when all was accepted, otherwise a
         * record per attribute that failed. */
        ezb_zcl_cmd_config_report_rsp_message_t *rsp = message;
        uint16_t caddr = src_short_addr(rsp->in.header);
        uint8_t  cep   = rsp->in.header ? rsp->in.header->src_ep : 0;
        bool     refused = false;
        for (ezb_zcl_config_report_rsp_variable_t *v = rsp->in.variables; v; v = v->next) {
            refused = refused || v->status != EZB_ZCL_STATUS_SUCCESS;
            if (v->status == EZB_ZCL_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "Device 0x%04x ep %d accepted the reporting configuration for cluster 0x%04x",
                         caddr, cep, rsp->info.cluster_id);
            } else {
                ESP_LOGW(TAG, "Device 0x%04x ep %d refused reporting for attribute 0x%04x on cluster 0x%04x"
                              " (status 0x%02x) - its state will only be known when we ask",
                         caddr, cep, v->attr_id, rsp->info.cluster_id, v->status);
            }
        }
        /* Answered either way, so it is not asked again this boot; silence is. */
        if (caddr != 0xFFFF) {
            mark_configured(caddr, cep, rsp->info.cluster_id, refused);
        }
        break;
    }

    case EZB_ZCL_CORE_WRITE_ATTR_RSP_CB_ID: {
        /* A lamp is free to refuse a write. Say so rather than leaving the fade
         * time looking applied when it never took. */
        ezb_zcl_cmd_write_attr_rsp_message_t *rsp = message;
        /* A wholly successful write answers with one SUCCESS record, attr_id 0xFFFF;
         * otherwise a record names each attribute that failed. Both are logged:
         * writes happen only on a settings change or a join. */
        uint16_t waddr = src_short_addr(rsp->in.header);
        for (ezb_zcl_write_attr_rsp_variable_t *v = rsp->in.variables; v; v = v->next) {
            if (v->status == EZB_ZCL_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "Device 0x%04x accepted the attribute write", waddr);
            } else {
                ESP_LOGW(TAG, "Device 0x%04x refused attribute 0x%04x (status 0x%02x)"
                              " - it keeps its own pace", waddr, v->attr_id, v->status);
            }
        }
        break;
    }
    case EZB_ZCL_CORE_GROUPS_ADD_GROUP_RSP_CB_ID: {
        /* Our own answer to join_remote_groups. 0x8a is DUPLICATE_EXISTS, which
         * is a success: the membership is kept in NVS and survives a reboot. */
        const ezb_zcl_groups_add_group_rsp_message_t *rsp = message;
        if (rsp->in.status == EZB_ZCL_STATUS_SUCCESS || rsp->in.status == 0x8a) {
            ESP_LOGI(TAG, "In group 0x%04x, so a BILRESA on that channel is heard",
                     rsp->in.group_id);
        } else {
            ESP_LOGW(TAG, "Joining group 0x%04x failed (status 0x%02x) - a BILRESA on that"
                          " channel will not be heard", rsp->in.group_id, rsp->in.status);
        }
        break;
    }
    case EZB_ZCL_CORE_DEFAULT_RSP_CB_ID: {
        ezb_zcl_cmd_default_rsp_message_t *rsp = message;
        if (rsp->in.status_code == EZB_ZCL_STATUS_SUCCESS) {
            break;
        }
        const uint16_t daddr = src_short_addr(rsp->in.header);
        const uint8_t  dep   = rsp->in.header ? rsp->in.header->src_ep : 0;
        ESP_LOGW(TAG, "Device 0x%04x ep %d rejected command 0x%02x on cluster 0x%04x"
                      " with status 0x%02x", daddr, dep, rsp->in.rsp_to_cmd,
                 rsp->info.cluster_id, rsp->in.status_code);

        /* A lamp without attribute reporting refuses Configure Reporting here, not
         * with a Configure Reporting Response: a Hue LCT010 answers 0x82 on On/Off
         * and Level. It is an answer all the same, and asking again every scan
         * gets the same one. 0x06 is also Level's StepWithOnOff, which nothing
         * sends to a lamp. */
        if (rsp->in.rsp_to_cmd == EZB_ZCL_CMD_CONFIGURE_REPORTING && daddr != 0xFFFF &&
            !config_done(daddr, dep, rsp->info.cluster_id)) {
            ESP_LOGW(TAG, "Device 0x%04x ep %d has no reporting for cluster 0x%04x - its state"
                          " will only be known when we ask", daddr, dep, rsp->info.cluster_id);
            mark_configured(daddr, dep, rsp->info.cluster_id, true);
        }

        /* A refused OffWithEffect leaves the lamp on. The command is optional, and the
         * off path relies on it as the only ZCL command that fades and switches
         * off with no gap for another command to land in. Fall back to a plain
         * Off for this endpoint from now on, and send one now: it is still lit. */
        if (rsp->info.cluster_id == EZB_ZCL_CLUSTER_ID_ON_OFF &&
            rsp->in.rsp_to_cmd == EZB_ZCL_CMD_ON_OFF_OFF_WITH_EFFECT_ID && daddr != 0xFFFF) {
            if (!no_off_effect(daddr, dep)) {
                ESP_LOGW(TAG, "Device 0x%04x ep %d has no OffWithEffect; using a plain Off"
                              " for it - it will switch off without a fade", daddr, dep);
                mark_no_off_effect(daddr, dep);
            }
            const zbc_cmd_t off = { .kind = ZBC_CMD_ONOFF, .a = ZB_ONOFF_OFF };
            gate_send_locked(daddr, dep, &off, ZBC_CH_LEVEL);
        }
        break;
    }
    case EZB_ZCL_CORE_OTA_UPGRADE_SERVER_PROGRESS_CB_ID:
        handle_ota_server_progress((ezb_zcl_ota_upgrade_server_progress_message_t *)message);
        break;
    default:
        ESP_LOGD(TAG, "Unhandled ZCL action 0x%04lx", (unsigned long)callback_id);
        break;
    }
}

/* ---------------------------------------------------------- OTA server */

/* Hands the stored image to the stack's OTA server. At network-up, and after an
 * upload. Stack lock held. */
static void ota_attach_locked(void)
{
    const void *ptr;
    uint32_t    len;
    if (s_ota_file || !zb_ota_image(&ptr, &len)) {
        return;
    }
    s_ota_file = ezb_zcl_ota_upgrade_add_ota_file(ZBC_GATEWAY_EP_ID, ptr, len);
    if (!s_ota_file) {
        ESP_LOGE(TAG, "The OTA server refused the stored image (%" PRIu32 " bytes)", len);
    }
    zb_ota_set_offered(s_ota_file != EZB_ZCL_OTA_FILE_INVALID_HANDLE);
}

/* ------------------------------------------------------- stack signals */

static void refresh_all_states(bool only_unknown);
static void reread_colours(void);
static void scan_neighbours(void);
static void warn_if_channels_collide(void);
static void channel_work(void);
static void survey_work(void);
static void settle_work(void);

/* ------------------------------------------------ the worker task
 *
 * Every job below takes the Zigbee stack lock, and none may run in the
 * esp_timer task: all esp_timer callbacks share one task, including the 10 ms
 * button sampler whose debounce and gesture timings depend on that rate. A job
 * waiting there on a lock the mainloop holds stalls the sampler, and a switch
 * then feels wrong with nothing in a log to say why. So the timers only notify
 * this task, which does the work. */
#define ZBC_WORK_SETTLE   (1u << 0)
#define ZBC_WORK_REFRESH  (1u << 1)
#define ZBC_WORK_RETRY    (1u << 2)
#define ZBC_WORK_ANNOUNCE (1u << 3)
#define ZBC_WORK_OTA      (1u << 4)
#define ZBC_WORK_OTA_WAKE (1u << 5)
#define ZBC_WORK_HUE_BIND (1u << 6)
#define ZBC_WORK_ASK      (1u << 7)
#define ZBC_WORK_CHANNEL  (1u << 8)
#define ZBC_WORK_SURVEY   (1u << 9)

static TaskHandle_t s_work_task;

static void work_post(uint32_t bits)
{
    if (s_work_task) {
        xTaskNotify(s_work_task, bits, eSetBits);
    }
}

/* The timer callbacks. Deliberately trivial - see above. */
static void settle_timer_cb(void *arg)  { (void)arg; work_post(ZBC_WORK_SETTLE); }
static void refresh_timer_cb(void *arg) { (void)arg; work_post(ZBC_WORK_REFRESH); }
static void commissioning_retry_cb(void *arg) { (void)arg; work_post(ZBC_WORK_RETRY); }
static void announce_timer_cb(void *arg) { (void)arg; work_post(ZBC_WORK_ANNOUNCE); }
static void ota_recheck_timer_cb(void *arg) { (void)arg; work_post(ZBC_WORK_OTA); }
static void ota_wake_post(void) { work_post(ZBC_WORK_OTA_WAKE); }
static void channel_timer_cb(void *arg) { (void)arg; work_post(ZBC_WORK_CHANNEL); }
static void survey_timer_cb(void *arg) { (void)arg; work_post(ZBC_WORK_SURVEY); }

/* A device heard from with no endpoints on record: a remote whose interrogation
 * missed because it had gone back to sleep. Having just sent something, it is
 * awake: ask again, from the work task (never inside frame dispatch), at most
 * every 5 s. Not counted against ZBC_INTERROGATE_TRIES.
 *
 * Bounded all the same, per device and per announce (interrogate_tries_reset
 * gives them back): a device with nothing to record - a sensor, a battery device
 * with no cluster a press can arrive on - has no endpoints for ever, and would
 * otherwise cost a whole interrogation per report. Zigbee task only, so no lock. */
#define ZBC_ASK_GAP_US  (5LL * 1000 * 1000)
#define ZBC_HEARD_ASKS  6
static volatile uint16_t s_ask_addr;
static int64_t           s_ask_at;

/* How many times something has been done for a device this boot - asked what it
 * is, here and in retry_unknown_devices - with the row free while that is 0.
 * One table per budget: the two are counted from different tasks (this one the
 * Zigbee task, retry_unknown_devices the work task), so sharing rows would let
 * both claim the same free one. */
typedef struct {
    uint8_t ieee[ZBC_IEEE_LEN];
    uint8_t n;
} zbc_budget_t;

/* The device's row, or a free one claimed for it; NULL when the table is full. */
static zbc_budget_t *budget_row(zbc_budget_t *t, const uint8_t ieee[ZBC_IEEE_LEN])
{
    zbc_budget_t *spare = NULL;
    for (int i = 0; i < CONFIG_ZBC_MAX_DEVICES; i++) {
        if (t[i].n && memcmp(t[i].ieee, ieee, ZBC_IEEE_LEN) == 0) {
            return &t[i];
        }
        if (!spare && !t[i].n) {
            spare = &t[i];
        }
    }
    if (spare) {
        memcpy(spare->ieee, ieee, ZBC_IEEE_LEN);
    }
    return spare;
}

static void budget_reset(zbc_budget_t *t, const uint8_t ieee[ZBC_IEEE_LEN])
{
    for (int i = 0; i < CONFIG_ZBC_MAX_DEVICES; i++) {
        if (memcmp(t[i].ieee, ieee, ZBC_IEEE_LEN) == 0) {
            t[i].n = 0;
        }
    }
}

static zbc_budget_t s_heard_asks[CONFIG_ZBC_MAX_DEVICES];

/* A bind that went unanswered is sent again by the next neighbour scan, but not
 * for ever: esp-zigbee-lib 2.0.4 keeps one block (~44 bytes) of every bind
 * request that ends without an answer. An Aqara lumi.sensor_switch.aq2, our
 * child on the Shelly, never answers one, and its three clusters every five
 * minutes took ~2 KB an hour from the heap - measured with
 * ZBC_HEAP_WATCH_TASKS, against nothing for the eight Configure Reporting
 * requests its Hue lamps leave unanswered every scan. So each device gets this
 * many scans that may bind it per boot, and an announce gives them back. */
#define ZBC_BIND_SCANS 6

static zbc_budget_t s_bind_scans[CONFIG_ZBC_MAX_DEVICES];

static bool bind_scan_take(const uint8_t ieee[ZBC_IEEE_LEN])
{
    zbc_budget_t *row = budget_row(s_bind_scans, ieee);
    if (!row || row->n >= ZBC_BIND_SCANS) {
        return false;
    }
    row->n++;
    return true;
}

static void ask_when_heard(uint16_t short_addr)
{
    if (device_db_ep_count_by_short(short_addr) != 0) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (s_ask_at != 0 && now - s_ask_at < ZBC_ASK_GAP_US) {
        return;
    }
    uint8_t ieee[ZBC_IEEE_LEN];
    if (!ieee_of(short_addr, ieee)) {
        return;
    }
    zbc_budget_t *row = budget_row(s_heard_asks, ieee);
    if (!row || row->n >= ZBC_HEARD_ASKS) {
        return;
    }
    row->n++;
    s_ask_at   = now;
    s_ask_addr = short_addr;
    work_post(ZBC_WORK_ASK);
}

static void ask_work(void)
{
    const uint16_t addr = s_ask_addr;
    if (!addr) {
        return;
    }
    ESP_LOGI(TAG, "Device 0x%04x was heard with no endpoints on record; asking what it is", addr);
    esp_zigbee_lock_acquire(portMAX_DELAY);
    interrogate_device(addr);
    esp_zigbee_lock_release();
}

/* ---------------------------------------------------- the Hue dimmer's cluster */

/* Where a Hue dimmer (RWL020/021) says what its buttons did, and where the
 * buttons are filed. Zigbee2MQTT's recipe: bind Philips' cluster on endpoint 2
 * to us, and write Basic 0x0031 = 0x000B with Signify's manufacturer code, which
 * it does for every Hue remote it configures. Its presses are filed under
 * endpoint 1 - where its standard commands come from, and where every button
 * set up for it already is. HUE_DIMMER_* and HUE_MANUF_CODE are defined with
 * remote_model_by_short, above. */
/* Between bind requests while none has been answered. A request to a sleepy
 * dimmer is held for it only until its next poll and dropped after a few
 * seconds, so it is asked again on a later press, but not on every press of a
 * burst. */
#define HUE_BIND_GAP_US      (5LL * 1000 * 1000)

static volatile uint16_t s_hue_bind_addr;
static int64_t           s_hue_bind_at;

void zb_get_hue_stats(zb_hue_stats_t *out) { *out = s_hue; }

static void hue_bind_cb(const ezb_zdp_bind_req_result_t *result, void *user_ctx)
{
    const uint16_t addr = (uint16_t)(uintptr_t)user_ctx;
    if (!result || result->error != EZB_ERR_NONE || !result->rsp) {
        s_hue.bind_status = -2;
        ESP_LOGW(TAG, "Hue dimmer 0x%04x: the button cluster bind got no answer", addr);
        return;
    }
    s_hue.bind_status = result->rsp->status;
    mark_bound(addr, HUE_DIMMER_NOTIFY_EP, ZB_REMOTE_CL_HUE,     /* answered, either way */
               result->rsp->status != EZB_ZDP_STATUS_SUCCESS);
    ESP_LOGI(TAG, "Hue dimmer 0x%04x: button cluster bind answered 0x%02x", addr, result->rsp->status);
}

/* Ask for the binding from the work task, never from inside frame dispatch.
 * `force` for an announce, the one moment the old binding is known to be gone;
 * otherwise at most every HUE_BIND_GAP_US, and not after the bind has been answered. */
static void hue_bind_post(uint16_t short_addr, bool force)
{
    const int64_t now = esp_timer_get_time();
    if (!force) {
        if (bind_done(short_addr, HUE_DIMMER_NOTIFY_EP, ZB_REMOTE_CL_HUE)) {
            return;
        }
        if (s_hue_bind_at != 0 && now - s_hue_bind_at < HUE_BIND_GAP_US) {
            return;
        }
    }
    s_hue_bind_at   = now;
    s_hue_bind_addr = short_addr;
    work_post(ZBC_WORK_HUE_BIND);
}

static void hue_bind_work(void)
{
    const uint16_t addr = s_hue_bind_addr;
    if (!addr) {
        return;
    }
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ESP_LOGI(TAG, "Hue dimmer 0x%04x: binding its button cluster 0x%04x on endpoint %d",
             addr, ZB_REMOTE_CL_HUE, HUE_DIMMER_NOTIFY_EP);
    s_hue.bind_asked++;
    bind_to_us(addr, HUE_DIMMER_NOTIFY_EP, ZB_REMOTE_CL_HUE, hue_bind_cb, (void *)(uintptr_t)addr);

    uint16_t            mode = 0x000B;
    ezb_zcl_attribute_t attr = {
        .id   = 0x0031,
        .data = { .type = EZB_ZCL_ATTR_TYPE_MAP16, .size = sizeof(mode), .value = &mode },
    };
    ezb_zcl_write_attr_cmd_t cmd = {
        .cmd_ctrl = {
            .dst_addr   = { .addr_mode = EZB_ADDR_MODE_SHORT, .u.short_addr = addr },
            .dst_ep     = HUE_DIMMER_NOTIFY_EP,
            .src_ep     = ZBC_GATEWAY_EP_ID,
            .cluster_id = EZB_ZCL_CLUSTER_ID_BASIC,
            .manuf_code = HUE_MANUF_CODE,
        },
        .payload = { .attr_number = 1, .attr_field = &attr },
    };
    const ezb_err_t err = ezb_zcl_write_attr_cmd_req(&cmd);
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Hue dimmer 0x%04x: the mode write could not be sent (0x%04x)", addr, err);
    }
    esp_zigbee_lock_release();
}

/* A frame on Philips' cluster from a dimmer: it speaks it, so that is where its
 * presses are read from now on. Recorded once. */
static void hue_note_notify(uint16_t short_addr)
{
    if (hue_buttons_on(short_addr)) {
        return;
    }
    uint8_t ieee[ZBC_IEEE_LEN];
    if (ieee_of(short_addr, ieee) &&
        device_db_add_features(ieee, HUE_DIMMER_BUTTON_EP, ZBC_FEAT_HUE_BUTTONS) == ESP_OK) {
        ESP_LOGI(TAG, "Hue dimmer 0x%04x speaks 0x%04x: its presses are read there from now on,"
                      " and its endpoint %d commands are not", short_addr, ZB_REMOTE_CL_HUE,
                 HUE_DIMMER_BUTTON_EP);
    }
}

/* A known device announcing itself: for a Hue dimmer that means reset,
 * re-paired or rejoined, so its binding to Philips' cluster may be gone. Its
 * endpoint 1 commands count again until 0xFC00 is heard from it, and it is asked
 * for the binding now, while awake. A new dimmer has no model yet; its first
 * press asks. The previous bind answer is forgotten with the flag, or a forced
 * request that went unanswered would never be repeated (hue_bind_post asks
 * nothing of a dimmer it believes is bound). */
static void hue_note_announce(uint16_t short_addr, const uint8_t ieee[ZBC_IEEE_LEN])
{
    if (remote_model_by_short(short_addr) != ZB_REMOTE_MODEL_HUE_DIMMER) {
        return;
    }
    device_db_clear_features(ieee, HUE_DIMMER_BUTTON_EP, ZBC_FEAT_HUE_BUTTONS);
    done_forget(s_bound, ieee, HUE_DIMMER_NOTIFY_EP, ZB_REMOTE_CL_HUE);
    hue_bind_post(short_addr, true);
}

static void retry_work(void)
{
    /* Ten quiet waits is five minutes, which is past patient; only then is it
     * worth assuming nothing is happening and starting over. */
    static int waits = 0;

    esp_zigbee_lock_acquire(portMAX_DELAY);
    forming_sample_locked();
    if (s_retry_mode == EZB_BDB_MODE_NETWORK_FORMATION && !s_network_up) {
        if (s_formation_failed) {
            /* The stack reported a failure, so nothing is in progress: start again
             * now rather than waiting out the count below. */
            s_formation_failed = false;
            waits = 0;
            (void)start_commissioning(s_retry_mode);
        } else if (++waits >= 10) {
            waits = 0;
            if (s_forming.bdb_status == EZB_BDB_STATUS_IN_PROGRESS) {
                /* While one is in progress a start is refused with EZB_ERR_BUSY and
                 * ezb_bdb_cancel_formation() returns EZB_ERR_NOT_SUPPORTED, so
                 * there is nothing to restart: log it, and let the formation log
                 * in /api/status show it. */
                ESP_LOGW(TAG, "Still no network after five minutes, and the formation is still "
                              "in progress");
            } else {
                ESP_LOGW(TAG, "Still no network after five minutes, starting formation again "
                              "(commissioning status %d)", s_forming.bdb_status);
                (void)start_commissioning(s_retry_mode);
            }
        } else {
            ESP_LOGW(TAG, "Forming a network, %ds so far - a scan shares the antenna with "
                          "Wi-Fi and this can take minutes (commissioning status %d)",
                     waits * 30, s_forming.bdb_status);
        }
        /* Every path out of here has to give the lock back. Returning while
         * holding it stalls the SDK mainloop - so formation never completes -
         * and blocks every HTTP handler that takes the lock. */
        esp_zigbee_lock_release();
        esp_timer_start_once(s_retry_timer, 30 * 1000000ULL);
        return;
    }
    (void)start_commissioning(s_retry_mode);
    esp_zigbee_lock_release();
}

/* Ask again for the endpoints of anything never successfully interrogated.
 *
 * A battery remote is asleep again within seconds of announcing, so the active
 * endpoint request can go unanswered, leaving a row with no endpoints, no
 * features and - for a remote that sends commands - no bindings. It then has
 * nowhere to send and is silent however much it is pressed.
 *
 * scan_neighbours() re-interrogates a device with no endpoints, but a sleepy end
 * device never appears in the coordinator's neighbour table, so that covers lamps
 * and not remotes.
 *
 * Bounded per device: a request to something not listening is wasted airtime.
 * Three tries come in a burst right after an announce (announce_work), the only
 * time a freshly joined battery device is reliably awake; the rest ride on the
 * neighbour scans. Returns how many devices still have no endpoints. */
#define ZBC_INTERROGATE_TRIES 6

static zbc_budget_t s_interrogate_tried[CONFIG_ZBC_MAX_DEVICES];

/* An announce is a new chance: the tries are rationed per boot, and a remote
 * re-paired a second time would otherwise find them spent. The asks made when it
 * is heard (ask_when_heard) come back with it. */
static void interrogate_tries_reset(const uint8_t ieee[ZBC_IEEE_LEN])
{
    budget_reset(s_interrogate_tried, ieee);
    budget_reset(s_heard_asks, ieee);
    budget_reset(s_bind_scans, ieee);
}

static int retry_unknown_devices(void)
{
    /* One row at a time, as reread_colours does: this runs at every refresh,
     * and a whole-registry copy is 8 KB. On the Shelly that copy, landing in the
     * same second as a capped Wi-Fi receive burst, took the heap to 6.4 KB. */
    uint8_t (*ieees)[ZBC_IEEE_LEN] = calloc(CONFIG_ZBC_MAX_DEVICES, ZBC_IEEE_LEN);
    if (!ieees) {
        return 0;
    }
    const size_t n = device_db_list_ieee(ieees, CONFIG_ZBC_MAX_DEVICES);
    int          unknown = 0;
    for (size_t i = 0; i < n; i++) {
        zbc_device_t dev;
        /* A row restored from another network's backup has no address here to
         * ask; it is interrogated when it joins and announces. */
        if (!device_db_get(ieees[i], &dev) || dev.ep_count > 0 || !zbc_short_known(dev.short_addr)) {
            continue;
        }
        unknown++;
        zbc_budget_t *row = budget_row(s_interrogate_tried, dev.ieee);
        if (!row || row->n >= ZBC_INTERROGATE_TRIES) {
            continue;
        }
        row->n++;
        ESP_LOGI(TAG, "Device 0x%04x still has no endpoints; asking again (try %d of %d)",
                 dev.short_addr, row->n, ZBC_INTERROGATE_TRIES);
        esp_zigbee_lock_acquire(portMAX_DELAY);
        interrogate_device(dev.short_addr);
        esp_zigbee_lock_release();
    }
    free(ieees);
    return unknown;
}

/* The burst after an announce: 3 s, 10 s and 30 s later, while a battery remote
 * that has just joined is still awake (the neighbour scans come at 5 s, 65 s and
 * then every five minutes). Stops once nothing is left to ask; each announce
 * starts it over. */
static void announce_work(void)
{
    static const uint32_t gaps_s[] = { 3, 7, 20 };   /* 3, 10, 30 s after the announce */
    if (retry_unknown_devices() == 0) {
        return;
    }
    if (++s_announce_step < sizeof(gaps_s) / sizeof(gaps_s[0])) {
        esp_timer_start_once(s_announce_timer, (uint64_t)gaps_s[s_announce_step] * 1000000ULL);
    }
}

/* A device restarts onto a downloaded image without necessarily announcing (a
 * Hue LST002 does not), so interrogation never re-reads its version. Ask it 1, 2,
 * 4 and 8 minutes after the Upgrade End, stopping once a different version comes
 * back. One device at a time, like the image. */
static portMUX_TYPE s_ota_recheck_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool     active;
    uint8_t  ieee[ZBC_IEEE_LEN];
    uint32_t from_version;
    uint8_t  step;
} s_ota_recheck;

static const uint32_t OTA_RECHECK_GAPS_S[] = { 60, 60, 120, 240, 240 };
#define OTA_RECHECK_STEPS (sizeof(OTA_RECHECK_GAPS_S) / sizeof(OTA_RECHECK_GAPS_S[0]))

static void schedule_ota_recheck(const uint8_t ieee[ZBC_IEEE_LEN], uint32_t from_version)
{
    portENTER_CRITICAL(&s_ota_recheck_mux);
    s_ota_recheck.active       = true;
    s_ota_recheck.step         = 0;
    s_ota_recheck.from_version = from_version;
    memcpy(s_ota_recheck.ieee, ieee, ZBC_IEEE_LEN);
    portEXIT_CRITICAL(&s_ota_recheck_mux);
    esp_timer_stop(s_ota_recheck_timer);
    esp_timer_start_once(s_ota_recheck_timer, (uint64_t)OTA_RECHECK_GAPS_S[0] * 1000000ULL);
}

static void ota_recheck_work(void)
{
    uint8_t  ieee[ZBC_IEEE_LEN];
    uint32_t from;
    uint8_t  step;
    portENTER_CRITICAL(&s_ota_recheck_mux);
    const bool active = s_ota_recheck.active;
    memcpy(ieee, s_ota_recheck.ieee, ZBC_IEEE_LEN);
    from = s_ota_recheck.from_version;
    step = s_ota_recheck.step++;
    portEXIT_CRITICAL(&s_ota_recheck_mux);
    if (!active) {
        return;
    }
    zb_ota_device_t info;
    if (zb_ota_device_info(ieee, &info) && info.version_known && info.file_version != from) {
        ESP_LOGI(TAG, "Device now reports firmware 0x%08" PRIx32 " (was 0x%08" PRIx32 ")", info.file_version, from);
        portENTER_CRITICAL(&s_ota_recheck_mux);
        s_ota_recheck.active = false;
        portEXIT_CRITICAL(&s_ota_recheck_mux);
        return;
    }
    /* The last tick only looks at what the previous ask brought back. */
    if (step + 1 >= OTA_RECHECK_STEPS) {
        ESP_LOGW(TAG, "Device still reports firmware 0x%08" PRIx32 " after its download", from);
        portENTER_CRITICAL(&s_ota_recheck_mux);
        s_ota_recheck.active = false;
        portEXIT_CRITICAL(&s_ota_recheck_mux);
        return;
    }
    zb_ota_server_ask(ieee);
    esp_timer_start_once(s_ota_recheck_timer, (uint64_t)OTA_RECHECK_GAPS_S[step + 1] * 1000000ULL);
}

/* A sleepy device a firmware notify was waiting for has just been heard from
 * (zb_ota_wake_heard, in the raw frame handler), so it is awake and about to
 * poll: send the notify now, and it collects it with that poll. */
static void ota_wake_work(void)
{
    uint8_t ieee[ZBC_IEEE_LEN];
    while (zb_ota_wake_take(ieee)) {
        zbc_device_t dev;
        if (!s_network_up || !device_db_get(ieee, &dev) || !zbc_short_known(dev.short_addr)) {
            continue;
        }
        const uint8_t   ep  = ota_client_ep(&dev);
        const esp_err_t err = send_image_notify(dev.short_addr, ep);
        zb_ota_wake_sent(err == ESP_OK);
        ESP_LOGI(TAG, "0x%04x woke up: Image Notify to ep %u %s", dev.short_addr, ep,
                 err == ESP_OK ? "sent" : zb_err_name(err));
    }
}

static void refresh_work(void)
{
    static int runs = 0;

    scan_neighbours();
    note_children(runs < 2 || runs % ZBC_ROUTER_WALK_EVERY == 0);
    retry_unknown_devices();
    /* Every lamp on the first pass; on the second, only what the first did not
     * learn. Reports carry the state from then on, and a lamp that is gone is
     * not worth a read every five minutes. */
    if (runs == 0) {
        refresh_all_states(false);
    } else if (runs == 1) {
        refresh_all_states(true);
    } else {
        /* Colour alone from here on: it is the one thing no lamp reports and
         * nothing else re-reads. See reread_colours. */
        reread_colours();
    }
    /* Every run, not just the first: Wi-Fi has usually not associated five
     * seconds in, so its channel is not yet known. Says its piece once. */
    warn_if_channels_collide();
    /* The neighbour table is still filling for the first minute after a restart,
     * and a scan five seconds in finds nothing. Look again shortly, then
     * periodically. */
    esp_timer_start_once(s_refresh_timer, (runs++ == 0 ? 60ULL : 300ULL) * 1000000ULL);
}

static void zb_work_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t bits = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        /* Every job below takes the stack lock and calls into the SDK, and
         * zb_coordinator_stop() ends in esp_zigbee_deinit(). A notification
         * that was already posted when the stack went down must not run. */
        if (!s_stack_started) {
            continue;
        }
        heap_watch_mark("zb_work", bits);
        if (bits & ZBC_WORK_RETRY)    { retry_work(); }
        if (bits & ZBC_WORK_SETTLE)   { settle_work(); }
        if (bits & ZBC_WORK_REFRESH)  { refresh_work(); }
        if (bits & ZBC_WORK_ANNOUNCE) { announce_work(); }
        if (bits & ZBC_WORK_OTA)      { ota_recheck_work(); }
        if (bits & ZBC_WORK_OTA_WAKE) { ota_wake_work(); }
        if (bits & ZBC_WORK_HUE_BIND) { hue_bind_work(); }
        if (bits & ZBC_WORK_ASK)      { ask_work(); }
        if (bits & ZBC_WORK_CHANNEL)  { channel_work(); }
        if (bits & ZBC_WORK_SURVEY)   { survey_work(); }
    }
}

static void schedule_commissioning_retry(ezb_bdb_comm_mode_mask_t mode, uint64_t delay_us)
{
    s_retry_mode = mode;
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, delay_us);
}

/* How long a delivered command may take before it counts as slow. A frame is
 * normally done in 24-130 ms; past half a second it has needed more than the
 * MAC's own quick retries. */
#define ZBC_SLOW_DELIVERY_US 500000LL

/* A "move to" command reports its destination only once the ramp is over, so an
 * immediate read catches the lamp halfway. Read again after the transition. */
static void data_confirm_cb(ezb_af_user_cnf_t *cnf, void *user_ctx)
{
    (void)user_ctx;
    if (!cnf) {
        return;
    }
    const uint16_t addr = cnf->dst_addr.u.short_addr;
    const uint8_t  ep   = cnf->dst_ep;

    /* Taken before gate_advance below sends the next frame and restarts the clock.
     * Each frame to a lamp waits for the one before, so one slow frame delays the
     * rest of that lamp's command by as much. */
    const int64_t took_us = cnf->status == 0 ? gate_inflight_age_us(addr, ep) : -1;
    if (took_us >= ZBC_SLOW_DELIVERY_US) {
        s_radio.cmd_slow++;
        s_radio.cmd_slow_ms += (uint32_t)(took_us / 1000);
    }

    reach_note(addr, cnf->status == 0);
    if (cnf->status != 0) {
        s_radio.cmd_undelivered++;
        ESP_LOGW(TAG, "Command to 0x%04x ep %d was not delivered (status 0x%02x)",
                 addr, ep, cnf->status);
    } else if (!gate_confirms_preset(addr, ep)) {
        /* The lamp has it. Silence or a disagreeing report from here on is about the
         * lamp or the answer, not about the command getting there, and
         * settle_work needs that before it decides to re-send.
         *
         * Not for the preset half of a fade in: the ramp that carries the intent
         * has not been sent yet, and a lost ramp must still be re-sent. */
        for (int i = 0; i < ZBC_SETTLE_SLOTS; i++) {
            if (s_settle[i].short_addr == addr && s_settle[i].ep == ep) {
                s_settle[i].delivered = true;
                break;
            }
        }

        /* Logged once per boot, as proof that this callback fires for a command that
         * worked: the gate opens on it. If this line never appears, every command
         * waits out ZBC_GATE_TIMEOUT_US behind the one before, and "No
         * confirmation for ..." warnings appear continuously. */
        static bool said = false;
        if (!said) {
            said = true;
            ESP_LOGI(TAG, "Delivery confirmations are working (0x%04x ep %d)", addr, ep);
        }
    }

    /* The stack has finished with this frame either way, so nothing of it can
     * still arrive: whatever was held back for this endpoint can go now. */
    const bool superseded = gate_advance(addr, ep);

    if (cnf->status == 0) {
        return;
    }

    /* A newer intent has just gone out in its place, so re-stating the old one
     * would undo it - and it would have to queue behind the new command to do
     * so, which is the reordering this whole mechanism exists to prevent. */
    if (superseded) {
        return;
    }

    for (int i = 0; i < ZBC_SETTLE_SLOTS; i++) {
        if (s_settle[i].short_addr != addr || s_settle[i].ep != ep) {
            continue;
        }
        /* Same bound as the correction above: a confirm that arrives after the
         * command has finished settling is reporting on something the user has
         * moved on from. */
        if (s_settle[i].reads_left == 0 || s_settle[i].want_on < 0 ||
            s_settle[i].fixes_left == 0) {
            break;
        }
        s_settle[i].fixes_left--;
        ESP_LOGW(TAG, "Sending it again straight away");
        resend_intent_locked(addr, ep, s_settle[i].want_on, s_settle[i].want_level);
        break;
    }
}

/* How long a lamp is given to answer a read before the next pass. Lamps are free
 * to ignore our transition time and ramp at their own pace - a Hue takes a few
 * seconds over a colour change. */
/* >>> host-test region: settle-body <<< */
/* How long after a transition the first read waits. The lamp has just been
 * told to take `transition_ds`; this is the grace on top of it, so a read is
 * not racing the last step of the lamp's own ramp. */
#define ZBC_SETTLE_MARGIN_US 400000LL

#define ZBC_SETTLE_GAP_US 2500000LL

/* How long to wait before asking a lamp that went quiet again, and how many
 * times. Longer than the settle gap because the lamp has already shown it is
 * not answering promptly, and short enough to beat the five-minute report that
 * is otherwise the only thing that clears an unknown state. */
#define ZBC_RECOVER_GAP_US 5000000LL
#define ZBC_RECOVER_TRIES  3

/* How far apart one pass spaces its reads. A group command gives every member
 * the same deadline, and reading them all in one tick puts up to three frames
 * per lamp on air at once, alongside the lamps' own reports. The first due slot
 * is read at once and the k-th put off by k of these; the timer then serves them
 * one per tick. */
#define ZBC_READ_STAGGER_US 50000LL

/* How far a lamp may sit from the colour it was told before the read that
 * follows counts it as not having taken it at all.
 *
 * Wide on purpose. This is not here to chase a lamp's rounding or its clamping
 * to its own physical range - both would have it corrected for ever - but to
 * catch a colour command the lamp ignored outright, which leaves it wherever it
 * was: typically its warmest, a hundred mireds or more away. 25 mireds is about
 * 100 K at 2000 K and 400 K at 4000 K.
 *
 * Hue is a circle of 254, so 0 and 253 are two apart, not 253. */
#define ZBC_CT_TOLERANCE_MIREDS 25
#define ZBC_HUE_TOLERANCE       8
#define ZBC_SAT_TOLERANCE       8

/* Whether `got` says the lamp did not take the colour the slot asked for.
 *
 * Only the part matching the kind of command that was sent is judged: after a
 * MoveToColorTemperature the lamp is in colour-temperature mode and its
 * CurrentHue is whatever it last was, so reading a hue difference there would
 * correct a colour nobody asked for.
 *
 * XY is deliberately absent: CurrentX and CurrentY are not in the settle read
 * (read_color_state), so there is nothing to judge, and adding them costs two
 * attributes on every colour read for every lamp. A lamp told xy is left to the
 * five-minute re-read, as before. */
static bool color_off_target(uint8_t kind, uint16_t want_a, uint16_t want_b,
                             int part, uint16_t got)
{
    if (kind == ZBC_CMD_CT && part == ZBC_COLOR_PART_CT) {
        int d = (int)got - (int)want_a;
        return (d < 0 ? -d : d) > ZBC_CT_TOLERANCE_MIREDS;
    }
    if (kind == ZBC_CMD_HS && part == ZBC_COLOR_PART_HUE) {
        int d = (int)got - (int)want_a;
        if (d < 0) {
            d = -d;
        }
        if (d > 127) {
            d = 254 - d;          /* the short way round the circle */
        }
        return d > ZBC_HUE_TOLERANCE;
    }
    if (kind == ZBC_CMD_HS && part == ZBC_COLOR_PART_SAT) {
        int d = (int)got - (int)want_b;
        return (d < 0 ? -d : d) > ZBC_SAT_TOLERANCE;
    }
    return false;
}

/* Arms the one settle timer for the earliest slot that still has a read due.
 * Caller holds the stack lock.
 *
 * Each slot carries its own deadline. Restarting the timer for the latest
 * command instead would cut short another lamp's window: its audit would run
 * before it could answer, its state be marked unknown, and its own command be
 * sent again. */
static void settle_rearm_locked(bool want_gate_tick)
{
    int64_t soonest = INT64_MAX;
    for (int i = 0; i < ZBC_SETTLE_SLOTS; i++) {
        if (s_settle[i].short_addr && s_settle[i].reads_left > 0 &&
            s_settle[i].due_us < soonest) {
            soonest = s_settle[i].due_us;
        }
    }
    if (want_gate_tick) {
        const int64_t t = esp_timer_get_time() + ZBC_SETTLE_GAP_US;
        if (t < soonest) {
            soonest = t;
        }
    }

    esp_timer_stop(s_settle_timer);
    if (soonest == INT64_MAX) {
        return;                       /* nothing outstanding; let it stay idle */
    }
    int64_t in_us = soonest - esp_timer_get_time();
    if (in_us < 1000) {
        in_us = 1000;
    }
    esp_timer_start_once(s_settle_timer, (uint64_t)in_us);
}

static void settle_work(void)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);

    /* A command the stack refused, or one whose confirmation never came, would
     * otherwise leave its endpoint's gate shut and anything queued behind it
     * stranded. This runs after every command, so it is the natural place. */
    const bool gate_tick = gate_flush_stale();
    const int64_t now    = esp_timer_get_time();
    int           sent   = 0;         /* reads put on air this pass, for the stagger */

    for (int i = 0; i < ZBC_SETTLE_SLOTS; i++) {
        if (!s_settle[i].short_addr || s_settle[i].reads_left == 0) {
            continue;
        }
        if (s_settle[i].due_us > now) {
            continue;                 /* this slot's turn has not come round yet */
        }
        if (sent > 0 && !s_settle[i].audit) {
            /* Another slot has already been read this pass; this one is put off
             * a little rather than sent beside it - see ZBC_READ_STAGGER_US. An
             * audit reads nothing, so it is not held back. */
            s_settle[i].due_us = now + (int64_t)sent * ZBC_READ_STAGGER_US;
            sent++;
            continue;
        }

        /* The audit pass reads nothing; it exists to notice that the last read
         * was never answered. */
        if (s_settle[i].audit) {
            s_settle[i].audit      = false;
            s_settle[i].reads_left = 0;
            s_settle[i].judge      = false;   /* the command is done settling */
            if (s_settle[i].answered) {
                continue;
            }
            /* Once per episode, not per attempt: the recovery reads below come back
             * through here, and one quiet lamp would otherwise count four times. */
            if (s_settle[i].recover_left == ZBC_RECOVER_TRIES) {
                s_radio.went_quiet++;
                ESP_LOGW(TAG, "Device 0x%04x ep %d has stopped answering; its state is unknown",
                         s_settle[i].short_addr, s_settle[i].ep);
                device_db_set_attr_by_short(s_settle[i].short_addr, s_settle[i].ep,
                                            ZBC_ATTR_ONOFF, ZBC_UNKNOWN);
            }

            /* Ask again before giving up on it. Nothing else re-reads a lamp, so an
             * unknown state would otherwise last until the lamp's own report, up
             * to ZBC_REPORT_MAX_INTERVAL_S later.
             *
             * Reads only: `want_on` is cleared so no correction fires and no
             * command is re-sent from these passes - a lamp that is not answering
             * is the worst moment to start telling it things. Bounded, so a lamp
             * that is gone stops being asked. */
            if (s_settle[i].recover_left > 0) {
                s_settle[i].recover_left--;
                s_settle[i].want_on    = -1;
                s_settle[i].fixes_left = 0;
                /* The colour intent too: the recovery read is judged like any
                 * last read, and color_correct_locked would otherwise send a
                 * lamp that has just come back the colour it missed. */
                s_settle[i].want_color       = ZBC_CMD_NONE;
                s_settle[i].color_fixes_left = 0;
                s_settle[i].answered   = false;
                s_settle[i].reads_left = 1;
                s_settle[i].due_us     = now + ZBC_RECOVER_GAP_US;
            }
            continue;
        }

        /* Silence since the previous read, and the command was never confirmed
         * delivered: only then is re-sending it right. After a confirmation,
         * silence means the answer went missing, and re-sending would restart a
         * ramp the lamp had already finished. Not on the first pass: no read has
         * been issued yet to go unanswered. */
        if (s_settle[i].reads_left < 2 && !s_settle[i].answered &&
            s_settle[i].want_on >= 0 && s_settle[i].fixes_left > 0 &&
            !s_settle[i].delivered) {
            s_settle[i].fixes_left--;
            ESP_LOGW(TAG, "Device 0x%04x ep %d never got the command and has not answered;"
                          " sending it again", s_settle[i].short_addr, s_settle[i].ep);
            resend_intent_locked(s_settle[i].short_addr, s_settle[i].ep,
                                 s_settle[i].want_on, s_settle[i].want_level);
            /* The command has just gone out again, so its reads start over: a read sent
             * beside it would be answered before the lamp acted, and as the judged
             * read it would provoke a third send. The budget above still bounds
             * the whole. */
            s_settle[i].reads_left = 2;
            s_settle[i].judge      = false;
            s_settle[i].audit      = false;
            s_settle[i].due_us     = now + ZBC_SETTLE_GAP_US;
            sent++;                       /* a command is airtime too */
            continue;
        }

        /* The first read is not judged; it only gives the lamp its settling time. A
         * lamp that has already reported since the command is alive and on its
         * way, so that read is skipped, and the judged one still follows at its
         * own time. `answered` stays set for the re-send test above: a lamp that
         * reported did get the command. */
        const bool reported_already = s_settle[i].reads_left == 2 && s_settle[i].answered;
        if (!reported_already) {
            s_settle[i].answered = false;
            read_light_state(s_settle[i].short_addr, s_settle[i].ep, s_settle[i].features);
            sent++;
        }
        if (--s_settle[i].reads_left) {
            s_settle[i].judge = false;
        } else {
            s_settle[i].judge      = true;   /* the lamp has had its time to settle */
            s_settle[i].audit      = true;   /* ...and one more tick to answer in */
            s_settle[i].reads_left = 1;
        }
        s_settle[i].due_us = now + ZBC_SETTLE_GAP_US;
    }

    settle_rearm_locked(gate_tick);
    esp_zigbee_lock_release();
}

/* The two reads that follow a command, counted from now: nothing judged,
 * answered or confirmed yet, due one transition plus a margin from now. Arming
 * and re-arming both come through here, so the rule lives in one place. The
 * intent fields (`want_*`, `fixes_left`, `recover_left`) belong to the command,
 * not the window, so they are the caller's.
 *
 * Caller holds the stack lock. */
static void settle_arm_locked(int slot, uint16_t transition_ds)
{
    s_settle[slot].reads_left = 2;
    s_settle[slot].judge      = false;
    s_settle[slot].answered   = false;
    s_settle[slot].audit      = false;
    /* This frame has not been confirmed yet; leaving a previous confirmation
     * standing would tell settle_work that a silent lamp had heard it. */
    s_settle[slot].delivered  = false;
    /* This slot's own deadline, so a command to another lamp cannot cut the
     * window short - see settle_rearm_locked. */
    s_settle[slot].due_us =
        esp_timer_get_time() + (int64_t)transition_ds * 100000LL + ZBC_SETTLE_MARGIN_US;
    settle_rearm_locked(false);
}

/* Arms the reads that follow a command, and records what the command asked for
 * so they can be judged against it.
 *
 * Takes the stack lock. Callers are public zb_* functions in the HTTP or button
 * task and have already released it after the send, while the Zigbee callbacks
 * read and write the slots under it; a half-written intent would be judged by
 * the next report and provoke a spurious correction. */
static void schedule_settle_read_ex(uint16_t short_addr, uint8_t ep, uint16_t features,
                                    uint16_t transition_ds, int8_t want_on, uint8_t want_level,
                                    const zbc_cmd_t *color)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);

    /* A command let through to a device that has stopped answering is a probe: its
     * confirmation says whether the device is back. Re-sends and recovery reads
     * would only be more of what it has stopped taking, so it gets neither. It
     * does get its two reads: read_attributes skips them while the device is
     * still gone, and once the probe is delivered they find out what the lamp did
     * with it - nothing else re-reads a lamp that does not report. */
    const bool probe = reach_is_gone(short_addr);
    if (probe) {
        want_on = -1;
    }

    int slot = -1, spare = -1, weakest = 0;
    for (int i = 0; i < ZBC_SETTLE_SLOTS; i++) {
        if (s_settle[i].short_addr == short_addr && s_settle[i].ep == ep) {
            slot = i;
            break;
        }
        if (spare < 0 && (!s_settle[i].short_addr || s_settle[i].reads_left == 0)) {
            spare = i;
        }
        if (s_settle[i].reads_left < s_settle[weakest].reads_left) {
            weakest = i;
        }
    }
    /* Whether this endpoint already had a slot, as opposed to being given a
     * recycled one - the difference matters to ZBC_WANT_KEEP below. */
    const bool reused = slot >= 0;
    if (!reused) {
        if (spare < 0) {
            /* Every slot is still settling another lamp's command. The one
             * with the fewest reads left is taken, and loses its audit and
             * its correction - counted, so it is visible when it happens. */
            s_radio.slots_short++;
        }
        slot = spare >= 0 ? spare : weakest;
    }

    /* Keeping the intent only makes sense while the command that set it is still
     * settling; once its reads are done the state it asked for is old news, and
     * reviving it would correct against a command the user has moved on from. */
    uint8_t fixes = 2;
    if (want_on == ZBC_WANT_KEEP) {
        const bool live = reused && s_settle[slot].reads_left > 0;
        want_on    = live ? s_settle[slot].want_on : -1;
        want_level = live ? s_settle[slot].want_level : 0;
        /* One user action, one budget: do not hand the pair a second helping. */
        fixes      = live ? s_settle[slot].fixes_left : 0;
    }

    s_settle[slot].short_addr = short_addr;
    s_settle[slot].ep         = ep;
    s_settle[slot].features   = features;
    s_settle[slot].want_on    = want_on;
    s_settle[slot].want_level = want_level;
    s_settle[slot].fixes_left = want_on < 0 ? 0 : fixes;
    /* The recovery budget is the command's, not the read window's: a command
     * whose window restarts keeps the tries it has left. So it is set here and
     * deliberately not in settle_arm_locked. A probe has none: a device still
     * gone when its reads fall due is not news, and must not count as a lamp
     * that went quiet. */
    s_settle[slot].recover_left = probe ? 0 : ZBC_RECOVER_TRIES;
    /* A command that is not a colour command clears the colour intent, so the
     * next colour read is judged against the colour last actually asked for and
     * never against the one before it. A curve sends its level command first and
     * its colour command second, so the colour survives where it matters. */
    s_settle[slot].want_color       = color ? color->kind : (uint8_t)ZBC_CMD_NONE;
    s_settle[slot].want_color_a     = color ? color->a : 0;
    s_settle[slot].want_color_b     = color ? color->b : 0;
    s_settle[slot].color_fixes_left = (color && !probe) ? 2 : 0;
    settle_arm_locked(slot, transition_ds);

    esp_zigbee_lock_release();
}

static void schedule_settle_read(uint16_t short_addr, uint8_t ep, uint16_t features,
                                 uint16_t transition_ds, int8_t want_on, uint8_t want_level)
{
    schedule_settle_read_ex(short_addr, ep, features, transition_ds, want_on, want_level, NULL);
}

/* The command this slot is watching has only now gone out, so its reads start
 * again from here.
 *
 * schedule_settle_read arms them when the command is queued, which is right
 * only for a command the gate lets straight through. One held behind another
 * waits tens of milliseconds to over a second; reads firing meanwhile get the
 * state from before the command, and the slot counts it as settled.
 *
 * On/off and level repair themselves, since both are reported. Colour is not
 * reported and is re-read only every five minutes (reread_colours), so an early
 * colour read would stand in /api/devices until then. A group drag - a level and
 * a colour command per member, only the newest of each surviving the gate -
 * produces exactly that.
 *
 * Caller holds the stack lock - this runs from the confirmation callback. */
static void settle_sent_locked(uint16_t short_addr, uint8_t ep, uint16_t transition_ds)
{
    for (int i = 0; i < ZBC_SETTLE_SLOTS; i++) {
        if (s_settle[i].short_addr != short_addr || s_settle[i].ep != ep) {
            continue;
        }
        if (s_settle[i].reads_left == 0) {
            return;    /* nothing is watching this endpoint; not ours to arm */
        }
        /* The window the command would have had, had it gone out when it was
         * asked for. Its intent and its budgets are untouched: this is the same
         * command, later. */
        settle_arm_locked(i, transition_ds);
        return;
    }
}
/* >>> end host-test region: settle-body <<< */

/* Endpoints that have told us they cannot do OffWithEffect. */
static bool no_off_effect(uint16_t short_addr, uint8_t ep)
{
    uint8_t ieee[ZBC_IEEE_LEN];
    if (!ieee_of(short_addr, ieee)) {
        return false;
    }
    for (size_t i = 0; i < sizeof(s_no_off_effect) / sizeof(s_no_off_effect[0]); i++) {
        if (s_no_off_effect[i].used && s_no_off_effect[i].ep == ep &&
            memcmp(s_no_off_effect[i].ieee, ieee, ZBC_IEEE_LEN) == 0) {
            return true;
        }
    }
    return false;
}

static void mark_no_off_effect(uint16_t short_addr, uint8_t ep)
{
    uint8_t ieee[ZBC_IEEE_LEN];
    if (no_off_effect(short_addr, ep) || !ieee_of(short_addr, ieee)) {
        return;
    }
    for (size_t i = 0; i < sizeof(s_no_off_effect) / sizeof(s_no_off_effect[0]); i++) {
        if (!s_no_off_effect[i].used) {
            s_no_off_effect[i].used = true;
            memcpy(s_no_off_effect[i].ieee, ieee, ZBC_IEEE_LEN);
            s_no_off_effect[i].ep   = ep;
            return;
        }
    }
}

/* Puts one command on the wire. Runs with the stack lock already held - the
 * caller is a Zigbee callback, or ZBC_SEND_RETRY took it.
 *
 * Every command sets dis_default_rsp. A cluster command has no response of its
 * own, so by default each draws a Default Response: an extra unicast and MAC ack
 * per command, multiplied by a group's member count. ZCL still requires one when
 * the command failed, so the rejections logged by EZB_ZCL_CORE_DEFAULT_RSP_CB_ID
 * still arrive. Global commands (read, write, configure reporting) are left
 * alone: each has its own response, and their error path is the Default
 * Response. */
static ezb_err_t send_cmd_now(uint16_t short_addr, uint8_t ep, const zbc_cmd_t *c)
{
    const ezb_zcl_cluster_cmd_ctrl_t ctrl = {
        .dst_addr        = { .addr_mode = EZB_ADDR_MODE_SHORT, .u.short_addr = short_addr },
        .dst_ep          = ep,
        .src_ep          = ZBC_GATEWAY_EP_ID,
        .dis_default_rsp = true,
        /* Every command carries it, because the gate opens on it: a command sent
         * without one would hold its endpoint shut until the timeout. */
        .cnf_ctx         = { .cb = data_confirm_cb },
    };

    switch ((zbc_cmd_kind_t)c->kind) {
    case ZBC_CMD_ONOFF: {
        ezb_zcl_on_off_cmd_t req = { .cmd_ctrl = ctrl };
        switch ((zb_onoff_cmd_t)c->a) {
        case ZB_ONOFF_ON:  return ezb_zcl_on_off_on_cmd_req(&req);
        case ZB_ONOFF_OFF: return ezb_zcl_on_off_off_cmd_req(&req);
        default:           return ezb_zcl_on_off_toggle_cmd_req(&req);
        }
    }

    /* A ramp of the level to zero is not equivalent: a Hue LST002 clamps at level
     * 1 and sits faintly lit, still on and reporting so. OffWithEffect does both
     * halves itself, with no gap for a later command to land in.
     *
     * It is optional, and a lamp without it refuses and stays on. Such a lamp
     * gets a plain Off - no fade, but off. See the default response handler for
     * how an endpoint gets onto that list. */
    case ZBC_CMD_FADE_OFF: {
        if (no_off_effect(short_addr, ep)) {
            ezb_zcl_on_off_cmd_t req = { .cmd_ctrl = ctrl };
            return ezb_zcl_on_off_off_cmd_req(&req);
        }
        ezb_zcl_on_off_off_with_effect_cmd_t req = {
            .cmd_ctrl = ctrl,
            .payload  = { .effect_id = 0x00, .effect_variant = 0x00 },
        };
        return ezb_zcl_on_off_off_with_effect_cmd_req(&req);
    }

    /* WithOnOff so that dragging the slider up from zero also switches the lamp
     * on, the way every other light app behaves. */
    case ZBC_CMD_LEVEL: {
        ezb_zcl_level_move_to_level_cmd_t req = {
            .cmd_ctrl = ctrl,
            .payload  = { .level = (uint8_t)c->a, .transition_time = c->transition_ds },
        };
        return ezb_zcl_level_move_to_level_with_on_off_cmd_req(&req);
    }

    /* Step 0 switches the lamp on at the bottom of its range so the ramp has
     * somewhere to travel from; step 1, sent once step 0 is confirmed, is the
     * ramp. The transition on step 0 is 0 - the visible part is what follows. */
    case ZBC_CMD_FADE_IN: {
        ezb_zcl_level_move_to_level_cmd_t req = {
            .cmd_ctrl = ctrl,
            .payload  = { .level           = c->step == 0 ? ZBC_FADE_FLOOR_LEVEL : (uint8_t)c->a,
                          .transition_time = c->step == 0 ? 0 : c->transition_ds },
        };
        return ezb_zcl_level_move_to_level_with_on_off_cmd_req(&req);
    }

    case ZBC_CMD_CT: {
        ezb_zcl_color_control_move_to_color_temperature_cmd_t req = {
            .cmd_ctrl = ctrl,
            .payload  = { .color_temperature_mireds = c->a, .transition_time = c->transition_ds },
        };
        return ezb_zcl_color_control_move_to_color_temperature_cmd_req(&req);
    }

    /* Step 0 sets the colour the lamp is to be left at, while it is still lit and
     * will act on it; step 1, sent once step 0 is confirmed, switches it off. One
     * command because the gate drains ZBC_CH_LEVEL before ZBC_CH_COLOR: as two
     * calls the off overtakes the colour and the lamp is dark when it arrives.
     * No transition on the colour, or the off cuts it off partway. */
    case ZBC_CMD_WARM_OFF: {
        if (c->step == 0) {
            ezb_zcl_color_control_move_to_color_temperature_cmd_t req = {
                .cmd_ctrl = ctrl,
                .payload  = { .color_temperature_mireds = c->a, .transition_time = 0 },
            };
            return ezb_zcl_color_control_move_to_color_temperature_cmd_req(&req);
        }
        if (c->b || no_off_effect(short_addr, ep)) {
            ezb_zcl_on_off_cmd_t req = { .cmd_ctrl = ctrl };
            return ezb_zcl_on_off_off_cmd_req(&req);
        }
        ezb_zcl_on_off_off_with_effect_cmd_t req = {
            .cmd_ctrl = ctrl,
            .payload  = { .effect_id = 0x00, .effect_variant = 0x00 },
        };
        return ezb_zcl_on_off_off_with_effect_cmd_req(&req);
    }

    case ZBC_CMD_HS: {
        ezb_zcl_color_control_move_to_hue_and_saturation_cmd_t req = {
            .cmd_ctrl = ctrl,
            .payload  = { .hue = (uint8_t)c->a, .saturation = (uint8_t)c->b,
                          .transition_time = c->transition_ds },
        };
        return ezb_zcl_color_control_move_to_hue_and_saturation_cmd_req(&req);
    }

    case ZBC_CMD_XY: {
        ezb_zcl_color_control_move_to_color_cmd_t req = {
            .cmd_ctrl = ctrl,
            .payload  = { .color_x = c->a, .color_y = c->b, .transition_time = c->transition_ds },
        };
        return ezb_zcl_color_control_move_to_color_cmd_req(&req);
    }

    default:
        return EZB_ERR_INV_ARG;
    }
}

/* ------------------------------------------- one command per endpoint */

/* Two commands to the same endpoint must never be in flight together.
 *
 * APS retries a frame whose acknowledgement was lost, and a retry can arrive
 * after a command issued later, leaving the lamp at an older intent with nothing
 * on air to explain it (`status 0xa7` in the log appears only when APS finally
 * gives up). Nothing above the radio can order two frames once both are out, so
 * only one is ever out.
 *
 * The data confirmation says the stack has finished with a frame - delivered, or
 * given up after its own retries - so nothing of it can still arrive; the next
 * command is sent from there. One that turns up meanwhile waits in a slot, and a
 * newer command for the same slot replaces it: an intent superseded before it
 * was sent is one nobody wants any more.
 *
 * Two queues per endpoint. A curve turns one brightness request into a level
 * command and a colour command for the same endpoint - two halves of one intent.
 * Collapsing them would drop the colour after curve_db recorded it as sent, and
 * its deadband would then hold back the colour the lamp still needs. Newest-wins
 * holds within a queue, not across the two. */

/* One slot per endpoint that can be commanded, for the same reason s_settle has
 * one: a fan-out reaches every member at once and each needs its own. */
/* >>> host-test region: gate-body <<< */
#define ZBC_GATE_SLOTS ZBC_COMMAND_SLOTS

/* How long a command may sit unconfirmed before the gate gives up waiting.
 * Comfortably past the stack's own APS retries, which is what the confirmation
 * is waiting on; this only exists so that a confirmation which never comes
 * cannot wedge an endpoint for good. */
#define ZBC_GATE_TIMEOUT_US (8 * 1000000LL)

static struct {
    uint16_t  short_addr;          /* 0 - never a lamp - marks a free slot */
    uint8_t   ep;
    bool      busy;
    int64_t   sent_at_us;
    int64_t   queued_at_us;      /* when the waiting command was handed to us */
    zbc_cmd_t inflight;
    zbc_cmd_t pending[ZBC_CH_COUNT];
} s_gate[ZBC_GATE_SLOTS];

/* The gate is written from the HTTP and button tasks and from the Zigbee
 * callbacks, so it needs a lock of its own. A spinlock rather than a mutex, and
 * never held across a send: the callbacks reach it holding the stack lock, so
 * taking the two in the other order anywhere would deadlock. */
static portMUX_TYPE s_gate_mux = portMUX_INITIALIZER_UNLOCKED;

static bool gate_idle(int i)
{
    return !s_gate[i].busy && s_gate[i].pending[ZBC_CH_LEVEL].kind == ZBC_CMD_NONE &&
           s_gate[i].pending[ZBC_CH_COLOR].kind == ZBC_CMD_NONE;
}

/* Caller holds s_gate_mux. */
static int gate_find(uint16_t short_addr, uint8_t ep)
{
    for (int i = 0; i < ZBC_GATE_SLOTS; i++) {
        if (s_gate[i].short_addr == short_addr && s_gate[i].ep == ep) {
            return i;
        }
    }
    return -1;
}

/* Caller holds s_gate_mux. Returns -1 when every slot is busy with another
 * endpoint, which the caller answers by sending unordered rather than not at
 * all - a command that goes out unsequenced beats one that never goes. */
static int gate_claim(uint16_t short_addr, uint8_t ep)
{
    int spare = -1;
    for (int i = 0; i < ZBC_GATE_SLOTS; i++) {
        if (s_gate[i].short_addr == short_addr && s_gate[i].ep == ep) {
            return i;
        }
        if (spare < 0 && gate_idle(i)) {
            spare = i;
        }
    }
    if (spare >= 0) {
        memset(&s_gate[spare], 0, sizeof(s_gate[spare]));
        s_gate[spare].short_addr = short_addr;
        s_gate[spare].ep         = ep;
    }
    return spare;
}

/* Decides what happens to a new command: sent now, or held until the one in
 * flight is confirmed. Caller sends `cmd` itself when this returns true. */
static bool gate_admit(uint16_t short_addr, uint8_t ep, const zbc_cmd_t *cmd, int ch)
{
    bool send_now = true, timed_out = false;
    portENTER_CRITICAL(&s_gate_mux);
    const int i = gate_claim(short_addr, ep);
    if (i < 0) {
        /* Sent unsequenced, and counted: if this ever moves, ZBC_COMMAND_SLOTS
         * is too small for the way the box is being used. */
        s_radio.slots_short++;
    }
    if (i >= 0) {
        if (s_gate[i].busy && esp_timer_get_time() - s_gate[i].sent_at_us < ZBC_GATE_TIMEOUT_US) {
            s_gate[i].pending[ch] = *cmd;
            s_gate[i].queued_at_us = esp_timer_get_time();
            send_now              = false;
        } else {
            /* A slot still busy here has waited out the timeout without the
             * flush getting to it: the same event gate_flush_stale counts and
             * warns about, so it is counted here too rather than stepped over
             * in silence - the counter is what says confirmations stopped. */
            timed_out               = s_gate[i].busy;
            s_gate[i].busy          = true;
            s_gate[i].inflight      = *cmd;
            s_gate[i].sent_at_us    = esp_timer_get_time();
            s_gate[i].pending[ch].kind = ZBC_CMD_NONE;
        }
    }
    if (timed_out) {
        s_radio.gate_timeouts++;
    }
    portEXIT_CRITICAL(&s_gate_mux);
    if (timed_out) {
        ESP_LOGW(TAG, "No confirmation for 0x%04x ep %d; sending the next command over it",
                 short_addr, ep);
    }
    return send_now;
}

/* A send that the stack refused is not coming back as a confirmation, so the
 * gate has to be opened here or the endpoint stays shut until the timeout. */
static void gate_abandon(uint16_t short_addr, uint8_t ep)
{
    portENTER_CRITICAL(&s_gate_mux);
    const int i = gate_find(short_addr, ep);
    if (i >= 0) {
        s_gate[i].busy          = false;
        s_gate[i].inflight.kind = ZBC_CMD_NONE;
    }
    portEXIT_CRITICAL(&s_gate_mux);
}

/* From a task that may block - every caller of the public zb_* functions. */
static esp_err_t gate_send(uint16_t short_addr, uint8_t ep, const zbc_cmd_t *cmd, int ch)
{
    if (!reach_admits(short_addr, true)) {
        return ZB_ERR_UNREACHABLE;
    }
    if (!gate_admit(short_addr, ep, cmd, ch)) {
        return ESP_OK;    /* queued; it goes out when the one in flight is done */
    }
    ezb_err_t err;
    ZBC_SEND_RETRY(err, send_cmd_now(short_addr, ep, cmd));
    if (err != EZB_ERR_NONE) {
        gate_abandon(short_addr, ep);
    }
    return esp_zigbee_err_to_esp(err);
}

/* From the Zigbee task with the stack lock already held, so no retry loop -
 * that drops the lock, which a callback may not do. */
static void gate_send_locked(uint16_t short_addr, uint8_t ep, const zbc_cmd_t *cmd, int ch)
{
    if (!reach_admits(short_addr, true)) {
        return;
    }
    if (!gate_admit(short_addr, ep, cmd, ch)) {
        return;
    }
    if (send_cmd_now(short_addr, ep, cmd) != EZB_ERR_NONE) {
        gate_abandon(short_addr, ep);
    }
}

/* Whether the frame just confirmed for this endpoint was the preset half of a
 * fade in. The ramp is still to come, so the command is not delivered yet. */
static bool gate_confirms_preset(uint16_t short_addr, uint8_t ep)
{
    bool preset = false;
    portENTER_CRITICAL(&s_gate_mux);
    const int i = gate_find(short_addr, ep);
    if (i >= 0 && s_gate[i].busy && s_gate[i].inflight.kind == ZBC_CMD_FADE_IN &&
        s_gate[i].inflight.step == 0) {
        preset = true;
    }
    portEXIT_CRITICAL(&s_gate_mux);
    return preset;
}

/* The command in flight to this endpoint is done with. Let the next one out.
 * Runs in the Zigbee task with the stack lock held. Returns true if something
 * went out, which tells the confirmation handler that a newer intent has already
 * superseded the one it was about to re-send. */
static bool gate_advance(uint16_t short_addr, uint8_t ep)
{
    zbc_cmd_t next = { .kind = ZBC_CMD_NONE };
    int64_t   waited_us = 0;

    portENTER_CRITICAL(&s_gate_mux);
    const int i = gate_find(short_addr, ep);
    /* Deliberately not conditional on the slot being busy. gate_flush_stale
     * clears that before calling here precisely so this runs, and a slot with
     * nothing in flight and nothing queued falls through to next.kind NONE
     * anyway, so a stray confirmation still costs nothing. */
    if (i >= 0) {
        if (s_gate[i].busy && s_gate[i].inflight.step == 0 &&
            (s_gate[i].inflight.kind == ZBC_CMD_FADE_IN ||
             s_gate[i].inflight.kind == ZBC_CMD_WARM_OFF)) {
            /* Half a command: the ramp - or the off - is the other half and the
             * endpoint stays shut for it. Sent whether or not the first frame
             * was delivered: the second is the part that matters, the first only
             * how it looks (FADE_IN) or what it is left at (WARM_OFF). */
            s_gate[i].inflight.step = 1;
            s_gate[i].sent_at_us    = esp_timer_get_time();
            next                    = s_gate[i].inflight;
        } else {
            s_gate[i].busy          = false;
            s_gate[i].inflight.kind = ZBC_CMD_NONE;
            for (int ch = 0; ch < ZBC_CH_COUNT; ch++) {
                if (s_gate[i].pending[ch].kind != ZBC_CMD_NONE) {
                    next                    = s_gate[i].pending[ch];
                    s_gate[i].pending[ch].kind = ZBC_CMD_NONE;
                    break;
                }
            }
            if (next.kind != ZBC_CMD_NONE) {
                waited_us            = esp_timer_get_time() - s_gate[i].queued_at_us;
                s_gate[i].busy       = true;
                s_gate[i].inflight   = next;
                s_gate[i].sent_at_us = esp_timer_get_time();
            }
        }
    }
    portEXIT_CRITICAL(&s_gate_mux);

    if (next.kind == ZBC_CMD_NONE) {
        return false;
    }
    /* How long the newer intent sat behind the older frame - the cost of the gate.
     * Usually 24-130 ms; only a wait long enough to be felt is logged. */
    if (waited_us > 250000) {
        ESP_LOGW(TAG, "Queued command for 0x%04x ep %d waited %lld ms",
                 short_addr, ep, (long long)(waited_us / 1000));
    }
    if (send_cmd_now(short_addr, ep, &next) != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Queued command for 0x%04x ep %d was refused", short_addr, ep);
        gate_abandon(short_addr, ep);
        return true;
    }
    /* Only now is the lamp being told; the reads that follow start from here. */
    settle_sent_locked(short_addr, ep, next.transition_ds);
    return true;
}

/* Nothing here should be needed, and all of it is cheap. A confirmation that
 * never arrives would hold an endpoint shut for good, and a command the stack
 * refused leaves whatever was queued behind it with nothing to open the gate.
 * Called from the settle timer, which is armed after every command anyway.
 * Runs with the stack lock held; returns true if it found something to do. */
static bool gate_flush_stale(void)
{
    bool did = false;
    for (int i = 0; i < ZBC_GATE_SLOTS; i++) {
        uint16_t addr    = 0;
        uint8_t  ep      = 0;
        uint16_t to_addr = 0;          /* a slot that timed out; never 0, a free slot's */
        uint8_t  to_ep   = 0;

        portENTER_CRITICAL(&s_gate_mux);
        if (s_gate[i].short_addr && s_gate[i].busy &&
            esp_timer_get_time() - s_gate[i].sent_at_us >= ZBC_GATE_TIMEOUT_US) {
            s_radio.gate_timeouts++;
            to_addr                 = s_gate[i].short_addr;
            to_ep                   = s_gate[i].ep;
            s_gate[i].busy          = false;
            s_gate[i].inflight.kind = ZBC_CMD_NONE;
        }
        if (s_gate[i].short_addr && !s_gate[i].busy && !gate_idle(i)) {
            addr = s_gate[i].short_addr;
            ep   = s_gate[i].ep;
        }
        portEXIT_CRITICAL(&s_gate_mux);

        /* Outside the spinlock, as gate_admit logs: nothing that can block may
         * run with interrupts off. */
        if (to_addr) {
            ESP_LOGW(TAG, "No confirmation for 0x%04x ep %d; letting the next command out", to_addr, to_ep);
        }
        if (addr) {
            /* Opens the gate properly and sends whatever was waiting. */
            did = gate_advance(addr, ep) || did;
        }
    }
    return did;
}
/* >>> end host-test region: gate-body <<< */

/* How long ago the frame in flight to this endpoint went out, or -1 if none is:
 * one sent unsequenced because every slot was taken, which has no clock. */
static int64_t gate_inflight_age_us(uint16_t short_addr, uint8_t ep)
{
    int64_t age = -1;
    portENTER_CRITICAL(&s_gate_mux);
    const int i = gate_find(short_addr, ep);
    if (i >= 0 && s_gate[i].busy) {
        age = esp_timer_get_time() - s_gate[i].sent_at_us;
    }
    portEXIT_CRITICAL(&s_gate_mux);
    return age;
}

/* Says again what the settle slot last asked for, after a lamp reported
 * something else or answered nothing at all.
 *
 * A plain On is re-sent as a plain On. The slot records want_level 0 for one (an
 * On has no level), and a move-to-level with on/off to 0 is how this code
 * switches a lamp off.
 *
 * An off is always OffWithEffect, including here: a ramp to zero leaves a Hue
 * LST002 clamped at level 1 and faintly lit. */
static void resend_intent_locked(uint16_t short_addr, uint8_t ep, int8_t want_on, uint8_t want_level)
{
    zbc_cmd_t c;
    if (!want_on) {
        c = (zbc_cmd_t){ .kind = ZBC_CMD_FADE_OFF, .transition_ds = ZBC_OFF_EFFECT_DS };
    } else if (want_level == 0) {
        c = (zbc_cmd_t){ .kind = ZBC_CMD_ONOFF, .a = ZB_ONOFF_ON };
    } else {
        c = (zbc_cmd_t){ .kind          = ZBC_CMD_LEVEL,
                         .a             = want_level,
                         .transition_ds = settings_transition_ds() };
    }
    gate_send_locked(short_addr, ep, &c, ZBC_CH_LEVEL);
}

/* A colour read that disagrees with the colour the lamp was told: send it again.
 *
 * On/off and level are reported, so a lamp that ignored one of those corrects
 * itself the moment it says so. Colour is never reported, and it is re-read only
 * by reread_colours every five minutes - which updates what the UI shows and
 * nothing else. So without this a colour a lamp did not take is permanent: the
 * deadbands in lighting.c believe what they sent has landed and send nothing
 * further, and every later request is a small step from a place the lamp is not.
 * The case that brought it about: a warm-off leaves the lamp at its warmest, the
 * switch-on that follows puts the curve's colour back, and a Hue that was still
 * dark when that frame arrived ignores it and stays warm for the evening.
 *
 * Bounded like the other two corrections, on its own budget (see the slot), and
 * only while the command's own reads are still expected - so the five-minute
 * re-read, and a lamp somebody changed at the wall, are never corrected.
 *
 * Caller holds the stack lock, and is the attribute handler in the Zigbee task. */
static void color_correct_locked(uint16_t short_addr, uint8_t ep, int part, uint16_t got)
{
    for (int i = 0; i < ZBC_SETTLE_SLOTS; i++) {
        if (s_settle[i].short_addr != short_addr || s_settle[i].ep != ep) {
            continue;
        }
        if (s_settle[i].reads_left == 0 || !s_settle[i].judge ||
            s_settle[i].want_color == ZBC_CMD_NONE || s_settle[i].color_fixes_left == 0 ||
            !color_off_target(s_settle[i].want_color, s_settle[i].want_color_a,
                              s_settle[i].want_color_b, part, got)) {
            return;
        }
        s_settle[i].color_fixes_left--;
        s_settle[i].judge = false;
        s_radio.corrections++;
        ESP_LOGW(TAG, "Device 0x%04x ep %d took no colour (%s %u, was told %u) - sending it again",
                 short_addr, ep,
                 part == ZBC_COLOR_PART_CT ? "mireds" : (part == ZBC_COLOR_PART_HUE ? "hue" : "sat"),
                 got, part == ZBC_COLOR_PART_SAT ? s_settle[i].want_color_b : s_settle[i].want_color_a);

        /* Transition 0: the lamp is where it should not be, and a ramp would only
         * put off the read that checks this took. */
        const zbc_cmd_t c = { .kind = s_settle[i].want_color,
                              .a    = s_settle[i].want_color_a,
                              .b    = s_settle[i].want_color_b };
        gate_send_locked(short_addr, ep, &c, ZBC_CH_COLOR);

        /* Same re-arm as the on/off and level corrections, for the same reasons. */
        s_settle[i].reads_left = 1;
        s_settle[i].audit      = false;
        s_settle[i].answered   = false;
        s_settle[i].delivered  = false;
        s_settle[i].due_us     = esp_timer_get_time() + 1500000LL;
        settle_rearm_locked(false);
        return;
    }
}

/* The two radios share one antenna, so a Zigbee channel inside the Wi-Fi
 * channel means dropped commands, poor link quality at close range and lamps
 * that answer only sometimes. Both channels are known here, so warn. */
static bool s_collide_said;   /* cleared by a channel move, to be judged again */

static void warn_if_channels_collide(void)
{
    if (s_collide_said) {
        return;
    }
    wifi_mgr_status_t wifi;
    wifi_manager_get_status(&wifi);
    if (wifi.channel == 0 || wifi.channel > 14) {
        return;
    }
    if (!s_network_up) {
        return;                       /* the channel is not settled until it is up */
    }
    /* The cached channel: this runs in the work task without the stack lock. */
    int zb_mhz   = 2405 + 5 * (s_net.channel - 11);
    int wifi_mhz = wifi.channel == 14 ? 2484 : 2412 + 5 * (wifi.channel - 1);

    s_collide_said = true;
    if (abs(zb_mhz - wifi_mhz) <= 11) {   /* Wi-Fi occupies its centre +/- 10 MHz */
        ESP_LOGW(TAG, "Zigbee channel %d (%d MHz) is inside Wi-Fi channel %d (%d MHz) - "
                      "they share one antenna, expect dropped commands and weak links",
                 s_net.channel, zb_mhz, wifi.channel, wifi_mhz);
        ESP_LOGW(TAG, "Pick a Zigbee channel at least 11 MHz clear, e.g. %d",
                 wifi_mhz > 2440 ? 15 : 25);
    }
}


/* ------------------------------------------------- moving to another channel
 *
 * The standard way, a Mgmt_NWK_Update_req with ScanDuration 0xFE, and three
 * things esp-zigbee-lib 2.0.5 does that decide the shape of it (seen on the dev
 * board, not documented):
 *
 *  - A device that receives the request changes channel 8-9 s later, about
 *    when routers do (the spec's nwkNetworkBroadcastDeliveryTime). Asked again
 *    meanwhile, it answers with status 0x80.
 *  - A broadcast is not delivered to its sender, but a request to our own short
 *    address is. So the coordinator asks itself with a second request, to
 *    0x0000, sent with the broadcast, and changes with the routers. This counts
 *    on that wait: 2.0.4 changed at once, which would put the coordinator on
 *    the new channel ahead of the lamps.
 *  - The new channel is saved when the stack changes. settings_zigbee_channel()
 *    is still the authority: a restart in the seconds before that comes up on
 *    the old channel, and channel_restore() moves the coordinator on at every
 *    network-up.
 *
 * The stack also refuses to set its own nwkUpdateId while on a network, so the
 * id of the last move is kept with the channel: a lamp may ignore a request that
 * does not carry a newer one. */

/* How long the coordinator waits for its own change before it asks again: the
 * stack's delay, and some to spare. */
#define ZBC_CHANNEL_SELF_WAIT_US  (15 * 1000000LL)
/* How often it looks meanwhile. */
#define ZBC_CHANNEL_CHECK_US      (1 * 1000000LL)
#define ZBC_CHANNEL_SELF_TRIES    2

static zb_channel_move_t  s_move;
static portMUX_TYPE       s_move_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t            s_move_update_id;
static uint8_t            s_move_tries;
static int64_t            s_move_asked_us;   /* the coordinator's last request to itself */
static esp_timer_handle_t s_channel_timer;

static bool survey_busy(void);

static void move_set(zb_move_state_t state, uint8_t from, uint8_t to, bool boot)
{
    portENTER_CRITICAL(&s_move_mux);
    s_move = (zb_channel_move_t){
        .state = state, .from = from, .to = to, .boot = boot, .at_us = esp_timer_get_time(),
    };
    portEXIT_CRITICAL(&s_move_mux);
    zbc_state_bump();
}

static bool move_busy(void)
{
    portENTER_CRITICAL(&s_move_mux);
    const bool busy = s_move.state == ZB_MOVE_ANNOUNCED || s_move.state == ZB_MOVE_MOVING;
    portEXIT_CRITICAL(&s_move_mux);
    return busy;
}

void zb_get_channel_move(zb_channel_move_t *out)
{
    if (!out) {
        return;
    }
    portENTER_CRITICAL(&s_move_mux);
    *out = s_move;
    portEXIT_CRITICAL(&s_move_mux);
}

/* A successful 0xFE request has no answer, so this hears only a refusal (a
 * notify with a status) or the end of the wait for an answer. */
static void channel_change_cb(const ezb_zdo_nwk_mgmt_nwk_update_req_result_t *result, void *user_ctx)
{
    (void)user_ctx;
    if (result && result->rsp) {
        ESP_LOGW(TAG, "Channel change answered with status 0x%02x", result->rsp->status);
    }
}

/* With the stack lock held, or in the Zigbee task. */
static ezb_err_t send_channel_change_locked(uint16_t dst, uint8_t channel, uint8_t update_id)
{
    const ezb_zdo_nwk_mgmt_nwk_update_req_t req = {
        .dst_nwk_addr = dst,
        .field = {
            .scan_channels = 1U << channel,
            .scan_duration = 0xFE,        /* "change to the channel in scan_channels" */
            .nwk_update_id = update_id,
        },
        .cb = channel_change_cb,
    };
    const ezb_err_t err = ezb_zdo_nwk_mgmt_nwk_update_req(&req);
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Channel change request to 0x%04x refused: %d", dst, err);
    }
    return err;
}

esp_err_t zb_move_channel(uint8_t channel)
{
    if (channel < 11 || channel > 26) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_stack_started || !s_network_up || survey_busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Claimed before anything else, so a second request cannot start a second
     * move between the reads and the send below. */
    portENTER_CRITICAL(&s_move_mux);
    const zb_channel_move_t was = s_move;
    const bool busy = was.state == ZB_MOVE_ANNOUNCED || was.state == ZB_MOVE_MOVING;
    if (!busy) {
        s_move.state = ZB_MOVE_ANNOUNCED;
        s_move.from  = s_net.channel;
        s_move.to    = channel;
    }
    portEXIT_CRITICAL(&s_move_mux);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_zigbee_lock_acquire(portMAX_DELAY);
    const uint8_t from     = ezb_nwk_get_current_channel();
    const uint8_t stack_id = ezb_nwk_get_update_id();
    esp_zigbee_lock_release();
    if (from == channel) {
        portENTER_CRITICAL(&s_move_mux);
        s_move = was;
        portEXIT_CRITICAL(&s_move_mux);
        return ESP_OK;
    }
    /* Whichever is further on, as a sequence number - it wraps. */
    const uint8_t stored_id = settings_zigbee_update_id();
    const uint8_t update_id = (uint8_t)(((int8_t)(stored_id - stack_id) > 0 ? stored_id : stack_id) + 1);

    /* Stored before it is announced: a power cut in between at worst puts the
     * coordinator on the new channel ahead of lamps that never heard. The other
     * order could leave every lamp on a channel the coordinator has forgotten. */
    const uint8_t old_channel = settings_zigbee_channel();
    esp_err_t     err         = settings_set_zigbee_channel(channel, update_id);
    if (err == ESP_OK) {
        esp_zigbee_lock_acquire(portMAX_DELAY);
        const ezb_err_t zerr = send_channel_change_locked(0xFFFF, channel, update_id);
        /* Its own with it, so its wait runs with the routers'. One that is
         * refused is sent again by channel_work when the wait is over. */
        if (zerr == EZB_ERR_NONE) {
            send_channel_change_locked(0x0000, channel, update_id);
        }
        esp_zigbee_lock_release();
        err = esp_zigbee_err_to_esp(zerr);
        if (err != ESP_OK) {
            settings_set_zigbee_channel(old_channel, stored_id);
        }
    }
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_move_mux);
        s_move = was;
        portEXIT_CRITICAL(&s_move_mux);
        return err;
    }

    s_move_update_id = update_id;
    s_move_tries     = 1;
    s_move_asked_us  = esp_timer_get_time();
    move_set(ZB_MOVE_ANNOUNCED, from, channel, false);
    esp_timer_stop(s_channel_timer);
    esp_timer_start_once(s_channel_timer, ZBC_CHANNEL_CHECK_US);
    heap_watch_mark("zb_channel", channel);
    ESP_LOGW(TAG, "Moving the network from channel %u to %u (update id %u)", from, channel, update_id);
    return ESP_OK;
}

/* zb_work_task: waits for the coordinator's own change, and asks again when it
 * does not come. */
static void channel_work(void)
{
    zb_channel_move_t m;
    zb_get_channel_move(&m);
    if (m.state != ZB_MOVE_ANNOUNCED && m.state != ZB_MOVE_MOVING) {
        return;
    }

    esp_zigbee_lock_acquire(portMAX_DELAY);
    const uint8_t now = ezb_nwk_get_current_channel();
    esp_zigbee_lock_release();
    if (now == m.to) {
        s_net.channel  = m.to;
        s_collide_said = false;
        reach_forget_all();
        move_set(ZB_MOVE_MOVED, m.from, m.to, m.boot);
        ESP_LOGW(TAG, "Now on channel %u", m.to);
        /* Link quality and states as the lamps see them from here. */
        esp_timer_stop(s_refresh_timer);
        esp_timer_start_once(s_refresh_timer, 5 * 1000000ULL);
        return;
    }
    if (esp_timer_get_time() - s_move_asked_us >= ZBC_CHANNEL_SELF_WAIT_US) {
        if (s_move_tries >= ZBC_CHANNEL_SELF_TRIES) {
            move_set(ZB_MOVE_FAILED, m.from, m.to, m.boot);
            ESP_LOGE(TAG, "Still on channel %u after asking %u times to move to %u", now,
                     s_move_tries, m.to);
            return;
        }
        ESP_LOGW(TAG, "Still on channel %u, asking again to move to %u", now, m.to);
        esp_zigbee_lock_acquire(portMAX_DELAY);
        send_channel_change_locked(0x0000, m.to, s_move_update_id);
        esp_zigbee_lock_release();
        s_move_tries++;
        s_move_asked_us = esp_timer_get_time();
        if (m.state == ZB_MOVE_ANNOUNCED) {
            move_set(ZB_MOVE_MOVING, m.from, m.to, m.boot);
        }
    }
    esp_timer_stop(s_channel_timer);
    esp_timer_start_once(s_channel_timer, ZBC_CHANNEL_CHECK_US);
}

/* The Zigbee task, at network-up from storage: the stack is back on the channel
 * it last saved, the lamps on the one the network was last moved to - after a
 * restart in the seconds before the stack changed. No broadcast: the lamps are
 * not listening on this channel. */
static void channel_restore(void)
{
    const uint8_t want = settings_zigbee_channel();
    const uint8_t have = ezb_nwk_get_current_channel();
    if (!want || want == have) {
        return;
    }
    ESP_LOGW(TAG, "Restored on channel %u, but the network was moved to %u: following it", have, want);
    s_move_update_id = settings_zigbee_update_id();
    s_move_tries     = 1;
    s_move_asked_us  = esp_timer_get_time();
    send_channel_change_locked(0x0000, want, s_move_update_id);
    move_set(ZB_MOVE_MOVING, have, want, true);
    esp_timer_stop(s_channel_timer);
    esp_timer_start_once(s_channel_timer, ZBC_CHANNEL_CHECK_US);
}

/* ------------------------------------------------------------ channel survey
 *
 * The band as the coordinator's own antenna hears it: the energy on each
 * channel over four short scans, and the other Zigbee networks that answer a
 * beacon request. Several short scans rather than one long one, because Wi-Fi
 * is bursty, and a gap between them so that the coordinator is back on its
 * channel and Wi-Fi has the antenna meanwhile. An energy scan has to outbid
 * Wi-Fi to run at all (radio_coex.c), and one the radio still cannot finish is
 * reported unmeasured by radio_ed.c; a scan the stack never ends is a failed
 * survey. */
#define ZBC_SURVEY_PASSES          ZB_SURVEY_PASSES
/* (2^2 + 1) x 15.36 ms = 77 ms a channel, 1.2 s a scan. The radio averages over
 * the window, so a longer one measures no more; what it costs is Wi-Fi, which
 * hears nothing while a scan runs (radio_coex.c) - 1.2 s at a time it rides out. */
#define ZBC_SURVEY_ED_DURATION     2
#define ZBC_SURVEY_BEACON_DURATION 2      /* 77 ms a channel */
#define ZBC_SURVEY_ALL_CHANNELS    0x07FFF800U
#define ZBC_SURVEY_GAP_US          (1500 * 1000LL)
/* radio_ed.c gives one channel 1.5 s before it reports it unmeasured, so a scan
 * of sixteen ends inside this whatever Wi-Fi does; past it, something else is
 * wrong. */
#define ZBC_SURVEY_SCAN_MAX_US     (30 * 1000000LL)

static zb_survey_t        s_survey;
static portMUX_TYPE       s_survey_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_survey_timer;
/* Under the stack lock (survey_work) or in the Zigbee task (the callbacks). */
static uint8_t            s_survey_step;       /* the energy scans, then the beacon scan */
static bool               s_survey_scanning;   /* a scan is out with the stack */
static int64_t            s_survey_scan_us;
static uint64_t           s_survey_own_pan;

static bool survey_busy(void)
{
    portENTER_CRITICAL(&s_survey_mux);
    const bool busy = s_survey.state == ZB_SURVEY_RUNNING;
    portEXIT_CRITICAL(&s_survey_mux);
    return busy;
}

static void survey_end(zb_survey_state_t state, const char *error)
{
    portENTER_CRITICAL(&s_survey_mux);
    s_survey.state = state;
    s_survey.error = error;
    s_survey.at_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_survey_mux);
    esp_timer_stop(s_survey_timer);
    reach_forget_all();
    if (error) {
        ESP_LOGW(TAG, "Channel survey failed: %s", error);
    } else {
        ESP_LOGI(TAG, "Channel survey done");
    }
}

void zb_get_channel_survey(zb_survey_t *out)
{
    if (!out) {
        return;
    }
    portENTER_CRITICAL(&s_survey_mux);
    *out = s_survey;
    portEXIT_CRITICAL(&s_survey_mux);
}

/* The Zigbee task. One call per channel, then NULL. */
static void survey_ed_cb(ezb_nwk_ed_scan_result_t *result, void *user_ctx)
{
    (void)user_ctx;
    if (result) {
        const int i = (int)result->channel_number - 11;
        /* INT8_MIN: radio_ed.c gave up on the channel, nothing was measured. */
        if (i < 0 || i >= ZB_SURVEY_CHANNELS || result->max_rssi == INT8_MIN) {
            return;
        }
        portENTER_CRITICAL(&s_survey_mux);
        if (s_survey.ch[i].samples < ZB_SURVEY_PASSES) {
            s_survey.ch[i].dbm[s_survey.ch[i].samples++] = result->max_rssi;
        }
        portEXIT_CRITICAL(&s_survey_mux);
        return;
    }
    s_survey_scanning = false;
    s_survey_step++;
    portENTER_CRITICAL(&s_survey_mux);
    s_survey.passes++;
    portEXIT_CRITICAL(&s_survey_mux);
    esp_timer_stop(s_survey_timer);
    esp_timer_start_once(s_survey_timer, ZBC_SURVEY_GAP_US);
}

/* The Zigbee task. One call per beacon, then NULL. Our own routers answer too. */
static void survey_beacon_cb(ezb_nwk_active_scan_result_t *result, void *user_ctx)
{
    (void)user_ctx;
    if (!result) {
        s_survey_scanning = false;
        survey_end(ZB_SURVEY_DONE, NULL);
        return;
    }
    if (result->extpanid.u64 == s_survey_own_pan) {
        return;
    }
    portENTER_CRITICAL(&s_survey_mux);
    bool known = false;
    for (int i = 0; i < s_survey.networks; i++) {
        known |= s_survey.net[i].ext_pan_id == result->extpanid.u64 &&
                 s_survey.net[i].channel == result->channel_number;
    }
    if (!known && s_survey.networks < ZB_SURVEY_NETWORKS) {
        s_survey.net[s_survey.networks].channel     = result->channel_number;
        s_survey.net[s_survey.networks].pan_id      = result->panid;
        s_survey.net[s_survey.networks].ext_pan_id  = result->extpanid.u64;
        s_survey.net[s_survey.networks].permit_join = result->permit_join;
        s_survey.networks++;
    }
    portEXIT_CRITICAL(&s_survey_mux);
}

esp_err_t zb_channel_survey_start(void)
{
    if (!s_stack_started || !s_network_up || move_busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_survey_mux);
    const bool busy = s_survey.state == ZB_SURVEY_RUNNING;
    if (!busy) {
        memset(&s_survey, 0, sizeof(s_survey));
        s_survey.state = ZB_SURVEY_RUNNING;
        s_survey.at_us = esp_timer_get_time();
    }
    portEXIT_CRITICAL(&s_survey_mux);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_extpanid_t own;
    ezb_nwk_get_extended_panid(&own);
    s_survey_own_pan  = own.u64;
    s_survey_step     = 0;
    s_survey_scanning = false;
    esp_zigbee_lock_release();
    heap_watch_mark("zb_survey", 0);
    ESP_LOGI(TAG, "Channel survey: %d energy scans and a beacon scan", ZBC_SURVEY_PASSES);
    work_post(ZBC_WORK_SURVEY);
    return ESP_OK;
}

/* zb_work_task: starts the next scan, or gives up on one that never ended. */
static void survey_work(void)
{
    if (!survey_busy()) {
        return;
    }
    esp_zigbee_lock_acquire(portMAX_DELAY);
    const int64_t now = esp_timer_get_time();
    if (s_survey_scanning) {
        const int64_t left = s_survey_scan_us + ZBC_SURVEY_SCAN_MAX_US - now;
        esp_zigbee_lock_release();
        if (left <= 0) {
            survey_end(ZB_SURVEY_FAILED, "the radio never finished the scan");
        } else {
            esp_timer_stop(s_survey_timer);
            esp_timer_start_once(s_survey_timer, left);
        }
        return;
    }
    const bool       beacon = s_survey_step >= ZBC_SURVEY_PASSES;
    ezb_nwk_scan_req_t req  = {
        .scan_type     = beacon ? EZB_NWK_SCAN_TYPE_ACTIVE : EZB_NWK_SCAN_TYPE_ED,
        .scan_duration = beacon ? ZBC_SURVEY_BEACON_DURATION : ZBC_SURVEY_ED_DURATION,
        .scan_channels = ZBC_SURVEY_ALL_CHANNELS,
    };
    if (beacon) {
        req.active_scan_cb = survey_beacon_cb;
    } else {
        req.ed_scan_cb = survey_ed_cb;
    }
    const ezb_err_t err = ezb_nwk_scan(&req);
    if (err == EZB_ERR_NONE) {
        s_survey_scanning = true;
        s_survey_scan_us  = now;
    }
    esp_zigbee_lock_release();
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Channel survey: the stack refused the scan (%d)", err);
        survey_end(ZB_SURVEY_FAILED, "the stack refused the scan");
        return;
    }
    esp_timer_stop(s_survey_timer);
    esp_timer_start_once(s_survey_timer, ZBC_SURVEY_SCAN_MAX_US);
}

/* Runs in the Zigbee task, at the two signals that bring a network up. */
static void note_network_identity(void)
{
    ezb_extaddr_t ext;
    ezb_nwk_get_extended_address(&ext);
    ezb_extpanid_t ext_pan;
    ezb_nwk_get_extended_panid(&ext_pan);
    s_net.pan_id     = ezb_nwk_get_panid();
    s_net.ext_pan_id = ext_pan.u64;
    s_net.channel    = ezb_nwk_get_current_channel();
    s_net.short_addr = ezb_nwk_get_short_address();
    for (int i = 0; i < 8; i++) {
        s_net.ieee[i] = ext.u8[7 - i];
    }
    s_net.ed_timeout_s = ed_timeout_seconds(ezb_nwk_get_ed_timeout());
    s_net.max_children = ezb_nwk_get_max_children();
}

static bool app_signal_handler(const ezb_app_signal_t *app_signal)
{
    ezb_app_signal_type_t signal_type = ezb_app_signal_get_type(app_signal);

    if (!s_network_up) {
        /* The BDB signals carry a status; the rest carry other things. */
        const bool bdb = signal_type == EZB_BDB_SIGNAL_DEVICE_FIRST_START ||
                         signal_type == EZB_BDB_SIGNAL_DEVICE_REBOOT ||
                         signal_type == EZB_BDB_SIGNAL_FORMATION ||
                         signal_type == EZB_BDB_SIGNAL_STEERING;
        const void *params = ezb_app_signal_get_params(app_signal);
        forming_note(ezb_app_signal_to_string(signal_type),
                     bdb && params ? *(const ezb_bdb_comm_status_t *)params : -1, 0, &s_forming.signals);
        forming_sample_locked();
    }

    switch (signal_type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initializing Zigbee stack");
        (void)start_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;

    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        ezb_bdb_comm_status_t status = *(ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal);
        if (status != EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGW(TAG, "%s failed with status(0x%02x), retrying",
                     ezb_app_signal_to_string(signal_type), status);
            schedule_commissioning_retry(EZB_BDB_MODE_INITIALIZATION, 1000000);
            break;
        }
        if (ezb_bdb_is_factory_new()) {
            ESP_LOGI(TAG, "No network yet, forming one on channel %d", formation_channel());
            ESP_ERROR_CHECK(start_commissioning(EZB_BDB_MODE_NETWORK_FORMATION));
            /* Forming takes seconds when its energy scan is answered, and for ever
             * when the driver loses it (radio_ed.c restarts it), with no signal
             * meanwhile - so report progress periodically, and keep the formation
             * log in /api/status. A start while one is in progress is refused, so
             * this schedule only reports; see retry_work. */
            schedule_commissioning_retry(EZB_BDB_MODE_NETWORK_FORMATION, 30 * 1000000ULL);
        } else {
            note_network_identity();
            s_network_up = true;
            ESP_LOGI(TAG, "Restored network: PAN ID(0x%04hx), Channel(%d), Short Address(0x%04hx)",
                     s_net.pan_id, s_net.channel, s_net.short_addr);
            channel_restore();
            join_remote_groups();
            ota_attach_locked();
            /* Paired devices are still out there; ask them where they stand. */
            esp_timer_start_once(s_refresh_timer, 5 * 1000000ULL);
        }
        break;
    }

    case EZB_BDB_SIGNAL_FORMATION: {
        ezb_bdb_comm_status_t status = *(ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal);
        if (status == EZB_BDB_STATUS_SUCCESS) {
            note_network_identity();
            s_network_up = true;
            esp_timer_stop(s_retry_timer);
            ezb_extpanid_t ext_pan_id;
            ezb_nwk_get_extended_panid(&ext_pan_id);
            ESP_LOGI(TAG, "Formed network: PAN ID(0x%04hx, EXT: 0x%llx), Channel(%d), Short Address(0x%04hx)",
                     s_net.pan_id, ext_pan_id.u64, s_net.channel, s_net.short_addr);
            join_remote_groups();
            ota_attach_locked();
            /* As on a restored network: the neighbour scans, the retried binds and
             * interrogations and the colour re-reads all run off this timer, and
             * the devices paired in the first boot after forming need them most. */
            esp_timer_start_once(s_refresh_timer, 5 * 1000000ULL);
        } else {
            ESP_LOGW(TAG, "Failed to form network with status(0x%02x), retrying", status);
            s_formation_failed = true;
            schedule_commissioning_retry(EZB_BDB_MODE_NETWORK_FORMATION, 1000000);
        }
        break;
    }

    case EZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        const ezb_zdo_signal_device_annce_params_t *params = ezb_app_signal_get_params(app_signal);
        bool    created = false;
        uint8_t ieee[ZBC_IEEE_LEN];
        /* The EUI-64 is held little-endian; store it the way it is printed so
         * the web API and the Zigbee logs agree. */
        for (int i = 0; i < ZBC_IEEE_LEN; i++) {
            ieee[i] = params->device_addr.u8[ZBC_IEEE_LEN - 1 - i];
        }
        device_db_upsert(ieee, params->short_addr, &created);
        interrogate_tries_reset(ieee);
        if (!created) {
            hue_note_announce(params->short_addr, ieee);
        }
        /* The announcement carries the device's own capability flags, which is
         * what tells a battery remote from a lamp - see runs_on_a_battery(). */
        note_power_source(ieee, !(params->capability & ZBC_MAC_CAP_MAINS_POWER) &&
                                    !(params->capability & ZBC_MAC_CAP_RX_ON_IDLE));
        ESP_LOGI(TAG, "%s device 0x%04x announced itself (capability 0x%02x)",
                 created ? "New" : "Known", params->short_addr, params->capability);
        heap_watch_mark("device_announce", params->short_addr);
        interrogate_device(params->short_addr);
        if (zb_ota_wake_heard(params->short_addr)) {
            ota_wake_post();
        }
        /* If that request goes unanswered - a battery device is asleep again
         * within seconds - ask again while it may still be awake. */
        s_announce_step = 0;
        esp_timer_stop(s_announce_timer);
        esp_timer_start_once(s_announce_timer, 3 * 1000000ULL);
        break;
    }

    case EZB_ZDO_SIGNAL_LEAVE_INDICATION: {
        const ezb_zdo_signal_leave_indication_params_t *params = ezb_app_signal_get_params(app_signal);
        ESP_LOGI(TAG, "Device 0x%04x left the network", params->short_addr);
        uint8_t ieee[ZBC_IEEE_LEN];
        if (ieee_of(params->short_addr, ieee)) {
            device_db_set_online(ieee, false);
        }
        /* A child of ours going - with rejoin, to look for a parent again. */
        uint8_t leaver[ZBC_IEEE_LEN];
        for (int i = 0; i < ZBC_IEEE_LEN; i++) {
            leaver[i] = params->device_addr.u8[ZBC_IEEE_LEN - 1 - i];
        }
        uint16_t was = ZBC_PARENT_UNKNOWN;
        bool     changed = false;
        portENTER_CRITICAL(&s_link_mux);
        zb_link_t *l = link_slot(params->short_addr, false);
        if (l) {
            was     = parent_addr(&l->parent);
            changed = parent_left(&l->parent);
            if (changed) {
                l->parent_us  = esp_timer_get_time();
                l->parent_lqi = -1;
            }
        }
        portEXIT_CRITICAL(&s_link_mux);
        parent_log(params->leave_type == EZB_ZDO_LEAVE_TYPE_REJOIN ? "leaving_to_rejoin" : "leaving",
                   leaver, params->short_addr, ZBC_PARENT_UNKNOWN, was);
        if (changed) {
            zbc_state_bump();
        }
        break;
    }

    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        uint8_t duration = *(uint8_t *)ezb_app_signal_get_params(app_signal);
        s_permit_deadline_us = duration ? esp_timer_get_time() + (int64_t)duration * 1000000LL : 0;
        if (duration) {
            ESP_LOGI(TAG, "Network open for joining for %d s", duration);
        } else {
            ESP_LOGI(TAG, "Network closed for joining");
        }
        break;
    }

    /* A join is three signals. Logging all of them shows a device that
     * associated and then failed authorisation, or joined through a router and
     * was turned away by the trust centre - otherwise indistinguishable from one
     * that never heard the network. */
    case EZB_ZDO_SIGNAL_DEVICE_UPDATE: {
        const ezb_zdo_signal_device_update_params_t *p = ezb_app_signal_get_params(app_signal);
        ESP_LOGI(TAG, "Device 0x%04x (%02x%02x%02x%02x%02x%02x%02x%02x) update: status %d,"
                      " trust centre action %d, parent 0x%04x",
                 p->short_addr, p->device_addr.u8[7], p->device_addr.u8[6], p->device_addr.u8[5],
                 p->device_addr.u8[4], p->device_addr.u8[3], p->device_addr.u8[2],
                 p->device_addr.u8[1], p->device_addr.u8[0], (int)p->status, (int)p->tc_action,
                 p->parent_short);
        /* The one moment a device is seen choosing its parent - a remote
         * rejoining through a lamp after it lost us. Logged, not taken as its
         * parent: the tables say where it stays, at the next walk. `was` is
         * what its row held, under the address it had before. */
        static const char *const UPDATES[] = {
            [EZB_ZDO_UPDDEV_SECURE_REJOIN] = "rejoined",
            [EZB_ZDO_UPDDEV_UNSECURE_JOIN] = "joined",
            [EZB_ZDO_UPDDEV_DEVICE_LEFT]   = "left",
            [EZB_ZDO_UPDDEV_TC_REJOIN]     = "rejoined_tc",
        };
        uint8_t ieee[ZBC_IEEE_LEN];
        for (int i = 0; i < ZBC_IEEE_LEN; i++) {
            ieee[i] = p->device_addr.u8[ZBC_IEEE_LEN - 1 - i];
        }
        uint16_t before = p->short_addr;
        (void)device_db_short_by_ieee(ieee, &before);
        uint16_t was = ZBC_PARENT_UNKNOWN;
        portENTER_CRITICAL(&s_link_mux);
        const zb_link_t *l = link_slot(before, false);
        if (l) {
            was = parent_addr(&l->parent);
        }
        portEXIT_CRITICAL(&s_link_mux);
        parent_log(p->status < sizeof(UPDATES) / sizeof(UPDATES[0]) ? UPDATES[p->status] : "updated",
                   ieee, p->short_addr,
                   p->status == EZB_ZDO_UPDDEV_DEVICE_LEFT ? ZBC_PARENT_UNKNOWN : p->parent_short, was);
        break;
    }

    case EZB_ZDO_SIGNAL_DEVICE_AUTHORIZED: {
        const ezb_zdo_signal_device_authorized_params_t *p = ezb_app_signal_get_params(app_signal);
        ESP_LOGI(TAG, "Device 0x%04x (%02x%02x%02x%02x%02x%02x%02x%02x) authorisation: type %d,"
                      " status %d",
                 p->short_addr, p->device_addr.u8[7], p->device_addr.u8[6], p->device_addr.u8[5],
                 p->device_addr.u8[4], p->device_addr.u8[3], p->device_addr.u8[2],
                 p->device_addr.u8[1], p->device_addr.u8[0], (int)p->type, (int)p->status);
        break;
    }

    default:
        /* CONFIG_LOG_MAXIMUM_LEVEL is INFO, so ESP_LOGD is compiled out, and "the
         * stack said nothing" could not be told from "the stack said something
         * unseen". Unhandled signals are therefore logged at INFO while there is
         * no network (a formation that goes nowhere) and while the network is
         * open for joining (a device that will not pair). Otherwise they are
         * frequent and uninteresting. */
        if (!s_network_up || s_permit_deadline_us > esp_timer_get_time()) {
            ESP_LOGI(TAG, "Zigbee signal %s: %s(0x%02x)", s_network_up ? "while joining is open"
                                                                       : "while forming",
                     ezb_app_signal_to_string(signal_type), signal_type);
        }
        break;
    }
    return true;
}

/* ------------------------------------------------------------ public API */

/* `only_unknown`: skip endpoints whose on/off is already known - the second
 * pass, which exists to catch what the first one lost. */
/* Whether a command's reads are still running for this endpoint. Caller holds
 * the stack lock. The periodic colour re-read stays out of their way: an answer
 * to a read of ours marks the slot `answered`, which is how settle_work tells a
 * lamp that heard the command from one that did not - and a lamp wrongly
 * counted as having answered never gets the command sent again. */
static bool settle_is_watching(uint16_t short_addr, uint8_t ep)
{
    for (int i = 0; i < ZBC_SETTLE_SLOTS; i++) {
        if (s_settle[i].short_addr == short_addr && s_settle[i].ep == ep) {
            return s_settle[i].reads_left > 0;
        }
    }
    return false;
}

/* How stale a lamp may be and still be worth asking: two report intervals, so
 * one lost report does not stop the re-read, and a lamp that is gone stops being
 * asked after ten minutes. */
#define ZBC_COLOR_REREAD_MAX_AGE_US ((int64_t)ZBC_REPORT_MAX_INTERVAL_S * 2 * 1000000LL)

/* Read back what colour the lamps are actually showing.
 *
 * On/off and level are reported, so a wrong reading of either repairs itself
 * within the report interval. Colour is not reported, and otherwise the settle
 * read after a colour command is the only look: if it is missed (the command
 * waited in the gate, the answer was lost, the slot was recycled), the registry
 * keeps the wrong value until the colour next moves by more than the deadband.
 *
 * One frame per lamp per five minutes, and only for a lamp heard from lately:
 * asking a lamp that is gone is what refresh_work avoids. */
static void reread_colours(void)
{
    /* One row at a time rather than a registry snapshot: this runs every five
     * minutes for the life of the box, and a whole-registry copy is 8 KB of heap
     * held across the pacing delays. Same pattern as http_server.c's
     * json_stream. The address list is 256 bytes and the row is one device. */
    uint8_t (*ieees)[ZBC_IEEE_LEN] = calloc(CONFIG_ZBC_MAX_DEVICES, ZBC_IEEE_LEN);
    if (!ieees) {
        return;
    }
    const size_t n = device_db_list_ieee(ieees, CONFIG_ZBC_MAX_DEVICES);
    int          asked = 0;
    for (size_t i = 0; i < n && s_stack_started; i++) {
        zbc_device_t dev;
        if (!device_db_get(ieees[i], &dev) || !zbc_short_known(dev.short_addr) ||
            !device_db_is_fresh(&dev, ZBC_COLOR_REREAD_MAX_AGE_US)) {
            continue;
        }
        for (int e = 0; e < dev.ep_count; e++) {
            const zbc_endpoint_t *ep = &dev.eps[e];
            if (!(ep->features & (ZBC_FEAT_COLOR_TEMP | ZBC_FEAT_ANY_COLOR))) {
                continue;
            }
            /* Decide before pacing, so an endpoint that is skipped costs
             * neither the delay nor the lock. */
            esp_zigbee_lock_acquire(portMAX_DELAY);
            const bool watching = settle_is_watching(dev.short_addr, ep->ep);
            esp_zigbee_lock_release();
            if (watching) {
                continue;              /* its own reads are already coming */
            }
            if (asked++) {
                vTaskDelay(pdMS_TO_TICKS(ZBC_REFRESH_PACE_MS));
            }
            if (!s_stack_started) {
                break;                 /* stopped for the AP while we slept */
            }
            esp_zigbee_lock_acquire(portMAX_DELAY);
            read_color_state(dev.short_addr, ep->ep, ep->features);
            esp_zigbee_lock_release();
        }
    }
    if (asked) {
        ESP_LOGI(TAG, "Asked %d endpoint%s what colour they are showing", asked,
                 asked == 1 ? "" : "s");
    }
    free(ieees);
}

static void refresh_all_states(bool only_unknown)
{
    zbc_device_t *devices = calloc(CONFIG_ZBC_MAX_DEVICES, sizeof(zbc_device_t));
    if (!devices) {
        return;
    }
    size_t n = device_db_snapshot(devices, CONFIG_ZBC_MAX_DEVICES);
    int    asked = 0;
    for (size_t i = 0; i < n; i++) {
        for (int e = 0; e < devices[i].ep_count; e++) {
            const zbc_endpoint_t *ep = &devices[i].eps[e];
            if (!(ep->features & ZBC_FEAT_ONOFF)) {
                continue;                  /* a remote: nothing to read */
            }
            /* On/off or level. A read is three requests answered separately, so losing
             * one can leave an endpoint that has said whether it is on but not how
             * bright. CurrentLevel reporting is configured, but cannot be relied
             * on to fill it in. */
            const bool known = ep->on_off != ZBC_UNKNOWN &&
                               (!(ep->features & ZBC_FEAT_LEVEL) || ep->level != ZBC_UNKNOWN);
            if (only_unknown && known) {
                continue;
            }
            if (asked++) {
                vTaskDelay(pdMS_TO_TICKS(ZBC_REFRESH_PACE_MS));
            }
            if (!s_stack_started) {
                break;                     /* stopped for the AP while we slept */
            }
            esp_zigbee_lock_acquire(portMAX_DELAY);
            read_light_state(devices[i].short_addr, ep->ep, ep->features);
            esp_zigbee_lock_release();
        }
    }
    if (asked) {
        ESP_LOGI(TAG, "Asked %d endpoint%s for %s state", asked, asked == 1 ? "" : "s",
                 only_unknown ? "their still unknown" : "their");
    }
    free(devices);
}

esp_err_t zb_ota_server_attach(void)
{
    if (!s_stack_started) {
        return ESP_ERR_INVALID_STATE;     /* network-up attaches it */
    }
    esp_zigbee_lock_acquire(portMAX_DELAY);
    if (s_network_up) {
        ota_attach_locked();
    }
    const bool attached = s_ota_file != EZB_ZCL_OTA_FILE_INVALID_HANDLE;
    esp_zigbee_lock_release();
    return attached ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void zb_ota_server_detach(void)
{
    if (s_stack_started) {
        esp_zigbee_lock_acquire(portMAX_DELAY);
        if (s_ota_file) {
            ezb_zcl_ota_upgrade_remove_ota_file(ZBC_GATEWAY_EP_ID, s_ota_file);
            s_ota_file = EZB_ZCL_OTA_FILE_INVALID_HANDLE;
        }
        esp_zigbee_lock_release();
    }
    zb_ota_set_offered(false);
}

/* Whether a device has its receiver on: anything with a lamp's On/Off. Every
 * endpoint of a remote is a switch, and a remote sleeps. */
static bool device_listens(const zbc_device_t *dev)
{
    for (int i = 0; i < dev->ep_count; i++) {
        if (dev->eps[i].features & ZBC_FEAT_ONOFF) {
            return true;
        }
    }
    return false;
}

/* The endpoint a device asks for firmware from: the one its simple descriptor
 * or a query named, else its first. Never 0xFF: a Hue LST002 takes a notify to
 * endpoint 0xFF as delivered and ignores it, and answers one to endpoint 11 in
 * 2 s. */
static uint8_t ota_client_ep(const zbc_device_t *dev)
{
    zb_ota_device_t info;
    return (zb_ota_device_info(dev->ieee, &info) && info.ep) ? info.ep : (dev->ep_count ? dev->eps[0].ep : 1);
}

/* Not through the gate. Like Stop, it carries no state for a late duplicate to
 * get wrong - it only asks the device to ask us - and nothing a lamp is doing
 * should hold it up. A jitter of 100 makes a device that hears it ask at once;
 * anything lower lets it roll dice. Takes the stack lock. */
/* Whether a unicast notify arrived. For a sleepy device this is the question
 * that matters: delivered means it polled, took the notify and still did not
 * ask; not delivered means it never polled while its parent held the frame. */
static void notify_confirm_cb(ezb_af_user_cnf_t *cnf, void *user_ctx)
{
    (void)user_ctx;
    if (!cnf) {
        return;
    }
    zb_ota_notify_confirmed(cnf->dst_addr.u.short_addr, cnf->status);
    if (cnf->status != 0) {
        ESP_LOGW(TAG, "Image Notify to 0x%04x ep %d was not delivered (status 0x%02x)",
                 cnf->dst_addr.u.short_addr, cnf->dst_ep, cnf->status);
    }
}

static esp_err_t send_image_notify(uint16_t dst, uint8_t dst_ep)
{
    const bool unicast = dst < 0xFFF8;
    ezb_zcl_ota_upgrade_image_notify_cmd_t cmd = {
        .cmd_ctrl = {
            .dst_addr        = { .addr_mode = EZB_ADDR_MODE_SHORT, .u.short_addr = dst },
            .dst_ep          = dst_ep,
            .src_ep          = ZBC_GATEWAY_EP_ID,
            .dis_default_rsp = true,
            .cnf_ctx         = { .cb = unicast ? notify_confirm_cb : NULL },
        },
        .payload = {
            .type         = EZB_ZCL_OTA_UPGRADE_IMAGE_NOTIFY_PAYLOAD_TYPE_JITTER,
            .query_jitter = EZB_ZCL_OTA_UPGRADE_MAX_QUERY_JITTER_VALUE,
        },
    };
    ezb_err_t err;
    ZBC_SEND_RETRY(err, ezb_zcl_ota_upgrade_image_notify_cmd_req(&cmd));
    return esp_zigbee_err_to_esp(err);
}

esp_err_t zb_ota_server_ask(const uint8_t ieee[ZBC_IEEE_LEN])
{
    if (!s_stack_started || !s_network_up) {
        return ESP_ERR_INVALID_STATE;
    }
    zbc_device_t dev;
    if (!device_db_get(ieee, &dev)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!zbc_short_known(dev.short_addr)) {
        return ZB_ERR_UNREACHABLE;     /* restored, not on this network yet */
    }
    /* A device with its receiver on answers a read of its OTA client at once;
     * a sleepy remote would not, and the read would only count against the
     * radio. Both hear the notify - a sleepy one when it is next heard from. */
    if (device_listens(&dev)) {
        const uint8_t ep = ota_client_ep(&dev);
        esp_zigbee_lock_acquire(portMAX_DELAY);
        read_ota_client_info(dev.short_addr, ep);
        esp_zigbee_lock_release();
    }
    return zb_ota_server_notify(ieee);
}

esp_err_t zb_ota_server_notify(const uint8_t *ieee)
{
    if (!s_stack_started || !s_network_up) {
        return ESP_ERR_INVALID_STATE;
    }
    /* To every device with its receiver on, or to one device. There is no
     * broadcast address mode in the SDK; 0xFFFD is the rx-on-when-idle
     * broadcast address, and 0xFF every endpoint. */
    uint16_t dst    = 0xFFFD;
    uint8_t  dst_ep = 0xFF;
    bool     sleeps = false;
    if (ieee) {
        zbc_device_t dev;
        if (!device_db_get(ieee, &dev)) {
            return ESP_ERR_NOT_FOUND;
        }
        /* Never 0xFFFF for a restored row: that would be every device. */
        if (!zbc_short_known(dev.short_addr)) {
            return ZB_ERR_UNREACHABLE;
        }
        dst    = dev.short_addr;
        dst_ep = ota_client_ep(&dev);
        sleeps = !device_listens(&dev);
    }
    const esp_err_t err = send_image_notify(dst, dst_ep);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Image Notify sent to 0x%04x ep %u", dst, dst_ep);
    }
    /* A sleepy device's parent holds that unicast for a few seconds only, and
     * the remote may well not poll in time: it is sent again when the device is
     * next heard from (ota_wake_work). */
    if (sleeps) {
        zb_ota_wake_arm(ieee);
    }
    return err;
}

esp_err_t zb_permit_join(uint8_t seconds)
{
    if (!s_stack_started) {
        return ESP_ERR_INVALID_STATE;     /* see lookup_endpoint */
    }
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_err_t err = seconds ? ezb_bdb_open_network(seconds) : ezb_bdb_close_network();
    esp_zigbee_lock_release();
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Permit join (%d s) failed with error 0x%04x", seconds, err);
    }
    return esp_zigbee_err_to_esp(err);
}

/* The settle read after a level or on/off command skips colour: asking for it is
 * airtime the next command has to fit around, and nothing here moved it. Colour
 * commands pass the full mask, because seeing the colour land is their point. */
static inline uint16_t without_color(uint16_t f)
{
    return f & ~(ZBC_FEAT_COLOR_TEMP | ZBC_FEAT_ANY_COLOR);
}

esp_err_t zb_set_onoff(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, zb_onoff_cmd_t cmd)
{
    /* lookup_endpoint, like every other command: a row restored from another
     * network's backup is ZB_ERR_UNREACHABLE (409 "pair it again" on the API),
     * where device_db_short_by_ieee() alone made it 404 "unknown device". */
    uint16_t  short_addr, features;
    esp_err_t err = lookup_endpoint(ieee, ep, &short_addr, &features);
    if (err != ESP_OK) {
        return err;
    }

    const zbc_cmd_t c = { .kind = ZBC_CMD_ONOFF, .a = (uint16_t)cmd };
    err = gate_send(short_addr, ep, &c, ZBC_CH_LEVEL);

    /* No read here: sent while the lamp is still switching, or answered before a
     * gated command has even been transmitted, it would write the previous state
     * into the registry. schedule_settle_read asks twice, after the transition. */
    if (err == ESP_OK) {
        schedule_settle_read(short_addr, ep, without_color(features),
                             5, cmd == ZB_ONOFF_TOGGLE ? -1 : (cmd == ZB_ONOFF_ON ? 1 : 0), 0);
    }

    ESP_LOGI(TAG, "Sent On/Off command %d to 0x%04x ep %d (%s)", cmd, short_addr, ep,
             zb_err_name(err));
    return err;
}

/* Resolves an endpoint to its short address and what it can do, so every command
 * below can refuse politely instead of shouting into the mesh. */
static esp_err_t lookup_endpoint(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint16_t *short_addr,
                                 uint16_t *features)
{
    /* A stored device keeps its short address, so in the setup portal the lookup
     * below still succeeds - and the send after it takes the stack lock, which
     * asserts before esp_zigbee_init(). Refused here, every sender is covered. */
    if (!s_stack_started) {
        return ESP_ERR_INVALID_STATE;
    }
    zbc_device_t dev;
    if (!device_db_get(ieee, &dev)) {
        return ESP_ERR_NOT_FOUND;
    }
    /* Restored from another network's backup and not joined here yet: the
     * quiet "not answering" of a lamp that has gone, not a fault - lighting.c
     * records nothing as sent and does not warn. */
    if (!zbc_short_known(dev.short_addr)) {
        return ZB_ERR_UNREACHABLE;
    }
    for (int i = 0; i < dev.ep_count; i++) {
        if (dev.eps[i].ep == ep) {
            *short_addr = dev.short_addr;
            *features   = dev.eps[i].features;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/* Fading a lamp out and switching it off in one command.
 *
 * A ramp to level zero does not switch every lamp off: a Hue LST002 clamps at
 * level 1 and stays faintly lit. A ramp followed by an explicit Off is two
 * commands with a race between them. OffWithEffect is the single ZCL command for
 * this: fade out, then off, decided by the lamp. Effect 0x00 variant 0x00 is
 * "fade to off over 0.8 s". */
/* Leave the lamp at `mireds` and then switch it off, as one gated command - the
 * two frames must not be two calls, see ZBC_CMD_WARM_OFF. `plain_off` sends a
 * bare Off instead of OffWithEffect, for a box whose fade is set to Instant.
 *
 * The full feature mask on the settle read, unlike zb_fade_off's: the colour is
 * the point of this one, and it is the only chance to see where the lamp was
 * left - nothing re-reads a lamp that is off. */
esp_err_t zb_warm_off(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint16_t mireds,
                      bool plain_off)
{
    uint16_t short_addr, features;
    ESP_RETURN_ON_ERROR(lookup_endpoint(ieee, ep, &short_addr, &features), TAG, "unknown endpoint");
    if (!(features & ZBC_FEAT_COLOR_TEMP)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const zbc_cmd_t c   = { .kind = ZBC_CMD_WARM_OFF, .step = 0, .a = mireds,
                            .b = plain_off ? 1 : 0,
                            .transition_ds = plain_off ? 0 : ZBC_OFF_EFFECT_DS };
    esp_err_t       err = gate_send(short_addr, ep, &c, ZBC_CH_LEVEL);
    if (err == ESP_OK) {
        schedule_settle_read(short_addr, ep, features, c.transition_ds, 0, 0);
    }
    ESP_LOGI(TAG, "Warm to %d mireds and off on 0x%04x ep %d (%s)", mireds, short_addr, ep,
             zb_err_name(err));
    return err;
}

esp_err_t zb_fade_off(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    uint16_t short_addr, features;
    ESP_RETURN_ON_ERROR(lookup_endpoint(ieee, ep, &short_addr, &features), TAG, "unknown endpoint");

    const zbc_cmd_t c = { .kind = ZBC_CMD_FADE_OFF, .transition_ds = ZBC_OFF_EFFECT_DS };
    esp_err_t       err = gate_send(short_addr, ep, &c, ZBC_CH_LEVEL);
    if (err == ESP_OK) {
        schedule_settle_read(short_addr, ep, without_color(features),
                             c.transition_ds, 0, 0);
    }
    ESP_LOGI(TAG, "Fade to off on 0x%04x ep %d (%s)", short_addr, ep, zb_err_name(err));
    return err;
}

/* A lamp switched off with OffWithEffect keeps its CurrentLevel (an OSRAM PAR16
 * reads on = false, level = 254 after an off), so a move to that level on the
 * next "on" has nowhere to travel and the lamp snaps to full.
 *
 * Dropping the level first gives the ramp somewhere to start, and it must be the
 * WithOnOff variant: many lamps, the OSRAM among them, ignore a plain Move to
 * Level while off. So the lamp is switched on at the bottom of its range first -
 * 1 % reads as a fade from black - with no transition, since the visible part is
 * the ramp.
 *
 * The two frames are one gated command. As two calls the ramp would supersede
 * the preset in the gate and the lamp would snap to full; as one, the endpoint is
 * held for both halves and the ramp goes only once the preset is confirmed.
 *
 * One settle read, for the ramp: it carries the intent, and a read for the
 * preset would only race it. */
esp_err_t zb_fade_in(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint8_t level,
                     uint16_t transition_ds)
{
    uint16_t short_addr, features;
    ESP_RETURN_ON_ERROR(lookup_endpoint(ieee, ep, &short_addr, &features), TAG, "unknown endpoint");
    if (!(features & ZBC_FEAT_LEVEL)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const zbc_cmd_t c = { .kind = ZBC_CMD_FADE_IN, .step = 0, .a = level,
                          .transition_ds = transition_ds };
    esp_err_t       err = gate_send(short_addr, ep, &c, ZBC_CH_LEVEL);
    if (err == ESP_OK) {
        schedule_settle_read(short_addr, ep, without_color(features),
                             transition_ds, 1, level);
    }
    ESP_LOGI(TAG, "Fade in to level %d on 0x%04x ep %d (%s)", level, short_addr, ep,
             zb_err_name(err));
    return err;
}

/* Halt whatever level transition the lamp is running, wherever it has got to.
 *
 * A held button is a stream of move-to-level commands, each a ramp the lamp
 * keeps running after the command that started it; when the stream ends the lamp
 * carries on to the last target. Stop ends the transition in place.
 *
 * StopWithOnOff (0x07) rather than plain Stop (0x03), because the ramp uses
 * MoveWithOnOff (0x05) and the spec pairs them. The pairing makes no difference
 * to an OSRAM PAR16, which ignores Stop under all four pairings and runs to the
 * rail; a Hue LST002 stops cleanly.
 *
 * So Stop is worth sending but must not be relied on: the move-to-level sent on
 * release is what puts every lamp where the finger left it.
 *
 * No settle read here - the caller schedules one; this only stops the motion. */
esp_err_t zb_level_stop(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    uint16_t short_addr, features;
    ESP_RETURN_ON_ERROR(lookup_endpoint(ieee, ep, &short_addr, &features), TAG, "unknown endpoint");
    if (!(features & ZBC_FEAT_LEVEL)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* Never a probe: a Stop to a lamp that is not there halts nothing. */
    if (!reach_admits(short_addr, false)) {
        return ZB_ERR_UNREACHABLE;
    }

    /* Stop does not go through the gate, and must not.
     *
     * Every other command carries an absolute target, so two in flight can be
     * reordered by an APS retry and the wrong one win. Stop carries no target: a
     * late duplicate changes nothing.
     *
     * Gated, the release's Stop and the move-to-level after it share
     * ZBC_CH_LEVEL, so the level command replaces the Stop in the pending slot
     * and no Stop is ever sent: the lamp finishes the ramp it was given and is
     * then pulled back to the release position.
     *
     * Its confirmation does not open the gate: a command that never occupied it
     * must not advance it. */
    ezb_zcl_level_stop_cmd_t stop = {
        .cmd_ctrl = {
            .dst_addr        = { .addr_mode = EZB_ADDR_MODE_SHORT, .u.short_addr = short_addr },
            .dst_ep          = ep,
            .src_ep          = ZBC_GATEWAY_EP_ID,
            .dis_default_rsp = true,
            .cnf_ctx         = { .cb = stop_confirm_cb },
        },
    };
    ezb_err_t ezb_err;
    ZBC_SEND_RETRY(ezb_err, ezb_zcl_level_stop_with_on_off_cmd_req(&stop));
    const esp_err_t err = esp_zigbee_err_to_esp(ezb_err);
    if (err == ESP_OK) {
        /* Disarm the correction before reading, or the coordinator undoes the stop:
         * the last sweep command left want_level at the level it was travelling
         * towards, the lamp is deliberately short of it, and convergence would
         * drive it on.
         *
         * want_on -1 tells schedule_settle_read there is no intent to hold the
         * lamp to (fixes_left 0). The read still happens: where the sweep ended
         * is exactly what this side does not know. Transition 0: nothing to
         * wait for. */
        schedule_settle_read(short_addr, ep,
                             without_color(features),
                             0, -1, 0);
    }
    ESP_LOGI(TAG, "Stop level ramp on 0x%04x ep %d (%s)", short_addr, ep, zb_err_name(err));
    return err;
}

#if CONFIG_ZBC_WLED

/* ------------------------------------------------ Hue entertainment frames
 *
 * Philips' own way of moving many Hue colour lamps fast, reverse engineered by
 * Bifrost (github.com/chrivers/bifrost, doc/hue-zigbee-clusters.md): cluster
 * 0xFC01, manufacturer 0x100B. One frame carries up to ten lamps; it is sent to
 * one of them, which repeats it as a MAC broadcast for the rest. No retries by
 * design - a late frame is worse than a lost one, which the next replaces - and
 * a counter in the frame lets a lamp drop one that arrives out of order.
 *
 * This is the transport and nothing more: lighting.c decides which lamps, when a
 * stream begins and ends, and what each frame says (the WLED interface feeds
 * it). Measured on an LST002 (2026-10-09): 20 frames a second for 30 minutes
 * without a refusal or a leak; the lamp repeats every frame, and the stack
 * drops the repeat below APS.
 *
 * - Frames bypass the gate, the second thing to after zb_level_stop. The gate
 *   holds one command per lamp and lets a newer one replace a waiting one; a
 *   stream through it would collapse into one lagging command. The counter
 *   does the gate's job of ordering.
 * - They are raw APSDE with no APS acknowledgement: an APS retry comes 1.6 s
 *   later, after thirty newer frames. MAC acknowledgement and retries remain.
 * - A lamp answers reads during a stream with its state from before it, holds
 *   the last frame when the stream ends, and only then brings its attributes up
 *   to date (level within 2 s, colour up to a minute after). */

/* >>> host-test region: ent-frame <<< */

/* The frame body, command 1:
 *
 *   u32 counter, u16 smoothing, then per lamp 7 bytes:
 *   u16 short address, u16 (brightness << 5 | mode), 3 bytes of xy
 *
 * all little-endian. Brightness is 11 bits (0..2047). Mode 0b01011 addresses a
 * whole lamp; 0 is a segment of a gradient lamp, at a virtual address command 7
 * hands out. xy is 12 bits each, scaled to the wide gamut's corner (0.7347,
 * 0.8264), packed x low byte, x high nibble with y low nibble, y high byte.
 *
 * Smoothing is how long a lamp takes to glide to the frame: linear, 0xFFFF
 * about 2.56 s. Hue's clients send 0x0400 (160 ms); Bifrost sets it to one frame
 * interval, so each glide ends as the next frame arrives. */
#define ZBC_ENT_LAMPS_MAX     10
#define ZBC_ENT_HDR_LEN       6
#define ZBC_ENT_REC_LEN       7
#define ZBC_ENT_MODE_DEVICE   0x0B
#define ZBC_ENT_BRI_MAX       2047
#define ZBC_ENT_SMOOTH_MAX_US 2560000LL
#define ZBC_ENT_GAMUT_X       0.7347f
#define ZBC_ENT_GAMUT_Y       0.8264f

static uint16_t ent_smoothing(int64_t interval_us)
{
    if (interval_us <= 0) {
        return 0;
    }
    if (interval_us >= ZBC_ENT_SMOOTH_MAX_US) {
        return 0xFFFF;
    }
    return (uint16_t)(interval_us * 0x10000 / ZBC_ENT_SMOOTH_MAX_US);
}

static uint16_t ent_quant(float v, float max)
{
    if (!(v > 0.0f)) {
        return 0;
    }
    const float q = v * 4095.0f / max;
    return q >= 4095.0f ? 4095 : (uint16_t)q;
}

static void ent_quant_xy(float x, float y, uint8_t out[3])
{
    const uint16_t qx = ent_quant(x, ZBC_ENT_GAMUT_X);
    const uint16_t qy = ent_quant(y, ZBC_ENT_GAMUT_Y);
    out[0] = (uint8_t)(qx & 0xFF);
    out[1] = (uint8_t)(((qx >> 8) & 0x0F) | ((qy & 0x0F) << 4));
    out[2] = (uint8_t)(qy >> 4);
}

static void ent_pack_record(uint16_t addr, uint16_t bri, uint8_t mode, const uint8_t xy[3],
                            uint8_t out[ZBC_ENT_REC_LEN])
{
    const uint16_t word = (uint16_t)(((bri > ZBC_ENT_BRI_MAX ? ZBC_ENT_BRI_MAX : bri) << 5) | (mode & 0x1F));
    out[0] = (uint8_t)addr;
    out[1] = (uint8_t)(addr >> 8);
    out[2] = (uint8_t)word;
    out[3] = (uint8_t)(word >> 8);
    out[4] = xy[0];
    out[5] = xy[1];
    out[6] = xy[2];
}

static void ent_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* The header of a frame; the records follow it, ZBC_ENT_REC_LEN each. */
static size_t ent_pack_header(uint32_t counter, uint16_t smoothing, uint8_t out[ZBC_ENT_HDR_LEN])
{
    ent_put_u32(out, counter);
    out[4] = (uint8_t)smoothing;
    out[5] = (uint8_t)(smoothing >> 8);
    return ZBC_ENT_HDR_LEN;
}

/* Command 3: sent to every lamp at the start and the end of a stream. Bifrost
 * has only seen the first byte 0 and the second 0 or 1, and sends 0, 1. */
static size_t ent_pack_sync(uint32_t counter, uint8_t out[6])
{
    out[0] = 0x00;
    out[1] = 0x01;
    ent_put_u32(&out[2], counter);
    return 6;
}

/* >>> end host-test region: ent-frame <<< */

_Static_assert(ZB_ENT_MAX_LAMPS == ZBC_ENT_LAMPS_MAX, "the request and the frame disagree on lamps per frame");

/* One stream at a time. The counters in `s_ent` are written by the caller's task
 * and the Zigbee task (confirmations, a lamp's answers) and read by the status
 * poll: words, so a torn read is one count off. Starting, sending and ending
 * take s_ent_lock, then the stack lock - never the other way round, and never
 * from a stack callback. */
static zb_ent_stats_t    s_ent;
static uint16_t          s_ent_addr[ZBC_ENT_LAMPS_MAX];
static uint8_t           s_ent_ep[ZBC_ENT_LAMPS_MAX];
static uint8_t           s_ent_tsn = 0x80;
static SemaphoreHandle_t s_ent_lock;
static StaticSemaphore_t s_ent_lock_buf;
/* The frame being sent; static so the stack can keep a pointer into it until it
 * has copied the frame, whatever it does. Only written under s_ent_lock. */
static uint8_t           s_ent_buf[5 + ZBC_ENT_HDR_LEN + ZBC_ENT_LAMPS_MAX * ZBC_ENT_REC_LEN];

static void ent_lock(void)
{
    /* Created on first use rather than at start: a static mutex needs no heap. */
    static portMUX_TYPE once = portMUX_INITIALIZER_UNLOCKED;
    taskENTER_CRITICAL(&once);
    if (!s_ent_lock) {
        s_ent_lock = xSemaphoreCreateMutexStatic(&s_ent_lock_buf);
    }
    taskEXIT_CRITICAL(&once);
    xSemaphoreTake(s_ent_lock, portMAX_DELAY);
}

static void ent_unlock(void)
{
    xSemaphoreGive(s_ent_lock);
}

static void ent_sync_confirm_cb(ezb_af_user_cnf_t *cnf, void *user_ctx)
{
    (void)user_ctx;
    if (cnf && cnf->status == 0) {
        s_ent.sync_delivered++;
    } else {
        s_ent.sync_undelivered++;
    }
}

/* From the Zigbee task, for every 0xFC01 frame reaching APS. A lamp's answer to
 * us (the balance write's, a Default Response refusing a command) is read here
 * because the stack does not hand answers on a cluster none of our endpoints
 * has to the ZCL callbacks; then passed on all the same. A lamp's repeat of a
 * frame is counted and, should one ever arrive (none did on the LST002), kept
 * from the stack: esp-zigbee-lib leaks per group-cast it is given (#914), and
 * twenty a second would find out quickly whether broadcasts do the same. */
static bool ent_note_aps(const ezb_apsde_data_ind_t *ind)
{
    if (ind->cluster_id != ZBC_ENT_CLUSTER || ind->asdu_length < 3) {
        return false;
    }
    const uint8_t fc     = ind->asdu[0];
    const size_t  cmd_at = (fc & 0x04) ? 4 : 2;

    if ((fc & 0x03) == 0x00 && ind->asdu_length > cmd_at + 1) {
        const uint8_t *pl = &ind->asdu[cmd_at + 1];
        const size_t   n  = ind->asdu_length - cmd_at - 1;
        s_ent.answers_heard++;
        if (ind->asdu[cmd_at] == 0x04) {                  /* Write Attributes Response */
            s_ent.balance_status = pl[0];
        } else if (ind->asdu[cmd_at] == 0x0B && n >= 2) { /* Default Response */
            s_ent.rejected_cmd    = pl[0];
            s_ent.rejected_status = pl[1];
        }
        return false;
    }
    if ((fc & 0x03) == 0x01 && ind->asdu_length > cmd_at && ind->asdu[cmd_at] == ZBC_ENT_CMD_FRAME) {
        s_ent.relay_heard++;
        return true;
    }
    return false;
}

/* Light balance, attribute 0x0005: how bright a lamp is in a stream relative to
 * the others. Bifrost writes 0xFE (all of it) at every start; a Hue on firmware
 * 1.76.11 refuses it, 1.122.2 takes it. Stack lock not held. */
static void ent_write_balance(uint16_t addr, uint8_t ep)
{
    uint8_t             bal  = 0xFE;
    ezb_zcl_attribute_t attr = {
        .id   = ZBC_ENT_ATTR_BAL,
        .data = { .type = EZB_ZCL_ATTR_TYPE_UINT8, .size = sizeof(bal), .value = &bal },
    };
    ezb_zcl_write_attr_cmd_t cmd = {
        .cmd_ctrl = {
            .dst_addr   = { .addr_mode = EZB_ADDR_MODE_SHORT, .u.short_addr = addr },
            .dst_ep     = ep,
            .src_ep     = ZBC_GATEWAY_EP_ID,
            .cluster_id = ZBC_ENT_CLUSTER,
            .manuf_code = ZBC_ENT_MANUF,
            .fc         = { .manuf_specific = 1 },
        },
        .payload = { .attr_number = 1, .attr_field = &attr },
    };
    ezb_err_t err;
    ZBC_SEND_RETRY(err, ezb_zcl_write_attr_cmd_req(&cmd));
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Entertainment: the balance write to 0x%04x was refused (0x%04x)", addr, err);
    }
}

/* Command 3 to every lamp of the stream, through the ZCL layer: APS-acknowledged,
 * since it marks the start and the end and is sent once. s_ent_lock held. */
static void ent_sync_all_locked(void)
{
    uint8_t      body[6];
    const size_t len = ent_pack_sync(s_ent.counter, body);
    for (int i = 0; i < s_ent.lamps; i++) {
        ezb_zcl_custom_cmd_t cmd = {
            .cmd_ctrl = {
                .dst_addr   = { .addr_mode = EZB_ADDR_MODE_SHORT, .u.short_addr = s_ent_addr[i] },
                .dst_ep     = s_ent_ep[i],
                .src_ep     = ZBC_GATEWAY_EP_ID,
                .cluster_id = ZBC_ENT_CLUSTER,
                .manuf_code = ZBC_ENT_MANUF,
                .fc         = { .manuf_specific = 1, .direction = 0, .dis_default_rsp = 1 },
                .cnf_ctx    = { .cb = ent_sync_confirm_cb },
            },
            .cmd_id      = ZBC_ENT_CMD_SYNC,
            .data_length = (uint16_t)len,
            .data        = body,
        };
        ezb_err_t err;
        ZBC_SEND_RETRY(err, ezb_zcl_custom_cmd_req(&cmd));
        if (err == EZB_ERR_NONE) {
            s_ent.sync_sent++;
        } else {
            ESP_LOGW(TAG, "Entertainment: the sync to 0x%04x was refused (0x%04x)", s_ent_addr[i], err);
        }
    }
}

esp_err_t zb_ent_begin(const zb_ent_lamp_t *lamps, uint8_t n)
{
    if (!lamps || n == 0 || n > ZBC_ENT_LAMPS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_stack_started || !s_network_up) {
        return ESP_ERR_INVALID_STATE;
    }
    uint16_t addr[ZBC_ENT_LAMPS_MAX];
    for (int i = 0; i < n; i++) {
        uint16_t features;
        if (lookup_endpoint(lamps[i].ieee, lamps[i].ep, &addr[i], &features) != ESP_OK) {
            return ESP_ERR_NOT_FOUND;
        }
    }

    ent_lock();
    if (s_ent.active) {
        ent_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    for (int i = 0; i < n; i++) {
        s_ent_addr[i] = addr[i];
        s_ent_ep[i]   = lamps[i].ep;
    }
    s_ent.lamps           = n;
    s_ent.proxy           = addr[0];
    s_ent.balance_status  = -1;
    s_ent.rejected_status = -1;
    s_ent.streams++;
    for (int i = 0; i < n; i++) {
        ent_write_balance(addr[i], lamps[i].ep);
    }
    /* The counter carries on from the last stream: a lamp that remembers it is
     * never asked to go backwards. */
    ent_sync_all_locked();
    s_ent.active = true;
    ent_unlock();
    ESP_LOGI(TAG, "Entertainment: stream to %u lamp(s) through 0x%04x", n, addr[0]);
    return ESP_OK;
}

/* The link cost a neighbour's LQI stands for, 1 (best) to 7, as Zigbee's
 * incoming cost: what the stack would have worked out itself. */
static uint8_t ent_lqi_cost(uint8_t lqi)
{
    return lqi >= 200 ? 1 : lqi >= 150 ? 2 : lqi >= 100 ? 3 : lqi >= 70 ? 5 : 7;
}

/* Keeps the first lamp a neighbour while it streams. A router stays one only
 * while its Link Status arrives (every 15 s); three periods without and the
 * stack takes the link for broken and starts a route discovery to the lamp,
 * which a lamp in a stream does not answer - and every frame then waits on it
 * and expires, ten seconds at a time, the buffers full and every new frame
 * refused (seen 2026-10-09: the lamp's age climbing 0-3 during a stream with a
 * WLED client on Wi-Fi, then the stall, once after 50 s, once after six
 * minutes). The lamp is plainly there: it acknowledges every frame at the MAC.
 * So its age is set back to 0, with the costs it had, every couple of seconds
 * while the stream runs. ezb_nwk_set_neighbor_info is one of the library's
 * certification utilities - pinned with the library, recheck on an upgrade.
 * Stack lock held. */
static void ent_keep_neighbour_locked(void)
{
    ezb_nwk_info_iterator_t it = EZB_NWK_INFO_ITERATOR_INIT;
    ezb_nwk_neighbor_info_t nb;
    while (ezb_nwk_get_next_neighbor(&it, &nb) == EZB_ERR_NONE) {
        if (nb.short_addr != s_ent.proxy) {
            continue;
        }
        if (nb.age > 0 && nb.relationship != EZB_NWK_RELATIONSHIP_CHILD &&
            ezb_nwk_set_neighbor_info(nb.short_addr, 0, nb.outgoing_cost ? nb.outgoing_cost : ent_lqi_cost(nb.lqi),
                                      ent_lqi_cost(nb.lqi)) == EZB_ERR_NONE) {
            s_ent.kept_fresh++;
        }
        return;
    }
}

esp_err_t zb_ent_frame(const zb_ent_light_t *lights, uint16_t glide_ms)
{
    ent_lock();
    if (!s_ent.active || !s_stack_started || !s_network_up) {
        ent_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t *p = &s_ent_buf[5];
    p += ent_pack_header(s_ent.counter, ent_smoothing((int64_t)glide_ms * 1000), p);
    for (int i = 0; i < s_ent.lamps; i++) {
        uint8_t xy[3];
        ent_quant_xy(lights[i].x, lights[i].y, xy);
        ent_pack_record(s_ent_addr[i], lights[i].bri, ZBC_ENT_MODE_DEVICE, xy, p);
        p += ZBC_ENT_REC_LEN;
    }
    /* Raw APSDE, the only way to send without an APS acknowledgement. The ZCL
     * header is ours: cluster-specific, manufacturer-specific, no default
     * response, to the server. So is the sequence number, which can equal one the
     * stack has in flight; that would matter only if the stack matched
     * confirmations by it, and 36,000 frames beside ordinary traffic caused no
     * gate timeout. */
    s_ent_buf[0] = 0x15;
    s_ent_buf[1] = (uint8_t)ZBC_ENT_MANUF;
    s_ent_buf[2] = (uint8_t)(ZBC_ENT_MANUF >> 8);
    s_ent_buf[3] = s_ent_tsn++;
    s_ent_buf[4] = ZBC_ENT_CMD_FRAME;
    const ezb_apsde_data_req_t req = {
        .dst_address  = { .addr_mode = EZB_ADDR_MODE_SHORT, .u.short_addr = s_ent.proxy },
        .src_endpoint = ZBC_GATEWAY_EP_ID,
        .dst_endpoint = s_ent_ep[0],
        .cluster_id   = ZBC_ENT_CLUSTER,
        .profile_id   = EZB_AF_HA_PROFILE_ID,
        .radius       = 0,
        .tx_options   = 0,
        .asdu_length  = (uint16_t)(p - s_ent_buf),
        .asdu         = s_ent_buf,
    };
    static int64_t kept_us;
    const int64_t  now_us = esp_timer_get_time();
    esp_zigbee_lock_acquire(portMAX_DELAY);
    const ezb_err_t err = ezb_apsde_data_request(&req);
    if (now_us - kept_us >= 2000000) {
        kept_us = now_us;
        ent_keep_neighbour_locked();
    }
    esp_zigbee_lock_release();
    s_ent.counter++;
    if (err == EZB_ERR_NONE) {
        s_ent.frames_sent++;
    } else {
        s_ent.frames_refused++;
        s_ent.last_refusal = err;
    }
    ent_unlock();
    return err == EZB_ERR_NONE ? ESP_OK : ESP_FAIL;
}

void zb_ent_end(bool sync)
{
    ent_lock();
    if (s_ent.active) {
        if (sync && s_stack_started && s_network_up) {
            ent_sync_all_locked();
        }
        s_ent.active = false;
        ESP_LOGI(TAG, "Entertainment: stream ended%s after frame %" PRIu32,
                 sync ? "" : " without a sync", s_ent.counter);
    }
    ent_unlock();
}

void zb_get_ent_stats(zb_ent_stats_t *out)
{
    *out = s_ent;
}

#endif /* CONFIG_ZBC_WLED */

esp_err_t zb_set_level(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint8_t level, uint16_t transition_ds)
{
    uint16_t short_addr, features;
    ESP_RETURN_ON_ERROR(lookup_endpoint(ieee, ep, &short_addr, &features), TAG, "unknown endpoint");
    if (!(features & ZBC_FEAT_LEVEL)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const zbc_cmd_t c = { .kind = ZBC_CMD_LEVEL, .a = level, .transition_ds = transition_ds };
    esp_err_t       err = gate_send(short_addr, ep, &c, ZBC_CH_LEVEL);
    if (err == ESP_OK) {
        /* Move-to-level-with-on-off at 0 would land the lamp off, so the level
         * says which state it is meant to land in. */
        schedule_settle_read(short_addr, ep, without_color(features),
                             transition_ds, level > 0 ? 1 : 0, level);
    }

    ESP_LOGI(TAG, "Set level %d on 0x%04x ep %d (%s)", level, short_addr, ep, zb_err_name(err));
    return err;
}

esp_err_t zb_set_color_temp(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint16_t mireds,
                            uint16_t transition_ds)
{
    uint16_t short_addr, features;
    ESP_RETURN_ON_ERROR(lookup_endpoint(ieee, ep, &short_addr, &features), TAG, "unknown endpoint");
    if (!(features & ZBC_FEAT_COLOR_TEMP)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const zbc_cmd_t c = { .kind = ZBC_CMD_CT, .a = mireds, .transition_ds = transition_ds };
    esp_err_t       err = gate_send(short_addr, ep, &c, ZBC_CH_COLOR);
    if (err == ESP_OK) {
        /* The slot is the level command's too (ZBC_WANT_KEEP), and this call moves
         * its deadline to the end of *this* transition. A long colour transition -
         * Apple Home's Adaptive Lighting asks for half an hour - would push the
         * level command's read and its correction out by as much. Nothing here
         * sends one longer than a minute; keep it that way or cap the delay. */
        schedule_settle_read_ex(short_addr, ep, features, transition_ds, ZBC_WANT_KEEP, 0, &c);
    }

    ESP_LOGI(TAG, "Set %d mireds on 0x%04x ep %d (%s)", mireds, short_addr, ep,
             zb_err_name(err));
    return err;
}

/* ZCL's hue and saturation are 0-254; colorspace.c works in degrees and percent.
 * The conversion lives there because curve_db needs the same one. */
static void hs_to_xy(uint8_t zcl_hue, uint8_t zcl_sat, uint16_t *out_x, uint16_t *out_y)
{
    float x, y;
    zbc_hs_to_xy((int)lroundf((float)zcl_hue * 360.0f / 254.0f),
                 (int)lroundf((float)zcl_sat * 100.0f / 254.0f), &x, &y);
    /* ZCL carries xy as a fraction of 65536. */
    *out_x = (uint16_t)(x * 65535.0f);
    *out_y = (uint16_t)(y * 65535.0f);
}

esp_err_t zb_set_color_hs(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint8_t hue, uint8_t saturation,
                          uint16_t transition_ds)
{
    uint16_t short_addr, features;
    ESP_RETURN_ON_ERROR(lookup_endpoint(ieee, ep, &short_addr, &features), TAG, "unknown endpoint");
    if (!(features & ZBC_FEAT_ANY_COLOR)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    zbc_cmd_t c = { .transition_ds = transition_ds };
    if (features & ZBC_FEAT_COLOR_HS) {
        c.kind = ZBC_CMD_HS;
        c.a    = hue;
        c.b    = saturation;
    } else {
        uint16_t x, y;
        hs_to_xy(hue, saturation, &x, &y);
        c.kind = ZBC_CMD_XY;
        c.a    = x;
        c.b    = y;
    }

    esp_err_t err = gate_send(short_addr, ep, &c, ZBC_CH_COLOR);
    if (err == ESP_OK) {
        schedule_settle_read_ex(short_addr, ep, features, transition_ds, ZBC_WANT_KEEP, 0, &c);
    }

    ESP_LOGI(TAG, "Set hue %d sat %d on 0x%04x ep %d (%s)", hue, saturation, short_addr, ep,
             zb_err_name(err));
    return err;
}

esp_err_t zb_set_color_xy(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint16_t x, uint16_t y,
                          uint16_t transition_ds)
{
    uint16_t short_addr, features;
    ESP_RETURN_ON_ERROR(lookup_endpoint(ieee, ep, &short_addr, &features), TAG, "unknown endpoint");
    if (!(features & ZBC_FEAT_ANY_COLOR)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    zbc_cmd_t c = { .transition_ds = transition_ds };
    if (features & ZBC_FEAT_COLOR_XY) {
        /* Straight through, and deliberately not clamped to the sRGB gamut on the
         * way out: a lamp's own gamut is usually wider than sRGB in the reds, and
         * clamping here would throw away a colour it can actually make. */
        c.kind = ZBC_CMD_XY;
        c.a    = x;
        c.b    = y;
    } else {
        float h, s;
        zbc_xy_to_hs((float)x / 65535.0f, (float)y / 65535.0f, &h, &s);
        c.kind = ZBC_CMD_HS;
        c.a    = (uint8_t)lroundf(h * 254.0f / 360.0f);
        c.b    = (uint8_t)lroundf(s * 254.0f / 100.0f);
    }

    esp_err_t err = gate_send(short_addr, ep, &c, ZBC_CH_COLOR);
    if (err == ESP_OK) {
        /* The full feature mask, as zb_set_color_hs and zb_set_color_temp use -
         * not the reduced one the level and on/off paths pass. The point of this
         * read is to see the colour land. And ZBC_WANT_KEEP, because a curve
         * turns one brightness request into a level command and then this one on
         * the same settle slot; -1 here would wipe the level command's intent.
         * The command goes in so the slot's colour intent is this one, even
         * where it is an XY that color_off_target cannot judge: left out, a
         * previous CT intent would stand and be judged against a lamp that has
         * since been told a colour. */
        schedule_settle_read_ex(short_addr, ep, features, transition_ds, ZBC_WANT_KEEP, 0, &c);
    }

    ESP_LOGI(TAG, "Set colour xy %u,%u on 0x%04x ep %d (%s)", x, y, short_addr, ep,
             zb_err_name(err));
    return err;
}

void zb_report_state(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint16_t features,
                    zb_report_state_t *out)
{
    zbc_report_want_t want[ZBC_WANTED_PER_EP];
    const int         n = wanted_reports(features, want);

    /* No lock: both tables are keyed by IEEE, so there is no short address to
     * translate, and each entry is written once and never rewritten - a reader
     * racing a write sees the answer either before or after it arrives. Called
     * from the HTTP task while the mainloop runs. */
    out->count = n < ZB_REPORT_MAX_CLUSTERS ? n : ZB_REPORT_MAX_CLUSTERS;
    for (int i = 0; i < out->count; i++) {
        const uint16_t cl          = want[i].cluster_id;
        out->c[i].cluster          = want[i].name;
        out->c[i].bind_refused     = done_refused_ieee(s_bound, ieee, ep, cl);
        out->c[i].bound            = done_has_ieee(s_bound, ieee, ep, cl) && !out->c[i].bind_refused;
        out->c[i].wants_config     = want[i].configure;
        out->c[i].config_refused   = want[i].configure && done_refused_ieee(s_configured, ieee, ep, cl);
        out->c[i].configured       = want[i].configure && done_has_ieee(s_configured, ieee, ep, cl) &&
                                     !out->c[i].config_refused;
    }
}

esp_err_t zb_remove_device(const uint8_t ieee[ZBC_IEEE_LEN])
{
    zbc_device_t dev;
    if (!device_db_get(ieee, &dev)) {
        return ESP_ERR_NOT_FOUND;
    }
    /* A row restored from another network's backup has no address here, so no
     * leave request can reach it - and it is often a device that will never join
     * this network again. Not device_db_short_by_ieee(), which answers false for
     * such a row and so made it impossible to remove (404 "unknown device"). */
    if (!zbc_short_known(dev.short_addr)) {
        return device_db_remove(ieee);
    }
    const uint16_t short_addr = dev.short_addr;
    /* In the setup portal the stack was never initialised, and the lock below
     * asserts - DELETE /api/devices/{ieee} is reachable there without a key. The
     * leave request cannot go out, but the local removal still can. */
    if (!s_stack_started) {
        return device_db_remove(ieee);
    }

    /* A sleepy remote is not asked to leave. It cannot hear the request while
     * asleep, so the request waits until it next polls - typically right after
     * the same remote has been re-paired, which then leaves the network again a
     * minute later. So a remote is only forgotten here; resetting it takes it off
     * the network, and the page says so. A lamp is always listening and gets the
     * request. Not device_listens(), which asks only for no On/Off and would take
     * a mains device whose endpoint dims without switching for a remote; here
     * every endpoint has to be a switch, or a motion sensor, which sleeps the
     * same way. */
    bool remote = runs_on_a_battery(ieee);
    if (!remote && dev.ep_count > 0) {
        remote = true;
        for (int i = 0; i < dev.ep_count; i++) {
            const uint16_t f = dev.eps[i].features;
            remote &= ep_presses_only(f) || (f & ZBC_FEAT_MOTION);
        }
    }
    if (remote) {
        ESP_LOGI(TAG, "Forgetting remote or sensor 0x%04x without a leave request - see zb_remove_device",
                 short_addr);
        return device_db_remove(ieee);
    }

    ezb_zdo_nwk_mgmt_leave_req_t req = {
        .dst_nwk_addr = short_addr,
        .field = {
            .remove_children = true,
            .rejoin          = false,
        },
    };
    for (int i = 0; i < ZBC_IEEE_LEN; i++) {
        req.field.device_addr.u8[i] = ieee[ZBC_IEEE_LEN - 1 - i];
    }

    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_err_t err = ezb_zdo_nwk_mgmt_leave_req(&req);
    esp_zigbee_lock_release();

    /* Drop it locally either way: the user asked for it gone, and an
     * unreachable device would otherwise be impossible to remove. */
    device_db_remove(ieee);
    return esp_zigbee_err_to_esp(err);
}

esp_err_t zb_apply_onoff_transition(uint16_t ds)
{
    if (!s_stack_started) {
        return ESP_ERR_INVALID_STATE;
    }
    zbc_device_t *devices = calloc(CONFIG_ZBC_MAX_DEVICES, sizeof(zbc_device_t));
    if (!devices) {
        return ESP_ERR_NO_MEM;
    }
    size_t n = device_db_snapshot(devices, CONFIG_ZBC_MAX_DEVICES);

    esp_zigbee_lock_acquire(portMAX_DELAY);
    for (size_t i = 0; i < n; i++) {
        for (int e = 0; e < devices[i].ep_count; e++) {
            if (devices[i].eps[e].features & ZBC_FEAT_LEVEL) {
                write_onoff_transition(devices[i].short_addr, devices[i].eps[e].ep, ds, true);
            }
        }
    }
    esp_zigbee_lock_release();

    free(devices);
    return ESP_OK;
}

void zb_get_forming(zb_forming_t *out)
{
    if (!out) {
        return;
    }
    portENTER_CRITICAL(&s_forming_mux);
    *out = s_forming;
    portEXIT_CRITICAL(&s_forming_mux);
}

void zb_get_radio_stats(zb_radio_stats_t *out)
{
    if (!out) {
        return;
    }
    /* Plain 32-bit reads of counters only ever incremented in the Zigbee task.
     * No lock: a torn count would be a wrong rate, and taking the stack lock
     * here would let a status poll block behind the mainloop for the sake of
     * five numbers that are approximate by nature. */
    out->cmd_undelivered  = s_radio.cmd_undelivered;
    out->read_undelivered = s_radio.read_undelivered;
    out->cmd_slow         = s_radio.cmd_slow;
    out->cmd_slow_ms      = s_radio.cmd_slow_ms;
    out->went_quiet       = s_radio.went_quiet;
    out->corrections      = s_radio.corrections;
    out->gate_timeouts    = s_radio.gate_timeouts;
    out->slots_short      = s_radio.slots_short;
    out->frames_heard     = s_radio.frames_heard;
    out->frames_missed    = s_radio.frames_missed;
    out->table_entries    = s_radio.table_entries;
    out->table_children   = s_radio.table_children;
    out->sends_skipped    = s_radio.sends_skipped;
    out->router_tables_asked      = s_radio.router_tables_asked;
    out->router_tables_answered   = s_radio.router_tables_answered;
    out->router_tables_unanswered = s_radio.router_tables_unanswered;
}

void zb_get_netinfo(zb_netinfo_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->network_up = s_network_up;

    int64_t now = esp_timer_get_time();
    out->permit_join_remaining_s =
        s_permit_deadline_us > now ? (uint32_t)((s_permit_deadline_us - now) / 1000000) : 0;

    if (!s_network_up) {
        return;
    }
    /* From the copy taken at network-up - no lock, and nothing to wait for. */
    out->pan_id       = s_net.pan_id;
    out->ext_pan_id   = s_net.ext_pan_id;
    out->channel      = s_net.channel;
    out->short_addr   = s_net.short_addr;
    out->ed_timeout_s = s_net.ed_timeout_s;
    out->max_children = s_net.max_children;
    memcpy(out->ieee, s_net.ieee, sizeof(out->ieee));
}

esp_err_t zb_factory_reset(void)
{
    /* In the setup hotspot the stack was never initialised (or has been stopped
     * for the fallback AP), and the lock below asserts - see zb_remove_device. */
    if (!s_stack_started) {
        return ESP_ERR_INVALID_STATE;
    }
    device_db_erase();
    esp_zigbee_lock_acquire(portMAX_DELAY);
    esp_zigbee_factory_reset();  /* leaves the network and reboots the chip */
    esp_zigbee_lock_release();
    return ESP_OK;
}

/* --------------------------------------------------------- stack startup */

static esp_err_t create_gateway_endpoint(void)
{
    ezb_af_device_desc_t            dev_desc = ezb_af_create_device_desc();
    ezb_zha_custom_gateway_config_t cfg      = EZB_ZHA_CUSTOM_GATEWAY_CONFIG();
    ezb_af_ep_desc_t                ep_desc  = ezb_zha_create_custom_gateway(ZBC_GATEWAY_EP_ID, &cfg);

    ezb_zcl_cluster_desc_t basic = ezb_af_endpoint_get_cluster_desc(ep_desc, EZB_ZCL_CLUSTER_ID_BASIC,
                                                                   EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                        (void *)ZBC_MANUFACTURER_NAME);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                        (void *)ZBC_MODEL_IDENTIFIER);

    /* A Groups server, so this endpoint can be a member of the groups an IKEA
     * BILRESA sends to - see join_remote_groups. The stock gateway has only Basic
     * and Identify, and the stack delivers a group-cast only to members. */
    ezb_zcl_groups_cluster_server_config_t groups_cfg = { .name_support = 0 };
    ESP_RETURN_ON_ERROR(ezb_af_endpoint_add_cluster_desc(
                            ep_desc, ezb_zcl_groups_create_cluster_desc(&groups_cfg,
                                                                        EZB_ZCL_CLUSTER_SERVER)),
                        TAG, "add groups cluster failed");

    /* The OTA Upgrade server devices ask for firmware. One file at a time: the
     * partition holds one image (see zb_ota.h). */
    ezb_zcl_ota_upgrade_cluster_server_config_t ota_cfg = { .ota_upgrade_file_table_size = 1 };
    ESP_RETURN_ON_ERROR(ezb_af_endpoint_add_cluster_desc(
                            ep_desc, ezb_zcl_ota_upgrade_create_cluster_desc(&ota_cfg, EZB_ZCL_CLUSTER_SERVER)),
                        TAG, "add OTA upgrade cluster failed");

    ESP_RETURN_ON_ERROR(ezb_af_device_add_endpoint_desc(dev_desc, ep_desc), TAG, "add endpoint failed");
    ESP_RETURN_ON_ERROR(ezb_af_device_desc_register(dev_desc), TAG, "register device failed");

    ezb_zcl_core_action_handler_register(zcl_core_action_handler);
    /* The action handler has no callback id for "a cluster-specific command
     * arrived", so a remote's On/Off or Level command would be invisible to it.
     * This sees every frame, and passes all of them on. */
    ezb_zcl_raw_command_handler_register(zcl_raw_frame_handler);
    /* Below that, the BILRESA's group-casts - see aps_group_frame_handler. */
    ezb_apsde_data_indication_handler_register(aps_group_frame_handler);
    return ESP_OK;
}

static void zb_main_task(void *arg)
{
    (void)arg;
    esp_zigbee_config_t config = {
        .device_config = {
            .device_type         = EZB_NWK_DEVICE_TYPE_COORDINATOR,
            .install_code_policy = false,
            .zczr_config         = { .max_children = 10 },
        },
        .platform_config = {
            .storage_partition_name = ZBC_STORAGE_PARTITION,
            .radio_config           = { .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE },
        },
    };

    /* Wi-Fi and 802.15.4 share one radio here, and nothing in ESP-IDF, OpenThread
     * or the Zigbee SDK turns the arbitration on - the application has to. Without
     * this call whichever stack starts first simply keeps the antenna: the mesh
     * forms and Wi-Fi never finds an access point, or Wi-Fi connects and the mesh
     * never forms. Leave the per-event priorities at their defaults; raising
     * 802.15.4 above Wi-Fi stops a SoftAP beaconing altogether. */
    ESP_ERROR_CHECK(esp_coex_wifi_i154_enable());
    /* Before the stack can ask for its first scan - see radio_ed.h. */
    radio_ed_start();

    ESP_ERROR_CHECK(esp_zigbee_init(&config));

    ezb_aps_secur_enable_distributed_security(false);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(1U << formation_channel()));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(0));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(app_signal_handler));
    ESP_ERROR_CHECK(create_gateway_endpoint());

    ESP_ERROR_CHECK(esp_zigbee_start(false));
    /* The radio is up from here to the deinit - see radio_coex.h. */
    radio_coex_start();
    esp_zigbee_launch_mainloop();

    radio_coex_stop();
    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

bool zb_is_running(void)
{
    return s_stack_started;
}

esp_err_t zb_coordinator_stop(void)
{
    /* Called from the Wi-Fi path, which may run before the stack was ever
     * started - the SDK asserts if it is used uninitialised. */
    if (!s_stack_started || !esp_zigbee_is_started()) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Stopping the Zigbee stack to free the radio for Wi-Fi");
    s_network_up    = false;
    s_stack_started = false;
    /* The timers would otherwise outlive the stack: esp_zigbee_stop() ends the
     * mainloop and zb_main_task then calls esp_zigbee_deinit(), while the
     * settle and refresh timers would still make zb_work_task call into a stack
     * that no longer exists - e.g. when the fallback AP stops Zigbee two minutes
     * after boot with a neighbour scan pending. */
    esp_timer_stop(s_settle_timer);
    esp_timer_stop(s_refresh_timer);
    esp_timer_stop(s_retry_timer);
    esp_timer_stop(s_announce_timer);
    esp_timer_stop(s_ota_recheck_timer);
    esp_timer_stop(s_channel_timer);
    esp_timer_stop(s_survey_timer);
    /* The stack's file table goes with it; the mapping stays with zb_ota. */
    s_ota_file = NULL;
    zb_ota_set_offered(false);
    return esp_zigbee_stop();
}

esp_err_t zb_coordinator_start(void)
{
    const esp_timer_create_args_t retry_args = {
        .callback = commissioning_retry_cb,
        .name     = "zb_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&retry_args, &s_retry_timer));

    const esp_timer_create_args_t settle_args = {
        .callback = settle_timer_cb,
        .name     = "zb_settle",
    };
    ESP_ERROR_CHECK(esp_timer_create(&settle_args, &s_settle_timer));

    const esp_timer_create_args_t refresh_args = {
        .callback = refresh_timer_cb,
        .name     = "zb_refresh",
    };
    ESP_ERROR_CHECK(esp_timer_create(&refresh_args, &s_refresh_timer));

    const esp_timer_create_args_t announce_args = {
        .callback = announce_timer_cb,
        .name     = "zb_announce",
    };
    ESP_ERROR_CHECK(esp_timer_create(&announce_args, &s_announce_timer));

    const esp_timer_create_args_t ota_recheck_args = {
        .callback = ota_recheck_timer_cb,
        .name     = "zb_ota_recheck",
    };
    ESP_ERROR_CHECK(esp_timer_create(&ota_recheck_args, &s_ota_recheck_timer));

    const esp_timer_create_args_t channel_args = {
        .callback = channel_timer_cb,
        .name     = "zb_channel",
    };
    ESP_ERROR_CHECK(esp_timer_create(&channel_args, &s_channel_timer));

    const esp_timer_create_args_t survey_args = {
        .callback = survey_timer_cb,
        .name     = "zb_survey",
    };
    ESP_ERROR_CHECK(esp_timer_create(&survey_args, &s_survey_timer));

    /* Started before the timers can ring: a notification to a task that does not
     * exist yet is simply dropped, and the first refresh is armed five seconds
     * in. Priority below the Zigbee mainloop, since everything it does waits on
     * that task's lock anyway. */
    if (xTaskCreate(zb_work_task, "zb_work", 4096, NULL, 4, &s_work_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(zb_main_task, "zigbee_main", 6144, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_stack_started = true;
    return ESP_OK;
}
