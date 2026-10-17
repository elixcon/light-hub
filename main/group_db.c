#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "group_db.h"
#include "state_rev.h"

static const char *TAG = "group_db";

/* Same namespace as the device registry, its own key: the two tables have
 * separate layouts and separate versions, so a change to one does not throw the
 * other away. */
#define GROUP_NVS_NAMESPACE "zbc"
/* In config_backup.c's list too: a backup carries this table under this key. */
#define GROUP_NVS_KEY       "groups"
#define GROUP_BLOB_VERSION  1

typedef struct {
    uint32_t version;
    uint16_t count;
    uint16_t reserved;
} group_blob_header_t;

static zbc_group_t       s_groups[CONFIG_ZBC_MAX_GROUPS];
static SemaphoreHandle_t s_lock;

#define GDB_LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define GDB_UNLOCK() xSemaphoreGiveRecursive(s_lock)

static zbc_group_t *find_by_id(uint16_t id)
{
    for (int i = 0; i < CONFIG_ZBC_MAX_GROUPS; i++) {
        if (s_groups[i].in_use && s_groups[i].id == id) {
            return &s_groups[i];
        }
    }
    return NULL;
}

static int member_index(const zbc_group_t *g, const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    for (int i = 0; i < g->member_count; i++) {
        if (g->members[i].ep == ep && memcmp(g->members[i].ieee, ieee, ZBC_IEEE_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

static esp_err_t gdb_save_locked(void)
{
    zbc_state_bump();          /* the page shows this table */
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(GROUP_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    size_t used = 0;
    for (int i = 0; i < CONFIG_ZBC_MAX_GROUPS; i++) {
        if (s_groups[i].in_use) {
            used++;
        }
    }

    size_t blob_size = sizeof(group_blob_header_t) + used * sizeof(zbc_group_t);
    uint8_t *blob = calloc(1, blob_size);
    if (!blob) {
        nvs_close(nvs);
        return ESP_ERR_NO_MEM;
    }

    group_blob_header_t hdr = { .version = GROUP_BLOB_VERSION, .count = (uint16_t)used };
    memcpy(blob, &hdr, sizeof(hdr));

    zbc_group_t *out = (zbc_group_t *)(blob + sizeof(hdr));
    for (int i = 0; i < CONFIG_ZBC_MAX_GROUPS; i++) {
        if (!s_groups[i].in_use) {
            continue;
        }
        *out = s_groups[i];
        out->last_pct = ZBC_UNKNOWN;  /* live state, not a property of the group */
        out++;
    }

    err = nvs_set_blob(nvs, GROUP_NVS_KEY, blob, blob_size);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    free(blob);
    nvs_close(nvs);
    return err;
}

bool group_db_blob_ok(const void *blob, size_t len)
{
    group_blob_header_t hdr;
    if (!blob || len < sizeof(hdr)) {
        return false;
    }
    memcpy(&hdr, blob, sizeof(hdr));
    return hdr.version == GROUP_BLOB_VERSION && len == sizeof(hdr) + (size_t)hdr.count * sizeof(zbc_group_t);
}

static void gdb_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open(GROUP_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    size_t blob_size = 0;
    esp_err_t err = nvs_get_blob(nvs, GROUP_NVS_KEY, NULL, &blob_size);
    if (err != ESP_OK || blob_size < sizeof(group_blob_header_t)) {
        nvs_close(nvs);
        return;
    }

    uint8_t *blob = malloc(blob_size);
    if (!blob) {
        nvs_close(nvs);
        return;
    }
    err = nvs_get_blob(nvs, GROUP_NVS_KEY, blob, &blob_size);
    nvs_close(nvs);
    if (err != ESP_OK) {
        free(blob);
        return;
    }

    if (!group_db_blob_ok(blob, blob_size)) {
        ESP_LOGW(TAG, "Stored group list is from an incompatible build, discarding");
        free(blob);
        return;
    }
    group_blob_header_t hdr;
    memcpy(&hdr, blob, sizeof(hdr));

    const zbc_group_t *in = (const zbc_group_t *)(blob + sizeof(hdr));
    int loaded = 0;
    for (int i = 0; i < hdr.count && loaded < CONFIG_ZBC_MAX_GROUPS; i++) {
        s_groups[loaded]          = in[i];
        s_groups[loaded].in_use   = true;
        s_groups[loaded].last_pct = ZBC_UNKNOWN;
        loaded++;
    }
    free(blob);
    ESP_LOGI(TAG, "Restored %d group(s) from NVS", loaded);
}

esp_err_t group_db_init(void)
{
    if (s_lock) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    memset(s_groups, 0, sizeof(s_groups));
    GDB_LOCK();
    gdb_load();
    GDB_UNLOCK();
    return ESP_OK;
}

esp_err_t group_db_create(const char *name, uint16_t *out_id)
{
    if (!name || !name[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    GDB_LOCK();
    zbc_group_t *slot = NULL;
    uint16_t     next = 1;
    for (int i = 0; i < CONFIG_ZBC_MAX_GROUPS; i++) {
        if (!s_groups[i].in_use) {
            if (!slot) {
                slot = &s_groups[i];
            }
        } else if (s_groups[i].id >= next) {
            next = s_groups[i].id + 1;
        }
    }
    if (!slot) {
        GDB_UNLOCK();
        ESP_LOGE(TAG, "Group table full (%d)", CONFIG_ZBC_MAX_GROUPS);
        return ESP_ERR_NO_MEM;
    }

    memset(slot, 0, sizeof(*slot));
    slot->in_use   = true;
    slot->id       = next;
    slot->last_pct = ZBC_UNKNOWN;
    strlcpy(slot->name, name, sizeof(slot->name));

    if (out_id) {
        *out_id = slot->id;
    }
    esp_err_t err = gdb_save_locked();
    GDB_UNLOCK();
    return err;
}

esp_err_t group_db_rename(uint16_t id, const char *name)
{
    if (!name || !name[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    GDB_LOCK();
    zbc_group_t *g = find_by_id(id);
    if (!g) {
        GDB_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(g->name, name, sizeof(g->name));
    esp_err_t err = gdb_save_locked();
    GDB_UNLOCK();
    return err;
}

esp_err_t group_db_remove(uint16_t id)
{
    GDB_LOCK();
    zbc_group_t *g = find_by_id(id);
    if (!g) {
        GDB_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    memset(g, 0, sizeof(*g));
    esp_err_t err = gdb_save_locked();
    GDB_UNLOCK();
    return err;
}

esp_err_t group_db_add_member(uint16_t id, const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    GDB_LOCK();
    zbc_group_t *g = find_by_id(id);
    if (!g) {
        GDB_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    if (member_index(g, ieee, ep) >= 0) {
        GDB_UNLOCK();
        return ESP_OK;  /* already a member; adding twice is not an error */
    }
    if (g->member_count >= CONFIG_ZBC_MAX_GROUP_MEMBERS) {
        GDB_UNLOCK();
        return ESP_ERR_NO_MEM;
    }
    memcpy(g->members[g->member_count].ieee, ieee, ZBC_IEEE_LEN);
    g->members[g->member_count].ep = ep;
    g->member_count++;
    esp_err_t err = gdb_save_locked();
    GDB_UNLOCK();
    return err;
}

/* Caller holds the lock. */
static bool drop_member_locked(zbc_group_t *g, int idx)
{
    if (idx < 0 || idx >= g->member_count) {
        return false;
    }
    for (int i = idx; i + 1 < g->member_count; i++) {
        g->members[i] = g->members[i + 1];
    }
    g->member_count--;
    memset(&g->members[g->member_count], 0, sizeof(g->members[0]));
    return true;
}

esp_err_t group_db_remove_member(uint16_t id, const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    GDB_LOCK();
    zbc_group_t *g = find_by_id(id);
    if (!g) {
        GDB_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    if (!drop_member_locked(g, member_index(g, ieee, ep))) {
        GDB_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = gdb_save_locked();
    GDB_UNLOCK();
    return err;
}

bool group_db_get(uint16_t id, zbc_group_t *out)
{
    GDB_LOCK();
    zbc_group_t *g = find_by_id(id);
    if (g && out) {
        *out = *g;
    }
    bool found = g != NULL;
    GDB_UNLOCK();
    return found;
}

size_t group_db_snapshot(zbc_group_t *buf, size_t max)
{
    size_t n = 0;
    GDB_LOCK();
    for (int i = 0; i < CONFIG_ZBC_MAX_GROUPS && n < max; i++) {
        if (s_groups[i].in_use) {
            buf[n++] = s_groups[i];
        }
    }
    GDB_UNLOCK();
    return n;
}

void group_db_note_level(uint16_t id, int pct)
{
    GDB_LOCK();
    zbc_group_t *g = find_by_id(id);
    if (g) {
        g->last_pct = (pct >= 0 && pct <= 100) ? (int16_t)pct : ZBC_UNKNOWN;
        zbc_state_bump();             /* shown as the group's brightness */
    }
    GDB_UNLOCK();
}

void group_db_forget_device(const uint8_t ieee[ZBC_IEEE_LEN])
{
    bool changed = false;
    GDB_LOCK();
    for (int i = 0; i < CONFIG_ZBC_MAX_GROUPS; i++) {
        if (!s_groups[i].in_use) {
            continue;
        }
        for (int m = s_groups[i].member_count - 1; m >= 0; m--) {
            if (memcmp(s_groups[i].members[m].ieee, ieee, ZBC_IEEE_LEN) == 0) {
                drop_member_locked(&s_groups[i], m);
                changed = true;
            }
        }
    }
    if (changed) {
        gdb_save_locked();
    }
    GDB_UNLOCK();
}

esp_err_t group_db_erase(void)
{
    GDB_LOCK();
    memset(s_groups, 0, sizeof(s_groups));
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(GROUP_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_erase_key(nvs, GROUP_NVS_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    GDB_UNLOCK();
    return err;
}
