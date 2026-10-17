#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"

#include "mlight_db.h"
#include "state_rev.h"

static const char *TAG = "mlight_db";

/* Same namespace as the other tables, its own key and its own version: adding
 * Matter lamps must not discard the devices, groups, curves, buttons or wired
 * lights. */
#define MLIGHT_NVS_NAMESPACE "zbc"
/* In config_backup.c's list too: a backup carries this table under this key. */
#define MLIGHT_NVS_KEY       "mlights"
#define MLIGHT_BLOB_VERSION  1

typedef struct {
    uint32_t version;
    uint16_t count;
    uint16_t reserved;
} mlight_blob_header_t;

static zbc_mlight_t      s_rows[CONFIG_ZBC_MAX_MATTER_LIGHTS];
static SemaphoreHandle_t s_lock;

#define MDB_LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define MDB_UNLOCK() xSemaphoreGiveRecursive(s_lock)

static zbc_mlight_t *find_by_id(uint16_t id)
{
    for (int i = 0; i < CONFIG_ZBC_MAX_MATTER_LIGHTS; i++) {
        if (s_rows[i].in_use && s_rows[i].id == id) {
            return &s_rows[i];
        }
    }
    return NULL;
}

static zbc_mlight_t *find_by_node(uint64_t node_id, uint16_t remote_ep)
{
    for (int i = 0; i < CONFIG_ZBC_MAX_MATTER_LIGHTS; i++) {
        if (s_rows[i].in_use && s_rows[i].node_id == node_id && s_rows[i].remote_ep == remote_ep) {
            return &s_rows[i];
        }
    }
    return NULL;
}

static esp_err_t save_locked(void)
{
    zbc_state_bump();          /* the page shows this table */
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MLIGHT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    size_t used = 0;
    for (int i = 0; i < CONFIG_ZBC_MAX_MATTER_LIGHTS; i++) {
        used += s_rows[i].in_use;
    }
    size_t   blob_size = sizeof(mlight_blob_header_t) + used * sizeof(zbc_mlight_t);
    uint8_t *blob      = calloc(1, blob_size);
    if (!blob) {
        nvs_close(nvs);
        return ESP_ERR_NO_MEM;
    }
    mlight_blob_header_t hdr = { .version = MLIGHT_BLOB_VERSION, .count = (uint16_t)used };
    memcpy(blob, &hdr, sizeof(hdr));
    zbc_mlight_t *out = (zbc_mlight_t *)(blob + sizeof(hdr));
    for (int i = 0; i < CONFIG_ZBC_MAX_MATTER_LIGHTS; i++) {
        if (s_rows[i].in_use) {
            *out++ = s_rows[i];
        }
    }

    err = nvs_set_blob(nvs, MLIGHT_NVS_KEY, blob, blob_size);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    free(blob);
    nvs_close(nvs);
    return err;
}

/* Strict: a blob of any other size is from a build whose record differs, and
 * reading it as this one would put a name where a node id belongs. There is no
 * older version to migrate from yet; the day there is, freeze its record here as
 * light_db.c does, because every row is a pairing somebody did. */
bool mlight_db_blob_ok(const void *blob, size_t len)
{
    mlight_blob_header_t hdr;
    if (!blob || len < sizeof(hdr)) {
        return false;
    }
    memcpy(&hdr, blob, sizeof(hdr));
    return hdr.version == MLIGHT_BLOB_VERSION && len == sizeof(hdr) + (size_t)hdr.count * sizeof(zbc_mlight_t);
}

static void load(void)
{
    nvs_handle_t nvs;
    if (nvs_open(MLIGHT_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    size_t    blob_size = 0;
    esp_err_t err       = nvs_get_blob(nvs, MLIGHT_NVS_KEY, NULL, &blob_size);
    if (err != ESP_OK || blob_size < sizeof(mlight_blob_header_t)) {
        nvs_close(nvs);
        return;
    }
    uint8_t *blob = malloc(blob_size);
    if (!blob) {
        nvs_close(nvs);
        return;
    }
    err = nvs_get_blob(nvs, MLIGHT_NVS_KEY, blob, &blob_size);
    nvs_close(nvs);
    if (err != ESP_OK) {
        free(blob);
        return;
    }

    if (!mlight_db_blob_ok(blob, blob_size)) {
        ESP_LOGW(TAG, "Stored Matter lamps are from an incompatible build, discarding");
        free(blob);
        return;
    }
    mlight_blob_header_t hdr;
    memcpy(&hdr, blob, sizeof(hdr));
    const zbc_mlight_t *in = (const zbc_mlight_t *)(blob + sizeof(hdr));
    int loaded = 0;
    for (int i = 0; i < hdr.count && loaded < CONFIG_ZBC_MAX_MATTER_LIGHTS; i++) {
        s_rows[loaded]        = in[i];
        s_rows[loaded].in_use = true;
        loaded++;
    }
    free(blob);
    ESP_LOGI(TAG, "Restored %d Matter lamp(s) from NVS", loaded);
}

esp_err_t mlight_db_init(void)
{
    if (s_lock) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    memset(s_rows, 0, sizeof(s_rows));
    MDB_LOCK();
    load();
    MDB_UNLOCK();
    return ESP_OK;
}

esp_err_t mlight_db_put(const zbc_mlight_t *in, uint16_t *out_id)
{
    if (!in || !in->name[0] || in->node_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    MDB_LOCK();
    zbc_mlight_t *row = find_by_node(in->node_id, in->remote_ep);
    if (row) {
        /* What the device says about itself is refreshed; the id, which groups
         * and curves name it by, and the name somebody gave it, are not. */
        char name[ZBC_MLIGHT_NAME_LEN];
        memcpy(name, row->name, sizeof(name));
        const uint16_t id = row->id;
        *row = *in;
        row->id = id;
        memcpy(row->name, name, sizeof(name));
    } else {
        uint16_t next = 1;
        for (int i = 0; i < CONFIG_ZBC_MAX_MATTER_LIGHTS; i++) {
            if (!s_rows[i].in_use) {
                if (!row) {
                    row = &s_rows[i];
                }
            } else if (s_rows[i].id >= next) {
                next = s_rows[i].id + 1;
            }
        }
        if (!row) {
            MDB_UNLOCK();
            ESP_LOGE(TAG, "Matter lamp table full (%d)", CONFIG_ZBC_MAX_MATTER_LIGHTS);
            return ESP_ERR_NO_MEM;
        }
        *row    = *in;
        row->id = next;
    }
    row->in_use = true;
    memset(row->reserved, 0, sizeof(row->reserved));
    if (out_id) {
        *out_id = row->id;
    }
    esp_err_t err = save_locked();
    MDB_UNLOCK();
    return err;
}

esp_err_t mlight_db_rename(uint16_t id, const char *name)
{
    if (!name || !name[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    MDB_LOCK();
    zbc_mlight_t *row = find_by_id(id);
    if (!row) {
        MDB_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(row->name, name, sizeof(row->name));
    esp_err_t err = save_locked();
    MDB_UNLOCK();
    return err;
}

esp_err_t mlight_db_remove(uint16_t id)
{
    MDB_LOCK();
    zbc_mlight_t *row = find_by_id(id);
    if (!row) {
        MDB_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    memset(row, 0, sizeof(*row));
    esp_err_t err = save_locked();
    MDB_UNLOCK();
    return err;
}

bool mlight_db_get(uint16_t id, zbc_mlight_t *out)
{
    MDB_LOCK();
    const zbc_mlight_t *row = find_by_id(id);
    if (row && out) {
        *out = *row;
    }
    const bool found = row != NULL;
    MDB_UNLOCK();
    return found;
}

uint16_t mlight_db_find(uint64_t node_id, uint16_t remote_ep)
{
    MDB_LOCK();
    const zbc_mlight_t *row = find_by_node(node_id, remote_ep);
    const uint16_t      id  = row ? row->id : 0;
    MDB_UNLOCK();
    return id;
}

size_t mlight_db_node_rows(uint64_t node_id)
{
    size_t n = 0;
    MDB_LOCK();
    for (int i = 0; i < CONFIG_ZBC_MAX_MATTER_LIGHTS; i++) {
        n += s_rows[i].in_use && s_rows[i].node_id == node_id;
    }
    MDB_UNLOCK();
    return n;
}

size_t mlight_db_snapshot(zbc_mlight_t *buf, size_t max)
{
    size_t n = 0;
    MDB_LOCK();
    for (int i = 0; i < CONFIG_ZBC_MAX_MATTER_LIGHTS && n < max; i++) {
        if (s_rows[i].in_use) {
            buf[n++] = s_rows[i];
        }
    }
    MDB_UNLOCK();
    return n;
}

size_t mlight_db_list_ids(uint16_t *out, size_t max)
{
    size_t n = 0;
    MDB_LOCK();
    for (int i = 0; i < CONFIG_ZBC_MAX_MATTER_LIGHTS && n < max; i++) {
        if (s_rows[i].in_use) {
            out[n++] = s_rows[i].id;
        }
    }
    MDB_UNLOCK();
    return n;
}

size_t mlight_db_count(void)
{
    size_t n = 0;
    MDB_LOCK();
    for (int i = 0; i < CONFIG_ZBC_MAX_MATTER_LIGHTS; i++) {
        n += s_rows[i].in_use;
    }
    MDB_UNLOCK();
    return n;
}

esp_err_t mlight_db_erase(void)
{
    MDB_LOCK();
    nvs_handle_t nvs;
    esp_err_t    err = nvs_open(MLIGHT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_erase_key(nvs, MLIGHT_NVS_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    memset(s_rows, 0, sizeof(s_rows));
    zbc_state_bump();
    MDB_UNLOCK();
    return err;
}
