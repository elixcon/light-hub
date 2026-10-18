#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "device_db.h"
#include "state_rev.h"

static const char *TAG = "device_db";

#define DB_NVS_NAMESPACE "zbc"
/* In config_backup.c's list too: a backup carries this table under this key. */
#define DB_NVS_KEY       "devices"
/* Bumped whenever zbc_device_t changes layout; a mismatch drops the stored
 * blob instead of reinterpreting it. */
#define DB_BLOB_VERSION  2

typedef struct {
    uint32_t version;
    uint16_t count;
    uint16_t reserved;
} db_blob_header_t;

static zbc_device_t      s_devices[CONFIG_ZBC_MAX_DEVICES];
static SemaphoreHandle_t s_lock;

/* Link quality from the last neighbour scan that saw each device, parallel to
 * s_devices by slot. It lives here rather than in zbc_device_t because the blob
 * is written raw: any new field changes sizeof(zbc_device_t), the load-time size
 * check rejects the stored table, and the names the user gave their lights are
 * gone. That price is not worth paying for a number that is meaningless a reboot
 * later anyway - lqa is a measurement of now, like online and last_seen_us. */
static int16_t           s_lqa[CONFIG_ZBC_MAX_DEVICES];

#define DB_LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define DB_UNLOCK() xSemaphoreGiveRecursive(s_lock)

static zbc_device_t *find_by_ieee(const uint8_t ieee[ZBC_IEEE_LEN])
{
    for (int i = 0; i < CONFIG_ZBC_MAX_DEVICES; i++) {
        if (s_devices[i].in_use && memcmp(s_devices[i].ieee, ieee, ZBC_IEEE_LEN) == 0) {
            return &s_devices[i];
        }
    }
    return NULL;
}

static zbc_device_t *find_by_short(uint16_t short_addr)
{
    if (!zbc_short_known(short_addr)) {
        return NULL;           /* every restored row not yet on this network */
    }
    for (int i = 0; i < CONFIG_ZBC_MAX_DEVICES; i++) {
        if (s_devices[i].in_use && s_devices[i].short_addr == short_addr) {
            return &s_devices[i];
        }
    }
    return NULL;
}

static esp_err_t db_save_locked(void)
{
    zbc_state_bump();          /* the page shows this table */
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(DB_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    size_t   used = 0;
    for (int i = 0; i < CONFIG_ZBC_MAX_DEVICES; i++) {
        if (s_devices[i].in_use) {
            used++;
        }
    }

    size_t blob_size = sizeof(db_blob_header_t) + used * sizeof(zbc_device_t);
    uint8_t *blob = calloc(1, blob_size);
    if (!blob) {
        nvs_close(nvs);
        return ESP_ERR_NO_MEM;
    }

    db_blob_header_t hdr = { .version = DB_BLOB_VERSION, .count = (uint16_t)used };
    memcpy(blob, &hdr, sizeof(hdr));

    zbc_device_t *out = (zbc_device_t *)(blob + sizeof(hdr));
    for (int i = 0; i < CONFIG_ZBC_MAX_DEVICES; i++) {
        if (!s_devices[i].in_use) {
            continue;
        }
        *out = s_devices[i];
        /* Runtime-only fields are not meaningful across a reboot. */
        out->online       = false;
        out->last_seen_us = 0;
        for (int e = 0; e < out->ep_count; e++) {
            /* Capabilities and the lamp's physical colour temperature range are
             * properties of the device; everything else is live state. */
            out->eps[e].on_off     = ZBC_UNKNOWN;
            out->eps[e].level      = ZBC_UNKNOWN;
            out->eps[e].ct_mireds  = ZBC_UNKNOWN;
            out->eps[e].hue        = ZBC_UNKNOWN;
            out->eps[e].saturation = ZBC_UNKNOWN;
            out->eps[e].color_mode = ZBC_UNKNOWN;
        }
        out++;
    }

    err = nvs_set_blob(nvs, DB_NVS_KEY, blob, blob_size);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    free(blob);
    nvs_close(nvs);
    return err;
}

bool device_db_blob_ok(const void *blob, size_t len)
{
    db_blob_header_t hdr;
    if (!blob || len < sizeof(hdr)) {
        return false;
    }
    memcpy(&hdr, blob, sizeof(hdr));
    return hdr.version == DB_BLOB_VERSION &&
           len == sizeof(hdr) + (size_t)hdr.count * sizeof(zbc_device_t);
}

void device_db_blob_forget_addresses(void *blob, size_t len)
{
    if (!device_db_blob_ok(blob, len)) {
        return;
    }
    db_blob_header_t hdr;
    memcpy(&hdr, blob, sizeof(hdr));
    uint8_t *row = (uint8_t *)blob + sizeof(hdr);
    for (int i = 0; i < hdr.count; i++, row += sizeof(zbc_device_t)) {
        const uint16_t none = ZBC_SHORT_NONE;
        memcpy(row + offsetof(zbc_device_t, short_addr), &none, sizeof(none));
    }
}

static void db_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open(DB_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        ESP_LOGI(TAG, "No stored device list yet");
        return;
    }

    size_t blob_size = 0;
    esp_err_t err = nvs_get_blob(nvs, DB_NVS_KEY, NULL, &blob_size);
    if (err != ESP_OK || blob_size < sizeof(db_blob_header_t)) {
        nvs_close(nvs);
        return;
    }

    uint8_t *blob = malloc(blob_size);
    if (!blob) {
        nvs_close(nvs);
        return;
    }
    err = nvs_get_blob(nvs, DB_NVS_KEY, blob, &blob_size);
    nvs_close(nvs);
    if (err != ESP_OK) {
        free(blob);
        return;
    }

    if (!device_db_blob_ok(blob, blob_size)) {
        ESP_LOGW(TAG, "Stored device list is from an incompatible build, discarding");
        free(blob);
        return;
    }
    db_blob_header_t hdr;
    memcpy(&hdr, blob, sizeof(hdr));

    const zbc_device_t *in = (const zbc_device_t *)(blob + sizeof(hdr));
    int loaded = 0;
    for (int i = 0; i < hdr.count && loaded < CONFIG_ZBC_MAX_DEVICES; i++) {
        s_devices[loaded]        = in[i];
        s_devices[loaded].in_use = true;
        s_devices[loaded].online = false;
        loaded++;
    }
    free(blob);
    ESP_LOGI(TAG, "Restored %d device(s) from NVS", loaded);
}

esp_err_t device_db_init(void)
{
    if (s_lock) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    memset(s_devices, 0, sizeof(s_devices));
    for (int i = 0; i < CONFIG_ZBC_MAX_DEVICES; i++) {
        s_lqa[i] = ZBC_UNKNOWN;
    }
    DB_LOCK();
    db_load();
    DB_UNLOCK();
    return ESP_OK;
}

esp_err_t device_db_upsert(const uint8_t ieee[ZBC_IEEE_LEN], uint16_t short_addr, bool *created)
{
    esp_err_t err = ESP_OK;
    if (created) {
        *created = false;
    }

    DB_LOCK();
    zbc_device_t *dev = find_by_ieee(ieee);
    if (!dev) {
        /* A rejoining device may still hold a stale row under a short address
         * that now belongs to nobody; the IEEE lookup above is authoritative. */
        for (int i = 0; i < CONFIG_ZBC_MAX_DEVICES; i++) {
            if (!s_devices[i].in_use) {
                dev = &s_devices[i];
                break;
            }
        }
        if (!dev) {
            DB_UNLOCK();
            ESP_LOGE(TAG, "Device table full (%d), ignoring new device", CONFIG_ZBC_MAX_DEVICES);
            return ESP_ERR_NO_MEM;
        }
        memset(dev, 0, sizeof(*dev));
        /* The slot may have belonged to a different device; its reading must not
         * be inherited by this one. */
        s_lqa[dev - s_devices] = ZBC_UNKNOWN;
        dev->in_use = true;
        memcpy(dev->ieee, ieee, ZBC_IEEE_LEN);
        char idstr[19];
        zbc_ieee_to_str(ieee, idstr);
        snprintf(dev->name, sizeof(dev->name), "Device %s", idstr + 12); /* last 3 bytes */
        if (created) {
            *created = true;
        }
    }
    /* The short address is persisted and changes on a rejoin, so it is saved for
     * an existing row too: a battery remote asleep again before the Basic read
     * lands would otherwise keep its old address across the next reboot, and
     * every press it sent would be "from a device we do not know". */
    const bool moved  = dev->short_addr != short_addr;
    if (!dev->online) {
        zbc_state_bump();
    }
    dev->short_addr   = short_addr;
    dev->online       = true;
    dev->last_seen_us = esp_timer_get_time();
    /* An address belongs to one device. A row still holding it from before - a
     * device that rejoined elsewhere, or left, without our hearing - would be
     * found first by find_by_short and take this device's frames as its own. It
     * gets its address back when it next announces. */
    if (moved && zbc_short_known(short_addr)) {
        for (int i = 0; i < CONFIG_ZBC_MAX_DEVICES; i++) {
            if (&s_devices[i] != dev && s_devices[i].in_use && s_devices[i].short_addr == short_addr) {
                s_devices[i].short_addr = ZBC_SHORT_NONE;
                s_devices[i].online     = false;
            }
        }
    }

    if ((created && *created) || moved) {
        err = db_save_locked();
    }
    DB_UNLOCK();
    return err;
}

esp_err_t device_db_set_identity(const uint8_t ieee[ZBC_IEEE_LEN], const char *manufacturer, const char *model)
{
    DB_LOCK();
    zbc_device_t *dev = find_by_ieee(ieee);
    if (!dev) {
        DB_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    /* Saved only when something stored actually changed. Interrogation runs
     * again on every announce, a lamp on a wall switch announces itself daily,
     * and a flash write from the Zigbee task stalls the mainloop. */
    bool changed = false;
    if (manufacturer && manufacturer[0] && strcmp(dev->manufacturer, manufacturer) != 0) {
        strlcpy(dev->manufacturer, manufacturer, sizeof(dev->manufacturer));
        changed = true;
    }
    if (model && model[0]) {
        if (strcmp(dev->model, model) != 0) {
            strlcpy(dev->model, model, sizeof(dev->model));
            changed = true;
        }
        /* Only replace the placeholder name, never a name the user picked. */
        if (strncmp(dev->name, "Device ", 7) == 0 && strcmp(dev->name, model) != 0) {
            strlcpy(dev->name, model, sizeof(dev->name));
            changed = true;
        }
    }
    dev->last_seen_us = esp_timer_get_time();
    dev->online       = true;
    esp_err_t err = changed ? db_save_locked() : ESP_OK;
    DB_UNLOCK();
    return err;
}

esp_err_t device_db_add_endpoint(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint16_t device_id,
                                 uint16_t features)
{
    DB_LOCK();
    zbc_device_t *dev = find_by_ieee(ieee);
    if (!dev) {
        DB_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    for (int i = 0; i < dev->ep_count; i++) {
        if (dev->eps[i].ep == ep) {
            /* Persisted when it changed, so a feature learned on a known endpoint
             * survives a reboot. */
            const bool changed = dev->eps[i].device_id != device_id ||
                                 (dev->eps[i].features | features) != dev->eps[i].features;
            dev->eps[i].device_id = device_id;
            dev->eps[i].features |= features;
            esp_err_t err = changed ? db_save_locked() : ESP_OK;
            DB_UNLOCK();
            return err;
        }
    }
    if (dev->ep_count >= CONFIG_ZBC_MAX_ENDPOINTS_PER_DEVICE) {
        DB_UNLOCK();
        return ESP_ERR_NO_MEM;
    }
    dev->eps[dev->ep_count++] = (zbc_endpoint_t){
        .ep            = ep,
        .device_id     = device_id,
        .features      = features,
        .on_off        = ZBC_UNKNOWN,
        .level         = ZBC_UNKNOWN,
        .ct_mireds     = ZBC_UNKNOWN,
        .ct_min_mireds = ZBC_UNKNOWN,
        .ct_max_mireds = ZBC_UNKNOWN,
        .hue           = ZBC_UNKNOWN,
        .saturation    = ZBC_UNKNOWN,
        .color_mode    = ZBC_UNKNOWN,
    };
    esp_err_t err = db_save_locked();
    DB_UNLOCK();
    return err;
}

esp_err_t device_db_add_features(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint16_t features)
{
    DB_LOCK();
    zbc_device_t *dev = find_by_ieee(ieee);
    esp_err_t     err = ESP_ERR_NOT_FOUND;
    if (dev) {
        for (int i = 0; i < dev->ep_count; i++) {
            if (dev->eps[i].ep == ep && (dev->eps[i].features & features) != features) {
                dev->eps[i].features |= features;
                err = db_save_locked();
                break;
            }
        }
    }
    DB_UNLOCK();
    return err;
}

esp_err_t device_db_clear_features(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint16_t features)
{
    DB_LOCK();
    zbc_device_t *dev = find_by_ieee(ieee);
    if (!dev) {
        DB_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (int i = 0; i < dev->ep_count; i++) {
        if (dev->eps[i].ep != ep) {
            continue;
        }
        const uint16_t before = dev->eps[i].features;
        dev->eps[i].features &= (uint16_t)~features;
        err = before == dev->eps[i].features ? ESP_OK : db_save_locked();
        break;
    }
    DB_UNLOCK();
    return err;
}

esp_err_t device_db_remove_endpoint(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    DB_LOCK();
    zbc_device_t *dev = find_by_ieee(ieee);
    esp_err_t     err = dev ? ESP_OK : ESP_ERR_NOT_FOUND;
    for (int i = 0; dev && i < dev->ep_count; i++) {
        if (dev->eps[i].ep != ep) {
            continue;
        }
        if (dev->eps[i].features & ZBC_FEAT_ONOFF) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        memmove(&dev->eps[i], &dev->eps[i + 1], (size_t)(dev->ep_count - i - 1) * sizeof(dev->eps[0]));
        dev->ep_count--;
        memset(&dev->eps[dev->ep_count], 0, sizeof(dev->eps[0]));
        err = db_save_locked();           /* which bumps the state revision */
        break;
    }
    DB_UNLOCK();
    return err;
}

esp_err_t device_db_rename(const uint8_t ieee[ZBC_IEEE_LEN], const char *name)
{
    DB_LOCK();
    zbc_device_t *dev = find_by_ieee(ieee);
    if (!dev) {
        DB_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(dev->name, name, sizeof(dev->name));
    esp_err_t err = db_save_locked();
    DB_UNLOCK();
    return err;
}

esp_err_t device_db_remove(const uint8_t ieee[ZBC_IEEE_LEN])
{
    DB_LOCK();
    zbc_device_t *dev = find_by_ieee(ieee);
    if (!dev) {
        DB_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    s_lqa[dev - s_devices] = ZBC_UNKNOWN;
    memset(dev, 0, sizeof(*dev));
    esp_err_t err = db_save_locked();
    DB_UNLOCK();
    return err;
}

esp_err_t device_db_set_attr_by_short(uint16_t short_addr, uint8_t ep, zbc_attr_t attr, int32_t value)
{
    DB_LOCK();
    zbc_device_t *dev = find_by_short(short_addr);
    if (!dev) {
        DB_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    /* An attribute value arriving from the device is a sighting. A caller
     * recording ZBC_UNKNOWN is saying the opposite - that the device has gone
     * quiet and its state is no longer known - so it must not refresh one. */
    if (value != ZBC_UNKNOWN) {
        dev->last_seen_us = esp_timer_get_time();
        dev->online       = true;
    }
    zbc_state_bump();                 /* state the page shows, saved or not */

    zbc_endpoint_t *e = NULL;
    for (int i = 0; i < dev->ep_count; i++) {
        if (dev->eps[i].ep == ep) {
            e = &dev->eps[i];
            break;
        }
    }
    if (!e) {
        DB_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }

    bool persist = false;
    switch (attr) {
    /* Normalise the ZCL boolean to 1/0, but let ZBC_UNKNOWN through: the
     * audit records it to say the lamp has gone quiet, and a plain
     * truth test would turn that -1 into a confident "on". */
    case ZBC_ATTR_ONOFF:      e->on_off     = value == ZBC_UNKNOWN ? ZBC_UNKNOWN
                                                                  : (value ? 1 : 0); break;
    case ZBC_ATTR_LEVEL:      e->level      = (int16_t)value;     break;
    case ZBC_ATTR_CT:         e->ct_mireds  = value;              break;
    case ZBC_ATTR_HUE:        e->hue        = (int16_t)value;     break;
    case ZBC_ATTR_SATURATION: e->saturation = (int16_t)value;     break;
    case ZBC_ATTR_COLOR_MODE: e->color_mode = (int8_t)value;      break;
    /* The physical limits describe the lamp, so they are worth storing - once.
     * They are re-read on every interrogation, and an unchanged value must not
     * cost a rewrite of the table. */
    case ZBC_ATTR_CT_MIN:     persist = e->ct_min_mireds != value; e->ct_min_mireds = value; break;
    case ZBC_ATTR_CT_MAX:     persist = e->ct_max_mireds != value; e->ct_max_mireds = value; break;
    }

    esp_err_t err = persist ? db_save_locked() : ESP_OK;
    DB_UNLOCK();
    return err;
}

uint16_t device_db_features_by_short(uint16_t short_addr, uint8_t ep)
{
    uint16_t features = 0;
    DB_LOCK();
    zbc_device_t *dev = find_by_short(short_addr);
    if (dev) {
        for (int i = 0; i < dev->ep_count; i++) {
            if (dev->eps[i].ep == ep) {
                features = dev->eps[i].features;
                break;
            }
        }
    }
    DB_UNLOCK();
    return features;
}

esp_err_t device_db_touch_by_short(uint16_t short_addr)
{
    DB_LOCK();
    zbc_device_t *dev = find_by_short(short_addr);
    if (dev) {
        if (!dev->online) {
            zbc_state_bump();
        }
        dev->last_seen_us = esp_timer_get_time();
        dev->online       = true;
    }
    DB_UNLOCK();
    return dev ? ESP_OK : ESP_ERR_NOT_FOUND;
}

bool device_db_short_by_ieee(const uint8_t ieee[ZBC_IEEE_LEN], uint16_t *out_short)
{
    DB_LOCK();
    zbc_device_t *dev = find_by_ieee(ieee);
    /* A row with no address on this network has nothing to give: every sender
     * asks here and stops, rather than sending to an address another device
     * may now hold. */
    const bool found = dev != NULL && zbc_short_known(dev->short_addr);
    if (found && out_short) {
        *out_short = dev->short_addr;
    }
    DB_UNLOCK();
    return found;
}

bool device_db_ieee_by_short(uint16_t short_addr, uint8_t out_ieee[ZBC_IEEE_LEN])
{
    DB_LOCK();
    zbc_device_t *dev = find_by_short(short_addr);
    if (dev && out_ieee) {
        memcpy(out_ieee, dev->ieee, ZBC_IEEE_LEN);
    }
    bool found = dev != NULL;
    DB_UNLOCK();
    return found;
}

int device_db_ep_count_by_short(uint16_t short_addr)
{
    DB_LOCK();
    zbc_device_t *dev = find_by_short(short_addr);
    const int     n   = dev ? dev->ep_count : -1;
    DB_UNLOCK();
    return n;
}

int64_t device_db_last_seen_by_short(uint16_t short_addr)
{
    DB_LOCK();
    zbc_device_t *dev  = find_by_short(short_addr);
    const int64_t seen = dev ? dev->last_seen_us : 0;
    DB_UNLOCK();
    return seen;
}

bool device_db_model_by_short(uint16_t short_addr, char out[ZBC_STRING_LEN])
{
    out[0] = '\0';
    DB_LOCK();
    zbc_device_t *dev = find_by_short(short_addr);
    if (dev) {
        strlcpy(out, dev->model, ZBC_STRING_LEN);
    }
    DB_UNLOCK();
    return dev != NULL;
}

esp_err_t device_db_set_online(const uint8_t ieee[ZBC_IEEE_LEN], bool online)
{
    DB_LOCK();
    zbc_device_t *dev = find_by_ieee(ieee);
    if (dev) {
        if (dev->online != online) {
            zbc_state_bump();
        }
        dev->online = online;
        if (online) {
            dev->last_seen_us = esp_timer_get_time();
        }
    }
    DB_UNLOCK();
    return dev ? ESP_OK : ESP_ERR_NOT_FOUND;
}

bool device_db_get(const uint8_t ieee[ZBC_IEEE_LEN], zbc_device_t *out)
{
    DB_LOCK();
    zbc_device_t *dev = find_by_ieee(ieee);
    if (dev && out) {
        *out = *dev;
    }
    bool found = dev != NULL;
    DB_UNLOCK();
    return found;
}

size_t device_db_snapshot(zbc_device_t *buf, size_t max)
{
    size_t n = 0;
    DB_LOCK();
    for (int i = 0; i < CONFIG_ZBC_MAX_DEVICES && n < max; i++) {
        if (s_devices[i].in_use) {
            buf[n++] = s_devices[i];
        }
    }
    DB_UNLOCK();
    return n;
}

size_t device_db_list_ieee(uint8_t out[][ZBC_IEEE_LEN], size_t max)
{
    size_t n = 0;
    DB_LOCK();
    for (int i = 0; i < CONFIG_ZBC_MAX_DEVICES && n < max; i++) {
        if (s_devices[i].in_use) {
            memcpy(out[n++], s_devices[i].ieee, ZBC_IEEE_LEN);
        }
    }
    DB_UNLOCK();
    return n;
}

bool device_db_is_fresh(const zbc_device_t *dev, int64_t max_age_us)
{
    return dev->online && dev->last_seen_us > 0 &&
           esp_timer_get_time() - dev->last_seen_us < max_age_us;
}

size_t device_db_count_fresh(int64_t max_age_us)
{
    size_t n = 0;
    DB_LOCK();
    for (int i = 0; i < CONFIG_ZBC_MAX_DEVICES; i++) {
        if (s_devices[i].in_use && device_db_is_fresh(&s_devices[i], max_age_us)) {
            n++;
        }
    }
    DB_UNLOCK();
    return n;
}

size_t device_db_count(void)
{
    size_t n = 0;
    DB_LOCK();
    for (int i = 0; i < CONFIG_ZBC_MAX_DEVICES; i++) {
        if (s_devices[i].in_use) {
            n++;
        }
    }
    DB_UNLOCK();
    return n;
}

void device_db_for_each(void (*cb)(const zbc_device_t *dev, void *ctx), void *ctx)
{
    DB_LOCK();
    for (int i = 0; i < CONFIG_ZBC_MAX_DEVICES; i++) {
        if (s_devices[i].in_use) {
            cb(&s_devices[i], ctx);
        }
    }
    DB_UNLOCK();
}

esp_err_t device_db_save(void)
{
    DB_LOCK();
    esp_err_t err = db_save_locked();
    DB_UNLOCK();
    return err;
}

esp_err_t device_db_erase(void)
{
    DB_LOCK();
    memset(s_devices, 0, sizeof(s_devices));
    for (int i = 0; i < CONFIG_ZBC_MAX_DEVICES; i++) {
        s_lqa[i] = ZBC_UNKNOWN;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(DB_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_erase_key(nvs, DB_NVS_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    DB_UNLOCK();
    return err;
}

void zbc_ieee_to_str(const uint8_t ieee[ZBC_IEEE_LEN], char out[19])
{
    snprintf(out, 19, "0x%02x%02x%02x%02x%02x%02x%02x%02x",
             ieee[0], ieee[1], ieee[2], ieee[3], ieee[4], ieee[5], ieee[6], ieee[7]);
}

bool zbc_str_to_ieee(const char *str, uint8_t out[ZBC_IEEE_LEN])
{
    if (!str) {
        return false;
    }
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        str += 2;
    }
    if (strlen(str) != ZBC_IEEE_LEN * 2) {
        return false;
    }
    for (int i = 0; i < ZBC_IEEE_LEN; i++) {
        char byte[3] = { str[i * 2], str[i * 2 + 1], 0 };
        char *end    = NULL;
        long v       = strtol(byte, &end, 16);
        if (end != byte + 2) {
            return false;
        }
        out[i] = (uint8_t)v;
    }
    return true;
}

esp_err_t device_db_set_lqa(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t lqa)
{
    DB_LOCK();
    zbc_device_t *dev = find_by_ieee(ieee);
    if (!dev) {
        DB_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    if (s_lqa[dev - s_devices] != (int16_t)lqa) {
        zbc_state_bump();
    }
    s_lqa[dev - s_devices] = (int16_t)lqa;
    DB_UNLOCK();
    return ESP_OK;
}

int device_db_lqa(const uint8_t ieee[ZBC_IEEE_LEN])
{
    DB_LOCK();
    zbc_device_t *dev = find_by_ieee(ieee);
    int lqa = dev ? s_lqa[dev - s_devices] : ZBC_UNKNOWN;
    DB_UNLOCK();
    return lqa;
}
