#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "button_db.h"
#include "state_rev.h"
#include "remote.h"

static const char *TAG = "button_db";

/* Same namespace as the other tables, its own key and its own version: adding
 * buttons must not discard the devices, the groups or the curves. */
#define BUTTON_NVS_NAMESPACE "zbc"
/* In config_backup.c's list too: a backup carries this table under this key. */
#define BUTTON_NVS_KEY       "buttons"
/* Each version is migrated, never discarded: a button is configuration somebody
 * typed in by hand, and a new version arrives over the air with nobody watching.
 * 1 -> 2 added the source (version 1 buttons are all pins).
 * 2 -> 3 added the input type (push button or switch) and reserved bytes.
 * 3 -> 4 added the triple press; every rule keeps its gesture.
 * 4 -> 5 made a rule 16 bytes (a light as a target, and "and") and put every
 *        gesture's rules in one pool; every rule keeps its room and gesture. */
#define BUTTON_BLOB_VERSION  5

/* How many gestures versions 1 to 3 stored. A literal, not ZBC_EV_COUNT: the
 * frozen records below describe bytes already in flash, and sized by the live
 * count they would change whenever a gesture is added. */
#define OLD_EV_COUNT 3
/* And how many gestures version 4 stored. */
#define OLD_EV_COUNT_V4 4
/* How many rules one gesture held in versions 1 to 4: CONFIG_ZBC_MAX_BUTTON_RULES,
 * which was 3 in every build that shipped. A literal for the same reason. */
#define OLD_RULES 3

/* Every rule a version-4 button could hold has to fit, or converting one would
 * drop some - see the range of CONFIG_ZBC_BUTTON_RULES. */
_Static_assert(CONFIG_ZBC_BUTTON_RULES >= OLD_EV_COUNT_V4 * OLD_RULES,
               "a converted version-4 button must keep every rule");

typedef struct {
    uint32_t version;
    uint16_t count;
    uint16_t reserved;
} button_blob_header_t;

/* Frozen copy of the rule versions 1 to 4 stored: a room and nothing else. */
typedef struct {
    uint16_t group_id;
    uint8_t  cond;
    uint8_t  cond_arg;
    uint8_t  action;
    uint8_t  action_arg;
} zbc_btn_rule_v4_t;

/* Frozen copy of the version-1 record, so the migration below reads what was
 * actually written rather than what zbc_button_t happens to look like now.
 * Never change it; it is a description of bytes already in flash. */
typedef struct {
    bool     in_use;
    uint16_t id;
    char     name[ZBC_BUTTON_NAME_LEN];
    int8_t   gpio;
    bool     inverted;
    uint8_t  pull;
    uint8_t  rule_count[OLD_EV_COUNT];
    zbc_btn_rule_v4_t rules[OLD_EV_COUNT][OLD_RULES];
} zbc_button_v1_t;

/* Frozen copy of the version-2 record - see zbc_button_v1_t. */
typedef struct {
    bool     in_use;
    uint16_t id;
    char     name[ZBC_BUTTON_NAME_LEN];
    uint8_t  src;
    int8_t   gpio;
    bool     inverted;
    uint8_t  pull;
    uint8_t  ieee[ZBC_BTN_IEEE_LEN];
    uint8_t  zb_ep;
    uint8_t  zb_key;
    uint8_t  rule_count[OLD_EV_COUNT];
    zbc_btn_rule_v4_t rules[OLD_EV_COUNT][OLD_RULES];
} zbc_button_v2_t;

/* Frozen copy of the version-3 record - see zbc_button_v1_t. */
typedef struct {
    bool     in_use;
    uint16_t id;
    char     name[ZBC_BUTTON_NAME_LEN];
    uint8_t  src;
    int8_t   gpio;
    bool     inverted;
    uint8_t  pull;
    uint8_t  ieee[ZBC_BTN_IEEE_LEN];
    uint8_t  zb_ep;
    uint8_t  zb_key;
    uint8_t  rule_count[OLD_EV_COUNT];
    zbc_btn_rule_v4_t rules[OLD_EV_COUNT][OLD_RULES];
    uint8_t  input;
    uint8_t  reserved[7];
} zbc_button_v3_t;

/* Frozen copy of the version-4 record - see zbc_button_v1_t. */
typedef struct {
    bool     in_use;
    uint16_t id;
    char     name[ZBC_BUTTON_NAME_LEN];
    uint8_t  src;
    int8_t   gpio;
    bool     inverted;
    uint8_t  pull;
    uint8_t  ieee[ZBC_BTN_IEEE_LEN];
    uint8_t  zb_ep;
    uint8_t  zb_key;
    uint8_t  rule_count[OLD_EV_COUNT_V4];
    zbc_btn_rule_v4_t rules[OLD_EV_COUNT_V4][OLD_RULES];
    uint8_t  input;
    uint8_t  reserved[7];
} zbc_button_v4_t;

/* The gestures an older record had, into a current one: each gesture's rules in
 * turn into the pool, each a room rule joined to nothing, which is what every
 * rule was. Gestures the record did not have start empty. */
static void copy_old_rules(zbc_button_t *b, int events, const uint8_t *count,
                           const zbc_btn_rule_v4_t rules[][OLD_RULES])
{
    int at = 0;
    for (int e = 0; e < events; e++) {
        const int n = count[e] <= OLD_RULES ? count[e] : OLD_RULES;
        b->rule_count[e] = (uint8_t)n;
        for (int r = 0; r < n; r++) {
            zbc_btn_rule_t *out = &b->rules[at++];
            memset(out, 0, sizeof(*out));
            out->group_id   = rules[e][r].group_id;
            out->cond       = rules[e][r].cond;
            out->cond_arg   = rules[e][r].cond_arg;
            out->action     = rules[e][r].action;
            out->action_arg = rules[e][r].action_arg;
        }
    }
}

/* The fields every version from 2 on has, alike. */
#define COPY_V2_FIELDS(b, in)                              \
    do {                                                   \
        (b)->in_use   = true;                              \
        (b)->id       = (in).id;                           \
        memcpy((b)->name, (in).name, sizeof((b)->name));   \
        (b)->src      = (in).src;                          \
        (b)->gpio     = (in).gpio;                         \
        (b)->inverted = (in).inverted;                     \
        (b)->pull     = (in).pull;                         \
        memcpy((b)->ieee, (in).ieee, sizeof((b)->ieee));   \
        (b)->zb_ep    = (in).zb_ep;                        \
        (b)->zb_key   = (in).zb_key;                       \
    } while (0)

static zbc_button_t      s_buttons[CONFIG_ZBC_MAX_BUTTONS];
/* Set by a version-1 load, so the converted table is written back once rather
 * than being converted again at every boot. */
static bool              s_migrated;
static SemaphoreHandle_t s_lock;

#define BDB_LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define BDB_UNLOCK() xSemaphoreGiveRecursive(s_lock)

static zbc_button_t *find_by_id(uint16_t id)
{
    for (int i = 0; i < CONFIG_ZBC_MAX_BUTTONS; i++) {
        if (s_buttons[i].in_use && s_buttons[i].id == id) {
            return &s_buttons[i];
        }
    }
    return NULL;
}

static esp_err_t bdb_save_locked(void)
{
    zbc_state_bump();          /* the page shows this table */
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(BUTTON_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    size_t used = 0;
    for (int i = 0; i < CONFIG_ZBC_MAX_BUTTONS; i++) {
        if (s_buttons[i].in_use) {
            used++;
        }
    }

    size_t blob_size = sizeof(button_blob_header_t) + used * sizeof(zbc_button_t);
    uint8_t *blob = calloc(1, blob_size);
    if (!blob) {
        nvs_close(nvs);
        return ESP_ERR_NO_MEM;
    }

    button_blob_header_t hdr = { .version = BUTTON_BLOB_VERSION, .count = (uint16_t)used };
    memcpy(blob, &hdr, sizeof(hdr));

    zbc_button_t *out = (zbc_button_t *)(blob + sizeof(hdr));
    for (int i = 0; i < CONFIG_ZBC_MAX_BUTTONS; i++) {
        if (s_buttons[i].in_use) {
            *out++ = s_buttons[i];
        }
    }

    err = nvs_set_blob(nvs, BUTTON_NVS_KEY, blob, blob_size);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    free(blob);
    nvs_close(nvs);
    return err;
}

/* Every version bdb_load converts, at the size its record had. */
bool button_db_blob_ok(const void *blob, size_t len)
{
    button_blob_header_t hdr;
    if (!blob || len < sizeof(hdr)) {
        return false;
    }
    memcpy(&hdr, blob, sizeof(hdr));
    size_t rec;
    switch (hdr.version) {
    case BUTTON_BLOB_VERSION: rec = sizeof(zbc_button_t);    break;
    case 4:                   rec = sizeof(zbc_button_v4_t); break;
    case 3:                   rec = sizeof(zbc_button_v3_t); break;
    case 2:                   rec = sizeof(zbc_button_v2_t); break;
    case 1:                   rec = sizeof(zbc_button_v1_t); break;
    default:                  return false;
    }
    return len == sizeof(hdr) + (size_t)hdr.count * rec;
}

static void bdb_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open(BUTTON_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    size_t blob_size = 0;
    esp_err_t err = nvs_get_blob(nvs, BUTTON_NVS_KEY, NULL, &blob_size);
    if (err != ESP_OK || blob_size < sizeof(button_blob_header_t)) {
        nvs_close(nvs);
        return;
    }

    uint8_t *blob = malloc(blob_size);
    if (!blob) {
        nvs_close(nvs);
        return;
    }
    err = nvs_get_blob(nvs, BUTTON_NVS_KEY, blob, &blob_size);
    nvs_close(nvs);
    if (err != ESP_OK) {
        free(blob);
        return;
    }

    if (!button_db_blob_ok(blob, blob_size)) {
        ESP_LOGW(TAG, "Stored buttons are from an incompatible build, discarding");
        free(blob);
        return;
    }
    button_blob_header_t hdr;
    memcpy(&hdr, blob, sizeof(hdr));
    size_t expected    = sizeof(hdr) + (size_t)hdr.count * sizeof(zbc_button_t);
    size_t expected_v1 = sizeof(hdr) + (size_t)hdr.count * sizeof(zbc_button_v1_t);
    size_t expected_v2 = sizeof(hdr) + (size_t)hdr.count * sizeof(zbc_button_v2_t);
    size_t expected_v3 = sizeof(hdr) + (size_t)hdr.count * sizeof(zbc_button_v3_t);
    size_t expected_v4 = sizeof(hdr) + (size_t)hdr.count * sizeof(zbc_button_v4_t);

    int loaded = 0;
    if (hdr.version == BUTTON_BLOB_VERSION && blob_size == expected) {
        const zbc_button_t *in = (const zbc_button_t *)(blob + sizeof(hdr));
        for (int i = 0; i < hdr.count && loaded < CONFIG_ZBC_MAX_BUTTONS; i++) {
            s_buttons[loaded]        = in[i];
            s_buttons[loaded].in_use = true;
            /* button_db_rules() trusts the counts; a record whose counts run past
             * the pool keeps its name and source and loses its rules. */
            if (button_db_rule_total(&s_buttons[loaded]) > CONFIG_ZBC_BUTTON_RULES) {
                memset(s_buttons[loaded].rule_count, 0, sizeof(s_buttons[loaded].rule_count));
            }
            loaded++;
        }
    } else if (hdr.version == 4 && blob_size == expected_v4) {
        const zbc_button_v4_t *in = (const zbc_button_v4_t *)(blob + sizeof(hdr));
        for (int i = 0; i < hdr.count && loaded < CONFIG_ZBC_MAX_BUTTONS; i++) {
            zbc_button_t *b = &s_buttons[loaded];
            memset(b, 0, sizeof(*b));
            COPY_V2_FIELDS(b, in[i]);
            copy_old_rules(b, OLD_EV_COUNT_V4, in[i].rule_count, in[i].rules);
            b->input    = in[i].input;
            loaded++;
        }
        ESP_LOGI(TAG, "Converted %d button(s) from the previous format", loaded);
        s_migrated = true;
    } else if (hdr.version == 3 && blob_size == expected_v3) {
        const zbc_button_v3_t *in = (const zbc_button_v3_t *)(blob + sizeof(hdr));
        for (int i = 0; i < hdr.count && loaded < CONFIG_ZBC_MAX_BUTTONS; i++) {
            zbc_button_t *b = &s_buttons[loaded];
            memset(b, 0, sizeof(*b));
            COPY_V2_FIELDS(b, in[i]);
            copy_old_rules(b, OLD_EV_COUNT, in[i].rule_count, in[i].rules);
            b->input    = in[i].input;
            loaded++;
        }
        ESP_LOGI(TAG, "Converted %d button(s) from the previous format", loaded);
        s_migrated = true;
    } else if (hdr.version == 2 && blob_size == expected_v2) {
        /* Every button before version 3 was a push button. */
        const zbc_button_v2_t *in = (const zbc_button_v2_t *)(blob + sizeof(hdr));
        for (int i = 0; i < hdr.count && loaded < CONFIG_ZBC_MAX_BUTTONS; i++) {
            zbc_button_t *b = &s_buttons[loaded];
            memset(b, 0, sizeof(*b));
            COPY_V2_FIELDS(b, in[i]);
            copy_old_rules(b, OLD_EV_COUNT, in[i].rule_count, in[i].rules);
            b->input    = ZBC_INPUT_BUTTON;
            loaded++;
        }
        ESP_LOGI(TAG, "Converted %d button(s) from the previous format", loaded);
        s_migrated = true;
    } else if (hdr.version == 1 && blob_size == expected_v1) {
        /* Every button that existed before remotes did was a pin. */
        const zbc_button_v1_t *in = (const zbc_button_v1_t *)(blob + sizeof(hdr));
        for (int i = 0; i < hdr.count && loaded < CONFIG_ZBC_MAX_BUTTONS; i++) {
            zbc_button_t *b = &s_buttons[loaded];
            memset(b, 0, sizeof(*b));
            b->in_use   = true;
            b->id       = in[i].id;
            memcpy(b->name, in[i].name, sizeof(b->name));
            b->src      = ZBC_BTN_SRC_GPIO;
            b->gpio     = in[i].gpio;
            b->inverted = in[i].inverted;
            b->pull     = in[i].pull;
            copy_old_rules(b, OLD_EV_COUNT, in[i].rule_count, in[i].rules);
            loaded++;
        }
        ESP_LOGI(TAG, "Converted %d button(s) from the previous format", loaded);
        s_migrated = true;
    } else {
        ESP_LOGW(TAG, "Stored buttons are from an incompatible build, discarding");
        free(blob);
        return;
    }
    free(blob);
    ESP_LOGI(TAG, "Restored %d button(s) from NVS", loaded);
}

esp_err_t button_db_init(void)
{
    if (s_lock) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    memset(s_buttons, 0, sizeof(s_buttons));
    BDB_LOCK();
    bdb_load();
    if (s_migrated) {
        bdb_save_locked();
        s_migrated = false;
    }
    BDB_UNLOCK();
    return ESP_OK;
}

esp_err_t button_db_create(const char *name, uint16_t *out_id)
{
    if (!name || !name[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    BDB_LOCK();
    zbc_button_t *slot = NULL;
    uint16_t      next = 1;
    for (int i = 0; i < CONFIG_ZBC_MAX_BUTTONS; i++) {
        if (!s_buttons[i].in_use) {
            if (!slot) {
                slot = &s_buttons[i];
            }
        } else if (s_buttons[i].id >= next) {
            next = s_buttons[i].id + 1;
        }
    }
    if (!slot) {
        BDB_UNLOCK();
        ESP_LOGE(TAG, "Button table full (%d)", CONFIG_ZBC_MAX_BUTTONS);
        return ESP_ERR_NO_MEM;
    }

    memset(slot, 0, sizeof(*slot));
    slot->in_use = true;
    slot->id     = next;
    slot->gpio   = -1;                 /* not wired up until someone picks a pin */
    slot->pull   = ZBC_PULL_UP;        /* a button to GND is what almost everyone wires */
    slot->inverted = true;
    strlcpy(slot->name, name, sizeof(slot->name));

    if (out_id) {
        *out_id = slot->id;
    }
    esp_err_t err = bdb_save_locked();
    BDB_UNLOCK();
    return err;
}

static bool is_dim(uint8_t action)
{
    return action == ZBC_ACT_DIM_UP || action == ZBC_ACT_DIM_DOWN || action == ZBC_ACT_DIM_CYCLE;
}

static bool ieee_is_zero(const uint8_t ieee[ZBC_BTN_IEEE_LEN])
{
    for (int i = 0; i < ZBC_BTN_IEEE_LEN; i++) {
        if (ieee[i]) {
            return false;
        }
    }
    return true;
}

/* Validation lives here rather than in the HTTP layer so a bad record cannot be
 * stored by any other route either. The pin itself is checked by the caller,
 * which is the only place that knows what this chip has. */
static bool valid_button(const zbc_button_t *in)
{
    if (!in->name[0] || in->pull >= ZBC_PULL_DOWN + 1 || in->src >= ZBC_BTN_SRC_COUNT) {
        return false;
    }
    /* A Zigbee button holds no pin, and a GPIO one holds no remote. Keeping the
     * unused half zeroed is what lets button_db_key_taken and the UI ask "does
     * this button have a source yet" without also asking which kind it is. */
    if (in->src == ZBC_BTN_SRC_ZIGBEE) {
        if (in->gpio != -1 || in->zb_key >= ZB_REMOTE_KEY_MAX) {
            return false;
        }
    } else if (in->zb_ep != 0 || in->zb_key != 0) {
        return false;
    }
    if (button_db_rule_total(in) > CONFIG_ZBC_BUTTON_RULES) {
        return false;
    }
    zbc_button_t b = *in;          /* button_db_rules() hands out non-const slices */
    for (int e = 0; e < ZBC_EV_COUNT; e++) {
        int n = 0;
        const zbc_btn_rule_t *rules = button_db_rules(&b, e, &n);
        int sweeps = 0;            /* dimming rules in the case being read */
        for (int r = 0; r < n; r++) {
            const zbc_btn_rule_t *rule = &rules[r];
            if (rule->cond >= ZBC_COND_COUNT || rule->action >= ZBC_ACT_COUNT) {
                return false;
            }
            if (rule->cond_arg > 100 || rule->action_arg > 100) {
                return false;
            }
            /* A step of zero would hold the light still however long you press. */
            if (is_dim(rule->action) && rule->action_arg == 0) {
                return false;
            }
            if (rule->action == ZBC_ACT_SET && rule->action_arg == 0) {
                return false;   /* 0 % is not a brightness; that is what "off" is for */
            }
            /* The first rule of a gesture has nothing to join. */
            if ((rule->flags & ~ZBC_RULE_AND) || (r == 0 && (rule->flags & ZBC_RULE_AND))) {
                return false;
            }
            /* A room or a light, never half of each: the rest of the code asks ep
             * which kind it is and trusts the answer. */
            if (rule->ep != 0 ? rule->group_id != 0 || ieee_is_zero(rule->ieee)
                              : !ieee_is_zero(rule->ieee)) {
                return false;
            }
            /* Adaptive Lighting is a mode of a group; a light has none. */
            if (rule->action == ZBC_ACT_ADAPTIVE && rule->ep != 0) {
                return false;
            }
            /* Each dimming rule of a hold is a sweep with state of its own for the
             * length of the hold, and buttons.c keeps ZBC_BTN_MAX_SWEEPS of them. */
            if (!(rule->flags & ZBC_RULE_AND)) {
                sweeps = 0;
            }
            if (e == ZBC_EV_LONG && is_dim(rule->action) && ++sweeps > ZBC_BTN_MAX_SWEEPS) {
                return false;
            }
        }
    }
    return true;
}

esp_err_t button_db_set(uint16_t id, const zbc_button_t *in)
{
    if (!in || !valid_button(in)) {
        return ESP_ERR_INVALID_ARG;
    }
    BDB_LOCK();
    zbc_button_t *b = find_by_id(id);
    if (!b) {
        BDB_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    *b        = *in;
    b->in_use = true;
    b->id     = id;                    /* the key is ours, not the caller's */
    esp_err_t err = bdb_save_locked();
    BDB_UNLOCK();
    return err;
}

esp_err_t button_db_remove(uint16_t id)
{
    BDB_LOCK();
    zbc_button_t *b = find_by_id(id);
    if (!b) {
        BDB_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    memset(b, 0, sizeof(*b));
    esp_err_t err = bdb_save_locked();
    BDB_UNLOCK();
    return err;
}

bool button_db_get(uint16_t id, zbc_button_t *out)
{
    BDB_LOCK();
    zbc_button_t *b = find_by_id(id);
    if (b && out) {
        *out = *b;
    }
    bool found = b != NULL;
    BDB_UNLOCK();
    return found;
}

size_t button_db_snapshot(zbc_button_t *buf, size_t max)
{
    size_t n = 0;
    BDB_LOCK();
    for (int i = 0; i < CONFIG_ZBC_MAX_BUTTONS && n < max; i++) {
        if (s_buttons[i].in_use) {
            buf[n++] = s_buttons[i];
        }
    }
    BDB_UNLOCK();
    return n;
}

size_t button_db_list_ids(uint16_t *ids, size_t max)
{
    size_t n = 0;
    BDB_LOCK();
    for (int i = 0; i < CONFIG_ZBC_MAX_BUTTONS && n < max; i++) {
        if (s_buttons[i].in_use) {
            ids[n++] = s_buttons[i].id;
        }
    }
    BDB_UNLOCK();
    return n;
}

bool button_db_pin_taken(int gpio, uint16_t except_id)
{
    if (gpio < 0) {
        return false;
    }
    bool taken = false;
    BDB_LOCK();
    for (int i = 0; i < CONFIG_ZBC_MAX_BUTTONS && !taken; i++) {
        taken = s_buttons[i].in_use && s_buttons[i].id != except_id &&
                s_buttons[i].gpio == gpio;
    }
    BDB_UNLOCK();
    return taken;
}

bool button_db_has_source(const zbc_button_t *b)
{
    if (!b) {
        return false;
    }
    if (b->src == ZBC_BTN_SRC_ZIGBEE) {
        for (int i = 0; i < ZBC_BTN_IEEE_LEN; i++) {
            if (b->ieee[i]) {
                return b->zb_ep != 0;
            }
        }
        return false;
    }
    return b->gpio >= 0;
}

bool button_db_key_taken(const uint8_t ieee[ZBC_BTN_IEEE_LEN], uint8_t ep, uint8_t key,
                         uint16_t except_id)
{
    if (!ieee || ep == 0) {
        return false;
    }
    bool taken = false;
    BDB_LOCK();
    for (int i = 0; i < CONFIG_ZBC_MAX_BUTTONS && !taken; i++) {
        taken = s_buttons[i].in_use && s_buttons[i].id != except_id &&
                s_buttons[i].src == ZBC_BTN_SRC_ZIGBEE && s_buttons[i].zb_ep == ep &&
                s_buttons[i].zb_key == key &&
                memcmp(s_buttons[i].ieee, ieee, ZBC_BTN_IEEE_LEN) == 0;
    }
    BDB_UNLOCK();
    return taken;
}

void button_db_forget_device(const uint8_t ieee[ZBC_BTN_IEEE_LEN])
{
    if (!ieee) {
        return;
    }
    bool changed = false;
    BDB_LOCK();
    for (int i = 0; i < CONFIG_ZBC_MAX_BUTTONS; i++) {
        zbc_button_t *b = &s_buttons[i];
        if (!b->in_use) {
            continue;
        }
        if (b->src == ZBC_BTN_SRC_ZIGBEE && memcmp(b->ieee, ieee, ZBC_BTN_IEEE_LEN) == 0) {
            memset(b->ieee, 0, ZBC_BTN_IEEE_LEN);
            b->zb_ep  = 0;
            b->zb_key = 0;
            changed = true;
        }
        /* A light that is gone leaves its rules unassigned rather than deleted,
         * as a room does - and never pointing at the address, which a wired
         * light or a Matter lamp made later may be given again. */
        const int total = button_db_rule_total(b);
        for (int r = 0; r < total; r++) {
            zbc_btn_rule_t *rule = &b->rules[r];
            if (rule->ep != 0 && memcmp(rule->ieee, ieee, ZBC_BTN_IEEE_LEN) == 0) {
                memset(rule->ieee, 0, ZBC_BTN_IEEE_LEN);
                rule->ep = 0;
                changed = true;
            }
        }
    }
    if (changed) {
        bdb_save_locked();
    }
    BDB_UNLOCK();
}

void button_db_forget_group(uint16_t group_id)
{
    bool changed = false;
    BDB_LOCK();
    for (int i = 0; i < CONFIG_ZBC_MAX_BUTTONS; i++) {
        if (!s_buttons[i].in_use) {
            continue;
        }
        const int total = button_db_rule_total(&s_buttons[i]);
        for (int r = 0; r < total; r++) {
            zbc_btn_rule_t *rule = &s_buttons[i].rules[r];
            if (rule->ep == 0 && rule->group_id == group_id) {
                rule->group_id = 0;
                changed = true;
            }
        }
    }
    if (changed) {
        bdb_save_locked();
    }
    BDB_UNLOCK();
}

esp_err_t button_db_erase(void)
{
    BDB_LOCK();
    memset(s_buttons, 0, sizeof(s_buttons));
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(BUTTON_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_erase_key(nvs, BUTTON_NVS_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    BDB_UNLOCK();
    return err;
}
