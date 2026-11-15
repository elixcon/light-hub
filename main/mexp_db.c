#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "mexp_db.h"
#include "state_rev.h"

static const char *TAG = "mexp_db";

#define MEXP_NVS_NAMESPACE "zbc"
/* In config_backup.c's list too: a backup carries this table under this key. */
#define MEXP_NVS_KEY       "matter_dev"
#define MEXP_BLOB_VERSION  1

/* Stored as a header and `count` entries, nothing after them: kept by count, so a
 * build with another CONFIG_ZBC_MAX_MATTER_EXPOSED loads what fits rather than
 * discarding the lot. */
typedef struct {
    uint16_t version;
    uint16_t count;
} mexp_hdr_t;

_Static_assert(sizeof(mexp_entry_t) == ZBC_IEEE_LEN + 1, "mexp_entry_t is stored as it is laid out");

#define MEXP_MAX CONFIG_ZBC_MAX_MATTER_EXPOSED

static struct {
    mexp_hdr_t   hdr;
    mexp_entry_t e[MEXP_MAX];
} s_blob;
static SemaphoreHandle_t s_lock;

static int index_of(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    for (int i = 0; i < s_blob.hdr.count; i++) {
        if (s_blob.e[i].ep == ep && memcmp(s_blob.e[i].ieee, ieee, ZBC_IEEE_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

/* Nothing past `count` is read, saved or listed, so the last entry fills the gap. */
static void remove_at_locked(int i)
{
    s_blob.e[i] = s_blob.e[--s_blob.hdr.count];
}

static esp_err_t save_locked(void)
{
    zbc_state_bump();          /* the device's page shows the setting */
    nvs_handle_t nvs;
    esp_err_t    err = nvs_open(MEXP_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(nvs, MEXP_NVS_KEY, &s_blob,
                       sizeof(s_blob.hdr) + (size_t)s_blob.hdr.count * sizeof(mexp_entry_t));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not save the Matter device list: %s", esp_err_to_name(err));
    }
    return err;
}

bool mexp_db_blob_ok(const void *blob, size_t len)
{
    mexp_hdr_t hdr;
    if (!blob || len < sizeof(hdr)) {
        return false;
    }
    memcpy(&hdr, blob, sizeof(hdr));
    return hdr.version == MEXP_BLOB_VERSION && len == sizeof(hdr) + (size_t)hdr.count * sizeof(mexp_entry_t);
}

esp_err_t mexp_db_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_blob, 0, sizeof(s_blob));
    s_blob.hdr.version = MEXP_BLOB_VERSION;

    nvs_handle_t nvs;
    if (nvs_open(MEXP_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return ESP_OK;
    }
    size_t len = 0;
    esp_err_t err = nvs_get_blob(nvs, MEXP_NVS_KEY, NULL, &len);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return ESP_OK;
    }
    uint8_t *raw = malloc(len);
    if (!raw) {
        nvs_close(nvs);
        return ESP_ERR_NO_MEM;
    }
    err = nvs_get_blob(nvs, MEXP_NVS_KEY, raw, &len);
    nvs_close(nvs);
    if (err != ESP_OK || !mexp_db_blob_ok(raw, len)) {
        ESP_LOGW(TAG, "Stored Matter device list is from an incompatible build, discarding");
        free(raw);
        return ESP_OK;
    }
    mexp_hdr_t hdr;
    memcpy(&hdr, raw, sizeof(hdr));
    const uint16_t keep = hdr.count > MEXP_MAX ? MEXP_MAX : hdr.count;
    memcpy(s_blob.e, raw + sizeof(hdr), (size_t)keep * sizeof(mexp_entry_t));
    s_blob.hdr.count = keep;
    free(raw);
    if (keep < hdr.count) {
        ESP_LOGW(TAG, "%u of %u devices shown in Matter kept: this build has room for %d", keep, hdr.count,
                 MEXP_MAX);
    }
    if (keep) {
        ESP_LOGI(TAG, "%u device endpoint(s) shown in Matter on their own", keep);
    }
    return ESP_OK;
}

bool mexp_db_has(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    if (!s_lock || !ieee) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool has = index_of(ieee, ep) >= 0;
    xSemaphoreGive(s_lock);
    return has;
}

esp_err_t mexp_db_set(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, bool expose)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!ieee || ep == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* A change that could not be stored is taken back: the caller is told it
     * failed, and an endpoint the bridge made on the strength of it would be gone
     * again, with whatever a controller had set up on it, at the next restart. */
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int i = index_of(ieee, ep);
    if (expose && i < 0) {
        if (s_blob.hdr.count >= MEXP_MAX) {
            err = ESP_ERR_NO_MEM;
        } else {
            memcpy(s_blob.e[s_blob.hdr.count].ieee, ieee, ZBC_IEEE_LEN);
            s_blob.e[s_blob.hdr.count].ep = ep;
            s_blob.hdr.count++;
            err = save_locked();
            if (err != ESP_OK) {
                s_blob.hdr.count--;
            }
        }
    } else if (!expose && i >= 0) {
        const mexp_entry_t was = s_blob.e[i];
        remove_at_locked(i);
        err = save_locked();
        if (err != ESP_OK) {
            /* Back at the end, where an entry's place means nothing. */
            s_blob.e[s_blob.hdr.count++] = was;
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

size_t mexp_db_list(mexp_entry_t *out, size_t max)
{
    if (!s_lock || !out) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t n = s_blob.hdr.count < max ? s_blob.hdr.count : max;
    memcpy(out, s_blob.e, n * sizeof(mexp_entry_t));
    xSemaphoreGive(s_lock);
    return n;
}

/* Drops the device's entries - all of them, or the one endpoint's - with one save. */
static void forget(const uint8_t ieee[ZBC_IEEE_LEN], int ep)
{
    if (!s_lock || !ieee) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool dropped = false;
    for (int i = 0; i < s_blob.hdr.count;) {
        if (memcmp(s_blob.e[i].ieee, ieee, ZBC_IEEE_LEN) == 0 && (ep < 0 || s_blob.e[i].ep == ep)) {
            remove_at_locked(i);
            dropped = true;
        } else {
            i++;
        }
    }
    if (dropped) {
        save_locked();
    }
    xSemaphoreGive(s_lock);
}

void mexp_db_forget_device(const uint8_t ieee[ZBC_IEEE_LEN])
{
    forget(ieee, -1);
}

void mexp_db_forget_endpoint(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    forget(ieee, ep);
}

esp_err_t mexp_db_erase(void)
{
    if (!s_lock) {
        return ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_blob, 0, sizeof(s_blob));
    s_blob.hdr.version = MEXP_BLOB_VERSION;
    nvs_handle_t nvs;
    esp_err_t    err = nvs_open(MEXP_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_erase_key(nvs, MEXP_NVS_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    zbc_state_bump();
    xSemaphoreGive(s_lock);
    return err;
}
