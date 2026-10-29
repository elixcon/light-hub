/* Buttons and what they do, persisted to NVS.
 *
 * A button is a source, and four gestures - short press, long press, double
 * press, triple press - each carrying a short ordered list of rules. Nothing
 * makes a triple out of separate presses: it exists for a remote that counts
 * its own, and a pin never sends one. The source is either a
 * GPIO pin and how it is wired, or one key of one endpoint of a paired Zigbee
 * remote; everything downstream of the gesture is the same either way, which is
 * the whole reason a remote is a button here rather than a feature of its own.
 *
 * A rule is a condition on its target's current state and an action to take on
 * that target, which is a room or one light. The first rule whose condition
 * holds wins, so the last rule with ZBC_COND_ALWAYS reads as "otherwise". That
 * is what lets one button say "80 % when the room is below 30 %, otherwise 30 %"
 * without a rules engine. A rule marked ZBC_RULE_AND does not compete: it runs
 * with the rule above it, whose condition decided for both - "turn the kitchen
 * on, and the reading lamp to 30 %". A rule and the ones joined to it are a
 * case; the first case whose head holds runs whole.
 *
 * Same shape as device_db, group_db and curve_db: fixed static table, own
 * recursive mutex, hands out copies, own NVS blob with its own version. Its own
 * blob is the point - adding buttons must not bump DB_BLOB_VERSION and throw
 * away the names the user gave their lights.
 *
 * Unlike them it does not keep the records it hands out. A copy carries room
 * for CONFIG_ZBC_BUTTON_RULES rules, while the table keeps each button's rules
 * in one pool all buttons share (CONFIG_ZBC_BUTTON_POOL), so a remote key with
 * one rule costs one rule and not twelve.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* >>> host-test region: bdb-types <<< */

#define ZBC_BUTTON_NAME_LEN 32

/* Which internal resistor the pin gets. A button wired to GND wants a pull-up
 * and reads inverted; one wired to 3V3 wants a pull-down and does not. */
/* Where a button's presses come from. GPIO is first so a stored record from
 * before remotes existed converts to it by zeroing.
 *
 * A motion sensor is a button too: one endpoint of a paired Zigbee sensor, whose
 * two events - motion, and no motion for clear_s - stand where a remote's
 * gestures do, with the same rules behind them. */
typedef enum {
    ZBC_BTN_SRC_GPIO = 0,
    ZBC_BTN_SRC_ZIGBEE,
    ZBC_BTN_SRC_SENSOR,
    ZBC_BTN_SRC_COUNT,
} zbc_btn_src_t;

/* Whether the source is an endpoint of a paired Zigbee device - a remote's key
 * or a sensor - named by ieee and zb_ep rather than by a pin. */
static inline bool zbc_btn_src_is_device(uint8_t src)
{
    return src == ZBC_BTN_SRC_ZIGBEE || src == ZBC_BTN_SRC_SENSOR;
}

typedef enum {
    ZBC_PULL_NONE = 0,
    ZBC_PULL_UP,
    ZBC_PULL_DOWN,
} zbc_btn_pull_t;

/* What is on the pin. A push button springs back, so presses, holds and double
 * presses mean something. A switch stays where it is put, so every flip - on or
 * off - is one short press, and the long and double rules are never used. */
typedef enum {
    ZBC_INPUT_BUTTON = 0,
    ZBC_INPUT_SWITCH,
    ZBC_INPUT_COUNT,
} zbc_btn_input_t;

typedef enum {
    ZBC_EV_SHORT = 0,
    ZBC_EV_LONG,
    ZBC_EV_DOUBLE,
    /* Appended in BUTTON_BLOB_VERSION 4; the order is the stored layout. */
    ZBC_EV_TRIPLE,
    ZBC_EV_COUNT,
} zbc_btn_event_t;

/* A sensor's two events, in the first two gestures' places in the pool: the
 * stored layout has no other room for them, and a sensor has no presses. A
 * sensor's double and triple press always stay empty. */
#define ZBC_EV_MOTION ZBC_EV_SHORT
#define ZBC_EV_CLEAR  ZBC_EV_LONG

/* How long a sensor waits without motion before its no-motion rules run, in
 * seconds. Not below half a minute: a Xiaomi or Aqara sensor reports motion at
 * most once a minute, so anything much shorter ends while somebody is still
 * moving in the room. */
#define ZBC_SENSOR_CLEAR_MIN_S     30
#define ZBC_SENSOR_CLEAR_MAX_S     3600
#define ZBC_SENSOR_CLEAR_DEFAULT_S 120

/* When a rule applies, asked of the rule's own target. Tried in order, first
 * match wins. A room or a light whose state is not known counts as off and as
 * 0 %, which is what lighting_group_any_on() already does - so "when off" and
 * "when below 30 %" both fire after a reboot rather than leaving the button
 * dead. */
typedef enum {
    ZBC_COND_ALWAYS = 0,
    ZBC_COND_OFF,
    ZBC_COND_ON,
    ZBC_COND_BELOW,      /* brightness < cond_arg % */
    ZBC_COND_ABOVE,      /* brightness > cond_arg % */
    /* Between sunset and sunrise at the location Adaptive Lighting uses. Asked
     * of the sun, not of the target, so like ALWAYS it needs none; without a
     * clock or a location it holds, as not knowing a light counts as off. */
    ZBC_COND_SUN_DOWN,
    ZBC_COND_COUNT,
} zbc_btn_cond_t;

/* Whether a condition is asked of the rule's own room or light. */
static inline bool zbc_cond_needs_target(uint8_t cond)
{
    return cond != ZBC_COND_ALWAYS && cond != ZBC_COND_SUN_DOWN;
}

/* State-dependence lives here rather than in a condition the user has to write:
 * TOGGLE and DIM_CYCLE are the two cases everybody wants, and spelling them as
 * actions keeps the common button one line long. */
typedef enum {
    ZBC_ACT_NONE = 0,
    ZBC_ACT_ON,
    ZBC_ACT_OFF,
    ZBC_ACT_TOGGLE,
    ZBC_ACT_SET,         /* to action_arg % */
    ZBC_ACT_DIM_UP,      /* by action_arg % per repeat while held */
    ZBC_ACT_DIM_DOWN,
    ZBC_ACT_DIM_CYCLE,   /* reverses at the rails and on each new hold */
    /* Starts the group's Adaptive Lighting mode, or re-enters it - a room only,
     * since the mode belongs to a group. Appended, so the stored rules keep
     * their values. */
    ZBC_ACT_ADAPTIVE,
    ZBC_ACT_COUNT,
} zbc_btn_action_t;

/* The actions that move while a button is held. */
static inline bool zbc_act_is_dim(uint8_t action)
{
    return action == ZBC_ACT_DIM_UP || action == ZBC_ACT_DIM_DOWN || action == ZBC_ACT_DIM_CYCLE;
}

#define ZBC_BTN_IEEE_LEN 8

/* The rule runs with the one above it rather than competing with it: the head
 * of its case decided, and this rule's own condition is not read. Never on the
 * first rule of a gesture, which has nothing to join. */
#define ZBC_RULE_AND 0x01

/* The target is a room when ep is 0, and one light's endpoint otherwise - the
 * same (ieee, ep) a group member is, so a wired light or a Matter lamp is named
 * by its made-up address. Neither set means unassigned: the room or the light
 * it named was deleted, and the rule is inert until somebody picks another. */
typedef struct {
    uint16_t group_id;    /* the room, when ep is 0; 0 otherwise */
    uint8_t  cond;        /* zbc_btn_cond_t */
    uint8_t  cond_arg;    /* percent, for BELOW / ABOVE */
    uint8_t  action;      /* zbc_btn_action_t */
    uint8_t  action_arg;  /* percent for SET, step size for the DIM_* actions */
    uint8_t  flags;       /* ZBC_RULE_AND */
    uint8_t  ep;          /* the light's endpoint; 0 when the target is a room */
    uint8_t  ieee[ZBC_BTN_IEEE_LEN];  /* the light; zero when the target is a room */
    /* Seconds after the event that the action runs; 0 runs it at once. The next
     * event of the same button drops whatever is still waiting, and so does a
     * restart (buttons.c). Never on a dimming rule, which only means something
     * while the button is held. */
    uint16_t delay_s;
} zbc_btn_rule_t;

/* How many dimming rules one case of a long press may hold. Each is a sweep of
 * its own, with state kept for the length of the hold (buttons.c), so the
 * number is bounded; two or three lights dimmed by one hold is what anybody
 * does, and more belong in a room. */
#define ZBC_BTN_MAX_SWEEPS 4

/* One button as the table hands it out and takes it back. */
typedef struct {
    uint16_t id;                      /* primary key, assigned here */
    char     name[ZBC_BUTTON_NAME_LEN];
    uint8_t  src;                     /* zbc_btn_src_t */
    /* Source: GPIO. */
    int8_t   gpio;                    /* -1 = not wired up yet */
    bool     inverted;                /* the pin reads low when pressed */
    uint8_t  pull;                    /* zbc_btn_pull_t */
    /* Source: Zigbee or sensor. All zero = not bound to a remote or a sensor
     * yet, which is also what a button is left as when its device is deleted. */
    uint8_t  ieee[ZBC_BTN_IEEE_LEN];
    uint8_t  zb_ep;
    uint8_t  zb_key;                  /* which button of that endpoint; 0 for a sensor */
    /* One list for every gesture: the short press's rules first, then the long
     * press's, and so on - see button_db_rules(). A wall switch has one gesture
     * and may use all of them. */
    uint8_t  rule_count[ZBC_EV_COUNT];
    zbc_btn_rule_t rules[CONFIG_ZBC_BUTTON_RULES];
    /* Source: GPIO. zbc_btn_input_t. */
    uint8_t  input;
    /* Source: sensor. Seconds without motion before the no-motion rules run,
     * ZBC_SENSOR_CLEAR_MIN_S to _MAX_S; zero for any other source. */
    uint16_t clear_s;
} zbc_button_t;

/* One gesture's rules: where they start in the pool, and how many (n may be
 * NULL). The counts are trusted, which valid_button() in button_db.c makes safe
 * for anything that came out of the table. */
static inline zbc_btn_rule_t *button_db_rules(zbc_button_t *b, int ev, int *n)
{
    int start = 0;
    for (int e = 0; e < ev; e++) {
        start += b->rule_count[e];
    }
    if (n) {
        *n = b->rule_count[ev];
    }
    return &b->rules[start];
}

static inline int button_db_rule_total(const zbc_button_t *b)
{
    int total = 0;
    for (int e = 0; e < ZBC_EV_COUNT; e++) {
        total += b->rule_count[e];
    }
    return total;
}

/* True when a rule names a target, room or light. */
static inline bool button_db_rule_assigned(const zbc_btn_rule_t *r)
{
    return r->ep != 0 || r->group_id != 0;
}

/* >>> end host-test region: bdb-types <<< */

/* True when this button has a source it can actually fire from. */
bool button_db_has_source(const zbc_button_t *b);

esp_err_t button_db_init(void);

esp_err_t button_db_create(const char *name, uint16_t *out_id);
/* Whole-record update: the editor saves name, wiring and every gesture at
 * once, so there is no per-field setter. The stored id is kept.
 * ESP_ERR_NO_MEM when the pool has no room for the button's rules (see
 * button_db_rules_room); the button is left as it was then. */
esp_err_t button_db_set(uint16_t id, const zbc_button_t *in);
esp_err_t button_db_remove(uint16_t id);

bool      button_db_get(uint16_t id, zbc_button_t *out);
/* The ids alone, for a caller that copies one button at a time rather than
 * holding the whole table - see stream_buttons in http_server.c. In the order
 * the buttons were made. */
size_t    button_db_list_ids(uint16_t *ids, size_t max);

/* How many rules button `id` could have: its own and the pool's free ones,
 * up to CONFIG_ZBC_BUTTON_RULES. 0 for an unknown id. */
int       button_db_rules_room(uint16_t id);

/* True when some other button already claims this pin, so two buttons cannot
 * fight over one line. Pass id 0 to ask about every button. */
bool      button_db_pin_taken(int gpio, uint16_t except_id);

/* The same, for a remote key: two buttons must not claim one key of one
 * endpoint, or a single press would run both their rule lists. */
bool      button_db_key_taken(const uint8_t ieee[ZBC_BTN_IEEE_LEN], uint8_t ep, uint8_t key,
                              uint16_t except_id);

/* And for a sensor's endpoint: one record per sensor, or one motion would run
 * two lists. */
bool      button_db_sensor_taken(const uint8_t ieee[ZBC_BTN_IEEE_LEN], uint8_t ep, uint16_t except_id);

/* A deleted group leaves its rules in place but unassigned, so the button keeps
 * its shape and the UI can say "pick a group" instead of silently losing the
 * work. Mirrors group_db_forget_device / curve_db_forget_device. */
void      button_db_forget_group(uint16_t group_id);

/* And the same when a device is deleted. A remote or a sensor: the button keeps
 * its name and its rules and loses only its source, so the UI can say "pick a
 * remote". A light: every rule that named it stays, unassigned, as for a room. */
void      button_db_forget_device(const uint8_t ieee[ZBC_BTN_IEEE_LEN]);

esp_err_t button_db_erase(void);

/* Whether a stored blob of this table is one the boot load takes rather than
 * discards. The one statement of that, shared with the configuration restore
 * (config_backup.c), which refuses a file whose table the load would throw away. */
bool      button_db_blob_ok(const void *blob, size_t len);

/* The longest blob that load takes, for the restore's line length. */
size_t    button_db_blob_max(void);

#ifdef __cplusplus
}
#endif
