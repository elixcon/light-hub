#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "al_db.h"
#include "state_rev.h"

static const char *TAG = "al_db";

/* Same namespace as the other tables, its own key and version, for the reason
 * every table here has its own: a change to one must not discard another. */
#define AL_NVS_NAMESPACE "zbc"
/* In config_backup.c's list too: a backup carries this table under this key. */
#define AL_NVS_KEY       "adaptive"
#define AL_BLOB_VERSION  2

/* A group has a record while either setting differs from its default, so a
 * group with no record is ZBC_AL_EXPOSE_OFF and ZBC_TURN_ON_LAST. `turn_on` was a
 * reserved byte, always written as 0 - which is ZBC_TURN_ON_LAST - so records from
 * before it read as they always behaved, and the version stays. */
typedef struct {
    uint16_t group_id;
    uint8_t  expose;          /* zbc_al_expose_t */
    uint8_t  turn_on;         /* zbc_turn_on_t */
} al_entry_t;

typedef struct {
    uint16_t   version;
    uint16_t   count;
    al_entry_t e[CONFIG_ZBC_MAX_GROUPS];
} al_blob_t;

/* Version 1 was a list of groups with a switch that turned Adaptive Lighting on at
 * every switch-on, aimed at Apple Home. Frozen here for the migration only. */
typedef struct {
    uint16_t version;
    uint16_t count;
    uint16_t ids[CONFIG_ZBC_MAX_GROUPS];
} al_blob_v1_t;

/* Each group's day: a key of its own, so a change to its format costs the days
 * and nothing else. A group with no row follows AL_DAY_DEFAULT; a row is written
 * the first time a group's day is set, and dropped when the group is deleted.
 * In config_backup.c's list too. */
#define AL_DAYS_KEY     "al_days"
#define AL_DAYS_VERSION 1

typedef struct {
    uint16_t         group_id;
    uint16_t         reserved;
    zbc_al_profile_t day;
} al_day_entry_t;

typedef struct {
    uint16_t       version;
    uint16_t       count;
    al_day_entry_t e[CONFIG_ZBC_MAX_GROUPS];
} al_days_blob_t;

/* A little warmer and dimmer than the lamps' own curves at night, a little cooler
 * at noon - a shift on top of a curve somebody drew, not a replacement for it. */
static const zbc_al_profile_t AL_DAY_DEFAULT = {
    .k_midnight = -1500, .k_noon = 500, .k_base = 4000, .pct_midnight = 20, .pct_noon = 100,
    .rise_win = { ZBC_AL_NO_TIME, ZBC_AL_NO_TIME }, .set_win = { ZBC_AL_NO_TIME, ZBC_AL_NO_TIME },
};

/* Which groups were running the mode: a key of its own, so the settings above
 * are never rewritten for it, and a format change here costs one resume rather
 * than anybody's settings. Written only when the set changes - a few times a day. */
#define AL_RUN_KEY     "al_running"
#define AL_RUN_VERSION 1

typedef struct {
    uint16_t version;
    uint16_t count;
    uint16_t ids[CONFIG_ZBC_MAX_GROUPS];
} al_run_blob_t;

static al_blob_t         s_blob;
static al_days_blob_t    s_days;
static al_run_blob_t     s_run;
static SemaphoreHandle_t s_lock;

static int index_of(uint16_t id)
{
    for (int i = 0; i < s_blob.count; i++) {
        if (s_blob.e[i].group_id == id) {
            return i;
        }
    }
    return -1;
}

static int day_index_of(uint16_t id)
{
    for (int i = 0; i < s_days.count; i++) {
        if (s_days.e[i].group_id == id) {
            return i;
        }
    }
    return -1;
}

static int run_index_of(uint16_t id)
{
    for (int i = 0; i < s_run.count; i++) {
        if (s_run.ids[i] == id) {
            return i;
        }
    }
    return -1;
}

static esp_err_t write_blob(const char *key, const void *blob, size_t len)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(AL_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(nvs, key, blob, len);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t save_locked(void)
{
    zbc_state_bump();          /* the group page shows the setting */
    return write_blob(AL_NVS_KEY, &s_blob, sizeof(s_blob));
}

static esp_err_t save_days_locked(void)
{
    zbc_state_bump();          /* the room page shows what its day gives now */
    return write_blob(AL_DAYS_KEY, &s_days, sizeof(s_days));
}

static esp_err_t save_run_locked(void)
{
    return write_blob(AL_RUN_KEY, &s_run, sizeof(s_run));
}

static void load_running(nvs_handle_t nvs)
{
    al_run_blob_t in;
    size_t        len = sizeof(in);
    if (nvs_get_blob(nvs, AL_RUN_KEY, &in, &len) != ESP_OK) {
        return;
    }
    if (len == sizeof(in) && in.version == AL_RUN_VERSION && in.count <= CONFIG_ZBC_MAX_GROUPS) {
        s_run = in;
        if (s_run.count) {
            ESP_LOGI(TAG, "%u group(s) were running Adaptive Lighting", s_run.count);
        }
    } else {
        ESP_LOGW(TAG, "Stored Adaptive Lighting running list is from an incompatible build, discarding");
    }
}

static bool al_values_valid(const zbc_al_profile_t *p)
{
    return p->k_midnight >= -ZBC_AL_OFFSET_K_LIMIT && p->k_midnight <= ZBC_AL_OFFSET_K_LIMIT &&
           p->k_noon >= -ZBC_AL_OFFSET_K_LIMIT && p->k_noon <= ZBC_AL_OFFSET_K_LIMIT &&
           p->k_base >= ZBC_AL_BASE_K_MIN && p->k_base <= ZBC_AL_BASE_K_MAX &&
           p->pct_midnight >= 1 && p->pct_midnight <= 100 &&
           p->pct_noon >= 1 && p->pct_noon <= 100;
}

static bool al_window_valid(const int16_t w[2])
{
    for (int i = 0; i < 2; i++) {
        if (w[i] != ZBC_AL_NO_TIME && (w[i] < 0 || w[i] >= ZBC_AL_DAY_MIN)) {
            return false;
        }
    }
    return w[0] == ZBC_AL_NO_TIME || w[1] == ZBC_AL_NO_TIME || w[0] <= w[1];
}

/* Strictly ascending x inside the two ends, y within lo..hi. */
static bool al_points_valid(uint8_t n, const uint8_t *x, const int16_t *y, int lo, int hi)
{
    if (n > ZBC_AL_SHAPE_POINTS) {
        return false;
    }
    for (uint8_t i = 0; i < n; i++) {
        if (x[i] < 2 || x[i] > 99 || (i > 0 && x[i] <= x[i - 1]) || y[i] < lo || y[i] > hi) {
            return false;
        }
    }
    return true;
}

static bool al_shape_valid(const zbc_al_profile_t *p)
{
    int16_t pct_y[ZBC_AL_SHAPE_POINTS];
    for (int i = 0; i < ZBC_AL_SHAPE_POINTS; i++) {
        pct_y[i] = p->pct_y[i];
    }
    return al_points_valid(p->n_pct, p->pct_x, pct_y, 1, 100) &&
           al_points_valid(p->n_k, p->k_x, p->k_y, -ZBC_AL_OFFSET_K_LIMIT, ZBC_AL_OFFSET_K_LIMIT);
}

static bool al_profile_valid(const zbc_al_profile_t *p)
{
    return al_values_valid(p) && al_window_valid(p->rise_win) && al_window_valid(p->set_win) && al_shape_valid(p);
}

bool al_db_day_ok(const zbc_al_profile_t *day)
{
    return day && al_profile_valid(day);
}

bool al_db_days_blob_ok(const void *blob, size_t len)
{
    uint16_t hdr[2];           /* version, count */
    if (!blob || len != sizeof(al_days_blob_t)) {
        return false;
    }
    memcpy(hdr, blob, sizeof(hdr));
    if (hdr[0] != AL_DAYS_VERSION || hdr[1] > CONFIG_ZBC_MAX_GROUPS) {
        return false;
    }
    /* A row at a time: the restore checks this on the HTTP task's stack. */
    const uint8_t *rows = (const uint8_t *)blob + offsetof(al_days_blob_t, e);
    for (uint16_t i = 0; i < hdr[1]; i++) {
        al_day_entry_t e;
        memcpy(&e, rows + i * sizeof(e), sizeof(e));
        if (!al_profile_valid(&e.day)) {
            return false;
        }
    }
    return true;
}

static void load_days(nvs_handle_t nvs)
{
    size_t          len = sizeof(s_days);
    const esp_err_t err = nvs_get_blob(nvs, AL_DAYS_KEY, &s_days, &len);
    if (err == ESP_OK && al_db_days_blob_ok(&s_days, len)) {
        if (s_days.count) {
            ESP_LOGI(TAG, "Adaptive Lighting days for %u group(s)", s_days.count);
        }
        return;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Stored Adaptive Lighting days are from an incompatible build, discarding");
    }
    memset(&s_days, 0, sizeof(s_days));
    s_days.version = AL_DAYS_VERSION;
}

bool al_db_blob_ok(const void *blob, size_t len)
{
    uint16_t hdr[2];           /* version, count */
    if (!blob || len < sizeof(hdr)) {
        return false;
    }
    memcpy(hdr, blob, sizeof(hdr));
    return (len == sizeof(al_blob_t) && hdr[0] == AL_BLOB_VERSION && hdr[1] <= CONFIG_ZBC_MAX_GROUPS) ||
           (len == sizeof(al_blob_v1_t) && hdr[0] == 1);
}

esp_err_t al_db_init(void)
{
    if (s_lock) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_blob, 0, sizeof(s_blob));
    s_blob.version = AL_BLOB_VERSION;
    memset(&s_days, 0, sizeof(s_days));
    s_days.version = AL_DAYS_VERSION;
    memset(&s_run, 0, sizeof(s_run));
    s_run.version = AL_RUN_VERSION;

    nvs_handle_t nvs;
    if (nvs_open(AL_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return ESP_OK;
    }
    load_running(nvs);
    load_days(nvs);
    al_blob_t in;
    size_t    len = sizeof(in);
    esp_err_t err = nvs_get_blob(nvs, AL_NVS_KEY, &in, &len);
    nvs_close(nvs);
    if (err != ESP_OK) {
        return ESP_OK;
    }
    if (!al_db_blob_ok(&in, len)) {
        ESP_LOGW(TAG, "Stored Adaptive Lighting groups are from an incompatible build, discarding");
        return ESP_OK;
    }

    if (in.version == AL_BLOB_VERSION) {
        s_blob = in;
        ESP_LOGI(TAG, "Adaptive Lighting and turn-on settings for %u group(s)", s_blob.count);
        return ESP_OK;
    }
    /* Version 1, the one other al_db_blob_ok() takes. The old switch only ever
     * existed to get Adaptive Lighting into Apple Home, so a group that had it on
     * is offered through the endpoint Home can use. */
    al_blob_v1_t v1;
    memcpy(&v1, &in, sizeof(v1));
    for (int i = 0; i < v1.count && i < CONFIG_ZBC_MAX_GROUPS; i++) {
        s_blob.e[s_blob.count].group_id = v1.ids[i];
        s_blob.e[s_blob.count].expose   = ZBC_AL_EXPOSE_ENDPOINT;
        s_blob.count++;
    }
    ESP_LOGI(TAG, "Converted %u Adaptive Lighting group(s) from the previous format", s_blob.count);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    save_locked();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

/* Stores both settings for a group, dropping its record once both are back at
 * their defaults. Caller holds s_lock. */
static esp_err_t put_locked(uint16_t group_id, uint8_t expose, uint8_t turn_on)
{
    const int  i       = index_of(group_id);
    const bool plain   = expose == ZBC_AL_EXPOSE_OFF && turn_on == ZBC_TURN_ON_LAST;
    if (i < 0) {
        if (plain) {
            return ESP_OK;
        }
        if (s_blob.count >= CONFIG_ZBC_MAX_GROUPS) {
            return ESP_ERR_NO_MEM;
        }
        s_blob.e[s_blob.count] = (al_entry_t){ .group_id = group_id, .expose = expose, .turn_on = turn_on };
        s_blob.count++;
        return save_locked();
    }
    if (plain) {
        s_blob.e[i] = s_blob.e[--s_blob.count];
        memset(&s_blob.e[s_blob.count], 0, sizeof(s_blob.e[0]));
        return save_locked();
    }
    if (s_blob.e[i].expose == expose && s_blob.e[i].turn_on == turn_on) {
        return ESP_OK;
    }
    s_blob.e[i].expose  = expose;
    s_blob.e[i].turn_on = turn_on;
    return save_locked();
}

uint8_t al_db_expose(uint16_t group_id)
{
    if (!s_lock) {
        return ZBC_AL_EXPOSE_OFF;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int     i = index_of(group_id);
    const uint8_t e = i >= 0 ? s_blob.e[i].expose : ZBC_AL_EXPOSE_OFF;
    xSemaphoreGive(s_lock);
    return e;
}

esp_err_t al_db_set(uint16_t group_id, uint8_t expose)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (expose >= ZBC_AL_EXPOSE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int       i   = index_of(group_id);
    const esp_err_t err = put_locked(group_id, expose, i >= 0 ? s_blob.e[i].turn_on : ZBC_TURN_ON_LAST);
    xSemaphoreGive(s_lock);
    return err;
}

uint8_t al_db_turn_on(uint16_t group_id)
{
    if (!s_lock) {
        return ZBC_TURN_ON_LAST;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int     i = index_of(group_id);
    const uint8_t t = i >= 0 && s_blob.e[i].turn_on < ZBC_TURN_ON_COUNT ? s_blob.e[i].turn_on
                                                                       : ZBC_TURN_ON_LAST;
    xSemaphoreGive(s_lock);
    return t;
}

esp_err_t al_db_set_turn_on(uint16_t group_id, uint8_t turn_on)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (turn_on >= ZBC_TURN_ON_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int       i   = index_of(group_id);
    const esp_err_t err = put_locked(group_id, i >= 0 ? s_blob.e[i].expose : ZBC_AL_EXPOSE_OFF, turn_on);
    xSemaphoreGive(s_lock);
    return err;
}

void al_db_day(uint16_t group_id, zbc_al_profile_t *out)
{
    *out = AL_DAY_DEFAULT;
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int i = day_index_of(group_id);
    if (i >= 0) {
        *out = s_days.e[i].day;
    }
    xSemaphoreGive(s_lock);
}

esp_err_t al_db_set_day(uint16_t group_id, const zbc_al_profile_t *day)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!al_db_day_ok(day)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    int       i   = day_index_of(group_id);
    if (i < 0) {
        if (s_days.count >= CONFIG_ZBC_MAX_GROUPS) {
            err = ESP_ERR_NO_MEM;
        } else {
            i = s_days.count++;
            memset(&s_days.e[i], 0, sizeof(s_days.e[i]));
            s_days.e[i].group_id = group_id;
        }
    }
    if (err == ESP_OK) {
        s_days.e[i].day = *day;
        err = save_days_locked();
    }
    xSemaphoreGive(s_lock);
    return err;
}

bool al_db_day_has_limits(const zbc_al_profile_t *d)
{
    return d->rise_win[0] != ZBC_AL_NO_TIME || d->rise_win[1] != ZBC_AL_NO_TIME ||
           d->set_win[0] != ZBC_AL_NO_TIME || d->set_win[1] != ZBC_AL_NO_TIME;
}

bool al_db_any_limits(void)
{
    if (!s_lock) {
        return false;
    }
    bool any = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_days.count && !any; i++) {
        any = al_db_day_has_limits(&s_days.e[i].day);
    }
    xSemaphoreGive(s_lock);
    return any;
}

size_t al_db_running(uint16_t *ids, size_t max)
{
    if (!s_lock) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = 0;
    for (; n < s_run.count && n < max; n++) {
        ids[n] = s_run.ids[n];
    }
    xSemaphoreGive(s_lock);
    return n;
}

esp_err_t al_db_set_running(const uint16_t *ids, size_t n)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (n > CONFIG_ZBC_MAX_GROUPS) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* Sets, not lists: the ids have no duplicates, so the same count and every one
     * already stored is the same set. */
    bool same = s_run.count == n;
    for (size_t i = 0; i < n && same; i++) {
        same = run_index_of(ids[i]) >= 0;
    }
    esp_err_t err = ESP_OK;
    if (!same) {
        s_run.count = (uint16_t)n;
        memset(s_run.ids, 0, sizeof(s_run.ids));
        memcpy(s_run.ids, ids, n * sizeof(ids[0]));
        err = save_run_locked();
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t al_db_forget(uint16_t group_id)
{
    if (!s_lock) {
        return ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = put_locked(group_id, ZBC_AL_EXPOSE_OFF, ZBC_TURN_ON_LAST);
    const int d   = day_index_of(group_id);
    if (d >= 0) {
        s_days.e[d] = s_days.e[--s_days.count];
        memset(&s_days.e[s_days.count], 0, sizeof(s_days.e[0]));
        const esp_err_t day_err = save_days_locked();
        if (err == ESP_OK) {
            err = day_err;
        }
    }
    const int r   = run_index_of(group_id);
    if (r >= 0) {
        s_run.ids[r]            = s_run.ids[--s_run.count];
        s_run.ids[s_run.count]  = 0;
        const esp_err_t run_err = save_run_locked();
        if (err == ESP_OK) {
            err = run_err;
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t al_db_erase(void)
{
    if (!s_lock) {
        return ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_blob, 0, sizeof(s_blob));
    s_blob.version = AL_BLOB_VERSION;
    memset(&s_days, 0, sizeof(s_days));
    s_days.version = AL_DAYS_VERSION;
    memset(&s_run, 0, sizeof(s_run));
    s_run.version = AL_RUN_VERSION;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(AL_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_erase_key(nvs, AL_NVS_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        const char *const more[] = { AL_DAYS_KEY, AL_RUN_KEY };
        for (size_t i = 0; i < sizeof(more) / sizeof(more[0]); i++) {
            const esp_err_t e = nvs_erase_key(nvs, more[i]);
            if (err == ESP_OK && e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) {
                err = e;
            }
        }
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    xSemaphoreGive(s_lock);
    return err;
}
