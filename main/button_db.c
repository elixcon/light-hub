#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

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

/* >>> host-test region: bdb-table <<< */

/* Each version is migrated, never discarded: a button is configuration somebody
 * typed in by hand, and a new version arrives over the air with nobody watching.
 * 1 -> 2 added the source (version 1 buttons are all pins).
 * 2 -> 3 added the input type (push button or switch) and reserved bytes.
 * 3 -> 4 added the triple press; every rule keeps its gesture.
 * 4 -> 5 made a rule 16 bytes (a light as a target, and "and") and put every
 *        gesture's rules in one pool; every rule keeps its room and gesture.
 * 5 -> 6 gave a rule its delay and a button its no-motion time (sensors); every
 *        rule runs at once, as it did.
 * 6 -> 7 stores each button's rules alone, all buttons' in one pool behind
 *        the buttons, rather than room for twelve in every button; every button
 *        keeps its rules. */
#define BUTTON_BLOB_VERSION  7

/* How many gestures versions 1 to 3 stored. A literal, not ZBC_EV_COUNT: the
 * frozen records below describe bytes already in flash, and sized by the live
 * count they would change whenever a gesture is added. */
#define OLD_EV_COUNT 3
/* And how many gestures versions 4 to 7 store. */
#define OLD_EV_COUNT_V4 4
/* How many rules one gesture held in versions 1 to 4: CONFIG_ZBC_MAX_BUTTON_RULES,
 * which was 3 in every build that shipped. A literal for the same reason. */
#define OLD_RULES 3
/* How many rules one button held in versions 5 and 6: CONFIG_ZBC_BUTTON_RULES,
 * which was 12 in every build that shipped. */
#define OLD_POOL 12
/* The most buttons a build before version 7 could hold: the top of
 * CONFIG_ZBC_MAX_BUTTONS's range then. */
#define OLD_MAX_BUTTONS 16

/* Every rule an older button could hold has to fit, or converting one would
 * drop some - see the range of CONFIG_ZBC_BUTTON_RULES. */
_Static_assert(CONFIG_ZBC_BUTTON_RULES >= OLD_EV_COUNT_V4 * OLD_RULES &&
               CONFIG_ZBC_BUTTON_RULES >= OLD_POOL,
               "a converted button must keep every rule");
/* And one button with every rule it may have has to fit in the pool. */
_Static_assert(CONFIG_ZBC_BUTTON_POOL >= CONFIG_ZBC_BUTTON_RULES,
               "the pool must hold one full button");
_Static_assert(ZBC_EV_COUNT == OLD_EV_COUNT_V4,
               "a gesture added needs a new BUTTON_BLOB_VERSION");
/* Versions 6 and 7 store the rule as it is. One of another shape needs a frozen
 * copy of this one for them, and a new version. */
_Static_assert(sizeof(zbc_btn_rule_t) == 18, "the stored rule changed");

typedef struct {
    uint32_t version;
    uint16_t count;      /* buttons */
    uint16_t rules;      /* version 7 on: rules in the pool behind them; zero before */
} button_blob_header_t;

/* Frozen copy of the rule versions 1 to 4 stored: a room and nothing else. */
typedef struct {
    uint16_t group_id;
    uint8_t  cond;
    uint8_t  cond_arg;
    uint8_t  action;
    uint8_t  action_arg;
} zbc_btn_rule_v4_t;

/* Frozen copy of the rule version 5 stored: as now, less the delay. */
typedef struct {
    uint16_t group_id;
    uint8_t  cond;
    uint8_t  cond_arg;
    uint8_t  action;
    uint8_t  action_arg;
    uint8_t  flags;
    uint8_t  ep;
    uint8_t  ieee[ZBC_BTN_IEEE_LEN];
} zbc_btn_rule_v5_t;

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

/* Frozen copy of the version-5 record - see zbc_button_v1_t. */
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
    zbc_btn_rule_v5_t rules[OLD_POOL];
    uint8_t  input;
    uint8_t  reserved[7];
} zbc_button_v5_t;

/* Frozen copy of the version-6 record - see zbc_button_v1_t: version 5's with
 * each rule's delay and a sensor's no-motion time. */
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
    zbc_btn_rule_t rules[OLD_POOL];
    uint8_t  input;
    uint8_t  pad;
    uint16_t clear_s;
    uint8_t  reserved[6];
} zbc_button_v6_t;
_Static_assert(sizeof(zbc_button_v6_t) == 280, "version 6 stored 280 bytes a button");

/* A button as the table keeps it, and as version 7 stores it: everything but
 * its rules, which are in the pool. Frozen like the records above, since it is
 * a description of bytes in flash; zbc_button_t is free to change. */
typedef struct {
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
    uint8_t  input;
    uint8_t  pad;
    uint16_t clear_s;
    /* Zero. A field added in here later changes nothing stored, where one added
     * after it would bump BUTTON_BLOB_VERSION. */
    uint8_t  reserved[8];
} bdb_head_t;
_Static_assert(sizeof(bdb_head_t) == 64, "version 7 stores 64 bytes a button");

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

/* The fields every version from 2 on has, alike - and the table's heads. */
#define COPY_V2_FIELDS(b, in)                              \
    do {                                                   \
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

/* The table, laid out as version 7 stores it: the header, the heads of the
 * buttons in the order they were made, and then every rule of every button -
 * the first button's, the second's and so on, as its rule_count says. The
 * first hdr.count heads and the first hdr.rules rules are in use. Stored, the
 * rules follow the last head in use; here they start after the last head
 * there is room for, so neither moves the other, and bdb_save_locked closes
 * the gap for the write. */
static struct {
    button_blob_header_t hdr;
    bdb_head_t           head[CONFIG_ZBC_MAX_BUTTONS];
    zbc_btn_rule_t       rule[CONFIG_ZBC_BUTTON_POOL];
} s_table;
_Static_assert(offsetof(__typeof__(s_table), head) == sizeof(button_blob_header_t),
               "the heads must follow the header, as they are stored");


static int find_index(uint16_t id)
{
    for (int i = 0; i < s_table.hdr.count; i++) {
        if (s_table.head[i].id == id) {
            return i;
        }
    }
    return -1;
}

static int head_total(const bdb_head_t *h)
{
    int total = 0;
    for (int e = 0; e < OLD_EV_COUNT_V4; e++) {
        total += h->rule_count[e];
    }
    return total;
}

/* Where button i's rules start in the pool. */
static int rules_at(int i)
{
    int at = 0;
    for (int j = 0; j < i; j++) {
        at += head_total(&s_table.head[j]);
    }
    return at;
}

static void head_from(bdb_head_t *h, const zbc_button_t *b)
{
    memset(h, 0, sizeof(*h));
    COPY_V2_FIELDS(h, *b);
    memcpy(h->rule_count, b->rule_count, sizeof(h->rule_count));
    h->input   = b->input;
    h->clear_s = b->clear_s;
}

/* Button i whole, as the table hands it out. */
static void copy_out(int i, zbc_button_t *out)
{
    const bdb_head_t *h = &s_table.head[i];
    memset(out, 0, sizeof(*out));
    COPY_V2_FIELDS(out, *h);
    memcpy(out->rule_count, h->rule_count, sizeof(out->rule_count));
    out->input   = h->input;
    out->clear_s = h->clear_s;
    memcpy(out->rules, &s_table.rule[rules_at(i)], (size_t)head_total(h) * sizeof(out->rules[0]));
}

/* Makes the `have` rules at `at` into `want`, moving the ones behind them;
 * false, with nothing moved, when the pool cannot hold them. */
static bool resize_rules(int at, int have, int want)
{
    const int used = s_table.hdr.rules;
    if (used - have + want > CONFIG_ZBC_BUTTON_POOL) {
        return false;
    }
    memmove(&s_table.rule[at + want], &s_table.rule[at + have],
            (size_t)(used - at - have) * sizeof(s_table.rule[0]));
    s_table.hdr.rules = (uint16_t)(used - have + want);
    return true;
}

/* A button behind the last one, for the load; false when the table or the
 * pool is full. */
static bool append_locked(const zbc_button_t *b)
{
    const int n = button_db_rule_total(b);
    if (s_table.hdr.count >= CONFIG_ZBC_MAX_BUTTONS || s_table.hdr.rules + n > CONFIG_ZBC_BUTTON_POOL) {
        return false;
    }
    head_from(&s_table.head[s_table.hdr.count], b);
    memcpy(&s_table.rule[s_table.hdr.rules], b->rules, (size_t)n * sizeof(b->rules[0]));
    s_table.hdr.count++;
    s_table.hdr.rules = (uint16_t)(s_table.hdr.rules + n);
    return true;
}


/* Every version bdb_load converts, at the size its record had. Version 7's
 * counts must add up to the rules stored behind it, or no button's rules
 * could be found. */
bool button_db_blob_ok(const void *blob, size_t len)
{
    button_blob_header_t hdr;
    if (!blob || len < sizeof(hdr)) {
        return false;
    }
    memcpy(&hdr, blob, sizeof(hdr));
    size_t rec;
    switch (hdr.version) {
    case BUTTON_BLOB_VERSION: {
        if (len != sizeof(hdr) + (size_t)hdr.count * sizeof(bdb_head_t) +
                   (size_t)hdr.rules * sizeof(zbc_btn_rule_t)) {
            return false;
        }
        size_t total = 0;
        for (int i = 0; i < hdr.count; i++) {
            bdb_head_t h;
            memcpy(&h, (const uint8_t *)blob + sizeof(hdr) + (size_t)i * sizeof(h), sizeof(h));
            total += (size_t)head_total(&h);
        }
        return total == hdr.rules;
    }
    case 6: rec = sizeof(zbc_button_v6_t); break;
    case 5: rec = sizeof(zbc_button_v5_t); break;
    case 4: rec = sizeof(zbc_button_v4_t); break;
    case 3: rec = sizeof(zbc_button_v3_t); break;
    case 2: rec = sizeof(zbc_button_v2_t); break;
    case 1: rec = sizeof(zbc_button_v1_t); break;
    default: return false;
    }
    return len == sizeof(hdr) + (size_t)hdr.count * rec;
}

size_t button_db_blob_max(void)
{
    const size_t now = sizeof(button_blob_header_t) + sizeof(s_table.head) + sizeof(s_table.rule);
    const size_t v6  = sizeof(button_blob_header_t) + OLD_MAX_BUTTONS * sizeof(zbc_button_v6_t);
    return now > v6 ? now : v6;
}

/* Record i of a blob that button_db_blob_ok passed, as a current button.
 * `pool` is where version 7's next rules are; each version 7 record takes its
 * own from there. */
static void decode_record(uint32_t version, const uint8_t *recs, int i, const uint8_t **pool,
                          zbc_button_t *b)
{
    memset(b, 0, sizeof(*b));
    if (version == BUTTON_BLOB_VERSION) {
        bdb_head_t h;
        memcpy(&h, recs + (size_t)i * sizeof(h), sizeof(h));
        COPY_V2_FIELDS(b, h);
        b->input   = h.input;
        b->clear_s = h.clear_s;
        const int n = head_total(&h);
        /* From a build that let a button have more: it keeps its name and its
         * source, and loses its rules. */
        if (n <= CONFIG_ZBC_BUTTON_RULES) {
            memcpy(b->rule_count, h.rule_count, sizeof(b->rule_count));
            memcpy(b->rules, *pool, (size_t)n * sizeof(b->rules[0]));
        }
        *pool += (size_t)n * sizeof(zbc_btn_rule_t);
    } else if (version == 6) {
        /* As now, but stored with room for twelve. */
        const zbc_button_v6_t *in = (const zbc_button_v6_t *)recs + i;
        COPY_V2_FIELDS(b, *in);
        memcpy(b->rule_count, in->rule_count, sizeof(b->rule_count));
        memcpy(b->rules, in->rules, sizeof(in->rules));
        b->input   = in->input;
        b->clear_s = in->clear_s;
    } else if (version == 5) {
        /* The same record less the delays and the no-motion time; version 5 had
         * no sensors, so every clear_s stays zero. */
        const zbc_button_v5_t *in = (const zbc_button_v5_t *)recs + i;
        COPY_V2_FIELDS(b, *in);
        memcpy(b->rule_count, in->rule_count, sizeof(b->rule_count));
        for (int r = 0; r < OLD_POOL; r++) {
            const zbc_btn_rule_v5_t *o = &in->rules[r];
            zbc_btn_rule_t          *n = &b->rules[r];
            n->group_id   = o->group_id;
            n->cond       = o->cond;
            n->cond_arg   = o->cond_arg;
            n->action     = o->action;
            n->action_arg = o->action_arg;
            n->flags      = o->flags;
            n->ep         = o->ep;
            memcpy(n->ieee, o->ieee, sizeof(n->ieee));
        }
        b->input = in->input;
    } else if (version == 4) {
        const zbc_button_v4_t *in = (const zbc_button_v4_t *)recs + i;
        COPY_V2_FIELDS(b, *in);
        copy_old_rules(b, OLD_EV_COUNT_V4, in->rule_count, in->rules);
        b->input = in->input;
    } else if (version == 3) {
        const zbc_button_v3_t *in = (const zbc_button_v3_t *)recs + i;
        COPY_V2_FIELDS(b, *in);
        copy_old_rules(b, OLD_EV_COUNT, in->rule_count, in->rules);
        b->input = in->input;
    } else if (version == 2) {
        /* Every button before version 3 was a push button. */
        const zbc_button_v2_t *in = (const zbc_button_v2_t *)recs + i;
        COPY_V2_FIELDS(b, *in);
        copy_old_rules(b, OLD_EV_COUNT, in->rule_count, in->rules);
        b->input = ZBC_INPUT_BUTTON;
    } else {
        /* Every button that existed before remotes did was a pin. */
        const zbc_button_v1_t *in = (const zbc_button_v1_t *)recs + i;
        b->id       = in->id;
        memcpy(b->name, in->name, sizeof(b->name));
        b->src      = ZBC_BTN_SRC_GPIO;
        b->gpio     = in->gpio;
        b->inverted = in->inverted;
        b->pull     = in->pull;
        copy_old_rules(b, OLD_EV_COUNT, in->rule_count, in->rules);
    }
    /* button_db_rules() trusts the counts; a version 5 or 6 record whose counts
     * run past its twelve keeps its name and source and loses its rules. */
    if (version < BUTTON_BLOB_VERSION && button_db_rule_total(b) > OLD_POOL) {
        memset(b->rule_count, 0, sizeof(b->rule_count));
    }
}

/* A blob that button_db_blob_ok passed, into the empty table, one current
 * button at a time through `b`, whatever its version. A table from a build
 * with more room keeps the buttons that fit here; returns how many did not. */
static int table_load(const uint8_t *blob, zbc_button_t *b)
{
    button_blob_header_t hdr;
    memcpy(&hdr, blob, sizeof(hdr));
    const uint8_t *recs = blob + sizeof(hdr);
    const uint8_t *pool = recs + (size_t)hdr.count * sizeof(bdb_head_t);
    int dropped = 0;
    for (int i = 0; i < hdr.count; i++) {
        decode_record(hdr.version, recs, i, &pool, b);
        if (!append_locked(b)) {
            dropped++;
        }
    }
    return dropped;
}

/* Button i becomes `in`, its rules where its old ones were; false, with
 * nothing changed, when the pool has no room for them. */
static bool table_set(int i, const zbc_button_t *in)
{
    const uint16_t id   = s_table.head[i].id;
    const int      at   = rules_at(i);
    const int      want = button_db_rule_total(in);
    if (!resize_rules(at, head_total(&s_table.head[i]), want)) {
        return false;
    }
    memcpy(&s_table.rule[at], in->rules, (size_t)want * sizeof(in->rules[0]));
    head_from(&s_table.head[i], in);
    s_table.head[i].id = id;           /* the key is ours, not the caller's */
    return true;
}

static void table_remove(int i)
{
    resize_rules(rules_at(i), head_total(&s_table.head[i]), 0);
    memmove(&s_table.head[i], &s_table.head[i + 1],
            (size_t)(s_table.hdr.count - i - 1) * sizeof(s_table.head[0]));
    s_table.hdr.count--;
}

/* The table is the blob, once its rules follow the last head in use: they
 * are moved down for `write` and back after it, which costs no copy of it.
 * Nothing reads the heads past hdr.count, and the caller's lock keeps
 * everyone else out meanwhile. */
static int table_write(int (*write)(void *ctx, const void *blob, size_t len), void *ctx)
{
    s_table.hdr.version = BUTTON_BLOB_VERSION;
    uint8_t     *gap   = (uint8_t *)&s_table.head[s_table.hdr.count];
    const size_t rules = (size_t)s_table.hdr.rules * sizeof(s_table.rule[0]);
    memmove(gap, s_table.rule, rules);
    const int err = write(ctx, &s_table, (size_t)(gap - (uint8_t *)&s_table) + rules);
    memmove(s_table.rule, gap, rules);
    return err;
}

/* >>> end host-test region: bdb-table <<< */

/* Set by a load of an older version, so the converted table is written back
 * once rather than being converted again at every boot. */
static bool              s_migrated;
static SemaphoreHandle_t s_lock;

#define BDB_LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define BDB_UNLOCK() xSemaphoreGiveRecursive(s_lock)

static int nvs_write(void *ctx, const void *blob, size_t len)
{
    return nvs_set_blob(*(nvs_handle_t *)ctx, BUTTON_NVS_KEY, blob, len);
}

static esp_err_t bdb_save_locked(void)
{
    zbc_state_bump();          /* the page shows this table */
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(BUTTON_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = table_write(nvs_write, &nvs);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
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

    uint8_t      *blob = malloc(blob_size);
    zbc_button_t *b    = malloc(sizeof(*b));
    if (!blob || !b) {
        free(blob);
        free(b);
        nvs_close(nvs);
        return;
    }
    err = nvs_get_blob(nvs, BUTTON_NVS_KEY, blob, &blob_size);
    nvs_close(nvs);
    if (err != ESP_OK || !button_db_blob_ok(blob, blob_size)) {
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "Stored buttons are from an incompatible build, discarding");
        }
        free(blob);
        free(b);
        return;
    }
    button_blob_header_t hdr;
    memcpy(&hdr, blob, sizeof(hdr));
    const int dropped = table_load(blob, b);
    free(blob);
    free(b);
    if (hdr.version != BUTTON_BLOB_VERSION) {
        ESP_LOGI(TAG, "Converted %d button(s) from the previous format", s_table.hdr.count);
        s_migrated = true;
    }
    if (dropped) {
        ESP_LOGW(TAG, "%d stored button(s) did not fit: %d buttons and %d rules at most",
                 dropped, CONFIG_ZBC_MAX_BUTTONS, CONFIG_ZBC_BUTTON_POOL);
    }
    ESP_LOGI(TAG, "Restored %d button(s) with %d rule(s) from NVS", s_table.hdr.count, s_table.hdr.rules);
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
    memset(&s_table, 0, sizeof(s_table));
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
    if (s_table.hdr.count >= CONFIG_ZBC_MAX_BUTTONS) {
        BDB_UNLOCK();
        ESP_LOGE(TAG, "Button table full (%d)", CONFIG_ZBC_MAX_BUTTONS);
        return ESP_ERR_NO_MEM;
    }
    uint16_t next = 1;
    for (int i = 0; i < s_table.hdr.count; i++) {
        if (s_table.head[i].id >= next) {
            next = s_table.head[i].id + 1;
        }
    }

    /* No rules yet, so nothing in the pool moves. */
    bdb_head_t *h = &s_table.head[s_table.hdr.count++];
    memset(h, 0, sizeof(*h));
    h->id       = next;
    h->gpio     = -1;                  /* not wired up until someone picks a pin */
    h->pull     = ZBC_PULL_UP;         /* a button to GND is what almost everyone wires */
    h->inverted = true;
    strlcpy(h->name, name, sizeof(h->name));

    if (out_id) {
        *out_id = h->id;
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
    } else if (in->src == ZBC_BTN_SRC_SENSOR) {
        /* A sensor has a motion and a no-motion event and nothing else, and a
         * time for the second. */
        if (in->gpio != -1 || in->zb_key != 0 || in->clear_s < ZBC_SENSOR_CLEAR_MIN_S ||
            in->clear_s > ZBC_SENSOR_CLEAR_MAX_S || in->rule_count[ZBC_EV_DOUBLE] != 0 ||
            in->rule_count[ZBC_EV_TRIPLE] != 0) {
            return false;
        }
    } else if (in->zb_ep != 0 || in->zb_key != 0) {
        return false;
    }
    if (in->src != ZBC_BTN_SRC_SENSOR && in->clear_s != 0) {
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
            /* A dim moves while a button is held, and a sensor is never held;
             * a dim that waits would start after the hold that meant it. */
            if (is_dim(rule->action) && (rule->delay_s != 0 || in->src == ZBC_BTN_SRC_SENSOR)) {
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
    const int i = find_index(id);
    if (i < 0) {
        BDB_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    if (!table_set(i, in)) {
        ESP_LOGW(TAG, "No room for %d rule(s) on button %u: %d of %d in use",
                 button_db_rule_total(in), id, s_table.hdr.rules, CONFIG_ZBC_BUTTON_POOL);
        BDB_UNLOCK();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = bdb_save_locked();
    BDB_UNLOCK();
    return err;
}

esp_err_t button_db_remove(uint16_t id)
{
    BDB_LOCK();
    const int i = find_index(id);
    if (i < 0) {
        BDB_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    table_remove(i);
    esp_err_t err = bdb_save_locked();
    BDB_UNLOCK();
    return err;
}

bool button_db_get(uint16_t id, zbc_button_t *out)
{
    BDB_LOCK();
    const int i = find_index(id);
    if (i >= 0 && out) {
        copy_out(i, out);
    }
    BDB_UNLOCK();
    return i >= 0;
}

size_t button_db_list_ids(uint16_t *ids, size_t max)
{
    size_t n = 0;
    BDB_LOCK();
    for (int i = 0; i < s_table.hdr.count && n < max; i++) {
        ids[n++] = s_table.head[i].id;
    }
    BDB_UNLOCK();
    return n;
}

int button_db_rules_room(uint16_t id)
{
    int room = 0;
    BDB_LOCK();
    const int i = find_index(id);
    if (i >= 0) {
        room = CONFIG_ZBC_BUTTON_POOL - s_table.hdr.rules + head_total(&s_table.head[i]);
    }
    BDB_UNLOCK();
    return room < CONFIG_ZBC_BUTTON_RULES ? room : CONFIG_ZBC_BUTTON_RULES;
}

bool button_db_pin_taken(int gpio, uint16_t except_id)
{
    if (gpio < 0) {
        return false;
    }
    bool taken = false;
    BDB_LOCK();
    for (int i = 0; i < s_table.hdr.count && !taken; i++) {
        taken = s_table.head[i].id != except_id && s_table.head[i].gpio == gpio;
    }
    BDB_UNLOCK();
    return taken;
}

bool button_db_has_source(const zbc_button_t *b)
{
    if (!b) {
        return false;
    }
    if (b->src == ZBC_BTN_SRC_ZIGBEE || b->src == ZBC_BTN_SRC_SENSOR) {
        for (int i = 0; i < ZBC_BTN_IEEE_LEN; i++) {
            if (b->ieee[i]) {
                return b->zb_ep != 0;
            }
        }
        return false;
    }
    return b->gpio >= 0;
}

static bool source_taken(uint8_t src, const uint8_t ieee[ZBC_BTN_IEEE_LEN], uint8_t ep, uint8_t key,
                         uint16_t except_id)
{
    if (!ieee || ep == 0) {
        return false;
    }
    bool taken = false;
    BDB_LOCK();
    for (int i = 0; i < s_table.hdr.count && !taken; i++) {
        const bdb_head_t *h = &s_table.head[i];
        taken = h->id != except_id && h->src == src && h->zb_ep == ep && h->zb_key == key &&
                memcmp(h->ieee, ieee, ZBC_BTN_IEEE_LEN) == 0;
    }
    BDB_UNLOCK();
    return taken;
}

bool button_db_key_taken(const uint8_t ieee[ZBC_BTN_IEEE_LEN], uint8_t ep, uint8_t key,
                         uint16_t except_id)
{
    return source_taken(ZBC_BTN_SRC_ZIGBEE, ieee, ep, key, except_id);
}

bool button_db_sensor_taken(const uint8_t ieee[ZBC_BTN_IEEE_LEN], uint8_t ep, uint16_t except_id)
{
    return source_taken(ZBC_BTN_SRC_SENSOR, ieee, ep, 0, except_id);
}

void button_db_forget_device(const uint8_t ieee[ZBC_BTN_IEEE_LEN])
{
    if (!ieee) {
        return;
    }
    bool changed = false;
    BDB_LOCK();
    for (int i = 0; i < s_table.hdr.count; i++) {
        bdb_head_t *h = &s_table.head[i];
        if ((h->src == ZBC_BTN_SRC_ZIGBEE || h->src == ZBC_BTN_SRC_SENSOR) &&
            memcmp(h->ieee, ieee, ZBC_BTN_IEEE_LEN) == 0) {
            memset(h->ieee, 0, ZBC_BTN_IEEE_LEN);
            h->zb_ep  = 0;
            h->zb_key = 0;
            changed = true;
        }
    }
    /* A light that is gone leaves its rules unassigned rather than deleted,
     * as a room does - and never pointing at the address, which a wired
     * light or a Matter lamp made later may be given again. Every rule in the
     * pool is some button's. */
    for (int r = 0; r < s_table.hdr.rules; r++) {
        zbc_btn_rule_t *rule = &s_table.rule[r];
        if (rule->ep != 0 && memcmp(rule->ieee, ieee, ZBC_BTN_IEEE_LEN) == 0) {
            memset(rule->ieee, 0, ZBC_BTN_IEEE_LEN);
            rule->ep = 0;
            changed = true;
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
    for (int r = 0; r < s_table.hdr.rules; r++) {
        zbc_btn_rule_t *rule = &s_table.rule[r];
        if (rule->ep == 0 && rule->group_id == group_id) {
            rule->group_id = 0;
            changed = true;
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
    memset(&s_table, 0, sizeof(s_table));
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
