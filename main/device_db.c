#include <stddef.h>
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
#include "node_proto.h"
#include "state_rev.h"

static const char *TAG = "device_db";

/* A record per device, in a namespace of their own, under a key made from the
 * device's IEEE address (dr_key). A change rewrites that device's record alone,
 * a few dozen bytes. The whole table was a quarter of a kilobyte per device, and
 * saving it needed room for two copies at once in the partition the Zigbee
 * stack's network shares, 36 KB whose place is fixed: about 40 devices was as
 * far as it went. */
#define DB_REC_NAMESPACE "zbc_dev"
/* The whole table under one key: what builds before the records stored, and what
 * a configuration restore writes (config_backup.c lists the key). The boot
 * replaces every record with it, then erases it (db_load_table). */
#define DB_NVS_NAMESPACE "zbc"
#define DB_NVS_KEY       "devices"

/* >>> host-test region: devrow <<< */
/* Stored records.
 *
 * A record is a device's stored fields written out a byte at a time, not the
 * struct: a field added to zbc_device_t costs no stored device. Only what
 * survives a restart is in it - the address, the names, and per endpoint its
 * number, device id, features and colour-temperature range. Live state and
 * last_seen never were kept. Strings take their length and endpoints their
 * number, so a lamp with one endpoint and a short model is about 60 bytes.
 *
 *    0  DR_VERSION          1  its slot: its place in the table, so the list
 *    2  endpoint count         keeps the order devices joined in
 *    3  IEEE, 8 bytes      11  short address
 *   13  name, manufacturer, model: a length byte each, then its bytes
 *   then per endpoint DR_EP_BYTES: number, device id, features (16 bits each),
 *        coolest and warmest colour temperature in mireds (32 bits each, signed)
 *
 * Numbers are little-endian. */
#define DR_VERSION  1
#define DR_HEAD     13
#define DR_EP_BYTES 13
#define DR_MAX      (DR_HEAD + 3 + (ZBC_NAME_LEN - 1) + 2 * (ZBC_STRING_LEN - 1) + \
                     CONFIG_ZBC_MAX_ENDPOINTS_PER_DEVICE * DR_EP_BYTES)

/* The table as one blob, for a configuration backup: the header the old table
 * had - version (32 bits), count, two bytes reserved - then per device its
 * record's length (16 bits) and the record. */
#define DR_TABLE_V2   2      /* the struct array builds before the records stored */
#define DR_TABLE_V3   3
#define DR_TABLE_HEAD 8
#define DR_TABLE_MAX(n) (DR_TABLE_HEAD + (size_t)(n) * (2 + DR_MAX))

_Static_assert(CONFIG_ZBC_MAX_DEVICES <= 255, "a record keeps its slot in one byte");

/* The table as the builds before the records wrote it, frozen as it was: four
 * endpoints a device, the structs as laid out then. Read from such a build's
 * flash, or from its backup; never written. */
typedef struct {
    uint8_t  ep;
    uint16_t device_id;
    uint16_t features;
    int8_t   on_off;
    int16_t  level;
    int32_t  ct_mireds;
    int32_t  ct_min_mireds;
    int32_t  ct_max_mireds;
    int16_t  hue;
    int16_t  saturation;
    int8_t   color_mode;
} dr_v2_endpoint_t;

typedef struct {
    bool             in_use;
    uint8_t          ieee[8];
    uint16_t         short_addr;
    char             name[32];
    char             manufacturer[33];
    char             model[33];
    uint8_t          ep_count;
    dr_v2_endpoint_t eps[4];
    int64_t          last_seen_us;
    bool             online;
} dr_v2_device_t;

_Static_assert(sizeof(dr_v2_endpoint_t) == 32, "the old table's endpoint is 32 bytes");
_Static_assert(sizeof(dr_v2_device_t) == 256 && offsetof(dr_v2_device_t, short_addr) == 10 &&
               offsetof(dr_v2_device_t, eps) == 112, "the old table's row is laid out as it was");

static void dr_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void dr_put32(uint8_t *p, uint32_t v)
{
    dr_put16(p, (uint16_t)v);
    dr_put16(p + 2, (uint16_t)(v >> 16));
}

static uint16_t dr_get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t dr_get32(const uint8_t *p)
{
    return dr_get16(p) | (uint32_t)dr_get16(p + 2) << 16;
}

/* A record's key: "d" and the IEEE address in eleven characters of six bits
 * each. An NVS key holds 15, too few for the address in hex. */
static void dr_key(const uint8_t ieee[ZBC_IEEE_LEN], char out[13])
{
    static const char DIGITS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    uint64_t v = 0;
    for (int i = 0; i < ZBC_IEEE_LEN; i++) {
        v = v << 8 | ieee[i];
    }
    out[0] = 'd';
    for (int i = 11; i >= 1; i--) {
        out[i] = DIGITS[v & 63];
        v >>= 6;
    }
    out[12] = '\0';
}

/* An endpoint as stored: what it is, and every live value unknown. */
static void dr_endpoint(zbc_endpoint_t *e, uint8_t ep, uint16_t device_id, uint16_t features,
                        int32_t ct_min, int32_t ct_max)
{
    *e = (zbc_endpoint_t){
        .ep            = ep,
        .device_id     = device_id,
        .features      = features,
        .on_off        = ZBC_UNKNOWN,
        .level         = ZBC_UNKNOWN,
        .ct_mireds     = ZBC_UNKNOWN,
        .ct_min_mireds = ct_min,
        .ct_max_mireds = ct_max,
        .hue           = ZBC_UNKNOWN,
        .saturation    = ZBC_UNKNOWN,
        .color_mode    = ZBC_UNKNOWN,
    };
}

/* The record of a device at table slot `slot`: its length, or 0 when it does not
 * fit in `cap`. */
static size_t dr_encode(const zbc_device_t *d, uint8_t slot, uint8_t *out, size_t cap)
{
    const char  *str[3] = { d->name, d->manufacturer, d->model };
    const size_t max[3] = { sizeof(d->name), sizeof(d->manufacturer), sizeof(d->model) };
    const int    eps    = d->ep_count < CONFIG_ZBC_MAX_ENDPOINTS_PER_DEVICE ? d->ep_count
                                                                           : CONFIG_ZBC_MAX_ENDPOINTS_PER_DEVICE;
    size_t len[3];
    size_t need = DR_HEAD + 3 + (size_t)eps * DR_EP_BYTES;
    for (int k = 0; k < 3; k++) {
        len[k] = strnlen(str[k], max[k] - 1);
        need += len[k];
    }
    if (need > cap) {
        return 0;
    }
    out[0] = DR_VERSION;
    out[1] = slot;
    out[2] = (uint8_t)eps;
    memcpy(out + 3, d->ieee, ZBC_IEEE_LEN);
    dr_put16(out + 11, d->short_addr);
    uint8_t *p = out + DR_HEAD;
    for (int k = 0; k < 3; k++) {
        *p++ = (uint8_t)len[k];
        memcpy(p, str[k], len[k]);
        p += len[k];
    }
    for (int e = 0; e < eps; e++, p += DR_EP_BYTES) {
        const zbc_endpoint_t *src = &d->eps[e];
        p[0] = src->ep;
        dr_put16(p + 1, src->device_id);
        dr_put16(p + 3, src->features);
        dr_put32(p + 5, (uint32_t)src->ct_min_mireds);
        dr_put32(p + 9, (uint32_t)src->ct_max_mireds);
    }
    return need;
}

/* A record back into a device: in use, offline, never seen, every live value
 * unknown. False for one this build does not read - another version, or a length
 * its counts do not add up to. A string longer than this build keeps is cut, and
 * endpoints past its limit are left out. */
static bool dr_decode(const uint8_t *in, size_t len, zbc_device_t *d, uint8_t *slot)
{
    if (len < DR_HEAD || in[0] != DR_VERSION) {
        return false;
    }
    memset(d, 0, sizeof(*d));
    char        *str[3] = { d->name, d->manufacturer, d->model };
    const size_t max[3] = { sizeof(d->name), sizeof(d->manufacturer), sizeof(d->model) };
    size_t       at     = DR_HEAD;
    for (int k = 0; k < 3; k++) {
        if (at >= len) {
            return false;
        }
        const size_t n = in[at++];
        if (n > len - at) {
            return false;
        }
        const size_t keep = n < max[k] - 1 ? n : max[k] - 1;
        memcpy(str[k], in + at, keep);
        str[k][keep] = '\0';
        at += n;
    }
    const size_t eps = in[2];
    if (len - at != eps * DR_EP_BYTES) {
        return false;
    }
    d->in_use = true;
    memcpy(d->ieee, in + 3, ZBC_IEEE_LEN);
    d->short_addr = dr_get16(in + 11);
    for (size_t e = 0; e < eps && e < CONFIG_ZBC_MAX_ENDPOINTS_PER_DEVICE; e++, at += DR_EP_BYTES) {
        dr_endpoint(&d->eps[d->ep_count++], in[at], dr_get16(in + at + 1), dr_get16(in + at + 3),
                    (int32_t)dr_get32(in + at + 5), (int32_t)dr_get32(in + at + 9));
    }
    *slot = in[1];
    return true;
}

/* A row of the old table, as the boot load of the builds before took it. */
static void dr_from_v2(const dr_v2_device_t *v, zbc_device_t *d)
{
    memset(d, 0, sizeof(*d));
    d->in_use = true;
    memcpy(d->ieee, v->ieee, ZBC_IEEE_LEN);
    d->short_addr = v->short_addr;
    memcpy(d->name, v->name, sizeof(d->name) - 1);
    memcpy(d->manufacturer, v->manufacturer, sizeof(d->manufacturer) - 1);
    memcpy(d->model, v->model, sizeof(d->model) - 1);
    for (int e = 0; e < v->ep_count && e < 4 && e < CONFIG_ZBC_MAX_ENDPOINTS_PER_DEVICE; e++) {
        const dr_v2_endpoint_t *src = &v->eps[e];
        dr_endpoint(&d->eps[d->ep_count++], src->ep, src->device_id, src->features, src->ct_min_mireds,
                    src->ct_max_mireds);
    }
}

/* `d` into a table of `max` slots: at `slot` when that is free, else at the
 * first free one. The slot it took, or -1 when the table is full. */
static int dr_place(zbc_device_t *out, int max, const zbc_device_t *d, int slot)
{
    if (slot < 0 || slot >= max || out[slot].in_use) {
        for (slot = 0; slot < max && out[slot].in_use; slot++) {
        }
        if (slot == max) {
            return -1;
        }
    }
    out[slot] = *d;
    return slot;
}

static bool dr_known(const zbc_device_t *out, int max, const uint8_t ieee[ZBC_IEEE_LEN])
{
    for (int i = 0; i < max; i++) {
        if (out[i].in_use && memcmp(out[i].ieee, ieee, ZBC_IEEE_LEN) == 0) {
            return true;
        }
    }
    return false;
}

/* Every device of a table blob into `out`, a table of `max` slots: a record at
 * the slot it names, the old table's rows in their order. Devices past `max` are
 * left out, as the builds before left out those past a smaller table. The count
 * placed, or -1 for a blob this build does not read, with `out` then holding
 * some of it. With `out` NULL it only checks the blob. */
static int dr_table_parse(const uint8_t *blob, size_t len, zbc_device_t *out, int max)
{
    if (!blob || len < DR_TABLE_HEAD) {
        return -1;
    }
    const uint32_t version = dr_get32(blob);
    const size_t   count   = dr_get16(blob + 4);
    int            placed  = 0;
    zbc_device_t   d;
    if (version == DR_TABLE_V2) {
        if (len != DR_TABLE_HEAD + count * sizeof(dr_v2_device_t)) {
            return -1;
        }
        for (size_t i = 0; out && i < count; i++) {
            dr_v2_device_t row;
            memcpy(&row, blob + DR_TABLE_HEAD + i * sizeof(row), sizeof(row));
            dr_from_v2(&row, &d);
            if (!dr_known(out, max, d.ieee) && dr_place(out, max, &d, -1) >= 0) {
                placed++;
            }
        }
        return placed;
    }
    if (version != DR_TABLE_V3) {
        return -1;
    }
    size_t at = DR_TABLE_HEAD;
    for (size_t i = 0; i < count; i++) {
        if (len - at < 2 || len - at - 2 < dr_get16(blob + at)) {
            return -1;
        }
        const size_t rl = dr_get16(blob + at);
        uint8_t      slot;
        if (!dr_decode(blob + at + 2, rl, &d, &slot)) {
            return -1;
        }
        at += 2 + rl;
        if (out && !dr_known(out, max, d.ieee) && dr_place(out, max, &d, slot) >= 0) {
            placed++;
        }
    }
    return at == len ? placed : -1;
}

/* The table blob of `devs`, a table of `n` slots, in slot order: its length, or
 * 0 when `cap` is too small. */
static size_t dr_table_pack(const zbc_device_t *devs, int n, uint8_t *out, size_t cap)
{
    if (cap < DR_TABLE_HEAD) {
        return 0;
    }
    size_t   at    = DR_TABLE_HEAD;
    uint16_t count = 0;
    for (int i = 0; i < n; i++) {
        if (!devs[i].in_use) {
            continue;
        }
        const size_t rl = cap - at >= 2 ? dr_encode(&devs[i], (uint8_t)i, out + at + 2, cap - at - 2) : 0;
        if (!rl) {
            return 0;
        }
        dr_put16(out + at, (uint16_t)rl);
        at += 2 + rl;
        count++;
    }
    dr_put32(out, DR_TABLE_V3);
    dr_put16(out + 4, count);
    dr_put16(out + 6, 0);
    return at;
}

/* Every device of a table blob this build reads given ZBC_SHORT_NONE. */
static void dr_table_forget_addresses(uint8_t *blob, size_t len)
{
    if (dr_table_parse(blob, len, NULL, 0) < 0) {
        return;
    }
    const size_t count = dr_get16(blob + 4);
    if (dr_get32(blob) == DR_TABLE_V2) {
        for (size_t i = 0; i < count; i++) {
            dr_put16(blob + DR_TABLE_HEAD + i * sizeof(dr_v2_device_t) + offsetof(dr_v2_device_t, short_addr),
                     ZBC_SHORT_NONE);
        }
        return;
    }
    for (size_t i = 0, at = DR_TABLE_HEAD; i < count; i++) {
        dr_put16(blob + at + 2 + 11, ZBC_SHORT_NONE);
        at += 2 + dr_get16(blob + at);
    }
}
/* >>> end host-test region: devrow <<< */

static zbc_device_t      s_devices[CONFIG_ZBC_MAX_DEVICES];
static SemaphoreHandle_t s_lock;

/* Link quality from the last neighbour scan that saw each device, parallel to
 * s_devices by slot. A measurement of now, like online and last_seen_us, so it
 * is never stored. */
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

static esp_err_t db_write_record(nvs_handle_t nvs, int slot)
{
    uint8_t      row[DR_MAX];
    const size_t len = dr_encode(&s_devices[slot], (uint8_t)slot, row, sizeof(row));
    char         key[13];
    dr_key(s_devices[slot].ieee, key);
    return len ? nvs_set_blob(nvs, key, row, len) : ESP_ERR_INVALID_SIZE;
}

/* The device's record, after a change to it. */
static esp_err_t db_save_locked(const zbc_device_t *dev)
{
    zbc_state_bump();          /* the page shows this table */
    nvs_handle_t nvs;
    esp_err_t    err = nvs_open(DB_REC_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = db_write_record(nvs, (int)(dev - s_devices));
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not store \"%s\": %s", dev->name, esp_err_to_name(err));
    }
    return err;
}

/* The record of a device that has gone. */
static esp_err_t db_forget_locked(const uint8_t ieee[ZBC_IEEE_LEN])
{
    zbc_state_bump();
    char key[13];
    dr_key(ieee, key);
    nvs_handle_t nvs;
    esp_err_t    err = nvs_open(DB_REC_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_erase_key(nvs, key);
        if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    return err;
}

bool device_db_blob_ok(const void *blob, size_t len)
{
    return dr_table_parse(blob, len, NULL, 0) >= 0;
}

size_t device_db_blob_max(void)
{
    const size_t v2 = DR_TABLE_HEAD + CONFIG_ZBC_MAX_DEVICES * sizeof(dr_v2_device_t);
    const size_t v3 = DR_TABLE_MAX(CONFIG_ZBC_MAX_DEVICES);
    return v2 > v3 ? v2 : v3;
}

void device_db_blob_forget_addresses(void *blob, size_t len)
{
    dr_table_forget_addresses(blob, len);
}

void device_db_drop_records(void)
{
    nvs_handle_t nvs;
    if (nvs_open(DB_REC_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

/* A table under the old key, from a build before the records or from a restore:
 * true when there is one this build reads, which then is the table and replaces
 * every record. It is erased once each of its devices has its record and not
 * before, so a power cut part way moves it again at the next boot. One this
 * build does not read is dropped, as the builds before dropped it. */
static bool db_load_table(void)
{
    nvs_handle_t nvs;
    size_t       len = 0;
    if (nvs_open(DB_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    if (nvs_get_blob(nvs, DB_NVS_KEY, NULL, &len) != ESP_OK) {
        nvs_close(nvs);
        return false;
    }
    uint8_t *blob = malloc(len ? len : 1);
    if (!blob) {
        nvs_close(nvs);          /* left for the next boot */
        return false;
    }
    int n = -1;
    if (nvs_get_blob(nvs, DB_NVS_KEY, blob, &len) == ESP_OK) {
        n = dr_table_parse(blob, len, s_devices, CONFIG_ZBC_MAX_DEVICES);
    }
    free(blob);
    if (n < 0) {
        memset(s_devices, 0, sizeof(s_devices));
        ESP_LOGW(TAG, "The stored device table is from an incompatible build, discarding it");
        nvs_erase_key(nvs, DB_NVS_KEY);
        nvs_commit(nvs);
        nvs_close(nvs);
        return false;
    }
    nvs_handle_t rec;
    bool         all = nvs_open(DB_REC_NAMESPACE, NVS_READWRITE, &rec) == ESP_OK;
    if (all) {
        all = nvs_erase_all(rec) == ESP_OK;
        for (int i = 0; i < CONFIG_ZBC_MAX_DEVICES && all; i++) {
            all = !s_devices[i].in_use || db_write_record(rec, i) == ESP_OK;
        }
        all = all && nvs_commit(rec) == ESP_OK;
        nvs_close(rec);
    }
    if (all) {
        nvs_erase_key(nvs, DB_NVS_KEY);
        nvs_commit(nvs);
        ESP_LOGI(TAG, "Moved %d device(s) from the table to a record each", n);
    } else {
        ESP_LOGE(TAG, "Could not write every device's record: the table stays, and moves again at the next boot");
    }
    nvs_close(nvs);
    return true;
}

/* Every record: at the slot it names, or, when that is taken or past this
 * build's table, the first free one, and then stored again with that slot. A
 * record this build does not read is left in flash as it is. */
static void db_load_records(void)
{
    nvs_handle_t nvs;
    if (nvs_open(DB_REC_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;                  /* no records yet */
    }
    bool moved[CONFIG_ZBC_MAX_DEVICES] = { false };
    for (int pass = 0; pass < 2; pass++) {
        nvs_iterator_t it = NULL;
        for (esp_err_t res = nvs_entry_find(NVS_DEFAULT_PART_NAME, DB_REC_NAMESPACE, NVS_TYPE_BLOB, &it);
             res == ESP_OK; res = nvs_entry_next(&it)) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);
            size_t       len = 0;
            uint8_t     *row = NULL;
            zbc_device_t d;
            uint8_t      slot = 0;
            const bool   read = nvs_get_blob(nvs, info.key, NULL, &len) == ESP_OK && (row = malloc(len ? len : 1)) &&
                              nvs_get_blob(nvs, info.key, row, &len) == ESP_OK && dr_decode(row, len, &d, &slot);
            free(row);
            if (!read) {
                if (pass == 0) {
                    ESP_LOGW(TAG, "Record %s is in a format this build does not read; left as it is", info.key);
                }
                continue;
            }
            if (find_by_ieee(d.ieee)) {
                continue;        /* placed in the first pass */
            }
            if (pass == 0) {
                if (slot < CONFIG_ZBC_MAX_DEVICES && !s_devices[slot].in_use) {
                    s_devices[slot] = d;
                }
                continue;
            }
            const int at = dr_place(s_devices, CONFIG_ZBC_MAX_DEVICES, &d, -1);
            if (at < 0) {
                ESP_LOGW(TAG, "No room for \"%s\" (%d devices at most); its record is left in flash", d.name,
                         CONFIG_ZBC_MAX_DEVICES);
                continue;
            }
            moved[at] = true;
        }
        nvs_release_iterator(it);
    }
    nvs_close(nvs);
    for (int i = 0; i < CONFIG_ZBC_MAX_DEVICES; i++) {
        if (moved[i]) {
            db_save_locked(&s_devices[i]);
        }
    }
}

static void db_load(void)
{
    if (!db_load_table()) {
        db_load_records();
    }
    int loaded = 0;
    for (int i = 0; i < CONFIG_ZBC_MAX_DEVICES; i++) {
        loaded += s_devices[i].in_use;
    }
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
                db_save_locked(&s_devices[i]);
            }
        }
    }

    if ((created && *created) || moved) {
        err = db_save_locked(dev);
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
    esp_err_t err = changed ? db_save_locked(dev) : ESP_OK;
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
            esp_err_t err = changed ? db_save_locked(dev) : ESP_OK;
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
    esp_err_t err = db_save_locked(dev);
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
                err = db_save_locked(dev);
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
        err = before == dev->eps[i].features ? ESP_OK : db_save_locked(dev);
        break;
    }
    DB_UNLOCK();
    return err;
}

static esp_err_t erase_endpoint_locked(zbc_device_t *dev, int i)
{
    memmove(&dev->eps[i], &dev->eps[i + 1], (size_t)(dev->ep_count - i - 1) * sizeof(dev->eps[0]));
    dev->ep_count--;
    memset(&dev->eps[dev->ep_count], 0, sizeof(dev->eps[0]));
    return db_save_locked(dev);           /* which bumps the state revision */
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
        err = erase_endpoint_locked(dev, i);
        break;
    }
    DB_UNLOCK();
    return err;
}

esp_err_t device_db_drop_endpoint(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    DB_LOCK();
    zbc_device_t *dev = find_by_ieee(ieee);
    esp_err_t     err = dev ? ESP_OK : ESP_ERR_NOT_FOUND;
    for (int i = 0; dev && i < dev->ep_count; i++) {
        if (dev->eps[i].ep == ep) {
            err = erase_endpoint_locked(dev, i);
            break;
        }
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
    esp_err_t err = db_save_locked(dev);
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
    uint8_t ieee_copy[ZBC_IEEE_LEN];
    memcpy(ieee_copy, dev->ieee, ZBC_IEEE_LEN);
    memset(dev, 0, sizeof(*dev));
    esp_err_t err = db_forget_locked(ieee_copy);
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

    esp_err_t err = persist ? db_save_locked(dev) : ESP_OK;
    DB_UNLOCK();
    return err;
}

bool device_db_is_node(const uint8_t ieee[ZBC_IEEE_LEN])
{
    bool node = false;
    DB_LOCK();
    const zbc_device_t *dev = find_by_ieee(ieee);
    for (int i = 0; dev && i < dev->ep_count && !node; i++) {
        node = dev->eps[i].ep == NODE_CONTROL_EP && (dev->eps[i].features & ZBC_FEAT_NODE);
    }
    DB_UNLOCK();
    return node;
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

esp_err_t device_db_erase(void)
{
    DB_LOCK();
    memset(s_devices, 0, sizeof(s_devices));
    for (int i = 0; i < CONFIG_ZBC_MAX_DEVICES; i++) {
        s_lqa[i] = ZBC_UNKNOWN;
    }
    zbc_state_bump();
    nvs_handle_t nvs;
    esp_err_t    err = nvs_open(DB_REC_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_erase_all(nvs);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    /* And a table under the old key not yet moved, or the next boot would bring
     * it all back. */
    if (err == ESP_OK && (err = nvs_open(DB_NVS_NAMESPACE, NVS_READWRITE, &nvs)) == ESP_OK) {
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

esp_err_t device_db_export_table(uint8_t **out, size_t *len)
{
    *out = NULL;
    *len = 0;
    DB_LOCK();
    int n = 0;
    for (int i = 0; i < CONFIG_ZBC_MAX_DEVICES; i++) {
        n += s_devices[i].in_use;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (n) {
        const size_t cap = DR_TABLE_MAX(n);
        *out             = malloc(cap);
        *len             = *out ? dr_table_pack(s_devices, CONFIG_ZBC_MAX_DEVICES, *out, cap) : 0;
        err              = *len ? ESP_OK : ESP_ERR_NO_MEM;
    }
    DB_UNLOCK();
    if (err != ESP_OK) {
        free(*out);
        *out = NULL;
        *len = 0;
    }
    return err;
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
