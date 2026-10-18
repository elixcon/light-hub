/* Zigbee coordinator: forms the network, interrogates devices that join, and
 * turns them on and off.
 *
 * Everything here may be called from any task; the implementation takes the
 * Zigbee stack lock before touching the stack, as the SDK requires. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"
#include "device_db.h"
#include "remote.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZB_ONOFF_OFF = 0,
    ZB_ONOFF_ON,
    ZB_ONOFF_TOGGLE,
} zb_onoff_cmd_t;

typedef struct {
    bool     network_up;
    uint16_t pan_id;
    /* With pan_id, what tells this network from one formed after a reset:
     * config_backup.c restores device addresses only onto the network they
     * were recorded on. */
    uint64_t ext_pan_id;
    uint8_t  channel;
    uint16_t short_addr;
    uint8_t  ieee[8];
    uint32_t permit_join_remaining_s;
    /* How the stack treats children, read at network-up: the end-device
     * timeout it applies where a child has not asked for one of its own, and
     * how many children it takes before it stops offering itself as a parent. */
    uint32_t ed_timeout_s;
    uint8_t  max_children;
} zb_netinfo_t;

/* Running totals of the things that look like software faults and are usually
 * the radio. Never reset, so the useful reading is a rate: sample twice and
 * divide by the uptime between, e.g. to judge a radio setting. Exposed at
 * `GET /api/status` because opening the serial port resets the board. */
typedef struct {
    uint32_t cmd_undelivered;   /* the stack gave up getting a command to a lamp */
    uint32_t read_undelivered;  /* ...or a read */
    /* Commands that did get there, but took over half a second to: normally a
     * frame is done in 24-130 ms, and one that needed the stack's own retries
     * takes a second or more - which every later frame to that lamp waits out.
     * `cmd_slow_ms` is their time added up, so a difference over one press says
     * how slow as well as how many. */
    uint32_t cmd_slow;
    uint32_t cmd_slow_ms;
    uint32_t went_quiet;        /* a lamp stopped answering; its state is unknown */
    uint32_t corrections;       /* a lamp was somewhere it had not been asked to be */
    uint32_t gate_timeouts;     /* a delivery confirmation never arrived at all */
    uint32_t slots_short;       /* more endpoints commanded at once than there are slots */
    /* Frames devices sent on their own - reports, a remote's commands - counted
     * from the ZCL sequence numbers they carry: heard, and numbered but never
     * heard. The only measure there is of the *receiving* side of the radio,
     * which is the side a remote's press depends on. See zb_link_info_t. */
    uint32_t frames_heard;
    uint32_t frames_missed;
    /* The coordinator's own neighbour table at its last walk (note_children):
     * how many entries, and how many of them are this coordinator's children.
     * A remote that is nobody's child here is a child of a router, and its
     * presses cross that router's link to us as well as its own. */
    uint32_t table_entries;
    uint32_t table_children;
    /* Commands and reads not put on air because the device has stopped
     * answering - see ZB_ERR_UNREACHABLE. Airtime given back to everything
     * else, a remote's presses first among it. */
    uint32_t sends_skipped;
    /* The routers' neighbour tables the parent walk asks for (Mgmt_Lqi, one
     * request per page): asked, answered (a refusal included), and ended by
     * the stack with no answer. Asked minus the other two is what is still in
     * flight - or requests the stack never called back on at all. */
    uint32_t router_tables_asked;
    uint32_t router_tables_answered;
    uint32_t router_tables_unanswered;
} zb_radio_stats_t;

/* What a command to a device that has stopped answering returns instead of
 * being sent: an unplugged lamp. Its last few frames went undelivered and
 * nothing has been heard from it since, so it is sent a command at most once
 * every ZBC_UNREACH_PROBE_US - which is what finds it again when it comes back,
 * along with anything heard from it - and is not read at all meanwhile.
 *
 * A code of our own, above every ESP-IDF range (those all fit in 16 bits):
 * a borrowed one such as ESP_ERR_NOT_FINISHED is also what
 * esp_zigbee_err_to_esp() may make of a stack refusal, which would then be
 * answered as "not answering" and skipped quietly by lighting.c's colour
 * paths. esp_err_to_name() calls it UNKNOWN ERROR, so zb_coordinator.c's own
 * log lines name it through zb_err_name(). */
#define ZB_ERR_UNREACHABLE ((esp_err_t)0x5A0001)

/* How getting a Hue dimmer onto Philips' button cluster is going: the step
 * nothing on air shows. Runtime only. */
typedef struct {
    uint32_t bind_asked;      /* bind requests put out */
    int32_t  bind_status;     /* last answer: ZDP status, -1 none yet, -2 none came back */
    uint32_t aps_frames;      /* 0xFC00 frames from a dimmer seen at APS */
    uint32_t zcl_frames;      /* 0xFC00 frames from a dimmer that reached decoding, by either path */
} zb_hue_stats_t;
void zb_get_hue_stats(zb_hue_stats_t *out);

void zb_get_radio_stats(zb_radio_stats_t *out);

/* What the stack has said since boot, while there is no network: forming one
 * is silent for minutes, sometimes indefinitely, and the serial port that would
 * show it restarts the board and the formation with it. A ring of the last
 * stack signals and commissioning starts, oldest first, plus the BDB state as
 * last sampled under the stack lock. */
#define ZB_FORMING_NOTES 12

typedef struct {
    int64_t     at_us;
    const char *what;   /* the signal's name, or "start" for a commissioning start */
    int         code;   /* BDB status, or the start's return code; -1 for none */
    uint8_t     mode;   /* commissioning mode mask of a start; 0 for a signal */
} zb_forming_note_t;

typedef struct {
    uint32_t          starts;          /* commissioning starts, every mode */
    uint32_t          signals;         /* stack signals, every type */
    int               bdb_status;      /* ezb_bdb_get_commissioning_status(), -1 until sampled */
    uint8_t           bdb_mode;
    int64_t           sampled_us;
    uint8_t           count;
    zb_forming_note_t notes[ZB_FORMING_NOTES];
} zb_forming_t;

void zb_get_forming(zb_forming_t *out);

/* What the radio has heard from one device. Runtime only, and from received
 * frames rather than from the neighbour table: a sleepy remote is never in that
 * table, so this is the only link reading it ever gets. `rssi` is the last hop's
 * - a remote whose parent is a lamp is measured from the lamp. `missed` counts
 * ZCL sequence numbers this device used that never arrived, so it is a lost
 * frame count with no serial port; see link_gap() for its limits. `child` says
 * whether the device is this coordinator's own child (1), somebody else's or a
 * router (0), or not yet looked (-1). */
typedef struct {
    bool     heard;      /* a frame from it since boot; `rssi` means nothing until then */
    int8_t   rssi;
    int8_t   child;
    uint32_t frames;
    uint32_t missed;
    /* Whether it has sent to a group. Such a frame reaches us as its parent's
     * one unacknowledged broadcast unless we are the parent, so for this
     * device the parent decides how many presses arrive. */
    bool     groupcast;
    /* The device it hangs off, for an end device: this coordinator
     * (`parent_is_us`), a router, or not known (`parent_known` false). From our
     * own neighbour table and each router's, looked at every five minutes - see
     * zbc_parent_t. `parent_src` says which ("table" or "router"),
     * `parent_lqi` is the parent's reading of the link (-1 unknown), and
     * `parent_us` when the parent last changed. */
    bool        parent_known;
    bool        parent_is_us;
    uint16_t    parent_short;
    bool        parent_ieee_known;
    uint8_t     parent_ieee[ZBC_IEEE_LEN];
    const char *parent_src;
    int16_t     parent_lqi;
    int64_t     parent_us;
    /* For our own child only: the end-device timeout our table keeps it by, in
     * seconds (0 when not our child), and how many of them are left - the table
     * counts down, and a child that runs out is dropped, so its next poll sends
     * it looking for another parent. */
    uint32_t timeout_s;
    uint32_t timeout_left_s;
} zb_link_info_t;

/* False when nothing has been heard from it since boot. */
bool zb_device_link(const uint8_t ieee[ZBC_IEEE_LEN], zb_link_info_t *out);

/* The last changes of parent, oldest first: from our own table walk and the
 * routers' tables (`what` "table": now our child; "router": now that router's;
 * "gone": no longer in our table), and from the stack's signals as they happen
 * ("joined", "rejoined", "rejoined_tc", "left" through a parent - the Update
 * Device a router sends the trust centre; "leaving", "leaving_to_rejoin" from
 * a device of ours). `parent` and `was` are short addresses, 0x0000 this
 * coordinator and ZB_PARENT_UNKNOWN none. Runtime only; the one record of when
 * a remote moved and which way. */
#define ZB_PARENT_LOG_LEN  12
#define ZB_PARENT_UNKNOWN  0xFFFF

typedef struct {
    int64_t     at_us;
    const char *what;
    uint8_t     ieee[ZBC_IEEE_LEN];
    bool        have_ieee;
    uint16_t    short_addr;
    uint16_t    parent;
    uint16_t    was;
} zb_parent_note_t;

size_t zb_parent_changes(zb_parent_note_t *out, size_t max);

/* ------------------------------------------------------------------- remotes */

/* A press decoded from a paired remote. Registered by buttons.c at start-up
 * rather than called directly, because buttons.c already calls into here and a
 * call back the other way would close the loop - the same reason curve_db.h is
 * kept out of zb_coordinator.c. Shaped like the SDK's own handler registration.
 * The callback runs in the Zigbee task with the stack lock held, so it must not
 * take the lock, block, or send anything. */
typedef void (*zb_remote_cb_t)(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint8_t key,
                               zb_remote_ev_t ev);
void zb_set_remote_handler(zb_remote_cb_t cb);

/* The last few presses from any remote, newest first, for the web UI's "press
 * the button you want" capture. A remote cannot be configured from its cluster
 * list - an Aqara switch advertises no client clusters at all, and a rocker's
 * two halves are one endpoint - so which key somebody means is settled by them
 * pressing it. `seq` counts presses since boot and never repeats, which is how
 * a client tells a new press from the same one polled twice. */
#define ZBC_REMOTE_LOG_LEN 8

typedef struct {
    uint32_t seq;
    uint8_t  ieee[ZBC_IEEE_LEN];
    uint8_t  ep;
    uint8_t  key;
    uint8_t  ev;          /* zb_remote_ev_t */
    int64_t  at_us;
    /* The frame it decoded from, so a duplicate (same tsn) can be told from a
     * second press, and a group-cast from a bound unicast. cluster 0xFFFF when
     * it came from an attribute report rather than a command; group 0xFFFF when
     * it was sent to us rather than to a group. */
    uint16_t cluster;
    uint16_t group;
    uint8_t  cmd;
    uint8_t  tsn;
    uint8_t  payload_len;     /* the whole length; only the first three are kept */
    uint8_t  payload[3];
} zb_remote_log_t;

/* Copies out up to `max` entries, newest first, and returns how many. */
size_t zb_recent_remote_events(zb_remote_log_t *out, size_t max);

/* Every decoded command frame from a remote, Stops included - the ring above
 * keeps only presses and holds, eight of them, which cannot show what a whole
 * turn of a dial looked like. Diagnostic only; nothing acts on it. */
#define ZBC_REMOTE_FRAMES_LEN 64

typedef struct {
    int64_t  at_us;
    uint16_t short_addr;
    uint16_t cluster;
    uint8_t  ep;
    uint8_t  cmd;
    uint8_t  tsn;
    uint8_t  payload_len;
    uint8_t  payload[4];
} zb_remote_frame_t;

/* Copies out up to `max` frames, oldest first, and returns how many. */
size_t zb_recent_remote_frames(zb_remote_frame_t *out, size_t max);

esp_err_t zb_coordinator_start(void);

/* Stops the stack and frees the radio. Used when the coordinator has to fall
 * back to the provisioning SoftAP: Wi-Fi and 802.15.4 share one antenna, and an
 * always-listening coordinator keeps clients from associating with the AP. */
esp_err_t zb_coordinator_stop(void);

/* True once the stack is up. Anything that sends a command from outside the web
 * UI has to ask first: calling into the Zigbee lib before esp_zigbee_init()
 * asserts and panics, and the short address of a stored device survives a
 * reboot, so the device lookup that would otherwise stop the call succeeds even
 * in provisioning-AP mode. */
bool      zb_is_running(void);

esp_err_t zb_permit_join(uint8_t seconds);

/* The OTA Upgrade server's image (see zb_ota.h), which the stack reads through
 * a flash mapping. Attach after an image is stored - network-up attaches too,
 * and ESP_ERR_INVALID_STATE means it will. Detach before anything touches the
 * partition: from then on the stack must not read the mapping. */
esp_err_t zb_ota_server_attach(void);
void      zb_ota_server_detach(void);

/* Image Notify: to one device, or with NULL to every device that listens. It
 * only asks the device to ask; what it is then told is zb_ota_admit's call. */
esp_err_t zb_ota_server_notify(const uint8_t *ieee);

/* Makes a device say what firmware it runs, without offering it anything: a
 * read of its OTA client attributes when its receiver is on, and an Image
 * Notify, which makes it send a Query Next Image. Its answers land in zb_ota's
 * identity table. Also run by itself after a device finishes a download. */
esp_err_t zb_ota_server_ask(const uint8_t ieee[ZBC_IEEE_LEN]);

/* Level, colour temperature and colour are all "move to X" commands: the lamp
 * ramps over transition_ds tenths of a second and reports the result. */
esp_err_t zb_set_level(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint8_t level, uint16_t transition_ds);
esp_err_t zb_set_color_temp(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint16_t mireds, uint16_t transition_ds);
/* Hue and saturation as ZCL 0-254. Lamps that only speak CIE xy get the same
 * colour converted for them, so callers never have to care which kind it is. */
/* CIE xy as ZCL fractions of 65536 - what a blend between a colour and a colour
 * temperature comes out as. Lamps that only speak hue/saturation get it
 * converted for them; the mirror of zb_set_color_hs, which converts the other
 * way for lamps that only speak xy. */
esp_err_t zb_set_color_xy(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint16_t x, uint16_t y,
                          uint16_t transition_ds);
esp_err_t zb_set_color_hs(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint8_t hue, uint8_t saturation,
                          uint16_t transition_ds);
esp_err_t zb_set_onoff(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, zb_onoff_cmd_t cmd);
/* Fades the lamp out and switches it off, as one command the lamp carries out
 * itself - see the note in zb_coordinator.c for why two commands will not do. */
esp_err_t zb_fade_off(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep);
/* Set the lamp's colour temperature and then switch it off, as one command: the
 * colour reaches it while it is still lit and will act on it, and the off cannot
 * overtake the colour on the way. `plain_off` for a bare Off rather than
 * OffWithEffect. See lighting.c's off_endpoint for why a lamp is warmed at all. */
esp_err_t zb_warm_off(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint16_t mireds,
                      bool plain_off);

/* Fades a lamp that is off up to `level`.
 *
 * Two frames, because one will not do: a lamp switched off with OffWithEffect
 * keeps its old CurrentLevel, so a move to the target has nowhere to travel and
 * the lamp snaps to full - and a lamp that is off ignores a plain Move to Level,
 * so there is no way to reposition it while dark. It is therefore switched on at
 * the bottom of its range first, then ramped.
 *
 * This is one call rather than two because the pair has to stay one command as
 * far as the ordering gate is concerned; issued separately, the ramp would
 * supersede the preset it depends on. The second frame goes out when the first
 * is confirmed, so it can no longer overtake it either. */
esp_err_t zb_fade_in(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint8_t level,
                     uint16_t transition_ds);

/* Ends any level transition in progress, leaving the lamp where it has got to.
 * A held button is a stream of ramps; without this the lamp keeps travelling
 * towards the last one long after the button was released. */
esp_err_t zb_level_stop(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep);

#if CONFIG_ZBC_WLED
/* Philips' entertainment frames (cluster 0xFC01): up to ZB_ENT_MAX_LAMPS Hue
 * colour lamps moved together many times a second, one frame for all of them,
 * outside the command gate. One stream at a time; lighting.c runs it. Only with
 * CONFIG_ZBC_WLED, whose interface is the one source. */
#define ZB_ENT_MAX_LAMPS 10

typedef struct {
    uint8_t ieee[ZBC_IEEE_LEN];
    uint8_t ep;
} zb_ent_lamp_t;

typedef struct {
    uint16_t bri;   /* 0-2047 */
    float    x, y;  /* CIE 1931 */
} zb_ent_light_t;

typedef struct {
    bool     active;
    uint8_t  lamps;
    uint16_t proxy;            /* short address every frame goes to; it repeats it */
    uint32_t streams;          /* begun since boot */
    uint32_t counter;          /* the frame counter, carried across streams */
    uint32_t frames_sent;      /* accepted by the stack */
    uint32_t frames_refused;   /* refused by the stack; never retried */
    int32_t  last_refusal;     /* the stack's error code for the last one */
    uint32_t sync_sent;
    uint32_t sync_delivered;
    uint32_t sync_undelivered;
    int32_t  balance_status;   /* the last balance write's ZCL status; -1 none */
    int32_t  rejected_status;  /* a Default Response on 0xFC01 this stream; -1 none */
    uint8_t  rejected_cmd;
    uint32_t answers_heard;    /* a lamp's answers on 0xFC01, seen at APS */
    uint32_t relay_heard;      /* a lamp's repeat of a frame reaching APS (none so far) */
    uint32_t kept_fresh;       /* times the first lamp's neighbour entry was set back to age 0 */
} zb_ent_stats_t;

/* Makes the lamps ready: the balance write and a sync to each, the first lamp
 * the one every frame goes to. ESP_ERR_INVALID_STATE while a stream runs or the
 * network is down, ESP_ERR_NOT_FOUND for a lamp not in the registry. */
esp_err_t zb_ent_begin(const zb_ent_lamp_t *lamps, uint8_t n);
/* One frame, a light per lamp in begin's order, the lamps gliding to it over
 * `glide_ms` (at most about 2.5 s). Never retried. */
esp_err_t zb_ent_frame(const zb_ent_light_t *lights, uint16_t glide_ms);
/* `sync`: tell the lamps, which then hold the last frame. Without it they leave
 * the stream by themselves 2-10 s after the last frame. */
void      zb_ent_end(bool sync);
void      zb_get_ent_stats(zb_ent_stats_t *out);
#endif

/* What an endpoint needs before it will volunteer anything, and whether it did.
 *
 * A device reports attributes only once it has a ZDO binding and has accepted a
 * Configure Reporting - two separate answers, either of which can be lost or
 * refused. A lamp that answered neither is only ever known when asked, which is
 * a different fault from a bad link and wants a different fix (re-pairing
 * rather than a channel change). `bound` and `configured` say the device
 * accepted; `bind_refused` and `config_refused` that it answered no, which is
 * final for this boot - neither of a pair set means no answer yet. The
 * configured pair is absent for a cluster that is only bound - a remote's
 * On/Off, where the binding is what makes it speak and there is no attribute to
 * report. */
#define ZB_REPORT_MAX_CLUSTERS 3

typedef struct {
    int count;
    struct {
        const char *cluster;      /* "onoff", "level", "switch" */
        bool        bound;
        bool        bind_refused;
        bool        configured;
        bool        config_refused;
        bool        wants_config;
    } c[ZB_REPORT_MAX_CLUSTERS];
} zb_report_state_t;

void zb_report_state(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint16_t features,
                     zb_report_state_t *out);

/* Writes OnOffTransitionTime to every dimmable endpoint we know about. Plain
 * On/Off commands carry no transition of their own, so this is what decides
 * whether switching a lamp fades or snaps. */
esp_err_t zb_apply_onoff_transition(uint16_t ds);
esp_err_t zb_remove_device(const uint8_t ieee[ZBC_IEEE_LEN]);
void      zb_get_netinfo(zb_netinfo_t *out);

/* Moves the whole network to another channel, keeping every pairing: a
 * Mgmt_NWK_Update_req broadcast tells the routers, and the coordinator follows
 * once they have had time to pass it on. Returns at once; the move takes about
 * ten seconds (zb_get_channel_move). The channel is stored first and put back at
 * every boot - see settings_zigbee_channel.
 *
 * Sleepy remotes do not hear broadcasts: they find the network again when a poll
 * goes unanswered, usually at their next press. A lamp without power now stays
 * on the old channel.
 *
 * ESP_ERR_INVALID_ARG outside 11-26; ESP_ERR_INVALID_STATE while the network is
 * not up, or a move or survey is in progress. The channel in use is ESP_OK with
 * nothing sent. */
esp_err_t zb_move_channel(uint8_t channel);

typedef enum {
    ZB_MOVE_IDLE = 0,
    ZB_MOVE_ANNOUNCED,   /* broadcast sent; the coordinator waits for it to spread */
    ZB_MOVE_MOVING,      /* the coordinator has asked itself to change */
    ZB_MOVE_MOVED,
    ZB_MOVE_FAILED,      /* the coordinator did not change; the lamps may have */
} zb_move_state_t;

typedef struct {
    zb_move_state_t state;
    uint8_t         from;
    uint8_t         to;
    bool            boot;    /* put back at boot, not a move asked for */
    int64_t         at_us;   /* the last change of state */
} zb_channel_move_t;

void zb_get_channel_move(zb_channel_move_t *out);

/* Measures channels 11-26 from the coordinator's own antenna: four energy
 * scans and one beacon scan for other Zigbee networks, about 12 s in all. The
 * coordinator is off its own channel for 1.2 s per scan, and nothing sent to it
 * then is heard; Wi-Fi loses the antenna for the energy scans. Returns at once;
 * zb_get_channel_survey has the answer.
 * ESP_ERR_INVALID_STATE while the network is not up, or a move or survey is in
 * progress. */
esp_err_t zb_channel_survey_start(void);

#define ZB_SURVEY_CHANNELS 16   /* 11-26 */
#define ZB_SURVEY_PASSES   4    /* energy scans per survey */
#define ZB_SURVEY_NETWORKS 8

typedef enum {
    ZB_SURVEY_IDLE = 0,
    ZB_SURVEY_RUNNING,
    ZB_SURVEY_DONE,
    ZB_SURVEY_FAILED,
} zb_survey_state_t;

typedef struct {
    zb_survey_state_t state;
    const char       *error;      /* why it failed, or NULL */
    uint8_t           passes;     /* energy scans that finished */
    int64_t           at_us;      /* started, or finished once done or failed */
    struct {
        uint8_t samples;                  /* passes that measured this channel */
        int8_t  dbm[ZB_SURVEY_PASSES];    /* each one's reading, in that order */
    } ch[ZB_SURVEY_CHANNELS];             /* index 0 is channel 11 */
    uint8_t networks;
    struct {
        uint8_t  channel;
        uint16_t pan_id;
        uint64_t ext_pan_id;
        bool     permit_join;
    } net[ZB_SURVEY_NETWORKS];    /* other PANs; ours is left out */
} zb_survey_t;

void zb_get_channel_survey(zb_survey_t *out);

/* Leaves the Zigbee network and clears the stack's own NVS storage. */
esp_err_t zb_factory_reset(void);

#ifdef __cplusplus
}
#endif
