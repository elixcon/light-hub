#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"

#include "light_db.h"
#include "state_rev.h"

static const char *TAG = "light_db";

/* Same namespace as the other tables, its own key and its own version: adding
 * wired lights must not discard the devices, groups, curves or buttons. */
#define LIGHT_NVS_NAMESPACE "zbc"
/* In config_backup.c's list too: a backup carries this table under this key. */
#define LIGHT_NVS_KEY       "lights"
/* 3 added the three-channel light and the drawn mixing curves. A table from
 * another version is discarded, not converted: a wired light is a few fields
 * on one page, quicker to define again than a conversion is to keep correct. */
#define LIGHT_BLOB_VERSION  3

typedef struct {
    uint32_t version;
    uint16_t count;
    uint16_t reserved;
} light_blob_header_t;

static void set_tunable_defaults(zbc_light_t *l)
{
    l->gpio_warm    = -1;
    l->gpio_neutral = -1;
    l->mix          = ZBC_MIX_CONSTANT;
    l->pwm_hz       = ZBC_LIGHT_DEFAULT_HZ;
    l->cold_k       = ZBC_LIGHT_DEFAULT_COLD_K;
    l->neutral_k    = ZBC_LIGHT_DEFAULT_NEUTRAL_K;
    l->warm_k       = ZBC_LIGHT_DEFAULT_WARM_K;
    memset(l->curve, 0, sizeof(l->curve));
}

static zbc_light_t       s_lights[CONFIG_ZBC_MAX_LIGHTS];
static bool              s_discarded;       /* a table of another format was found */
static SemaphoreHandle_t s_lock;

static void forget_stored_lights(void);

#define LDB_LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define LDB_UNLOCK() xSemaphoreGiveRecursive(s_lock)

/* >>> host-test region: lightaddr <<< */
/* 02:5A:42:43:4C:00:<id>. Bit 1 of the first byte is the locally administered
 * flag, which no manufacturer-assigned EUI-64 carries, so this range cannot
 * collide with a real device joining the network. "ZBCL" in the middle is only
 * there to make the address recognisable in a log. */
static const uint8_t LIGHT_IEEE_PREFIX[6] = { 0x02, 0x5A, 0x42, 0x43, 0x4C, 0x00 };
/* A Matter lamp's: the same range, "ZBCM". The two prefixes differ in byte 4,
 * so neither id function can take the other kind's address for its own. */
static const uint8_t MLIGHT_IEEE_PREFIX[6] = { 0x02, 0x5A, 0x42, 0x43, 0x4D, 0x00 };

static void made_up_ieee(const uint8_t prefix[6], uint16_t id, uint8_t out[8])
{
    for (int i = 0; i < 6; i++) {
        out[i] = prefix[i];
    }
    out[6] = (uint8_t)(id >> 8);
    out[7] = (uint8_t)id;
}

static uint16_t made_up_id(const uint8_t prefix[6], const uint8_t ieee[8])
{
    for (int i = 0; i < 6; i++) {
        if (ieee[i] != prefix[i]) {
            return 0;
        }
    }
    return (uint16_t)((ieee[6] << 8) | ieee[7]);
}

void zbc_light_ieee(uint16_t id, uint8_t out[8])
{
    made_up_ieee(LIGHT_IEEE_PREFIX, id, out);
}

uint16_t zbc_light_id_from_ieee(const uint8_t ieee[8])
{
    return made_up_id(LIGHT_IEEE_PREFIX, ieee);
}

void zbc_mlight_ieee(uint16_t id, uint8_t out[8])
{
    made_up_ieee(MLIGHT_IEEE_PREFIX, id, out);
}

uint16_t zbc_mlight_id_from_ieee(const uint8_t ieee[8])
{
    return made_up_id(MLIGHT_IEEE_PREFIX, ieee);
}

bool zbc_light_boot_on(uint8_t boot, int stored)
{
    switch (boot) {
    case ZBC_LIGHT_BOOT_ON:
        return true;
    case ZBC_LIGHT_BOOT_LAST:
        return stored == 1;          /* nothing stored yet reads as off */
    default:
        return false;                /* including a value from a later build */
    }
}

int zbc_light_pin_level(bool on, bool active_low)
{
    return on != active_low ? 1 : 0;
}
/* >>> end host-test region: lightaddr <<< */

static zbc_light_t *find_by_id(uint16_t id)
{
    for (int i = 0; i < CONFIG_ZBC_MAX_LIGHTS; i++) {
        if (s_lights[i].in_use && s_lights[i].id == id) {
            return &s_lights[i];
        }
    }
    return NULL;
}

static esp_err_t ldb_save_locked(void)
{
    zbc_state_bump();          /* the page shows this table */
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(LIGHT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    size_t used = 0;
    for (int i = 0; i < CONFIG_ZBC_MAX_LIGHTS; i++) {
        used += s_lights[i].in_use;
    }

    size_t   blob_size = sizeof(light_blob_header_t) + used * sizeof(zbc_light_t);
    uint8_t *blob      = calloc(1, blob_size);
    if (!blob) {
        nvs_close(nvs);
        return ESP_ERR_NO_MEM;
    }

    light_blob_header_t hdr = { .version = LIGHT_BLOB_VERSION, .count = (uint16_t)used };
    memcpy(blob, &hdr, sizeof(hdr));
    zbc_light_t *out = (zbc_light_t *)(blob + sizeof(hdr));
    for (int i = 0; i < CONFIG_ZBC_MAX_LIGHTS; i++) {
        if (s_lights[i].in_use) {
            *out++ = s_lights[i];
        }
    }

    err = nvs_set_blob(nvs, LIGHT_NVS_KEY, blob, blob_size);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    free(blob);
    nvs_close(nvs);
    return err;
}

bool light_db_blob_ok(const void *blob, size_t len)
{
    light_blob_header_t hdr;
    if (!blob || len < sizeof(hdr)) {
        return false;
    }
    memcpy(&hdr, blob, sizeof(hdr));
    return hdr.version == LIGHT_BLOB_VERSION && len == sizeof(hdr) + (size_t)hdr.count * sizeof(zbc_light_t);
}

static void ldb_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open(LIGHT_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    size_t    blob_size = 0;
    esp_err_t err       = nvs_get_blob(nvs, LIGHT_NVS_KEY, NULL, &blob_size);
    if (err != ESP_OK || blob_size < sizeof(light_blob_header_t)) {
        nvs_close(nvs);
        return;
    }
    uint8_t *blob = malloc(blob_size);
    if (!blob) {
        nvs_close(nvs);
        return;
    }
    err = nvs_get_blob(nvs, LIGHT_NVS_KEY, blob, &blob_size);
    nvs_close(nvs);
    if (err != ESP_OK) {
        free(blob);
        return;
    }

    light_blob_header_t hdr;
    memcpy(&hdr, blob, sizeof(hdr));

    int loaded = 0;
    if (light_db_blob_ok(blob, blob_size)) {
        const zbc_light_t *in = (const zbc_light_t *)(blob + sizeof(hdr));
        for (int i = 0; i < hdr.count && loaded < CONFIG_ZBC_MAX_LIGHTS; i++) {
            s_lights[loaded]        = in[i];
            s_lights[loaded].in_use = true;
            loaded++;
        }
    } else {
        ESP_LOGW(TAG, "Stored wired lights are from another build (format %lu), discarding - define them again",
                 (unsigned long)hdr.version);
        s_discarded = true;
        free(blob);
        return;
    }
    free(blob);
    ESP_LOGI(TAG, "Restored %d wired light(s) from NVS", loaded);
}

esp_err_t light_db_init(void)
{
    if (s_lock) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    memset(s_lights, 0, sizeof(s_lights));
    LDB_LOCK();
    ldb_load();
    if (s_discarded) {
        forget_stored_lights();
        s_discarded = false;
    }
    LDB_UNLOCK();
    return ESP_OK;
}

esp_err_t light_db_create(const char *name, uint16_t *out_id)
{
    if (!name || !name[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    LDB_LOCK();
    zbc_light_t *slot = NULL;
    uint16_t     next = 1;
    for (int i = 0; i < CONFIG_ZBC_MAX_LIGHTS; i++) {
        if (!s_lights[i].in_use) {
            if (!slot) {
                slot = &s_lights[i];
            }
        } else if (s_lights[i].id >= next) {
            next = s_lights[i].id + 1;
        }
    }
    if (!slot) {
        LDB_UNLOCK();
        ESP_LOGE(TAG, "Wired light table full (%d)", CONFIG_ZBC_MAX_LIGHTS);
        return ESP_ERR_NO_MEM;
    }
    memset(slot, 0, sizeof(*slot));
    slot->in_use = true;
    slot->id     = next;
    slot->kind   = ZBC_LIGHT_ONOFF;
    slot->gpio   = -1;                  /* not wired up until someone picks a pin */
    slot->boot   = ZBC_LIGHT_BOOT_OFF;
    set_tunable_defaults(slot);
    strlcpy(slot->name, name, sizeof(slot->name));
    if (out_id) {
        *out_id = slot->id;
    }
    esp_err_t err = ldb_save_locked();
    LDB_UNLOCK();
    return err;
}

/* The pins themselves are checked by the caller, which knows what this chip
 * has; this is only what no light could ever be. */
static bool valid_light(const zbc_light_t *in)
{
    for (int c = 0; c < ZBC_PWM_MAX_LIGHT_CH; c++) {
        if (!zbc_mix_curve_valid(&in->curve[c])) {
            return false;
        }
    }
    if (in->kind == ZBC_LIGHT_CNW && !(in->warm_k < in->neutral_k && in->neutral_k < in->cold_k)) {
        return false;
    }
    return in->name[0] && in->kind < ZBC_LIGHT_KIND_COUNT && in->boot < ZBC_LIGHT_BOOT_COUNT &&
           in->gpio >= -1 && in->gpio_warm >= -1 && in->gpio_neutral >= -1 && in->mix < ZBC_MIX_COUNT &&
           in->pwm_hz >= ZBC_PWM_HZ_MIN && in->pwm_hz <= ZBC_PWM_HZ_MAX &&
           in->cold_k <= ZBC_LIGHT_KELVIN_MAX && in->warm_k >= ZBC_LIGHT_KELVIN_MIN &&
           in->warm_k < in->cold_k;
}

esp_err_t light_db_set(uint16_t id, const zbc_light_t *in)
{
    if (!in || !valid_light(in)) {
        return ESP_ERR_INVALID_ARG;
    }
    LDB_LOCK();
    zbc_light_t *l = find_by_id(id);
    if (!l) {
        LDB_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    *l        = *in;
    l->in_use = true;
    l->id     = id;                     /* the key is ours, not the caller's */
    memset(l->reserved, 0, sizeof(l->reserved));
    esp_err_t err = ldb_save_locked();
    LDB_UNLOCK();
    return err;
}

esp_err_t light_db_rename(uint16_t id, const char *name)
{
    if (!name || !name[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    LDB_LOCK();
    zbc_light_t *l = find_by_id(id);
    if (!l) {
        LDB_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(l->name, name, sizeof(l->name));
    esp_err_t err = ldb_save_locked();
    LDB_UNLOCK();
    return err;
}

/* The state key, and the one an on/off light used before tunable white, which
 * held a bare u8. A new key rather than a new type under the old one: NVS does
 * not promise what writing a blob over a u8 of the same name does. */
static void state_key(uint16_t id, char *out, size_t cap)
{
    snprintf(out, cap, "lstate%u", (unsigned)id);
}

static void legacy_key(uint16_t id, char *out, size_t cap)
{
    snprintf(out, cap, "light%u", (unsigned)id);
}

static void erase_state_keys(nvs_handle_t nvs, uint16_t id)
{
    char key[16];
    state_key(id, key, sizeof(key));
    nvs_erase_key(nvs, key);
    legacy_key(id, key, sizeof(key));
    nvs_erase_key(nvs, key);
}

/* lstate<id> or the older light<id>, of any light. */
static bool is_state_key(const char *key)
{
    const char *digits = strncmp(key, "lstate", 6) == 0 ? key + 6
                         : strncmp(key, "light", 5) == 0 ? key + 5 : NULL;
    if (!digits || !*digits) {
        return false;
    }
    for (; *digits; digits++) {
        if (*digits < '0' || *digits > '9') {
            return false;
        }
    }
    return true;
}

/* A discarded table goes with every state its lights stored, whoever they
 * were: the ids are given out again from 1, and a light defined anew would
 * otherwise come up with the level, the colour and - set to "as it was" - the
 * on/off of the old light that had its id. One key at a time, the walk started
 * again after each, since the iterator is not to be trusted across an erase. */
static void forget_stored_lights(void)
{
    nvs_handle_t nvs;
    if (nvs_open(LIGHT_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    int  erased = 0;
    bool found  = true;
    while (found) {
        char           key[NVS_KEY_NAME_MAX_SIZE] = "";
        nvs_iterator_t it                         = NULL;
        esp_err_t      err                        = nvs_entry_find_in_handle(nvs, NVS_TYPE_ANY, &it);
        found = false;
        while (err == ESP_OK && !found) {
            nvs_entry_info_t info;
            if (nvs_entry_info(it, &info) == ESP_OK && is_state_key(info.key)) {
                strlcpy(key, info.key, sizeof(key));
                found = true;
            } else {
                err = nvs_entry_next(&it);
            }
        }
        nvs_release_iterator(it);
        if (found) {
            found = nvs_erase_key(nvs, key) == ESP_OK;
            erased += found;
        }
    }
    nvs_erase_key(nvs, LIGHT_NVS_KEY);
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Forgot the discarded wired lights and %d stored state(s)", erased);
}

esp_err_t light_db_remove(uint16_t id)
{
    LDB_LOCK();
    zbc_light_t *l = find_by_id(id);
    if (!l) {
        LDB_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    memset(l, 0, sizeof(*l));
    esp_err_t err = ldb_save_locked();

    /* Or a light created later with the same id would inherit it. */
    nvs_handle_t nvs;
    if (nvs_open(LIGHT_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        erase_state_keys(nvs, id);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    LDB_UNLOCK();
    return err;
}

bool light_db_get(uint16_t id, zbc_light_t *out)
{
    LDB_LOCK();
    zbc_light_t *l = find_by_id(id);
    if (l && out) {
        *out = *l;
    }
    bool found = l != NULL;
    LDB_UNLOCK();
    return found;
}

size_t light_db_snapshot(zbc_light_t *buf, size_t max)
{
    size_t n = 0;
    LDB_LOCK();
    for (int i = 0; i < CONFIG_ZBC_MAX_LIGHTS && n < max; i++) {
        if (s_lights[i].in_use) {
            buf[n++] = s_lights[i];
        }
    }
    LDB_UNLOCK();
    return n;
}

bool light_db_pin_taken(int gpio, uint16_t except_id)
{
    if (gpio < 0) {
        return false;
    }
    bool taken = false;
    LDB_LOCK();
    for (int i = 0; i < CONFIG_ZBC_MAX_LIGHTS && !taken; i++) {
        const zbc_light_t *l = &s_lights[i];
        taken = l->in_use && l->id != except_id &&
                (l->gpio == gpio || (zbc_light_tunable(l->kind) && l->gpio_warm == gpio) ||
                 (l->kind == ZBC_LIGHT_CNW && l->gpio_neutral == gpio));
    }
    LDB_UNLOCK();
    return taken;
}

bool light_db_load_state(uint16_t id, zbc_light_state_t *out)
{
    nvs_handle_t nvs;
    if (nvs_open(LIGHT_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    char   key[16];
    size_t size  = sizeof(*out);
    bool   found = false;
    state_key(id, key, sizeof(key));
    if (nvs_get_blob(nvs, key, out, &size) == ESP_OK && size == sizeof(*out)) {
        found = true;
    } else {
        uint8_t on = 0;
        legacy_key(id, key, sizeof(key));
        if (nvs_get_u8(nvs, key, &on) == ESP_OK) {
            *out  = (zbc_light_state_t){ .on = on ? 1 : 0 };
            found = true;
        }
    }
    nvs_close(nvs);
    return found;
}

void light_db_save_state(uint16_t id, const zbc_light_state_t *st)
{
    /* Only written when it changed: a rewrite of the same value still costs a
     * flash entry, and a light is switched many times a day. */
    zbc_light_state_t now;
    if (light_db_load_state(id, &now) && memcmp(&now, st, sizeof(now)) == 0) {
        return;
    }
    nvs_handle_t nvs;
    if (nvs_open(LIGHT_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    char key[16];
    state_key(id, key, sizeof(key));
    if (nvs_set_blob(nvs, key, st, sizeof(*st)) == ESP_OK) {
        legacy_key(id, key, sizeof(key));
        nvs_erase_key(nvs, key);        /* read once as a fallback, then gone */
        nvs_commit(nvs);
    }
    nvs_close(nvs);
}

esp_err_t light_db_erase(void)
{
    LDB_LOCK();
    nvs_handle_t nvs;
    esp_err_t    err = nvs_open(LIGHT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        for (int i = 0; i < CONFIG_ZBC_MAX_LIGHTS; i++) {
            if (s_lights[i].in_use) {
                erase_state_keys(nvs, s_lights[i].id);
            }
        }
        err = nvs_erase_key(nvs, LIGHT_NVS_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    memset(s_lights, 0, sizeof(s_lights));
    LDB_UNLOCK();
    return err;
}
