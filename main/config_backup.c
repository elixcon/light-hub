#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "al_db.h"
#include "auth.h"
#include "button_db.h"
#include "config_backup.h"
#include "curve_db.h"
#include "device_db.h"
#include "group_db.h"
#include "light_db.h"
#include "mlight_db.h"
#include "settings.h"
#include "zb_coordinator.h"

#if CONFIG_ZBC_CONFIG_BACKUP

static const char *TAG = "backup";

/* Where every table and setting lives, and where a restore waits for the boot
 * that carries it out. The stage has the live keys' own names; the marker is
 * written last, so a stage without one is an upload that never finished. */
#define CB_NS       "zbc"
#define CB_STAGE_NS "zbc_rst"
#define CB_MARK_KEY "_apply"        /* u8: the sections the boot replaces */

#define CB_FORMAT   "elixcon-config"
#define CB_FORMAT_V 1

/* >>> host-test region: cfg-plan <<< */
/* The parts of a backup. Only the two secrets can be left out of a download or
 * kept through a restore; the rest always travels whole. */
#define CB_SECT_SETTINGS (1u << 0)  /* settings.c: scalars, and Adaptive Lighting's shape */
#define CB_SECT_WIFI     (1u << 1)  /* the network's SSID and password */
#define CB_SECT_AUTH     (1u << 2)  /* the access keys, as their SHA-256 hashes */
#define CB_SECT_TABLES   (1u << 3)  /* devices, rooms, curves, buttons, lights, Adaptive */
#define CB_SECT_ALL      0x0Fu
#define CB_SECT_SECRETS  (CB_SECT_WIFI | CB_SECT_AUTH)

typedef enum { CB_U8, CB_I8, CB_U16, CB_I16, CB_U32, CB_I32, CB_STR, CB_BLOB } cb_type_t;

static const char *const CB_TYPE_NAMES[] = { "u8", "i8", "u16", "i16", "u32", "i32", "str", "blob" };

typedef struct {
    const char *key;   /* the NVS key in "zbc", and the file's "key" */
    cb_type_t   type;
    uint8_t     sect;  /* CB_SECT_* */
} cb_entry_t;

/* Everything a backup carries, and so everything a restore can write: a key
 * not listed here is neither downloaded nor restored. Left out on purpose:
 * - zb_chan, zb_upd_id: the Zigbee network's, not the configuration's.
 *   Restored onto a live network, channel_restore() would move the coordinator
 *   alone and leave every lamp behind on the old channel.
 * - al_running, lstate<id>, light<id>: runtime state.
 * - crash, zota_len, zota_crc, upd_wait, wifi_setup1: diagnostics and one-shots.
 * A new setting or table belongs here, or a backup silently leaves it out. */
static const cb_entry_t CB_ENTRIES[] = {
    { "name",        CB_STR,  CB_SECT_SETTINGS },
    { "trans_ds",    CB_U16,  CB_SECT_SETTINGS },
    { "warm_off",    CB_U8,   CB_SECT_SETTINGS },
    { "wifi_sleep",  CB_U8,   CB_SECT_SETTINGS },
    { "loc_lat",     CB_I32,  CB_SECT_SETTINGS },
    { "loc_lon",     CB_I32,  CB_SECT_SETTINGS },
    { "tz",          CB_STR,  CB_SECT_SETTINGS },
    { "tz_name",     CB_STR,  CB_SECT_SETTINGS },
    { "th_gpio",     CB_I8,   CB_SECT_SETTINGS },
    { "th_b",        CB_U16,  CB_SECT_SETTINGS },
    { "th_r0",       CB_U32,  CB_SECT_SETTINGS },
    { "th_rs",       CB_U32,  CB_SECT_SETTINGS },
    { "th_up",       CB_U8,   CB_SECT_SETTINGS },
    { "th_trip",     CB_U8,   CB_SECT_SETTINGS },
    { "wled_on",     CB_U8,   CB_SECT_SETTINGS },
    { "wled_room",   CB_U16,  CB_SECT_SETTINGS },
    { "wled_others", CB_U8,   CB_SECT_SETTINGS },
    { "wifi_ssid",   CB_STR,  CB_SECT_WIFI },
    { "wifi_pass",   CB_STR,  CB_SECT_WIFI },
    { "auth",        CB_BLOB, CB_SECT_AUTH },
    { "devices",     CB_BLOB, CB_SECT_TABLES },
    { "groups",      CB_BLOB, CB_SECT_TABLES },
    { "curves",      CB_BLOB, CB_SECT_TABLES },
    { "buttons",     CB_BLOB, CB_SECT_TABLES },
    { "lights",      CB_BLOB, CB_SECT_TABLES },
    { "mlights",     CB_BLOB, CB_SECT_TABLES },
    { "adaptive",    CB_BLOB, CB_SECT_TABLES },
    { "al_days",     CB_BLOB, CB_SECT_TABLES },
};
#define CB_ENTRY_COUNT (sizeof(CB_ENTRIES) / sizeof(CB_ENTRIES[0]))

static const cb_entry_t *cb_find(const char *key)
{
    for (size_t i = 0; key && i < CB_ENTRY_COUNT; i++) {
        if (strcmp(CB_ENTRIES[i].key, key) == 0) {
            return &CB_ENTRIES[i];
        }
    }
    return NULL;
}

/* The secret sections by name, as the API's omit= and keep= and the file's
 * "omitted" spell them. */
static uint8_t cb_secret_bit(const char *name, size_t len)
{
    if (len == 4 && strncmp(name, "wifi", 4) == 0) {
        return CB_SECT_WIFI;
    }
    if (len == 4 && strncmp(name, "auth", 4) == 0) {
        return CB_SECT_AUTH;
    }
    return 0;
}

static bool cb_parse_secrets(const char *list, uint8_t *out)
{
    uint8_t bits = 0;
    for (const char *p = list; p && *p;) {
        const char  *comma = strchr(p, ',');
        const size_t len   = comma ? (size_t)(comma - p) : strlen(p);
        if (len) {
            const uint8_t bit = cb_secret_bit(p, len);
            if (!bit) {
                return false;
            }
            bits |= bit;
        }
        p += len + (comma ? 1 : 0);
    }
    *out = bits;
    return true;
}

/* Which sections the boot replaces: all of them, less the secrets the request
 * keeps, less the secrets the file says were left out when it was made -
 * whatever the request says, or a file made without the keys would erase this
 * box's keys - and less the Wi-Fi when the file has no network to give, so a
 * restore never leaves the box with none. */
static uint8_t cb_replaced(uint8_t keep, uint8_t omitted, bool has_ssid)
{
    uint8_t replaced = CB_SECT_ALL & (uint8_t)~((keep | omitted) & CB_SECT_SECRETS);
    if (!has_ssid) {
        replaced &= (uint8_t)~CB_SECT_WIFI;
    }
    return replaced;
}

typedef enum { CB_LEAVE, CB_COPY, CB_ERASE } cb_action_t;

/* What the boot does with one key. A section it replaces becomes what the file
 * says: the staged value, or nothing where the file had none - a file with no
 * access keys is a box with none, a file with no curves a box without curves.
 * Every other section is left as it is. */
static cb_action_t cb_action(const cb_entry_t *e, uint8_t replaced, bool staged)
{
    if (!(e->sect & replaced)) {
        return CB_LEAVE;
    }
    return staged ? CB_COPY : CB_ERASE;
}

/* Whether a download carries this key: all of them but the secrets left out. */
static bool cb_exported(const cb_entry_t *e, uint8_t omit)
{
    return !(e->sect & omit & CB_SECT_SECRETS);
}
/* >>> end host-test region: cfg-plan <<< */

bool config_backup_parse_secrets(const char *list, uint8_t *out)
{
    return cb_parse_secrets(list, out);
}

/* The owning table's own test of a blob, the one its boot load applies. */
static bool blob_ok(const char *key, const void *blob, size_t len)
{
    if (strcmp(key, "devices") == 0) {
        return device_db_blob_ok(blob, len);
    }
    if (strcmp(key, "groups") == 0) {
        return group_db_blob_ok(blob, len);
    }
    if (strcmp(key, "curves") == 0) {
        return curve_db_blob_ok(blob, len);
    }
    if (strcmp(key, "buttons") == 0) {
        return button_db_blob_ok(blob, len);
    }
    if (strcmp(key, "lights") == 0) {
        return light_db_blob_ok(blob, len);
    }
    if (strcmp(key, "mlights") == 0) {
        return mlight_db_blob_ok(blob, len);
    }
    if (strcmp(key, "adaptive") == 0) {
        return al_db_blob_ok(blob, len);
    }
    if (strcmp(key, "al_days") == 0) {
        return al_db_days_blob_ok(blob, len);
    }
    if (strcmp(key, "auth") == 0) {
        return auth_blob_ok(blob, len);
    }
    return false;
}

/* What a blob is called in an error the page shows. */
static const char *blob_what(const char *key)
{
    static const struct { const char *key, *what; } NAMES[] = {
        { "devices", "devices" },   { "groups", "rooms" },          { "curves", "curves" },
        { "buttons", "buttons" },   { "lights", "wired lights" },   { "mlights", "Matter lamps" },
        { "adaptive", "Adaptive Lighting settings" }, { "al_days", "rooms' Adaptive Lighting days" },
        { "auth", "access keys" },
    };
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        if (strcmp(NAMES[i].key, key) == 0) {
            return NAMES[i].what;
        }
    }
    return key;
}

/* The largest blob a table of this build can hold, and so the longest line a
 * file from it has: the registry at 32 devices is ~8 KB. Anything longer is
 * from a build with bigger tables, which this one would truncate. */
static size_t blob_max(void)
{
    size_t m = 4096;
#define CB_TABLE(rec, n) do { const size_t s = 8 + (size_t)(n) * sizeof(rec); if (s > m) { m = s; } } while (0)
    CB_TABLE(zbc_device_t, CONFIG_ZBC_MAX_DEVICES);
    CB_TABLE(zbc_group_t, CONFIG_ZBC_MAX_GROUPS);
    CB_TABLE(zbc_curveset_t, CONFIG_ZBC_MAX_CURVES);
    CB_TABLE(zbc_button_t, CONFIG_ZBC_MAX_BUTTONS);
    CB_TABLE(zbc_light_t, CONFIG_ZBC_MAX_LIGHTS);
    CB_TABLE(zbc_mlight_t, CONFIG_ZBC_MAX_MATTER_LIGHTS);
#undef CB_TABLE
    return m;
}

static esp_err_t get_scalar(nvs_handle_t h, const cb_entry_t *e, int64_t *v)
{
    esp_err_t err = ESP_ERR_INVALID_ARG;
    switch (e->type) {
    case CB_U8:  { uint8_t x = 0;  err = nvs_get_u8(h, e->key, &x);  *v = x; break; }
    case CB_I8:  { int8_t x = 0;   err = nvs_get_i8(h, e->key, &x);  *v = x; break; }
    case CB_U16: { uint16_t x = 0; err = nvs_get_u16(h, e->key, &x); *v = x; break; }
    case CB_I16: { int16_t x = 0;  err = nvs_get_i16(h, e->key, &x); *v = x; break; }
    case CB_U32: { uint32_t x = 0; err = nvs_get_u32(h, e->key, &x); *v = x; break; }
    case CB_I32: { int32_t x = 0;  err = nvs_get_i32(h, e->key, &x); *v = x; break; }
    default: break;
    }
    return err;
}

static esp_err_t set_scalar(nvs_handle_t h, const cb_entry_t *e, int64_t v)
{
    switch (e->type) {
    case CB_U8:  return nvs_set_u8(h, e->key, (uint8_t)v);
    case CB_I8:  return nvs_set_i8(h, e->key, (int8_t)v);
    case CB_U16: return nvs_set_u16(h, e->key, (uint16_t)v);
    case CB_I16: return nvs_set_i16(h, e->key, (int16_t)v);
    case CB_U32: return nvs_set_u32(h, e->key, (uint32_t)v);
    case CB_I32: return nvs_set_i32(h, e->key, (int32_t)v);
    default:     return ESP_ERR_INVALID_ARG;
    }
}

/* Whether a number from the file is a whole value its type can hold. */
static bool scalar_fits(cb_type_t type, double d)
{
    static const double LO[] = { 0, -128, 0, -32768, 0, -2147483648.0 };
    static const double HI[] = { 255, 127, 65535, 32767, 4294967295.0, 2147483647.0 };
    /* The range first: casting a double outside int64_t's is undefined. */
    return type <= CB_I32 && d >= LO[type] && d <= HI[type] && d == (double)(int64_t)d;
}

/* A string or a blob, whole, into a buffer of its own. */
static esp_err_t get_bytes(nvs_handle_t h, const cb_entry_t *e, uint8_t **out, size_t *len)
{
    *out = NULL;
    *len = 0;
    esp_err_t err = e->type == CB_STR ? nvs_get_str(h, e->key, NULL, len) : nvs_get_blob(h, e->key, NULL, len);
    if (err != ESP_OK) {
        return err;
    }
    *out = malloc(*len ? *len : 1);
    if (!*out) {
        return ESP_ERR_NO_MEM;
    }
    err = e->type == CB_STR ? nvs_get_str(h, e->key, (char *)*out, len) : nvs_get_blob(h, e->key, *out, len);
    if (err != ESP_OK) {
        free(*out);
        *out = NULL;
    }
    return err;
}

/* ---- Download ---------------------------------------------------------- */

/* One small object as one line: printed, freed, and a newline after it. */
static esp_err_t emit_json(cb_sink_fn sink, void *ctx, cJSON *item)
{
    char *text = item ? cJSON_PrintUnformatted(item) : NULL;
    cJSON_Delete(item);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = sink(ctx, text, strlen(text));
    free(text);
    return err == ESP_OK ? sink(ctx, "\n", 1) : err;
}

/* A blob's line, base64-encoded in slices: three bytes become four characters,
 * so every slice but the last ends on a whole group and the slices join into
 * one string. 384 bytes a slice keeps the buffer on the server task's stack
 * small. */
static esp_err_t emit_blob(cb_sink_fn sink, void *ctx, const char *key, const uint8_t *blob, size_t len)
{
    char      head[64];
    const int n   = snprintf(head, sizeof(head), "{\"key\":\"%s\",\"type\":\"blob\",\"value\":\"", key);
    esp_err_t err = sink(ctx, head, (size_t)n);
    for (size_t at = 0; at < len && err == ESP_OK;) {
        unsigned char out[513];
        const size_t  take = len - at < 384 ? len - at : 384;
        size_t        olen = 0;
        if (mbedtls_base64_encode(out, sizeof(out), &olen, blob + at, take) != 0) {
            return ESP_FAIL;
        }
        err = sink(ctx, (const char *)out, olen);
        at += take;
    }
    return err == ESP_OK ? sink(ctx, "\"}\n", 3) : err;
}

/* What the file holds that needs keeping private, in words, or NULL. */
static const char *warning_for(bool wifi, bool keys)
{
    if (wifi && keys) {
        return "Contains the Wi-Fi password in plain text and the access keys' hashes. Keep it private.";
    }
    if (wifi) {
        return "Contains the Wi-Fi password in plain text. Keep it private.";
    }
    if (keys) {
        return "Contains the access keys' hashes. Keep it private.";
    }
    return NULL;
}

static cJSON *header_json(nvs_handle_t nvs, uint8_t omit)
{
    cJSON *h = cJSON_CreateObject();
    if (!h) {
        return NULL;
    }
    cJSON_AddStringToObject(h, "format", CB_FORMAT);
    cJSON_AddNumberToObject(h, "v", CB_FORMAT_V);
    cJSON_AddStringToObject(h, "firmware", esp_app_get_description()->version);
    cJSON_AddStringToObject(h, "name", settings_name());
    cJSON_AddStringToObject(h, "hostname", settings_hostname());

    /* Which network the device addresses belong to: a restore keeps them only
     * on this one, so a file made while the network is down keeps none. */
    zb_netinfo_t zb;
    zb_get_netinfo(&zb);
    if (zb.network_up) {
        char text[19];
        zbc_ieee_to_str(zb.ieee, text);
        cJSON_AddStringToObject(h, "ieee", text);
        cJSON_AddNumberToObject(h, "pan_id", zb.pan_id);
        snprintf(text, sizeof(text), "%016llx", (unsigned long long)zb.ext_pan_id);
        cJSON_AddStringToObject(h, "ext_pan_id", text);
    } else {
        cJSON_AddNullToObject(h, "ieee");
        cJSON_AddNullToObject(h, "pan_id");
        cJSON_AddNullToObject(h, "ext_pan_id");
    }
    const time_t now = time(NULL);
    if (now >= 1735689600) {        /* SNTP has answered; before 2025 it has not */
        cJSON_AddNumberToObject(h, "created", (double)now);
    } else {
        cJSON_AddNullToObject(h, "created");
    }

    /* Always present, so a restore tells "left out" from "the box had none":
     * keys left out are kept, no keys on the box is protection removed. */
    cJSON *omitted = cJSON_AddArrayToObject(h, "omitted");
    if (!omitted ||
        ((omit & CB_SECT_WIFI) && !cJSON_AddItemToArray(omitted, cJSON_CreateString("wifi"))) ||
        ((omit & CB_SECT_AUTH) && !cJSON_AddItemToArray(omitted, cJSON_CreateString("auth")))) {
        cJSON_Delete(h);           /* a header that does not say what is missing */
        return NULL;
    }
    /* Named for what this file really holds: a box with no keys has none to
     * leave out, and one never set up has no password. */
    const bool wifi = !(omit & CB_SECT_WIFI) && nvs_find_key(nvs, "wifi_pass", NULL) == ESP_OK;
    const bool keys = !(omit & CB_SECT_AUTH) && nvs_find_key(nvs, "auth", NULL) == ESP_OK;
    const char *warning = warning_for(wifi, keys);
    if (warning) {
        cJSON_AddStringToObject(h, "warning", warning);
    }
    return h;
}

static esp_err_t export_entry(nvs_handle_t nvs, const cb_entry_t *e, cb_sink_fn sink, void *ctx)
{
    if (e->type == CB_STR || e->type == CB_BLOB) {
        uint8_t  *bytes;
        size_t    len;
        esp_err_t err = get_bytes(nvs, e, &bytes, &len);
        if (err != ESP_OK) {
            return err;
        }
        if (e->type == CB_BLOB) {
            err = emit_blob(sink, ctx, e->key, bytes, len);
            free(bytes);
            return err;
        }
        cJSON *line = cJSON_CreateObject();
        if (line) {
            cJSON_AddStringToObject(line, "key", e->key);
            cJSON_AddStringToObject(line, "type", CB_TYPE_NAMES[e->type]);
            cJSON_AddStringToObject(line, "value", (const char *)bytes);
        }
        free(bytes);
        return emit_json(sink, ctx, line);
    }
    int64_t   v;
    esp_err_t err = get_scalar(nvs, e, &v);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *line = cJSON_CreateObject();
    if (line) {
        cJSON_AddStringToObject(line, "key", e->key);
        cJSON_AddStringToObject(line, "type", CB_TYPE_NAMES[e->type]);
        cJSON_AddNumberToObject(line, "value", (double)v);
    }
    return emit_json(sink, ctx, line);
}

esp_err_t config_backup_export(uint8_t omit, cb_sink_fn sink, void *ctx)
{
    omit &= CB_SECT_SECRETS;
    nvs_handle_t nvs;
    esp_err_t    err = nvs_open(CB_NS, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = emit_json(sink, ctx, header_json(nvs, omit));
    int entries = 0;
    for (size_t i = 0; i < CB_ENTRY_COUNT && err == ESP_OK; i++) {
        const cb_entry_t *e = &CB_ENTRIES[i];
        if (!cb_exported(e, omit)) {
            continue;
        }
        err = export_entry(nvs, e, sink, ctx);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;          /* a default, or a table with nothing in it */
            continue;
        }
        if (err == ESP_OK) {
            entries++;
        }
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Download stopped: %s", esp_err_to_name(err));
        return err;
    }
    /* Last, so a file cut short anywhere above is one a restore refuses. */
    char end[40];
    const int n = snprintf(end, sizeof(end), "{\"end\":true,\"entries\":%d}\n", entries);
    err = sink(ctx, end, (size_t)n);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Configuration downloaded: %d entries%s%s", entries,
                 omit & CB_SECT_WIFI ? ", without the Wi-Fi" : "",
                 omit & CB_SECT_AUTH ? ", without the access keys" : "");
    }
    return err;
}

/* ---- Restore ----------------------------------------------------------- */

typedef struct {
    cb_read_fn read;
    void      *ctx;
    char       in[256];
    size_t     in_len;
    size_t     in_pos;
    bool       eof;
    char      *line;
    size_t     line_cap;
    size_t     line_len;
    size_t     line_max;
} cb_reader_t;

/* The next line, without its newline (or a CR before it), in r->line:
 * 1 for a line, 0 at the end of the file, -1 when the upload broke off, -2 for
 * a line longer than any this build writes. */
static int next_line(cb_reader_t *r)
{
    r->line_len = 0;
    for (;;) {
        if (r->in_pos == r->in_len) {
            if (r->eof) {
                return r->line_len ? 1 : 0;
            }
            const int n = r->read(r->ctx, r->in, sizeof(r->in));
            if (n < 0) {
                return -1;
            }
            r->in_len = (size_t)n;
            r->in_pos = 0;
            r->eof    = n == 0;
            continue;
        }
        const char c = r->in[r->in_pos++];
        if (c == '\n') {
            if (r->line_len && r->line[r->line_len - 1] == '\r') {
                r->line[--r->line_len] = '\0';
            }
            return 1;
        }
        if (r->line_len + 1 >= r->line_cap) {
            if (r->line_len >= r->line_max) {
                return -2;
            }
            size_t cap = r->line_cap ? r->line_cap * 2 : 512;
            if (cap > r->line_max + 1) {
                cap = r->line_max + 1;
            }
            char *grown = realloc(r->line, cap);
            if (!grown) {
                return -2;
            }
            r->line     = grown;
            r->line_cap = cap;
        }
        r->line[r->line_len++] = c;
        r->line[r->line_len]   = '\0';
    }
}

/* A line's text is not needed once it is parsed. Giving a long one back
 * before its blob is decoded keeps the peak at the tree and the bytes, not the
 * line as well - ~19 KB rather than ~30 at a full registry. */
static void drop_long_line(cb_reader_t *r)
{
    if (r->line_cap > 1024) {
        free(r->line);
        r->line     = NULL;
        r->line_cap = 0;
        r->line_len = 0;
    }
}

#define FAIL(code, ...) do { err = (code); snprintf(out->error, sizeof(out->error), __VA_ARGS__); goto done; } while (0)

/* The secret sections a file's header says were left out, or false for a
 * header this build does not read. */
static bool header_omitted(const cJSON *hdr, uint8_t *omitted)
{
    *omitted = 0;
    const cJSON *list = cJSON_GetObjectItem(hdr, "omitted");
    if (!list) {
        return true;
    }
    if (!cJSON_IsArray(list)) {
        return false;
    }
    const cJSON *it;
    cJSON_ArrayForEach(it, list) {
        const uint8_t bit = cJSON_IsString(it) ? cb_secret_bit(it->valuestring, strlen(it->valuestring)) : 0;
        if (!bit) {
            return false;
        }
        *omitted |= bit;
    }
    return true;
}

/* Whether the file was made on the network that is up now: the same PAN and
 * extended PAN. Not the same, or either side unknown, and every device address
 * in it is forgotten. Both, because the extended PAN is the coordinator's own
 * IEEE address: a network formed after a reset has the same one, and only the
 * PAN tells them apart (dev board, 2026-10-10: 0x5fb3 -> 0x6120). */
static bool same_network(const cJSON *hdr)
{
    zb_netinfo_t zb;
    zb_get_netinfo(&zb);
    const cJSON *pan = cJSON_GetObjectItem(hdr, "pan_id");
    const cJSON *ext = cJSON_GetObjectItem(hdr, "ext_pan_id");
    if (!zb.network_up || !cJSON_IsNumber(pan) || !cJSON_IsString(ext)) {
        return false;
    }
    char text[19];
    snprintf(text, sizeof(text), "%016llx", (unsigned long long)zb.ext_pan_id);
    return pan->valuedouble == zb.pan_id && strcasecmp(ext->valuestring, text) == 0;
}

static esp_err_t stage_value(nvs_handle_t stage, const cb_entry_t *e, const cJSON *value, bool keep_addresses,
                             cb_result_t *out)
{
    esp_err_t err = ESP_OK;
    if (e->type <= CB_I32) {
        if (!cJSON_IsNumber(value) || !scalar_fits(e->type, value->valuedouble)) {
            FAIL(ESP_ERR_INVALID_ARG, "\"%s\" is not a %s", e->key, CB_TYPE_NAMES[e->type]);
        }
        err = set_scalar(stage, e, (int64_t)value->valuedouble);
        goto done;
    }
    if (!cJSON_IsString(value)) {
        FAIL(ESP_ERR_INVALID_ARG, "\"%s\" is not a string", e->key);
    }
    if (e->type == CB_STR) {
        /* Every string here is a name, an SSID or a password: none near this. */
        if (strlen(value->valuestring) > 64) {
            FAIL(ESP_ERR_INVALID_ARG, "\"%s\" is longer than this firmware stores", e->key);
        }
        err = nvs_set_str(stage, e->key, value->valuestring);
        goto done;
    }

    const unsigned char *b64  = (const unsigned char *)value->valuestring;
    const size_t         slen = strlen(value->valuestring);
    size_t               len  = 0;
    mbedtls_base64_decode(NULL, 0, &len, b64, slen);   /* how long it decodes to */
    uint8_t *blob = malloc(len ? len : 1);
    if (!blob) {
        FAIL(ESP_ERR_NO_MEM, "out of memory");
    }
    if (mbedtls_base64_decode(blob, len, &len, b64, slen) != 0) {
        free(blob);
        FAIL(ESP_ERR_INVALID_ARG, "the %s are damaged (not base64)", blob_what(e->key));
    }
    /* A table its own load would throw away is not restored at all: the box
     * would lose what it has and gain nothing. */
    if (!blob_ok(e->key, blob, len)) {
        free(blob);
        FAIL(ESP_ERR_INVALID_ARG, "the %s in this file are in a format this firmware does not read"
                                  " (made by a newer one?)", blob_what(e->key));
    }
    if (strcmp(e->key, "devices") == 0 && !keep_addresses) {
        device_db_blob_forget_addresses(blob, len);
    }
    err = nvs_set_blob(stage, e->key, blob, len);
    free(blob);
done:
    return err;
}

esp_err_t config_backup_stage(cb_read_fn read, void *ctx, size_t size_hint, uint8_t keep, cb_result_t *out)
{
    memset(out, 0, sizeof(*out));
    keep &= CB_SECT_SECRETS;

    esp_err_t    err    = ESP_OK;
    cJSON       *line   = NULL;
    nvs_handle_t stage  = 0;
    bool         opened = false;
    cb_reader_t *r      = calloc(1, sizeof(*r));
    if (!r) {
        snprintf(out->error, sizeof(out->error), "out of memory");
        return ESP_ERR_NO_MEM;
    }
    r->read     = read;
    r->ctx      = ctx;
    r->line_max = (blob_max() + 2) / 3 * 4 + 128;

    /* A first look at the room, before a byte is written: what a file decodes
     * to, in 32-byte entries. Only a first look - a write that finds no room
     * after all fails the same way, with the stage cleared. */
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) == ESP_OK && st.available_entries < size_hint * 3 / 4 / 32 + 8) {
        FAIL(ESP_ERR_NVS_NOT_ENOUGH_SPACE, "not enough room in flash to stage this file"
                                           " (%u entries free)", (unsigned)st.available_entries);
    }
    err = nvs_open(CB_STAGE_NS, NVS_READWRITE, &stage);
    if (err != ESP_OK) {
        FAIL(err, "could not open the stage: %s", esp_err_to_name(err));
    }
    opened = true;
    nvs_erase_all(stage);          /* whatever an earlier attempt left */

    int rc = next_line(r);
    if (rc != 1 || !(line = cJSON_ParseWithLength(r->line, r->line_len))) {
        FAIL(rc == -1 ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_ARG, "%s",
             rc == -1 ? "the upload broke off" : "this is not a configuration file");
    }
    const cJSON *format = cJSON_GetObjectItem(line, "format");
    const cJSON *v      = cJSON_GetObjectItem(line, "v");
    uint8_t      omitted;
    if (!cJSON_IsString(format) || strcmp(format->valuestring, CB_FORMAT) != 0) {
        FAIL(ESP_ERR_INVALID_ARG, "this is not a configuration file");
    }
    if (!cJSON_IsNumber(v) || v->valueint != CB_FORMAT_V || !header_omitted(line, &omitted)) {
        FAIL(ESP_ERR_INVALID_ARG, "this configuration file is from a newer firmware");
    }
    out->addresses_kept = same_network(line);
    cJSON_Delete(line);
    line = NULL;

    const uint8_t excluded = keep | omitted;
    bool          has_ssid = false;
    bool          ended    = false;
    int           in_file  = 0;
    while ((rc = next_line(r)) == 1) {
        if (r->line_len == 0) {
            continue;
        }
        if (ended) {
            FAIL(ESP_ERR_INVALID_ARG, "the file goes on after its last line");
        }
        line = cJSON_ParseWithLength(r->line, r->line_len);
        drop_long_line(r);
        if (!line) {
            FAIL(ESP_ERR_INVALID_ARG, "line %d is not JSON", in_file + 2);
        }
        if (cJSON_IsTrue(cJSON_GetObjectItem(line, "end"))) {
            const cJSON *n = cJSON_GetObjectItem(line, "entries");
            if (!cJSON_IsNumber(n) || n->valueint != in_file) {
                FAIL(ESP_ERR_INVALID_ARG, "the file is incomplete (%d of %d entries)", in_file,
                     cJSON_IsNumber(n) ? n->valueint : -1);
            }
            ended = true;
            cJSON_Delete(line);
            line = NULL;
            continue;
        }
        in_file++;
        const cJSON      *key   = cJSON_GetObjectItem(line, "key");
        const cJSON      *type  = cJSON_GetObjectItem(line, "type");
        const cb_entry_t *e     = cJSON_IsString(key) ? cb_find(key->valuestring) : NULL;
        if (!e) {
            /* A setting of a newer firmware: what this one knows still restores. */
            ESP_LOGW(TAG, "Restore: skipping \"%s\", which this firmware does not know",
                     cJSON_IsString(key) ? key->valuestring : "?");
            out->skipped++;
        } else if (!cJSON_IsString(type) || strcmp(type->valuestring, CB_TYPE_NAMES[e->type]) != 0) {
            FAIL(ESP_ERR_INVALID_ARG, "\"%s\" has the wrong type", e->key);
        } else if (e->sect & omitted) {
            FAIL(ESP_ERR_INVALID_ARG, "the file has \"%s\" although its header says it was left out", e->key);
        } else {
            const cJSON *value = cJSON_GetObjectItem(line, "value");
            if (strcmp(e->key, "wifi_ssid") == 0) {
                has_ssid = cJSON_IsString(value) && value->valuestring[0];
            }
            if (!(e->sect & excluded)) {
                err = stage_value(stage, e, value, out->addresses_kept, out);
                if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
                    FAIL(err, "not enough room in flash to stage this file");
                }
                if (err != ESP_OK) {
                    if (!out->error[0]) {
                        snprintf(out->error, sizeof(out->error), "could not stage \"%s\": %s", e->key,
                                 esp_err_to_name(err));
                    }
                    goto done;
                }
                out->entries++;
            }
        }
        cJSON_Delete(line);
        line = NULL;
    }
    if (rc == -1) {
        FAIL(ESP_ERR_TIMEOUT, "the upload broke off");
    }
    if (rc == -2) {
        FAIL(ESP_ERR_INVALID_ARG, "line %d is longer than any this firmware writes", in_file + 2);
    }
    if (!ended) {
        FAIL(ESP_ERR_INVALID_ARG, "the file is cut short (it has no last line)");
    }

    const uint8_t replaced = cb_replaced(keep, omitted, has_ssid);
    out->wifi              = replaced & CB_SECT_WIFI;
    out->auth              = replaced & CB_SECT_AUTH;
    err                    = nvs_set_u8(stage, CB_MARK_KEY, replaced);
    if (err == ESP_OK) {
        err = nvs_commit(stage);
    }
    if (err != ESP_OK) {
        FAIL(err, "could not stage the file: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "Restore staged: %d entries, %d skipped, sections 0x%x, addresses %s", out->entries,
             out->skipped, replaced, out->addresses_kept ? "kept" : "forgotten (another network)");

done:
    cJSON_Delete(line);
    if (opened) {
        if (err != ESP_OK) {
            /* Nothing of a refused file may reach the next boot. */
            nvs_erase_all(stage);
            nvs_commit(stage);
        }
        nvs_close(stage);
    }
    free(r->line);
    free(r);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Restore refused: %s", out->error);
    }
    return err;
}

#undef FAIL

/* ---- At boot ----------------------------------------------------------- */

static esp_err_t copy_entry(nvs_handle_t from, nvs_handle_t to, const cb_entry_t *e)
{
    if (e->type == CB_STR || e->type == CB_BLOB) {
        uint8_t  *bytes;
        size_t    len;
        esp_err_t err = get_bytes(from, e, &bytes, &len);
        if (err == ESP_OK) {
            err = e->type == CB_STR ? nvs_set_str(to, e->key, (const char *)bytes)
                                    : nvs_set_blob(to, e->key, bytes, len);
            free(bytes);
        }
        return err;
    }
    int64_t   v;
    esp_err_t err = get_scalar(from, e, &v);
    return err == ESP_OK ? set_scalar(to, e, v) : err;
}

/* "light12" or "lstate12": a wired light's saved state, by id. Not "lights",
 * the table itself. */
static bool is_light_state_key(const char *key)
{
    const char *digits = strncmp(key, "lstate", 6) == 0 ? key + 6
                       : strncmp(key, "light", 5) == 0  ? key + 5
                                                        : NULL;
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

/* The ids in a restored table are the file's: a saved state, or a running
 * Adaptive Lighting set, would now describe some other light or room. */
static void forget_runtime_keys(nvs_handle_t live)
{
    nvs_erase_key(live, "al_running");
    char        keys[2 * CONFIG_ZBC_MAX_LIGHTS + 8][NVS_KEY_NAME_MAX_SIZE];
    size_t      n  = 0;
    nvs_iterator_t it = NULL;
    for (esp_err_t res = nvs_entry_find(NVS_DEFAULT_PART_NAME, CB_NS, NVS_TYPE_ANY, &it);
         res == ESP_OK && n < sizeof(keys) / sizeof(keys[0]); res = nvs_entry_next(&it)) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (is_light_state_key(info.key)) {
            strlcpy(keys[n++], info.key, sizeof(keys[0]));
        }
    }
    nvs_release_iterator(it);
    for (size_t i = 0; i < n; i++) {
        nvs_erase_key(live, keys[i]);
    }
}

void config_backup_apply(void)
{
    nvs_handle_t stage;
    /* Read-only first: opening for writing would create the namespace, and an
     * entry for it, at every boot. */
    if (nvs_open(CB_STAGE_NS, NVS_READONLY, &stage) != ESP_OK) {
        return;
    }
    nvs_close(stage);
    if (nvs_open(CB_STAGE_NS, NVS_READWRITE, &stage) != ESP_OK) {
        return;
    }
    uint8_t replaced = 0;
    if (nvs_get_u8(stage, CB_MARK_KEY, &replaced) != ESP_OK) {
        /* An upload that broke off part way, before the cleanup could run. */
        ESP_LOGW(TAG, "Dropping an unfinished restore");
        nvs_erase_all(stage);
        nvs_commit(stage);
        nvs_close(stage);
        return;
    }

    nvs_handle_t live;
    if (nvs_open(CB_NS, NVS_READWRITE, &live) != ESP_OK) {
        nvs_close(stage);
        return;
    }
    /* Every staged key stays until the end, so a power cut anywhere in here
     * repeats the whole thing at the next boot and comes to the same result. */
    int copied = 0;
    int erased = 0;
    int failed = 0;
    for (size_t i = 0; i < CB_ENTRY_COUNT; i++) {
        const cb_entry_t *e      = &CB_ENTRIES[i];
        const bool        staged = nvs_find_key(stage, e->key, NULL) == ESP_OK;
        esp_err_t         err    = ESP_OK;
        switch (cb_action(e, replaced, staged)) {
        case CB_COPY:
            err = copy_entry(stage, live, e);
            copied += err == ESP_OK;
            break;
        case CB_ERASE:
            err = nvs_erase_key(live, e->key);
            if (err == ESP_ERR_NVS_NOT_FOUND) {
                err = ESP_OK;
            } else {
                erased += err == ESP_OK;
            }
            break;
        case CB_LEAVE:
            break;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Restore: \"%s\" failed: %s", e->key, esp_err_to_name(err));
            failed++;
        }
    }
    if (replaced & CB_SECT_TABLES) {
        forget_runtime_keys(live);
    }
    nvs_commit(live);
    nvs_close(live);

    /* Last: from here the restore is done, and a failure above is not retried
     * at every boot for ever. */
    nvs_erase_all(stage);
    nvs_commit(stage);
    nvs_close(stage);
    ESP_LOGW(TAG, "Configuration restored from a backup: %d written, %d removed, %d failed%s%s", copied, erased,
             failed, replaced & CB_SECT_WIFI ? "" : "; Wi-Fi kept", replaced & CB_SECT_AUTH ? "" : "; access keys kept");
}

#endif /* CONFIG_ZBC_CONFIG_BACKUP */
