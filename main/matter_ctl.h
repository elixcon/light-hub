/* Matter lamps: the bridge is also a controller.
 *
 * The board pairs external Matter lamps - on Wi-Fi, on Thread behind somebody's
 * border router, or behind a hub such as a Hue Bridge - from a code another app
 * gives out ("Turn on pairing mode" in Apple Home), and switches them. Each lamp
 * endpoint is a row in mlight_db and a made-up address on endpoint 1, so
 * lighting.c routes one zb_* call to one mctl_* call and everything above it -
 * groups, curves, buttons, fades, warm-off, Adaptive Lighting, the bridge - takes
 * a Matter lamp unchanged.
 *
 * All of it runs on the Matter server's own stack: our fabric is one more entry
 * in the Server's FabricTable, PASE and CASE run on its SessionManager, and the
 * subscriptions are its InteractionModelEngine's. esp_matter's commissioner is
 * compiled out while the server is on, and a second CHIP controller stack would
 * re-initialise the IM engine under the bridge - see matter_fabric.cpp.
 *
 * Every function here may be called from any task and returns at once: a call
 * writes the intent into the lamp's gate under a spinlock and wakes the CHIP task,
 * which does the sending. Nothing here takes the Zigbee lock, and nothing here
 * waits on the CHIP lock except mctl_forget_all(). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "zb_coordinator.h"   /* zb_onoff_cmd_t, ZB_ERR_UNREACHABLE, and device_db.h's ZBC_UNKNOWN */

#ifdef __cplusplus
extern "C" {
#endif

/* After matter_bridge_start() has succeeded, and never in provisioning-AP mode:
 * finds our fabric, if there is one yet, and subscribes to every stored device. */
esp_err_t mctl_start(void);
bool      mctl_running(void);

/* Re-reads mlight_db after rows were added or removed outside this module (the
 * device routes), and starts or ends subscriptions to match. */
void      mctl_reload(void);

/* The zb_* calls, one to one, by mlight id. Each answers ESP_OK when the command
 * is on its way (the API's 202), ZB_ERR_UNREACHABLE when the lamp's device has
 * been out of reach for more than half a minute (503; lighting.c records nothing),
 * ESP_ERR_NOT_FOUND for an unknown lamp, ESP_ERR_NOT_SUPPORTED for a command the
 * lamp has no cluster for, and ESP_ERR_INVALID_STATE while Matter is not running
 * (409). A device that is still connecting - at boot, or just after it dropped -
 * is waited for, so a press in the first seconds after a restart is not refused. */
esp_err_t mctl_set_onoff(uint16_t id, zb_onoff_cmd_t cmd);
esp_err_t mctl_set_level(uint16_t id, uint8_t level, uint16_t transition_ds);
esp_err_t mctl_fade_in(uint16_t id, uint8_t level, uint16_t transition_ds);
esp_err_t mctl_fade_off(uint16_t id);
esp_err_t mctl_warm_off(uint16_t id, uint16_t mireds, bool plain_off);
esp_err_t mctl_set_ct(uint16_t id, uint16_t mireds, uint16_t transition_ds);
esp_err_t mctl_set_hs(uint16_t id, uint8_t hue, uint8_t saturation, uint16_t transition_ds);
esp_err_t mctl_set_xy(uint16_t id, uint16_t x, uint16_t y, uint16_t transition_ds);
esp_err_t mctl_level_stop(uint16_t id);

/* What the lamp last reported through its subscription, in the units a Zigbee
 * lamp reports (level 0-254, mireds, hue and saturation 0-254, colour mode 0/1/2),
 * ZBC_UNKNOWN until it has. `ep`, `features` and the mired limits are left to the
 * caller, which has the row. False for an unknown lamp. */
typedef struct {
    int8_t  on_off;
    int16_t level;
    int32_t ct_mireds;
    int16_t hue;
    int16_t saturation;
    int8_t  color_mode;
    bool    online;           /* its device's subscription is up */
    int64_t last_seen_us;     /* the last report or keep-alive, 0 never */
} mctl_state_t;
bool      mctl_get(uint16_t id, mctl_state_t *out);

/* Whether this box has a fabric of its own at all: without one, every stored lamp
 * needs pairing again (a Matter reset erased it). */
bool      mctl_has_fabric(void);
/* Our fabric's index in the Server's FabricTable, 0 when there is none - so the
 * bridge's fabric list can tell it from a controller's. */
uint8_t   mctl_fabric_index(void);

/* Why our fabric could not be made - the first pairing makes it: the box's Matter
 * fabric table is full (a controller has to go), or a controller is adding the box
 * at this moment and the table's pending state is theirs. Codes of our own, above
 * every ESP-IDF range, as ZB_ERR_UNREACHABLE is. */
#define MCTL_ERR_NO_FABRIC_SLOT ((esp_err_t)0x5A0101)
#define MCTL_ERR_BRIDGE_BUSY    ((esp_err_t)0x5A0102)

/* Pairing: `code` is the 11- or 21-digit manual code or the "MT:" QR payload of a
 * device in pairing mode. ESP_OK when a pairing has started (202), ESP_ERR_INVALID_ARG
 * for a code that does not parse (400), ESP_ERR_INVALID_STATE when one is already
 * running or Matter is not (409), and the two above (409). Our fabric is made here,
 * the first time, before the device is looked for. Progress is in mctl_get_status. */
esp_err_t mctl_pair(const char *code);

/* Reads a device's endpoints again - a lamp added to a hub after it was paired
 * appears only this way. ESP_ERR_NOT_FOUND for a node with no rows. */
esp_err_t mctl_rescan_node(uint64_t node_id);

/* Tells a device to forget our fabric (RemoveFabric), best effort, and stops
 * talking to it. For after its last row was removed from mlight_db. */
esp_err_t mctl_remove_node(uint64_t node_id, uint8_t lamp_fabric);

/* RemoveFabric on every device, for a Matter or factory reset that is about to
 * erase our fabric. Waits at most `timeout_ms` for the answers; a lamp that misses
 * it keeps a stale entry in its own fabric table. */
void      mctl_forget_all(uint32_t timeout_ms);

typedef enum {
    MCTL_PAIR_IDLE = 0,
    MCTL_PAIR_RUNNING,
    MCTL_PAIR_DONE,
    MCTL_PAIR_FAILED,
} mctl_pair_state_t;

#define MCTL_STATUS_NODES CONFIG_ZBC_MAX_MATTER_NODES

typedef struct {
    uint64_t node_id;
    uint8_t  lamps;           /* rows in mlight_db */
    bool     online;
    bool     subscribed;      /* a subscription exists, up or retrying */
    uint32_t drops;           /* times the subscription went down since boot */
    uint32_t reports;         /* reports received since boot */
    int64_t  last_seen_us;
    int64_t  down_since_us;   /* 0 while up */
    uint32_t last_error;      /* CHIP_ERROR of the last failure, 0 none */
    uint32_t heap_low_up;     /* the boot-wide lowest free heap when it last came up */
} mctl_node_status_t;

typedef struct {
    bool     running;
    uint8_t  fabric_index;    /* 0 none yet */
    char     fabric_error[64]; /* why making it last failed, "" otherwise */
    uint64_t own_node_id;
    uint8_t  pair_state;      /* mctl_pair_state_t */
    char     pair_step[16];   /* what the running pairing is doing */
    char     pair_error[96];  /* why the last one failed, "" otherwise */
    uint64_t pair_node_id;
    uint8_t  pair_lamps;      /* lamps the last successful pairing added */
    uint32_t pair_ago_s;      /* since the last pairing started */
    /* The boot-wide lowest free heap as each step of the last pairing ended,
     * by step (MCTL_PAIR_STEPS, the order of `pair_step` names; 0 not reached).
     * It only ever falls, so the step it falls in is the one that cost it. */
#define MCTL_PAIR_STEPS 12
    uint32_t pair_heap_low[MCTL_PAIR_STEPS];
    const char *pair_step_names[MCTL_PAIR_STEPS];
    uint8_t  nodes;
    mctl_node_status_t node[MCTL_STATUS_NODES];
    uint32_t commands_sent;
    uint32_t commands_failed;
    uint32_t commands_dropped; /* waited past the stale limit for a session */
    uint32_t gate_timeouts;
    /* The CHIP task's stack at its fullest since boot, as bytes never used: the
     * controller's callbacks all run on it, at the bottom of CHIP's own. */
    uint32_t chip_stack_unused;
} mctl_status_t;

void      mctl_get_status(mctl_status_t *out);

#ifdef __cplusplus
}
#endif
