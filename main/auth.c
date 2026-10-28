#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "psa/crypto.h"

#include "auth.h"

static const char *TAG = "auth";

/* Same namespace as every other table, its own key and its own version - a
 * change here must not throw away anybody's device names, and the reverse. */
#define AUTH_NVS_NAMESPACE "zbc"
/* In config_backup.c's list too: a backup carries this table under this key. */
#define AUTH_NVS_KEY       "auth"
#define AUTH_BLOB_VERSION  1

typedef struct {
    uint8_t  in_use;
    uint8_t  kind;
    uint16_t id;
    char     name[ZBC_AUTH_NAME_LEN];
    uint8_t  hash[32];
} auth_record_t;

typedef struct {
    uint32_t version;
    uint16_t count;
    uint16_t reserved;
} auth_blob_header_t;

static auth_record_t     s_keys[CONFIG_ZBC_MAX_AUTH_KEYS];
/* Beside the table rather than in it, so the stored record never carries it. */
static int64_t           s_last_used[CONFIG_ZBC_MAX_AUTH_KEYS];
static SemaphoreHandle_t s_lock;

/* A spinlock, not the mutex: the button sampler opens the window from the
 * esp_timer task, which must never wait on anything. */
static portMUX_TYPE s_window_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t      s_window_until;

#define AUTH_LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define AUTH_UNLOCK() xSemaphoreGiveRecursive(s_lock)

/* >>> host-test region: authkey <<< */

/* Crockford base32: no I, L, O or U, so nothing in a key can be mistaken for
 * something else when it is read off a piece of paper. 32 symbols, so one random
 * byte masked to five bits picks one with no bias. */
static const char AUTH_ALPHABET[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static void auth_encode(const uint8_t random[ZBC_AUTH_KEY_CHARS], char *out)
{
    for (int i = 0; i < ZBC_AUTH_KEY_CHARS; i++) {
        out[i] = AUTH_ALPHABET[random[i] & 0x1F];
    }
    out[ZBC_AUTH_KEY_CHARS] = '\0';
}

/* The forgiving direction. Somebody typing a key in from a note will get the
 * case wrong, drop or add dashes, and read a 0 as an O - all of which are
 * accepted, because the alphabet was chosen so that none of them is ambiguous.
 * Anything else, including a U, means it is not a key. */
bool auth_normalize(const char *in, char *out)
{
    if (!in || !out) {
        return false;
    }
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned char c = *p;
        if (c == '-' || c == ' ') {
            continue;
        }
        if (c >= 'a' && c <= 'z') {
            c = (unsigned char)(c - 'a' + 'A');
        }
        if (c == 'O') {
            c = '0';
        } else if (c == 'I' || c == 'L') {
            c = '1';
        }
        if (!strchr(AUTH_ALPHABET, c) || n == ZBC_AUTH_KEY_CHARS) {
            return false;
        }
        out[n++] = (char)c;
    }
    if (n != ZBC_AUTH_KEY_CHARS) {
        return false;
    }
    out[n] = '\0';
    return true;
}

/* Four groups of five, the way a key is shown and written down. */
static void auth_format(const char *canon, char *out)
{
    size_t n = 0;
    for (int i = 0; i < ZBC_AUTH_KEY_CHARS; i++) {
        if (i > 0 && i % 5 == 0) {
            out[n++] = '-';
        }
        out[n++] = canon[i];
    }
    out[n] = '\0';
}

/* >>> end host-test region: authkey <<< */

/* SHA-256 through PSA: IDF 6's Mbed TLS keeps mbedtls_sha256() private. The
 * digest is the same, so keys hashed by an older build still match. */
static void hash_key(const char *canon, uint8_t out[32])
{
    size_t len;
    psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)canon, ZBC_AUTH_KEY_CHARS, out, 32, &len);
}

static esp_err_t save_locked(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(AUTH_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    size_t used = 0;
    for (int i = 0; i < CONFIG_ZBC_MAX_AUTH_KEYS; i++) {
        used += s_keys[i].in_use ? 1 : 0;
    }
    /* With nothing stored the key is erased rather than written empty, so "no
     * keys" looks the same on flash whichever way it came about. */
    if (used == 0) {
        err = nvs_erase_key(nvs, AUTH_NVS_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        size_t   size = sizeof(auth_blob_header_t) + used * sizeof(auth_record_t);
        uint8_t *blob = calloc(1, size);
        if (!blob) {
            nvs_close(nvs);
            return ESP_ERR_NO_MEM;
        }
        auth_blob_header_t hdr = { .version = AUTH_BLOB_VERSION, .count = (uint16_t)used };
        memcpy(blob, &hdr, sizeof(hdr));
        auth_record_t *out = (auth_record_t *)(blob + sizeof(hdr));
        for (int i = 0; i < CONFIG_ZBC_MAX_AUTH_KEYS; i++) {
            if (s_keys[i].in_use) {
                *out++ = s_keys[i];
            }
        }
        err = nvs_set_blob(nvs, AUTH_NVS_KEY, blob, size);
        free(blob);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

bool auth_blob_ok(const void *blob, size_t len)
{
    auth_blob_header_t hdr;
    if (!blob || len < sizeof(hdr)) {
        return false;
    }
    memcpy(&hdr, blob, sizeof(hdr));
    return hdr.version == AUTH_BLOB_VERSION && len == sizeof(hdr) + (size_t)hdr.count * sizeof(auth_record_t);
}

static void load_locked(void)
{
    nvs_handle_t nvs;
    if (nvs_open(AUTH_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    size_t size = 0;
    if (nvs_get_blob(nvs, AUTH_NVS_KEY, NULL, &size) != ESP_OK || size < sizeof(auth_blob_header_t)) {
        nvs_close(nvs);
        return;
    }
    uint8_t *blob = malloc(size);
    if (!blob) {
        nvs_close(nvs);
        return;
    }
    esp_err_t err = nvs_get_blob(nvs, AUTH_NVS_KEY, blob, &size);
    nvs_close(nvs);
    if (err != ESP_OK) {
        free(blob);
        return;
    }

    if (!auth_blob_ok(blob, size)) {
        /* Discarding keys opens the coordinator to everyone, which is the
         * failsafe working as designed rather than a silent lock-out - but it
         * should never happen quietly. */
        ESP_LOGW(TAG, "Stored access keys are from an incompatible build, discarding - access is open");
        free(blob);
        return;
    }
    auth_blob_header_t hdr;
    memcpy(&hdr, blob, sizeof(hdr));
    const auth_record_t *in = (const auth_record_t *)(blob + sizeof(hdr));
    int loaded = 0;
    for (int i = 0; i < hdr.count && loaded < CONFIG_ZBC_MAX_AUTH_KEYS; i++) {
        s_keys[loaded]        = in[i];
        s_keys[loaded].in_use = 1;
        loaded++;
    }
    free(blob);
    ESP_LOGI(TAG, "%d access key(s); %s", loaded,
             loaded ? "the web UI needs one" : "the web UI is open to everyone");
}

esp_err_t auth_init(void)
{
    if (s_lock) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    AUTH_LOCK();
    memset(s_keys, 0, sizeof(s_keys));
    load_locked();
    if (!auth_key_count()) {
        ESP_LOGI(TAG, "No access keys; the web UI is open to everyone");
    }
    AUTH_UNLOCK();
    return ESP_OK;
}

uint16_t auth_check(const char *presented)
{
    char canon[ZBC_AUTH_KEY_CANON_LEN];
    if (!s_lock || !auth_normalize(presented, canon)) {
        return 0;
    }
    uint8_t h[32];
    hash_key(canon, h);

    /* Every record is compared all the way through. What is compared is a hash,
     * so a timing difference could only ever say something about the hash - but
     * there is no reason to hand even that out. */
    uint16_t found = 0;
    AUTH_LOCK();
    for (int i = 0; i < CONFIG_ZBC_MAX_AUTH_KEYS; i++) {
        uint8_t diff = 0;
        for (int j = 0; j < 32; j++) {
            diff |= (uint8_t)(s_keys[i].hash[j] ^ h[j]);
        }
        if (s_keys[i].in_use && diff == 0) {
            found = s_keys[i].id;
        }
    }
    AUTH_UNLOCK();
    return found;
}

void auth_touch(uint16_t id)
{
    AUTH_LOCK();
    for (int i = 0; i < CONFIG_ZBC_MAX_AUTH_KEYS; i++) {
        if (s_keys[i].in_use && s_keys[i].id == id) {
            s_last_used[i] = esp_timer_get_time();
            break;
        }
    }
    AUTH_UNLOCK();
}

size_t auth_key_count(void)
{
    size_t n = 0;
    AUTH_LOCK();
    for (int i = 0; i < CONFIG_ZBC_MAX_AUTH_KEYS; i++) {
        n += s_keys[i].in_use ? 1 : 0;
    }
    AUTH_UNLOCK();
    return n;
}

esp_err_t auth_create(zbc_key_kind_t kind, const char *name, char *shown_out, uint16_t *id_out)
{
    AUTH_LOCK();
    int      slot = -1;
    uint16_t next = 1;
    for (int i = 0; i < CONFIG_ZBC_MAX_AUTH_KEYS; i++) {
        if (!s_keys[i].in_use) {
            if (slot < 0) {
                slot = i;
            }
        } else if (s_keys[i].id >= next) {
            next = s_keys[i].id + 1;
        }
    }
    if (slot < 0) {
        AUTH_UNLOCK();
        return ESP_ERR_NO_MEM;
    }

    uint8_t random[ZBC_AUTH_KEY_CHARS];
    char    canon[ZBC_AUTH_KEY_CANON_LEN];
    esp_fill_random(random, sizeof(random));
    auth_encode(random, canon);
    memset(random, 0, sizeof(random));

    auth_record_t *r = &s_keys[slot];
    memset(r, 0, sizeof(*r));
    r->in_use = 1;
    r->kind   = (uint8_t)kind;
    r->id     = next;
    if (name && name[0]) {
        strlcpy(r->name, name, sizeof(r->name));
    } else {
        snprintf(r->name, sizeof(r->name), "%s %u", kind == ZBC_KEY_API ? "API key" : "Browser", next);
    }
    hash_key(canon, r->hash);

    esp_err_t err = save_locked();
    if (err != ESP_OK) {
        /* A key that works until the next boot and then silently stops is worse
         * than no key: it would be written down and trusted. */
        memset(r, 0, sizeof(*r));
        AUTH_UNLOCK();
        memset(canon, 0, sizeof(canon));
        return err;
    }
    s_last_used[slot] = esp_timer_get_time();
    ESP_LOGI(TAG, "Issued %s key %u (\"%s\")", kind == ZBC_KEY_API ? "API" : "browser", r->id, r->name);
    if (id_out) {
        *id_out = r->id;
    }
    AUTH_UNLOCK();

    auth_format(canon, shown_out);
    memset(canon, 0, sizeof(canon));
    return ESP_OK;
}

void auth_canonical(const char *shown, char *canon_out)
{
    if (!auth_normalize(shown, canon_out)) {
        canon_out[0] = '\0';
    }
}

static void copy_out(int i, zbc_auth_key_t *out)
{
    out->id           = s_keys[i].id;
    out->kind         = s_keys[i].kind;
    out->last_used_us = s_last_used[i];
    strlcpy(out->name, s_keys[i].name, sizeof(out->name));
}

bool auth_get(uint16_t id, zbc_auth_key_t *out)
{
    bool found = false;
    AUTH_LOCK();
    for (int i = 0; i < CONFIG_ZBC_MAX_AUTH_KEYS; i++) {
        if (s_keys[i].in_use && s_keys[i].id == id) {
            if (out) {
                copy_out(i, out);
            }
            found = true;
            break;
        }
    }
    AUTH_UNLOCK();
    return found;
}

size_t auth_snapshot(zbc_auth_key_t *buf, size_t max)
{
    size_t n = 0;
    AUTH_LOCK();
    for (int i = 0; i < CONFIG_ZBC_MAX_AUTH_KEYS && n < max; i++) {
        if (s_keys[i].in_use) {
            copy_out(i, &buf[n++]);
        }
    }
    AUTH_UNLOCK();
    return n;
}

esp_err_t auth_revoke(uint16_t id)
{
    AUTH_LOCK();
    for (int i = 0; i < CONFIG_ZBC_MAX_AUTH_KEYS; i++) {
        if (s_keys[i].in_use && s_keys[i].id == id) {
            memset(&s_keys[i], 0, sizeof(s_keys[i]));
            s_last_used[i] = 0;
            esp_err_t err = save_locked();
            ESP_LOGW(TAG, "Revoked key %u%s", id,
                     auth_key_count() ? "" : " - no keys left, the web UI is open to everyone");
            AUTH_UNLOCK();
            return err;
        }
    }
    AUTH_UNLOCK();
    return ESP_ERR_NOT_FOUND;
}

esp_err_t auth_erase(void)
{
    AUTH_LOCK();
    memset(s_keys, 0, sizeof(s_keys));
    memset(s_last_used, 0, sizeof(s_last_used));
    esp_err_t err = save_locked();
    AUTH_UNLOCK();
    auth_failsafe_close();
    return err;
}

void auth_failsafe_open(void)
{
    const int64_t until = esp_timer_get_time() + (int64_t)CONFIG_ZBC_UNLOCK_WINDOW_S * 1000000;
    portENTER_CRITICAL(&s_window_mux);
    s_window_until = until;
    portEXIT_CRITICAL(&s_window_mux);
}

void auth_failsafe_close(void)
{
    portENTER_CRITICAL(&s_window_mux);
    s_window_until = 0;
    portEXIT_CRITICAL(&s_window_mux);
}

int auth_failsafe_left_s(void)
{
    portENTER_CRITICAL(&s_window_mux);
    const int64_t until = s_window_until;
    portEXIT_CRITICAL(&s_window_mux);
    const int64_t left = until - esp_timer_get_time();
    return left > 0 ? (int)((left + 999999) / 1000000) : 0;
}
