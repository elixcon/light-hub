#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_check.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

#include "al_db.h"
#include "auth.h"
#include "auth_http.h"
#include "button_db.h"
#include "buttons.h"
#include "config_backup.h"
#include "crash_log.h"
#include "curve_db.h"
#include "device_db.h"
#include "group_db.h"
#include "http_server.h"
#include "light_db.h"
#include "lighting.h"
#include "local_lights.h"
#include "matter_bridge.h"
#include "matter_ctl.h"
#include "mlight_db.h"
#include "ota.h"
#include "settings.h"
#include "state_rev.h"
#include "ntc.h"
#include "sun.h"
#include "tz.h"
#include "thermal.h"
#include "wifi_manager.h"
#include "zb_coordinator.h"
#include "radio_ed.h"
#include "radio_coex.h"
#include "zb_ota.h"
#include "heap_watch.h"
#include "wled.h"

static const char *TAG = "http";

/* Devices we have not heard from in a while are shown as stale in the UI. */
#define OFFLINE_AFTER_US (10 * 60 * 1000000LL)
/* A whole button - its wiring and every gesture's rules - is the largest body
 * anything posts here: about 1.8 KB with a full pool of rules that each name a
 * light. */
#define MAX_BODY_LEN     4096

extern const uint8_t index_html_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_gz_end");

static httpd_handle_t s_server;

/* ------------------------------------------------------------- utilities */

static esp_err_t send_json(httpd_req_t *req, cJSON *root, const char *status)
{
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return httpd_resp_send_500(req);
    }
    heap_watch_note_body(strlen(body));
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

/* A JSON response written as it is built, in chunks.
 *
 * send_json holds the whole cJSON tree and then its printed text at once, and
 * cJSON costs five to six bytes of heap per byte of JSON: a full /api/state
 * would take about 58 KB of a heap with about 88 KB free. Here each device,
 * group or button is built, printed into one small buffer and freed before the
 * next, and the buffer goes out as a chunk whenever it fills.
 *
 * Once the first chunk is out the status is sent, so a failure after that can
 * only end the response early; json_stream_end then answers ESP_FAIL and the
 * server drops the connection, which the page treats as a failed poll. */
#define JSON_STREAM_BUF 2048

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message);

typedef struct {
    httpd_req_t *req;
    char        *buf;
    size_t       len;
    size_t       total;
    bool         failed;
} json_stream_t;

static bool json_stream_begin(json_stream_t *js, httpd_req_t *req)
{
    *js = (json_stream_t){ .req = req, .buf = malloc(JSON_STREAM_BUF) };
    if (!js->buf) {
        return false;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return true;
}

static void json_stream_flush(json_stream_t *js)
{
    if (js->len && !js->failed && httpd_resp_send_chunk(js->req, js->buf, js->len) != ESP_OK) {
        js->failed = true;
    }
    js->total += js->len;
    js->len = 0;
}

static void json_stream_raw(json_stream_t *js, const char *text)
{
    for (size_t n = strlen(text); n;) {
        if (js->len == JSON_STREAM_BUF) {
            json_stream_flush(js);
        }
        const size_t take = MIN(n, JSON_STREAM_BUF - js->len);
        memcpy(js->buf + js->len, text, take);
        js->len += take;
        text += take;
        n -= take;
    }
}

/* Writes `sep` and the item, and frees the item. Printed straight into the
 * buffer when it fits there, or into an empty one; only an item larger than
 * the whole buffer is printed on its own, which nothing streamed here now is.
 * False when it could not be printed at all - nothing was written. */
static bool json_stream_item(json_stream_t *js, const char *sep, cJSON *item)
{
    if (!item) {
        return false;
    }
    const size_t seplen = strlen(sep);
    bool         done   = false;
    for (int attempt = 0; attempt < 2 && !done; attempt++) {
        if (attempt == 1) {
            json_stream_flush(js);
        }
        /* cJSON_PrintPreallocated wants a few bytes more than it writes. */
        const size_t room = JSON_STREAM_BUF - js->len;
        if (room > seplen + 6 &&
            cJSON_PrintPreallocated(item, js->buf + js->len + seplen, (int)(room - seplen), false)) {
            memcpy(js->buf + js->len, sep, seplen);
            js->len += seplen + strlen(js->buf + js->len + seplen);
            done = true;
        }
    }
    if (!done) {
        char *text = cJSON_PrintUnformatted(item);
        if (text) {
            json_stream_raw(js, sep);
            json_stream_raw(js, text);
            free(text);
            done = true;
        }
    }
    cJSON_Delete(item);
    return done;
}

/* An object member's value: null rather than nothing, so the JSON stays whole. */
static void json_stream_value(json_stream_t *js, cJSON *item)
{
    if (!json_stream_item(js, "", item)) {
        json_stream_raw(js, "null");
    }
}

/* An array element, with the comma when it is not the first. An element that
 * could not be built is left out, never written as null - the page reads a
 * device's fields without checking for one. */
static void json_stream_element(json_stream_t *js, bool *first, cJSON *item)
{
    if (json_stream_item(js, *first ? "" : ",", item)) {
        *first = false;
    }
}

/* An object member's name, with the comma when it is not the first. The braces
 * are the caller's and the value follows, so a member can be left out - or be
 * an object streamed in turn - without the separators going wrong. */
static void json_stream_key(json_stream_t *js, bool *first, const char *name)
{
    json_stream_raw(js, *first ? "\"" : ",\"");
    json_stream_raw(js, name);
    json_stream_raw(js, "\":");
    *first = false;
}

/* A member whose value is one small tree, built and freed here. */
static void json_stream_member(json_stream_t *js, bool *first, const char *name, cJSON *value)
{
    json_stream_key(js, first, name);
    json_stream_value(js, value);
}

/* A table could not be copied. Before anything went out that is an ordinary
 * error answer; after, the response is cut off, so the page keeps what it
 * showed and polls again - an empty list would read as every device gone. */
static esp_err_t json_stream_fail(json_stream_t *js)
{
    if (js->total == 0 && !js->failed) {
        free(js->buf);
        js->buf = NULL;
        return send_error(js->req, "500 Internal Server Error", "out of memory");
    }
    js->failed = true;
    free(js->buf);
    js->buf = NULL;
    return ESP_FAIL;
}

static esp_err_t json_stream_end(json_stream_t *js)
{
    json_stream_flush(js);
    if (!js->failed && httpd_resp_send_chunk(js->req, NULL, 0) != ESP_OK) {
        js->failed = true;
    }
    free(js->buf);
    js->buf = NULL;
    heap_watch_note_body(js->total);
    return js->failed ? ESP_FAIL : ESP_OK;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", message);
    return send_json(req, root, status);
}

static esp_err_t send_ok(httpd_req_t *req, const char *status)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root, status);
}

static void add_kelvin(cJSON *obj, const char *name, int32_t mireds)
{
    if (mireds <= 0) {
        cJSON_AddNullToObject(obj, name);
    } else {
        cJSON_AddNumberToObject(obj, name, 1000000 / mireds);
    }
}

/* Reads the request body and parses it as JSON. Returns NULL on any problem,
 * having already answered the request. */
static cJSON *read_json_body(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > MAX_BODY_LEN) {
        send_error(req, "400 Bad Request", "body missing or too large");
        return NULL;
    }
    char *buf = malloc(req->content_len + 1);
    if (!buf) {
        send_error(req, "500 Internal Server Error", "out of memory");
        return NULL;
    }
    /* Looped: one recv is not one body. esp_http_server hands over whatever has
     * arrived, and a body split across TCP segments would otherwise be parsed
     * half-read and rejected as malformed. ota.c's multipart parser handles the
     * same. */
    int received = 0;
    int timeouts = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, buf + received, req->content_len - received);
        /* Bounded, as ota.c bounds it: esp_http_server serves one request at a
         * time, and a client that announces a body and never sends it would
         * otherwise hold the only server task for ever - from the unguarded
         * sign-in route, without a key. */
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) {
            continue;
        }
        if (n <= 0) {
            free(buf);
            send_error(req, "400 Bad Request", "could not read body");
            return NULL;
        }
        received += n;
    }
    buf[received] = '\0';

    cJSON *json = cJSON_Parse(buf);
    free(buf);
    if (!json) {
        send_error(req, "400 Bad Request", "body is not valid JSON");
        return NULL;
    }
    return json;
}

/* Copies path segment `index` of the URI (0 = first) into out. */
static bool uri_segment(const char *uri, int index, char *out, size_t out_size)
{
    const char *p = uri;
    for (int i = 0; i <= index; i++) {
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            return false;
        }
        if (i == index) {
            size_t n = 0;
            while (p[n] && p[n] != '/' && p[n] != '?') {
                n++;
            }
            if (n >= out_size) {
                return false;
            }
            memcpy(out, p, n);
            out[n] = '\0';
            return true;
        }
        while (*p && *p != '/') {
            p++;
        }
    }
    return false;
}

/* --------------------------------------------------------------- access */

/* Finding the key a request carries is auth_http.c's, shared with the updater,
 * which has to accept the same cookie. Who needs one is decided here. */

typedef enum {
    ACCESS_LOCKED = 0,
    ACCESS_SETUP,      /* the provisioning AP: whoever is on it is standing at the board */
    ACCESS_OPEN,       /* no keys yet */
    ACCESS_FAILSAFE,   /* a button was pressed five times */
} access_mode_t;

static const char *const ACCESS_NAMES[] = { "locked", "setup", "open", "failsafe" };

/* Whether a request with no key gets in, and why. The setup portal is open on
 * purpose: the person on its network is within Wi-Fi range of the board, the
 * captive-portal browsers phones open throw cookies away, and a lock on the one
 * page that gets the box onto a network would be a lock on the only way back. */
static access_mode_t access_mode(void)
{
    if (wifi_manager_mode() == WIFI_MGR_MODE_AP) {
        return ACCESS_SETUP;
    }
    if (auth_key_count() == 0) {
        return ACCESS_OPEN;
    }
    if (auth_failsafe_left_s() > 0) {
        return ACCESS_FAILSAFE;
    }
    return ACCESS_LOCKED;
}

typedef esp_err_t (*route_fn_t)(httpd_req_t *req);

/* Every route but three is registered through this, with the real handler in
 * user_ctx - so a route added later cannot forget to check, which a line at the
 * top of each handler could. */
static esp_err_t guarded(httpd_req_t *req)
{
    const uint16_t id = auth_http_presented_key(req, NULL, NULL);
    if (id) {
        auth_touch(id);
    } else if (access_mode() == ACCESS_LOCKED) {
        /* The upload reads its reason from this header, and its body has to stay
         * ESPHome's - see ota.c. */
        if (strncmp(req->uri, "/update", 7) == 0) {
            httpd_resp_set_hdr(req, "X-Ota-Error", "This browser is not signed in to the coordinator");
        }
        return send_error(req, "401 Unauthorized", "locked");
    }
    heap_watch_request_begin();
    const esp_err_t err = ((route_fn_t)req->user_ctx)(req);
    heap_watch_request_end(req);
    return err;
}

/* The three routes a locked caller reaches, measured like the guarded ones. */
static esp_err_t watched(httpd_req_t *req)
{
    heap_watch_request_begin();
    const esp_err_t err = ((route_fn_t)req->user_ctx)(req);
    heap_watch_request_end(req);
    return err;
}

static cJSON *auth_json(httpd_req_t *req)
{
    const access_mode_t mode = access_mode();
    const uint16_t      id   = auth_http_presented_key(req, NULL, NULL);
    zbc_auth_key_t      k;

    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "mode", ACCESS_NAMES[mode]);
    cJSON_AddBoolToObject(a, "authorized", id != 0 || mode != ACCESS_LOCKED);
    cJSON_AddNumberToObject(a, "keys", auth_key_count());
    cJSON_AddNumberToObject(a, "failsafe_left_s", auth_failsafe_left_s());
    if (id && auth_get(id, &k)) {
        cJSON *cur = cJSON_AddObjectToObject(a, "current");
        cJSON_AddNumberToObject(cur, "id", k.id);
        cJSON_AddStringToObject(cur, "name", k.name);
    } else {
        cJSON_AddNullToObject(a, "current");
    }
    return a;
}

/* ------------------------------------------------------------- handlers */

static esp_err_t index_get(httpd_req_t *req)
{
    /* The page changes with every firmware image and never between two, so the
     * image's own hash is its ETag. A browser that revalidates (no-cache below)
     * then gets a 304 and its cached copy instead of the whole page on every
     * reload, which a phone makes every time the page is brought to the front. */
    char build[17];
    char etag[24];
    esp_app_get_elf_sha256(build, sizeof(build));
    snprintf(etag, sizeof(etag), "\"%s\"", build);
    httpd_resp_set_hdr(req, "ETag", etag);
    /* The page changes with every firmware image, and a browser left to guess
     * keeps showing the old one after an update. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    char have[32];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", have, sizeof(have)) == ESP_OK &&
        strcmp(have, etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, NULL, 0);
    }

    /* Embedded gzipped (see main/CMakeLists.txt), so the whole length is sent:
     * dropping a trailing byte, as a text embed would, truncates the stream. */
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

/* The three views the UI polls, split out from their handlers so `/api/state`
 * can answer with all of them at once. esp_http_server serves one request at a
 * time, so three separate GETs every two seconds are three serialised
 * round-trips on the one antenna the Zigbee mesh is also trying to use. */
/* The status, written as it is built - see json_stream_t.
 *
 * It carries no table and was still the largest single tree in /api/state: the
 * radio counters, the Zigbee OTA state and heap_watch's three tables printed to
 * a few kilobytes, and cJSON holds five to six bytes of heap per byte of JSON.
 * So each object below is built, printed into the stream's buffer and freed
 * before the next one is built, and the deepest the status goes is its largest
 * single piece rather than their sum. The pieces are small enough that none is
 * printed on its own any more.
 *
 * Every member goes through json_stream_member / json_stream_key, so a piece
 * that could not be built is a null member rather than a broken object, and a
 * member left out takes its separator with it. */

static cJSON *wifi_json(void)
{
    wifi_mgr_status_t wifi;
    wifi_manager_get_status(&wifi);

    cJSON *w = cJSON_CreateObject();
    cJSON_AddStringToObject(w, "mode", wifi.mode == WIFI_MGR_MODE_AP ? "ap" : "sta");
    cJSON_AddBoolToObject(w, "connected", wifi.connected);
    cJSON_AddStringToObject(w, "ssid", wifi.ssid);
    cJSON_AddStringToObject(w, "ip", wifi.ip);
    cJSON_AddStringToObject(w, "hostname", wifi.hostname);
    cJSON_AddNumberToObject(w, "rssi", wifi.rssi);
    cJSON_AddNumberToObject(w, "channel", wifi.channel);
    cJSON_AddBoolToObject(w, "weak_uplink", wifi.weak);
    /* A hotspot started by ten button presses, which goes back to the stored
     * network by itself - the setup page shows where to and when. */
    cJSON_AddBoolToObject(w, "temporary_hotspot", wifi.temporary);
    if (wifi.temporary) {
        cJSON_AddStringToObject(w, "home_ssid", wifi.home_ssid);
        cJSON_AddNumberToObject(w, "returns_in_s", wifi.returns_in_s);
    }
    return w;
}

/* Only while there is no network: what the stack has said since boot, so a
 * formation that goes nowhere can be read here rather than on a serial port
 * that restarts it. `at_s` is uptime. */
static cJSON *zb_forming_json(void)
{
    zb_forming_t fm;
    zb_get_forming(&fm);

    cJSON *f = cJSON_CreateObject();
    cJSON_AddNumberToObject(f, "starts", fm.starts);
    cJSON_AddNumberToObject(f, "signals", fm.signals);
    cJSON_AddNumberToObject(f, "bdb_status", fm.bdb_status);
    cJSON_AddNumberToObject(f, "bdb_mode", fm.bdb_mode);
    cJSON_AddNumberToObject(f, "sampled_at_s", (double)(fm.sampled_us / 100000) / 10);
    cJSON *notes = cJSON_AddArrayToObject(f, "notes");
    for (int i = 0; i < fm.count; i++) {
        cJSON *n = cJSON_CreateObject();
        cJSON_AddNumberToObject(n, "at_s", (double)(fm.notes[i].at_us / 100000) / 10);
        cJSON_AddStringToObject(n, "what", fm.notes[i].what ? fm.notes[i].what : "?");
        cJSON_AddNumberToObject(n, "code", fm.notes[i].code);
        if (fm.notes[i].mode) {
            cJSON_AddNumberToObject(n, "mode", fm.notes[i].mode);
        }
        cJSON_AddItemToArray(notes, n);
    }
    return f;
}

/* Running totals since boot, not rates: sample twice and divide by the uptime
 * between, to judge the link or a radio setting without a serial capture
 * (which resets the board). */
static cJSON *zb_radio_json(void)
{
    zb_radio_stats_t rs;
    zb_get_radio_stats(&rs);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "cmd_undelivered", rs.cmd_undelivered);
    cJSON_AddNumberToObject(r, "read_undelivered", rs.read_undelivered);
    cJSON_AddNumberToObject(r, "cmd_slow", rs.cmd_slow);
    cJSON_AddNumberToObject(r, "cmd_slow_ms", rs.cmd_slow_ms);
    cJSON_AddNumberToObject(r, "went_quiet", rs.went_quiet);
    cJSON_AddNumberToObject(r, "corrections", rs.corrections);
    cJSON_AddNumberToObject(r, "gate_timeouts", rs.gate_timeouts);
    cJSON_AddNumberToObject(r, "slots_short", rs.slots_short);
    cJSON_AddNumberToObject(r, "frames_heard", rs.frames_heard);
    cJSON_AddNumberToObject(r, "frames_missed", rs.frames_missed);
    cJSON_AddNumberToObject(r, "table_entries", rs.table_entries);
    cJSON_AddNumberToObject(r, "table_children", rs.table_children);
    cJSON_AddNumberToObject(r, "sends_skipped", rs.sends_skipped);
    cJSON_AddNumberToObject(r, "router_tables_asked", rs.router_tables_asked);
    cJSON_AddNumberToObject(r, "router_tables_answered", rs.router_tables_answered);
    cJSON_AddNumberToObject(r, "router_tables_unanswered", rs.router_tables_unanswered);
    /* Energy scans, and those the driver never finished and radio_ed.c started
     * again - see radio_ed.h. Formation needs one. */
    radio_ed_stats_t ed;
    radio_ed_get_stats(&ed);
    cJSON_AddNumberToObject(r, "energy_scans", ed.requested);
    cJSON_AddNumberToObject(r, "energy_scans_done", ed.completed);
    cJSON_AddNumberToObject(r, "energy_scans_restarted", ed.restarted);
    cJSON_AddNumberToObject(r, "energy_scans_refused", ed.refused);
    cJSON_AddNumberToObject(r, "energy_scans_refused_coex", ed.refused_coex);
    cJSON_AddNumberToObject(r, "energy_scans_given_up", ed.given_up);
    /* Frame by frame, and what Wi-Fi took from the radio - see radio_coex.h. */
    radio_coex_stats_t cx;
    radio_coex_get_stats(&cx);
    cJSON *mac = cJSON_AddObjectToObject(r, "mac");
    cJSON_AddNumberToObject(mac, "tx_done", cx.tx_done);
    cJSON_AddNumberToObject(mac, "tx_no_ack", cx.tx_no_ack);
    cJSON_AddNumberToObject(mac, "tx_bad_ack", cx.tx_bad_ack);
    cJSON_AddNumberToObject(mac, "tx_coex", cx.tx_coex);
    cJSON_AddNumberToObject(mac, "tx_cca_busy", cx.tx_cca_busy);
    cJSON_AddNumberToObject(mac, "tx_other", cx.tx_other);
    cJSON_AddNumberToObject(mac, "rx", cx.rx);
    cJSON_AddNumberToObject(mac, "rx_coex", cx.rx_coex);
    cJSON_AddNumberToObject(mac, "rx_ack_coex", cx.rx_ack_coex);
    cJSON_AddNumberToObject(mac, "tx_ack_coex", cx.tx_ack_coex);
    cJSON_AddNumberToObject(mac, "tx_requests", ed.tx_requests);
    cJSON_AddNumberToObject(mac, "tx_request_errors", ed.tx_request_errors);
    return r;
}

/* Getting a Hue dimmer onto Philips' button cluster - see zb_hue_stats_t. */
static cJSON *zb_hue_json(void)
{
    zb_hue_stats_t hs;
    zb_get_hue_stats(&hs);
    cJSON *hue = cJSON_CreateObject();
    cJSON_AddNumberToObject(hue, "bind_asked", hs.bind_asked);
    if (hs.bind_status == -1) {
        cJSON_AddNullToObject(hue, "bind_status");
    } else if (hs.bind_status == -2) {
        cJSON_AddStringToObject(hue, "bind_status", "no_answer");
    } else {
        cJSON_AddNumberToObject(hue, "bind_status", hs.bind_status);
    }
    cJSON_AddNumberToObject(hue, "aps_frames", hs.aps_frames);
    cJSON_AddNumberToObject(hue, "frames", hs.zcl_frames);
    return hue;
}

#if CONFIG_ZBC_WLED
/* Hue entertainment frames (zb_ent_begin): what was sent, refused and heard
 * back. Free-running since boot, like the radio counters; on every poll. */
static cJSON *zb_ent_json(void)
{
    zb_ent_stats_t es;
    zb_get_ent_stats(&es);
    cJSON *e = cJSON_CreateObject();
    cJSON_AddBoolToObject(e, "active", es.active);
    cJSON_AddNumberToObject(e, "streams", es.streams);
    if (es.streams) {
        cJSON_AddNumberToObject(e, "lamps", es.lamps);
        cJSON_AddNumberToObject(e, "proxy", es.proxy);
    }
    cJSON_AddNumberToObject(e, "counter", es.counter);
    cJSON_AddNumberToObject(e, "frames_sent", es.frames_sent);
    cJSON_AddNumberToObject(e, "frames_refused", es.frames_refused);
    cJSON_AddNumberToObject(e, "last_refusal", es.last_refusal);
    cJSON_AddNumberToObject(e, "sync_sent", es.sync_sent);
    cJSON_AddNumberToObject(e, "sync_delivered", es.sync_delivered);
    cJSON_AddNumberToObject(e, "sync_undelivered", es.sync_undelivered);
    if (es.balance_status >= 0) {
        cJSON_AddNumberToObject(e, "balance", es.balance_status);
    } else {
        cJSON_AddNullToObject(e, "balance");
    }
    if (es.rejected_status >= 0) {
        cJSON *rej = cJSON_AddObjectToObject(e, "rejected");
        cJSON_AddNumberToObject(rej, "cmd", es.rejected_cmd);
        cJSON_AddNumberToObject(rej, "status", es.rejected_status);
    } else {
        cJSON_AddNullToObject(e, "rejected");
    }
    cJSON_AddNumberToObject(e, "answers_heard", es.answers_heard);
    cJSON_AddNumberToObject(e, "relay_heard", es.relay_heard);
    cJSON_AddNumberToObject(e, "kept_fresh", es.kept_fresh);
    return e;
}
#endif

/* A channel move (zb_move_channel), or the coordinator putting itself back on
 * the moved-to channel at boot. */
static cJSON *zb_channel_move_json(void)
{
    zb_channel_move_t m;
    zb_get_channel_move(&m);
    static const char *const STATES[] = { "idle", "announced", "moving", "moved", "failed" };
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", STATES[m.state <= ZB_MOVE_FAILED ? m.state : 0]);
    if (m.state != ZB_MOVE_IDLE) {
        cJSON_AddNumberToObject(o, "from", m.from);
        cJSON_AddNumberToObject(o, "to", m.to);
        cJSON_AddBoolToObject(o, "boot", m.boot);
        cJSON_AddNumberToObject(o, "s_ago", (double)((esp_timer_get_time() - m.at_us) / 1000000));
    }
    return o;
}

static void stream_zigbee(json_stream_t *js)
{
    zb_netinfo_t zb;
    zb_get_netinfo(&zb);

    bool first = true;
    json_stream_raw(js, "{");
    json_stream_member(js, &first, "network_up", cJSON_CreateBool(zb.network_up));
    json_stream_member(js, &first, "pan_id", cJSON_CreateNumber(zb.pan_id));
    json_stream_member(js, &first, "channel", cJSON_CreateNumber(zb.channel));
    json_stream_member(js, &first, "channel_move", zb_channel_move_json());
    json_stream_member(js, &first, "permit_join_remaining_s",
                       cJSON_CreateNumber(zb.permit_join_remaining_s));
    char ieee[19];
    zbc_ieee_to_str(zb.ieee, ieee);
    json_stream_member(js, &first, "ieee", cJSON_CreateString(ieee));
    /* How the stack keeps children: a child not heard from within its timeout
     * is dropped and sent looking for another parent. Per child in
     * /api/devices; this is what one gets that has not asked for its own. */
    if (zb.network_up) {
        json_stream_member(js, &first, "ed_timeout_s", cJSON_CreateNumber(zb.ed_timeout_s));
        json_stream_member(js, &first, "max_children", cJSON_CreateNumber(zb.max_children));
    } else {
        json_stream_member(js, &first, "forming", zb_forming_json());
    }
    json_stream_member(js, &first, "radio", zb_radio_json());
    json_stream_member(js, &first, "hue_dimmer", zb_hue_json());
#if CONFIG_ZBC_WLED
    json_stream_member(js, &first, "ent", zb_ent_json());
#endif
    /* Firmware for Zigbee devices. On every poll, unchanged or not, so a
     * transfer's progress moves without a table rebuild per block. */
    json_stream_member(js, &first, "ota", zb_ota_status_json());
    json_stream_raw(js, "}");
}

/* Overheat protection. On every poll, like the rest of the status, so the
 * temperature moves without a revision bump; a change of state bumps one. */
static cJSON *thermal_json(void)
{
    thermal_status_t th;
    thermal_get(&th);
    cJSON *t = cJSON_CreateObject();
    static const char *const THERMAL_STATES[] = { "ok", "hot", "fault" };
    cJSON_AddStringToObject(t, "state", !th.configured ? "off" : THERMAL_STATES[th.state < 3 ? th.state : 2]);
    if (th.configured && th.have) {
        cJSON_AddNumberToObject(t, "temp_c", th.deci_c / 10.0);
        cJSON_AddNumberToObject(t, "max_c", th.max_deci_c / 10.0);
    } else {
        cJSON_AddNullToObject(t, "temp_c");
        cJSON_AddNullToObject(t, "max_c");
    }
    cJSON_AddNumberToObject(t, "trip_c", th.trip_c);
    cJSON_AddNumberToObject(t, "release_c", th.trip_c - ZBC_THERMAL_RELEASE_DECI_C / 10.0);
    cJSON_AddNumberToObject(t, "trips", th.trips);
    cJSON_AddNumberToObject(t, "mv", th.mv);
    cJSON_AddBoolToObject(t, "held", local_lights_held());
    return t;
}

/* The wired lights' fade task: how often it came late while something moved.
 * Free-running since boot, like the radio counters - sample around a fade. */
static cJSON *wired_json(void)
{
    local_lights_fade_stats_t f;
    local_lights_fade_stats(&f);
    cJSON *w = cJSON_CreateObject();
    cJSON_AddNumberToObject(w, "fade_ticks", f.ticks);
    cJSON_AddNumberToObject(w, "fade_late", f.late);
    cJSON_AddNumberToObject(w, "fade_max_gap_ms", f.max_gap_ms);
    return w;
}

/* This firmware and the updater beside it. */
static cJSON *ota_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "partition", ota_running_label());
    cJSON_AddBoolToObject(o, "pending_verify", ota_pending_verify());
    /* Which updater a box has, and whether it is whole: the coordinator will
     * not hand an update over to one that does not verify, and the page offers
     * to replace it. */
    const char *updater = ota_updater_version();
    if (updater) {
        cJSON_AddStringToObject(o, "updater", updater);
    } else {
        cJSON_AddNullToObject(o, "updater");
    }
    const char *updater_problem = ota_updater_problem();
    cJSON_AddBoolToObject(o, "updater_valid", updater_problem == NULL);
    if (updater_problem) {
        cJSON_AddStringToObject(o, "updater_problem", updater_problem);
    } else {
        cJSON_AddNullToObject(o, "updater_problem");
    }
    /* "old" until the updater has rewritten the partition table with room for
     * Zigbee device firmware - see partitions.csv. */
    cJSON_AddStringToObject(o, "layout", zb_ota_has_partition() ? "current" : "old");
    return o;
}

/* heap_watch's three tables, a row at a time: with the watch built in they are
 * the largest thing here, and a row is a few dozen bytes. The sink closes the
 * array it opened when the next one begins, and stream_heap closes the last. */
typedef struct {
    json_stream_t *js;
    bool          *first;      /* the `heap` object's own separator state */
    bool           open;       /* an array has been opened and wants closing */
    bool           row_first;
} heap_tables_t;

static void heap_table_begin(void *ctx, const char *name)
{
    heap_tables_t *h = ctx;
    if (h->open) {
        json_stream_raw(h->js, "]");
    }
    json_stream_key(h->js, h->first, name);
    json_stream_raw(h->js, "[");
    h->open      = true;
    h->row_first = true;
}

static void heap_table_row(void *ctx, cJSON *item)
{
    heap_tables_t *h = ctx;
    json_stream_element(h->js, &h->row_first, item);
}

/* Internal RAM, in bytes. `min_free` is the low-water mark since boot, the
 * number that says whether something new fits; `largest` is what one
 * allocation can still get. `tables_too` adds heap_watch's: only on /api/status.
 * The page never reads them, and in its poll they were ~5.5 KB of every answer
 * (bad-og: 8.3 KB where 2.8 would do) - an answer that, with the antenna busy,
 * sits in httpd's TCP buffers beside a Wi-Fi receive burst. Measured
 * 2026-10-11, the pair took bad-og's heap to 2.9 KB. */
static void stream_heap(json_stream_t *js, bool tables_too)
{
    bool first = true;
    json_stream_raw(js, "{");
    json_stream_member(js, &first, "free",
                       cJSON_CreateNumber(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    json_stream_member(js, &first, "min_free", cJSON_CreateNumber(heap_watch_min_free()));
    json_stream_member(js, &first, "largest",
                       cJSON_CreateNumber(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    heap_tables_t     tables = { .js = js, .first = &first };
    heap_watch_sink_t sink   = { .ctx = &tables, .table = heap_table_begin, .row = heap_table_row };
    if (tables_too) {
        heap_watch_stream_json(&sink);   /* nothing at all without CONFIG_ZBC_HEAP_WATCH */
    }
    if (tables.open) {
        json_stream_raw(js, "]");
    }
    json_stream_raw(js, "}");
}

/* `heap_tables`: see stream_heap. */
static void stream_status(json_stream_t *js, bool heap_tables)
{
    bool first = true;
    json_stream_raw(js, "{");
    /* Beside the hostname it derives, so the UI can show the name the moment the
     * poll lands rather than waiting on a second request. */
    json_stream_member(js, &first, "name", cJSON_CreateString(settings_name()));
    json_stream_member(js, &first, "wifi", wifi_json());
    json_stream_key(js, &first, "zigbee");
    stream_zigbee(js);
    json_stream_member(js, &first, "thermal", thermal_json());
    json_stream_member(js, &first, "wired", wired_json());
#if CONFIG_ZBC_WLED
    json_stream_member(js, &first, "wled", wled_status_json());
#endif
    json_stream_member(js, &first, "device_count", cJSON_CreateNumber(device_db_count()));
    json_stream_member(js, &first, "uptime_s",
                       cJSON_CreateNumber((double)(esp_timer_get_time() / 1000000)));
    /* Why this boot happened, so "the board seems to have crashed" can be told
     * from a power cut - and where the last crash was, since there is no core
     * dump partition and the panic handler prints only to the serial port. */
    json_stream_member(js, &first, "reset_reason",
                       cJSON_CreateString(crash_log_reset_reason_name(esp_reset_reason())));
    json_stream_member(js, &first, "last_crash", crash_log_json());
    json_stream_member(js, &first, "version",
                       cJSON_CreateString(esp_app_get_description()->version));
    /* Unique per image, where `version` is only the git description - two
     * builds from one uncommitted tree share it. The UI reloads itself when this
     * changes, so it has to change whenever the firmware does. */
    char build[17];
    esp_app_get_elf_sha256(build, sizeof(build));
    json_stream_member(js, &first, "build", cJSON_CreateString(build));
    json_stream_member(js, &first, "ota", ota_json());
    json_stream_key(js, &first, "heap");
    stream_heap(js, heap_tables);
    json_stream_raw(js, "}");
}

static esp_err_t status_get(httpd_req_t *req)
{
    json_stream_t js;
    if (!json_stream_begin(&js, req)) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    stream_status(&js, true);
    return json_stream_end(&js);
}

/* NULL when the snapshot cannot be allocated; the caller answers 500. */
/* zbc_light_boot_t, as the API spells it. */
static const char *const LIGHT_BOOT_NAMES[ZBC_LIGHT_BOOT_COUNT] = { "off", "on", "last" };
static const char *const LIGHT_KIND_NAMES[ZBC_LIGHT_KIND_COUNT] = { "onoff", "cwww", "dim", "cwnwww" };
static const char *const LIGHT_MIX_NAMES[ZBC_MIX_COUNT]         = { "constant", "full", "custom" };
/* A light's mixing curves, by ZBC_CH_* as the API names them. */
static const char *const LIGHT_CH_NAMES[ZBC_PWM_MAX_LIGHT_CH]  = { "cold", "neutral", "warm" };

/* {"points": [[x, y], ...]}, or null for a channel with no curve - the shape a
 * brightness curve has, so the page edits both with one chart. */
static cJSON *mix_curve_to_json(const zbc_mix_curve_t *c)
{
    if (c->n == 0) {
        return cJSON_CreateNull();
    }
    cJSON *obj    = cJSON_CreateObject();
    cJSON *points = cJSON_AddArrayToObject(obj, "points");
    for (int i = 0; i < c->n; i++) {
        cJSON *pair = cJSON_CreateArray();
        cJSON_AddItemToArray(pair, cJSON_CreateNumber(c->x[i]));
        cJSON_AddItemToArray(pair, cJSON_CreateNumber(c->y[i]));
        cJSON_AddItemToArray(points, pair);
    }
    return obj;
}

/* One device as /api/devices and /api/state list it. */
static cJSON *device_to_json(const zbc_device_t *dev, int64_t now)
{
    cJSON *d = cJSON_CreateObject();

    char ieee[19];
    zbc_ieee_to_str(dev->ieee, ieee);
    cJSON_AddStringToObject(d, "ieee", ieee);
    /* null for a row restored from another network's backup, until it joins. */
    if (zbc_short_known(dev->short_addr)) {
        cJSON_AddNumberToObject(d, "short_addr", dev->short_addr);
    } else {
        cJSON_AddNullToObject(d, "short_addr");
    }
    cJSON_AddStringToObject(d, "name", dev->name);
    cJSON_AddStringToObject(d, "manufacturer", dev->manufacturer);
    cJSON_AddStringToObject(d, "model", dev->model);

    /* Where the light is, and for a wired one what the editor edits. */
    const uint16_t light_id  = zbc_light_id_from_ieee(dev->ieee);
    const uint16_t mlight_id = zbc_mlight_id_from_ieee(dev->ieee);
    zbc_light_t    light;
    zbc_mlight_t   mlight;
    cJSON_AddStringToObject(d, "source", light_id ? "gpio" : (mlight_id ? "matter" : "zigbee"));
    if (mlight_id && mlight_db_get(mlight_id, &mlight)) {
        /* What pairing learnt, and whether the lamp can be reached at all: after
         * a Matter reset the rows are still here but our fabric is not. The node
         * id is a string - JSON numbers stop being exact at 2^53. */
        char node[19];
        snprintf(node, sizeof(node), "%016llX", (unsigned long long)mlight.node_id);
        cJSON_AddNumberToObject(d, "id", mlight_id);
        cJSON_AddStringToObject(d, "node_id", node);
        cJSON_AddNumberToObject(d, "remote_ep", mlight.remote_ep);
        cJSON_AddNumberToObject(d, "vendor_id", mlight.vendor_id);
        cJSON_AddNumberToObject(d, "product_id", mlight.product_id);
        cJSON_AddBoolToObject(d, "bridged", (mlight.flags & ZBC_MLIGHT_BRIDGED) != 0);
        cJSON_AddBoolToObject(d, "verified", (mlight.flags & ZBC_MLIGHT_VERIFIED) != 0);
        cJSON_AddBoolToObject(d, "needs_pairing", mctl_running() && !mctl_has_fabric());
    }
    if (light_id && light_db_get(light_id, &light)) {
        cJSON_AddNumberToObject(d, "id", light_id);
        if (light.gpio < 0) {
            cJSON_AddNullToObject(d, "gpio");
        } else {
            cJSON_AddNumberToObject(d, "gpio", light.gpio);
        }
        cJSON_AddBoolToObject(d, "active_low", light.active_low);
        cJSON_AddStringToObject(d, "power_on",
                                LIGHT_BOOT_NAMES[light.boot < ZBC_LIGHT_BOOT_COUNT ? light.boot : 0]);
        cJSON_AddStringToObject(d, "kind",
                                LIGHT_KIND_NAMES[light.kind < ZBC_LIGHT_KIND_COUNT ? light.kind : 0]);
        if (light.gpio_warm < 0) {
            cJSON_AddNullToObject(d, "gpio_warm");
        } else {
            cJSON_AddNumberToObject(d, "gpio_warm", light.gpio_warm);
        }
        cJSON_AddNumberToObject(d, "pwm_hz", light.pwm_hz);
        const int phase = local_light_pwm_phase(light_id);
        if (phase < 0) {
            cJSON_AddNullToObject(d, "pwm_phase");
        } else {
            cJSON_AddNumberToObject(d, "pwm_phase", phase);
        }
        cJSON_AddNumberToObject(d, "cold_kelvin", light.cold_k);
        cJSON_AddNumberToObject(d, "warm_kelvin", light.warm_k);
        if (light.gpio_neutral < 0) {
            cJSON_AddNullToObject(d, "gpio_neutral");
        } else {
            cJSON_AddNumberToObject(d, "gpio_neutral", light.gpio_neutral);
        }
        cJSON_AddNumberToObject(d, "neutral_kelvin", light.neutral_k);
        cJSON_AddStringToObject(d, "mix", LIGHT_MIX_NAMES[light.mix < ZBC_MIX_COUNT ? light.mix : 0]);
        cJSON *curves = cJSON_AddObjectToObject(d, "mix_curves");
        for (int c = 0; curves && c < ZBC_PWM_MAX_LIGHT_CH; c++) {
            cJSON_AddItemToObject(curves, LIGHT_CH_NAMES[c], mix_curve_to_json(&light.curve[c]));
        }
        /* Held off by the overheat protection; switching on is refused. */
        cJSON_AddBoolToObject(d, "held", local_lights_held());
    }

    bool fresh = dev->last_seen_us > 0 && (now - dev->last_seen_us) < OFFLINE_AFTER_US;
    cJSON_AddBoolToObject(d, "online", dev->online && fresh);
    cJSON_AddNumberToObject(d, "last_seen_s_ago",
                            dev->last_seen_us ? (double)((now - dev->last_seen_us) / 1000000) : -1);

    /* Link quality from the last neighbour scan, null until one has seen the
     * device. Read this before treating a dimming or state-tracking symptom
     * as a bug: nothing above the radio repairs a link at 0/255. */
    int lqa = device_db_lqa(dev->ieee);
    if (lqa == ZBC_UNKNOWN) {
        cJSON_AddNullToObject(d, "lqa");
    } else {
        cJSON_AddNumberToObject(d, "lqa", lqa);
    }
    /* The receiving side, from the frames the device itself sent: the RSSI
     * of the last one, how many arrived, and how many of its sequence
     * numbers never did. This is the reading a remote's lost presses show
     * up in, and the only one a sleepy remote gets - it is never in the
     * neighbour table the lqa comes from. */
    zb_link_info_t link;
    if (!light_id && !mlight_id && zb_device_link(dev->ieee, &link)) {
        /* A row can come from a neighbour table before any frame has: no RSSI yet. */
        if (link.heard) {
            cJSON_AddNumberToObject(d, "rssi", link.rssi);
        } else {
            cJSON_AddNullToObject(d, "rssi");
        }
        cJSON_AddNumberToObject(d, "frames_heard", link.frames);
        cJSON_AddNumberToObject(d, "frames_missed", link.missed);
        if (link.child < 0) {
            cJSON_AddNullToObject(d, "child");
        } else {
            cJSON_AddBoolToObject(d, "child", link.child == 1);
        }
        /* An end device's parent, and whether it sends to groups: together they
         * say whether its presses reach us acknowledged. See zb_link_info_t. */
        cJSON_AddBoolToObject(d, "groupcast", link.groupcast);
        if (link.parent_known) {
            cJSON *p = cJSON_AddObjectToObject(d, "parent");
            cJSON_AddBoolToObject(p, "coordinator", link.parent_is_us);
            cJSON_AddNumberToObject(p, "short_addr", link.parent_short);
            if (link.parent_ieee_known) {
                char pieee[19];
                zbc_ieee_to_str(link.parent_ieee, pieee);
                cJSON_AddStringToObject(p, "ieee", pieee);
            } else {
                cJSON_AddNullToObject(p, "ieee");
            }
            cJSON_AddStringToObject(p, "source", link.parent_src ? link.parent_src : "?");
            if (link.parent_lqi < 0) {
                cJSON_AddNullToObject(p, "lqi");
            } else {
                cJSON_AddNumberToObject(p, "lqi", link.parent_lqi);
            }
            cJSON_AddNumberToObject(p, "changed_s_ago", (double)((now - link.parent_us) / 1000000));
            if (link.parent_is_us && link.timeout_s) {
                cJSON_AddNumberToObject(p, "timeout_s", link.timeout_s);
                cJSON_AddNumberToObject(p, "timeout_left_s", link.timeout_left_s);
            }
        } else {
            cJSON_AddNullToObject(d, "parent");
        }
    } else {
        cJSON_AddNullToObject(d, "rssi");
        cJSON_AddNullToObject(d, "child");
        cJSON_AddNullToObject(d, "parent");
    }
    /* The firmware it said it runs, when it has an OTA client. */
    if (light_id || mlight_id) {
        cJSON_AddNullToObject(d, "ota");
    } else {
        zb_ota_add_device_json(d, dev->ieee);
    }

    cJSON *eps = cJSON_AddArrayToObject(d, "endpoints");
    for (int e = 0; e < dev->ep_count; e++) {
        const zbc_endpoint_t *src = &dev->eps[e];
        cJSON                *ep  = cJSON_CreateObject();

        cJSON_AddNumberToObject(ep, "ep", src->ep);
        cJSON_AddNumberToObject(ep, "device_id", src->device_id);

        /* What the UI may show for this lamp. */
        cJSON *feat = cJSON_AddArrayToObject(ep, "features");
        if (src->features & ZBC_FEAT_ONOFF) {
            cJSON_AddItemToArray(feat, cJSON_CreateString("onoff"));
        }
        if (src->features & ZBC_FEAT_LEVEL) {
            cJSON_AddItemToArray(feat, cJSON_CreateString("brightness"));
        }
        if (src->features & ZBC_FEAT_COLOR_TEMP) {
            cJSON_AddItemToArray(feat, cJSON_CreateString("color_temp"));
        }
        if (src->features & ZBC_FEAT_ANY_COLOR) {
            cJSON_AddItemToArray(feat, cJSON_CreateString("color"));
        }
        /* A remote, not a lamp. It shares the endpoint list because it is a
         * paired device like any other, and a client has to be able to tell
         * the two apart - the older Aqara switch carries "onoff" as well,
         * because it really does advertise the cluster. */
        if (src->features & ZBC_FEAT_SWITCH) {
            cJSON_AddItemToArray(feat, cJSON_CreateString("switch"));
        }
        /* The page offers a Hue dimmer's holds and double presses only once
         * they can arrive. */
        if (src->features & ZBC_FEAT_HUE_BUTTONS) {
            cJSON_AddItemToArray(feat, cJSON_CreateString("hue_buttons"));
        }

        if (src->on_off == ZBC_UNKNOWN) {
            cJSON_AddNullToObject(ep, "on");
        } else {
            cJSON_AddBoolToObject(ep, "on", src->on_off == 1);
        }

        /* Brightness as a percentage: ZCL level 1-254 maps to 1-100 %. */
        if (src->level == ZBC_UNKNOWN) {
            cJSON_AddNullToObject(ep, "brightness_pct");
        } else {
            cJSON_AddNumberToObject(ep, "brightness_pct", zbc_level_to_pct(src->level));
        }

        add_kelvin(ep, "kelvin", src->ct_mireds);
        /* Mireds run the other way round from kelvin, so the coolest limit is
         * the largest kelvin value. */
        add_kelvin(ep, "min_kelvin", src->ct_max_mireds);
        add_kelvin(ep, "max_kelvin", src->ct_min_mireds);

        if (src->hue == ZBC_UNKNOWN) {
            cJSON_AddNullToObject(ep, "hue");
        } else {
            cJSON_AddNumberToObject(ep, "hue", (int)(src->hue * 360 / 254) % 360);
        }
        if (src->saturation == ZBC_UNKNOWN) {
            cJSON_AddNullToObject(ep, "sat");
        } else {
            cJSON_AddNumberToObject(ep, "sat", (int)(src->saturation * 100 / 254));
        }
        if (src->color_mode == ZBC_UNKNOWN) {
            cJSON_AddNullToObject(ep, "color_mode");
        } else {
            cJSON_AddStringToObject(ep, "color_mode",
                                    src->color_mode == 2 ? "color_temp" : "color");
        }

        /* Which response curves this endpoint has, and the brightness that
         * was last asked for. The UI slider binds to input_pct rather than
         * brightness_pct whenever a level curve is in the way: the lamp
         * reports its mapped output, which would never agree with the
         * optimistic value and would leave the slider stuck. */
        bool has_level = false, has_chroma = false;
        curve_db_active(dev->ieee, src->ep, &has_level, &has_chroma);
        cJSON *curves = cJSON_AddObjectToObject(ep, "curves");
        cJSON_AddBoolToObject(curves, "level", has_level);
        cJSON_AddBoolToObject(curves, "chroma", has_chroma);

        int input_pct = curve_db_input_pct(dev->ieee, src->ep);
        if (input_pct == ZBC_UNKNOWN) {
            cJSON_AddNullToObject(ep, "input_pct");
        } else {
            cJSON_AddNumberToObject(ep, "input_pct", input_pct);
        }

        /* Whether this endpoint will ever volunteer its state - a different question
         * from whether the link is good: a device reports only once a ZDO bind and
         * a Configure Reporting have both been answered. `frames_heard` in the
         * same row is the other half: it counts only frames the device sent of
         * its own accord, so a lamp with every answer in and frames_heard stuck
         * at 0 accepted the configuration and ignores it. Not revision-tracked (an
         * answer bumps nothing), so a page sees it on its next full read. A wired
         * light has no radio and no bindings, hence nothing here, and a Matter
         * lamp reports through its subscription, which has no such halves. */
        if (!light_id && !mlight_id) {
            zb_report_state_t rs;
            zb_report_state(dev->ieee, src->ep, src->features, &rs);
            cJSON *reporting = cJSON_AddArrayToObject(ep, "reporting");
            for (int r = 0; r < rs.count; r++) {
                cJSON *c = cJSON_CreateObject();
                cJSON_AddStringToObject(c, "cluster", rs.c[r].cluster);
                cJSON_AddBoolToObject(c, "bound", rs.c[r].bound);
                if (rs.c[r].wants_config) {
                    cJSON_AddBoolToObject(c, "configured", rs.c[r].configured);
                } else {
                    cJSON_AddNullToObject(c, "configured");
                }
                cJSON_AddItemToArray(reporting, c);
            }
        }

        cJSON_AddItemToArray(eps, ep);
    }
    return d;
}

/* The device list, one device's JSON at a time. Only the addresses are taken up
 * front; each row is copied as it is written, and one removed in between is
 * left out. Wired lights follow, built by lighting_get_device, so everything in
 * device_to_json serves both kinds. */
static void stream_devices(json_stream_t *js)
{
    uint8_t       ieees[CONFIG_ZBC_MAX_DEVICES][ZBC_IEEE_LEN];
    const size_t  n     = device_db_list_ieee(ieees, CONFIG_ZBC_MAX_DEVICES);
    const int64_t now   = esp_timer_get_time();
    bool          first = true;
    zbc_device_t  dev;

    json_stream_raw(js, "[");
    for (size_t i = 0; i < n; i++) {
        if (device_db_get(ieees[i], &dev)) {
            json_stream_element(js, &first, device_to_json(&dev, now));
        }
    }
    zbc_light_t  lights[CONFIG_ZBC_MAX_LIGHTS];
    const size_t nl = light_db_snapshot(lights, CONFIG_ZBC_MAX_LIGHTS);
    for (size_t i = 0; i < nl; i++) {
        uint8_t ieee[ZBC_IEEE_LEN];
        zbc_light_ieee(lights[i].id, ieee);
        if (lighting_get_device(ieee, &dev)) {
            json_stream_element(js, &first, device_to_json(&dev, now));
        }
    }
    /* Then the Matter lamps, the same way: only their ids up front, each row
     * copied as it is written. */
    uint16_t     mids[CONFIG_ZBC_MAX_MATTER_LIGHTS];
    const size_t nm = mlight_db_list_ids(mids, CONFIG_ZBC_MAX_MATTER_LIGHTS);
    for (size_t i = 0; i < nm; i++) {
        uint8_t ieee[ZBC_IEEE_LEN];
        zbc_mlight_ieee(mids[i], ieee);
        if (lighting_get_device(ieee, &dev)) {
            json_stream_element(js, &first, device_to_json(&dev, now));
        }
    }
    json_stream_raw(js, "]");
}

static esp_err_t devices_get(httpd_req_t *req)
{
    json_stream_t js;
    if (!json_stream_begin(&js, req)) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    json_stream_raw(&js, "{\"devices\":");
    stream_devices(&js);
    json_stream_raw(&js, "}");
    return json_stream_end(&js);
}

static esp_err_t permit_join_post(httpd_req_t *req)
{
    int duration = 180;
    if (req->content_len > 0) {
        cJSON *json = read_json_body(req);
        if (!json) {
            return ESP_FAIL;
        }
        cJSON *d = cJSON_GetObjectItem(json, "duration");
        if (cJSON_IsNumber(d)) {
            duration = d->valueint;
        }
        cJSON_Delete(json);
    }
    if (duration < 0 || duration > 254) {
        return send_error(req, "400 Bad Request", "duration must be 0-254 seconds");
    }

    esp_err_t err = zb_permit_join((uint8_t)duration);
    if (err != ESP_OK) {
        return send_error(req, "503 Service Unavailable", "Zigbee network is not ready yet");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "duration", duration);
    return send_json(req, root, "200 OK");
}

/* Shared tail of the three "set something" routes: they all answer 202, because
 * the lamp's own report is what finally decides the state. */
static esp_err_t send_command_result(httpd_req_t *req, const uint8_t ieee[ZBC_IEEE_LEN], esp_err_t err)
{
    if (err == ESP_ERR_NOT_FOUND) {
        return send_error(req, "404 Not Found", "unknown device or endpoint");
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return send_error(req, "409 Conflict", "this light does not support that");
    }
    if (err == ESP_ERR_NOT_ALLOWED) {
        /* local_lights_hold: the overheat protection has the wired lights. */
        return send_error(req, "409 Conflict", "the board is too hot; wired lights are held off");
    }
    zbc_device_t dev;
    if (err == ZB_ERR_UNREACHABLE && device_db_get(ieee, &dev) && !zbc_short_known(dev.short_addr)) {
        /* Restored from another network's backup: nothing was sent, and no
         * amount of waiting brings it back - pairing it again does. */
        return send_error(req, "409 Conflict", "this light has not joined this network yet - pair it again");
    }
    if (err == ZB_ERR_UNREACHABLE) {
        return send_error(req, "503 Service Unavailable",
                          "this light is not answering - is it switched off at the wall? "
                          "It is tried again at most every 30 s");
    }
    if (err == ESP_ERR_INVALID_STATE && zbc_mlight_id_from_ieee(ieee)) {
        /* mctl_*: Matter is not running (the setup portal), or our fabric is gone. */
        return send_error(req, "409 Conflict",
                          mctl_running() ? "this Matter lamp needs pairing again"
                                         : "Matter is not running");
    }
    if (err != ESP_OK) {
        /* Name the error: "refused" on its own gives nobody anything to go on. */
        char msg[96];
        snprintf(msg, sizeof(msg), "the Zigbee stack refused the command (%s)",
                 esp_err_to_name(err));
        ESP_LOGW(TAG, "%s", msg);
        return send_error(req, "502 Bad Gateway", msg);
    }
    return send_ok(req, "202 Accepted");
}

/* {"state": "on"|"off"|"toggle"}, toggle when absent. Returns -1 on anything else. */
static int onoff_from_json(const cJSON *json)
{
    const cJSON *state = cJSON_GetObjectItem(json, "state");
    const char  *want  = cJSON_IsString(state) ? state->valuestring : "toggle";
    if (strcmp(want, "on") == 0) {
        return ZB_ONOFF_ON;
    }
    if (strcmp(want, "off") == 0) {
        return ZB_ONOFF_OFF;
    }
    if (strcmp(want, "toggle") == 0) {
        return ZB_ONOFF_TOGGLE;
    }
    return -1;
}

/* "transition_ms" in deciseconds, or the configured fade time when absent. */
static uint16_t transition_ds_from_json(const cJSON *json)
{
    const cJSON *ms = cJSON_GetObjectItem(json, "transition_ms");
    if (!cJSON_IsNumber(ms)) {
        return settings_transition_ds();
    }
    /* Clamped before the cast: -100 would otherwise wrap to 65535 ds, an
     * 1.8-hour ramp, and so would anything past the 16-bit field. */
    const int ds = ms->valueint / 100;
    return (uint16_t)(ds < 0 ? 0 : (ds > 0xFFFE ? 0xFFFE : ds));
}

/* The non-empty "name" string, or NULL. */
static const char *name_from_json(const cJSON *json)
{
    const cJSON *name = cJSON_GetObjectItem(json, "name");
    return cJSON_IsString(name) && name->valuestring[0] != '\0' ? name->valuestring : NULL;
}

/* POST /api/devices/{ieee}/ep/{ep}/onoff */
static esp_err_t device_onoff_post(httpd_req_t *req, const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    cJSON *json = read_json_body(req);
    if (!json) {
        return ESP_FAIL;
    }
    int cmd = onoff_from_json(json);
    cJSON_Delete(json);
    if (cmd < 0) {
        return send_error(req, "400 Bad Request", "state must be on, off or toggle");
    }

    /* The command is on its way; the light's own report settles the state. */
    return send_command_result(req, ieee, lighting_set_onoff(ieee, ep, (zb_onoff_cmd_t)cmd));
}

/* POST /api/devices/{ieee}/ep/{ep}/level  {"pct": 1-100} */
static esp_err_t device_level_post(httpd_req_t *req, const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    cJSON *json = read_json_body(req);
    if (!json) {
        return ESP_FAIL;
    }
    cJSON *pct = cJSON_GetObjectItem(json, "pct");
    if (!cJSON_IsNumber(pct) || pct->valueint < 1 || pct->valueint > 100) {
        cJSON_Delete(json);
        return send_error(req, "400 Bad Request", "pct must be 1-100");
    }
    uint16_t dsec = transition_ds_from_json(json);
    int      want = pct->valueint;
    cJSON_Delete(json);

    /* This is the brightness the user asked for, not the level the lamp gets:
     * the endpoint's response curve sits in between. */
    return send_command_result(req, ieee, lighting_set_brightness(ieee, ep, want, dsec));
}

/* POST /api/devices/{ieee}/ep/{ep}/color_temp  {"kelvin": 2000-6500} */
static esp_err_t device_color_temp_post(httpd_req_t *req, const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    cJSON *json = read_json_body(req);
    if (!json) {
        return ESP_FAIL;
    }
    cJSON *kelvin = cJSON_GetObjectItem(json, "kelvin");
    if (!cJSON_IsNumber(kelvin) || kelvin->valueint < 1000 || kelvin->valueint > 10000) {
        cJSON_Delete(json);
        return send_error(req, "400 Bad Request", "kelvin must be 1000-10000");
    }
    uint16_t dsec   = transition_ds_from_json(json);
    uint16_t mireds = (uint16_t)(1000000 / kelvin->valueint);
    cJSON_Delete(json);

    return send_command_result(req, ieee, lighting_set_color_temp(ieee, ep, mireds, dsec));
}

/* POST /api/devices/{ieee}/ep/{ep}/color  {"hue": 0-359, "sat": 0-100} */
static esp_err_t device_color_post(httpd_req_t *req, const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    cJSON *json = read_json_body(req);
    if (!json) {
        return ESP_FAIL;
    }
    cJSON *hue = cJSON_GetObjectItem(json, "hue");
    cJSON *sat = cJSON_GetObjectItem(json, "sat");
    if (!cJSON_IsNumber(hue) || !cJSON_IsNumber(sat) ||
        hue->valueint < 0 || hue->valueint > 360 || sat->valueint < 0 || sat->valueint > 100) {
        cJSON_Delete(json);
        return send_error(req, "400 Bad Request", "hue must be 0-360 and sat 0-100");
    }
    uint16_t dsec    = transition_ds_from_json(json);
    uint8_t  zcl_hue = (uint8_t)((hue->valueint % 360) * 254 / 360);
    uint8_t  zcl_sat = (uint8_t)(sat->valueint * 254 / 100);
    cJSON_Delete(json);

    return send_command_result(req, ieee, lighting_set_color_hs(ieee, ep, zcl_hue, zcl_sat, dsec));
}

static esp_err_t device_patch(httpd_req_t *req, const uint8_t ieee[ZBC_IEEE_LEN])
{
    cJSON *json = read_json_body(req);
    if (!json) {
        return ESP_FAIL;
    }
    const char *name = name_from_json(json);
    if (!name) {
        cJSON_Delete(json);
        return send_error(req, "400 Bad Request", "name is required");
    }
    const uint16_t light_id  = zbc_light_id_from_ieee(ieee);
    const uint16_t mlight_id = zbc_mlight_id_from_ieee(ieee);
    esp_err_t      err       = light_id    ? light_db_rename(light_id, name)
                               : mlight_id ? mlight_db_rename(mlight_id, name)
                                           : device_db_rename(ieee, name);
    cJSON_Delete(json);

    if (err == ESP_ERR_NOT_FOUND) {
        return send_error(req, "404 Not Found", "unknown device");
    }
    return send_ok(req, "200 OK");
}

/* A Matter lamp is a row; its device is told to forget us once its last row
 * goes - a hub keeps our fabric while any of its lamps is still here. That is
 * best effort and never holds the removal up: a lamp that is unplugged keeps a
 * stale entry in its own fabric table, which its own app can remove. */
static esp_err_t mlight_delete(uint16_t id)
{
    zbc_mlight_t row;
    if (!mlight_db_get(id, &row)) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = mlight_db_remove(id);
    if (err != ESP_OK) {
        return err;
    }
    if (mlight_db_node_rows(row.node_id) == 0) {
        mctl_remove_node(row.node_id, row.lamp_fabric);
    }
    mctl_reload();
    return ESP_OK;
}

static esp_err_t device_delete(httpd_req_t *req, const uint8_t ieee[ZBC_IEEE_LEN])
{
    /* A wired light has no network to leave; removing it hands its pin back. */
    const uint16_t light_id  = zbc_light_id_from_ieee(ieee);
    const uint16_t mlight_id = zbc_mlight_id_from_ieee(ieee);
    esp_err_t      err       = light_id    ? light_db_remove(light_id)
                               : mlight_id ? mlight_delete(mlight_id)
                                           : zb_remove_device(ieee);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_error(req, "404 Not Found", "unknown device");
    }
    if (light_id) {
        local_lights_reload();
    }
    /* A device that is gone must not linger in a group and be commanded again,
     * nor leave a curve behind for whatever joins next. */
    group_db_forget_device(ieee);
    curve_db_forget_device(ieee);
    lighting_forget_device(ieee);
    /* A button whose source is a remote names the device, and one left pointing
     * at a device that is gone would fire at nothing - it keeps its name and
     * its rules and loses only its source. */
    button_db_forget_device(ieee);
    return send_ok(req, "200 OK");
}

/* ---------------------------------------------------------------- curves */

/* A response curve maps the brightness asked for onto what this lamp actually
 * does about it. The points are interpolated with a monotone cubic, so the
 * mapping can never reverse; see curve_db.c. */

static cJSON *curve_to_json(const zbc_curve_t *c)
{
    if (c->n == 0) {
        return cJSON_CreateNull();
    }
    cJSON *obj    = cJSON_CreateObject();
    cJSON *points = cJSON_AddArrayToObject(obj, "points");
    for (int i = 0; i < c->n; i++) {
        cJSON *pair = cJSON_CreateArray();
        cJSON_AddItemToArray(pair, cJSON_CreateNumber(c->x[i]));
        cJSON_AddItemToArray(pair, cJSON_CreateNumber(c->y[i]));
        cJSON_AddItemToArray(points, pair);
    }
    return obj;
}

/* A colour path's stops are of two kinds, and the compact [x, hue, sat] array
 * style has nowhere to put that. So a stop is an object and its kind is which
 * fields it carries: {"at":40,"kelvin":3300} or {"at":1,"hue":307,"sat":78}.
 * Self-describing, no "kind" string to keep in step, and what a GET emits can be
 * POSTed straight back - which is exactly what the editor does. */
static cJSON *chroma_to_json(const zbc_chroma_path_t *p)
{
    if (p->n == 0) {
        return cJSON_CreateNull();
    }
    cJSON *obj   = cJSON_CreateObject();
    cJSON *stops = cJSON_AddArrayToObject(obj, "stops");
    for (int i = 0; i < p->n; i++) {
        cJSON *stop = cJSON_CreateObject();
        cJSON_AddNumberToObject(stop, "at", p->x[i]);
        if (p->kind[i] == ZBC_STOP_KELVIN) {
            cJSON_AddNumberToObject(stop, "kelvin", p->kelvin[i]);
        } else {
            cJSON_AddNumberToObject(stop, "hue", p->hue[i]);
            cJSON_AddNumberToObject(stop, "sat", p->sat[i]);
        }
        cJSON_AddItemToArray(stops, stop);
    }
    return obj;
}

/* Reads {"points": [[x,y], ...]}. A missing or null member leaves the curve off.
 * Returns false only on a malformed member, so the caller can answer 400 rather
 * than quietly storing something the user did not ask for. */
static bool curve_from_json(const cJSON *obj, zbc_curve_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!obj || cJSON_IsNull(obj)) {
        return true;
    }
    const cJSON *points = cJSON_GetObjectItem(obj, "points");
    if (!cJSON_IsArray(points)) {
        return false;
    }
    int n = cJSON_GetArraySize(points);
    if (n < 2 || n > ZBC_CURVE_MAX_POINTS) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        const cJSON *pair = cJSON_GetArrayItem(points, i);
        if (!cJSON_IsArray(pair) || cJSON_GetArraySize(pair) != 2) {
            return false;
        }
        const cJSON *x = cJSON_GetArrayItem(pair, 0);
        const cJSON *y = cJSON_GetArrayItem(pair, 1);
        if (!cJSON_IsNumber(x) || !cJSON_IsNumber(y) ||
            x->valueint < 0 || x->valueint > 255 || y->valueint < 0 || y->valueint > 65535) {
            return false;
        }
        out->x[i] = (uint8_t)x->valueint;
        out->y[i] = (uint16_t)y->valueint;
    }
    out->n = (uint8_t)n;
    return true;
}

/* Reads {"stops": [{"at":1,"hue":307,"sat":78}, {"at":100,"kelvin":6000}, ...]}.
 *
 * Only the shape is checked here. The rules about where the stops may sit - two
 * to eight of them, strictly ascending, starting at 1 % and ending at 100 % -
 * stay in curve_db's valid_x, so the storage layer's invariant is enforced by the
 * storage layer and a hand-written POST is caught there whatever route it took. */
static bool chroma_from_json(const cJSON *obj, zbc_chroma_path_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!obj || cJSON_IsNull(obj)) {
        return true;
    }
    const cJSON *stops = cJSON_GetObjectItem(obj, "stops");
    if (!cJSON_IsArray(stops)) {
        return false;
    }
    int n = cJSON_GetArraySize(stops);
    if (n < 2 || n > ZBC_CHROMA_MAX_STOPS) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        const cJSON *stop = cJSON_GetArrayItem(stops, i);
        if (!cJSON_IsObject(stop)) {
            return false;
        }
        const cJSON *at = cJSON_GetObjectItem(stop, "at");
        const cJSON *k  = cJSON_GetObjectItem(stop, "kelvin");
        const cJSON *h  = cJSON_GetObjectItem(stop, "hue");
        const cJSON *sa = cJSON_GetObjectItem(stop, "sat");
        if (!cJSON_IsNumber(at) || at->valueint < 0 || at->valueint > 255) {
            return false;
        }
        out->x[i] = (uint8_t)at->valueint;

        const bool is_ct    = cJSON_IsNumber(k);
        const bool is_color = cJSON_IsNumber(h) && cJSON_IsNumber(sa);
        if (is_ct == is_color) {
            return false;              /* exactly one kind per stop, never both */
        }
        if (is_ct) {
            if (k->valueint < 1000 || k->valueint > 10000) {
                return false;
            }
            out->kind[i]   = ZBC_STOP_KELVIN;
            out->kelvin[i] = (uint16_t)k->valueint;
        } else {
            if (h->valueint < 0 || h->valueint > 359 ||
                sa->valueint < 0 || sa->valueint > 100) {
                return false;
            }
            out->kind[i] = ZBC_STOP_COLOR;
            out->hue[i]  = (uint16_t)h->valueint;
            out->sat[i]  = (uint8_t)sa->valueint;
        }
    }
    out->n = (uint8_t)n;
    return true;
}

/* GET /api/devices/{ieee}/ep/{ep}/curves */
static esp_err_t device_curves_get(httpd_req_t *req, const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    zbc_curveset_t cs;
    memset(&cs, 0, sizeof(cs));
    curve_db_get(ieee, ep, &cs);   /* absent is not an error: it means no curve */

    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "level", curve_to_json(&cs.level));
    cJSON_AddItemToObject(root, "chroma", chroma_to_json(&cs.chroma));
    if (cs.last_input_pct == ZBC_UNKNOWN || !cs.in_use) {
        cJSON_AddNullToObject(root, "input_pct");
    } else {
        cJSON_AddNumberToObject(root, "input_pct", cs.last_input_pct);
    }
    return send_json(req, root, "200 OK");
}

/* POST /api/devices/{ieee}/ep/{ep}/curves */
static esp_err_t device_curves_post(httpd_req_t *req, const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    zbc_device_t dev;
    if (!lighting_get_device(ieee, &dev) || !lighting_find_endpoint(&dev, ep)) {
        return send_error(req, "404 Not Found", "unknown device or endpoint");
    }

    cJSON *json = read_json_body(req);
    if (!json) {
        return ESP_FAIL;
    }
    /* Separate "color_temp" and "color" mappings are an older shape, since
     * replaced by one chroma path: refuse such a body with a 400 rather than
     * silently drop the curve. */
    const bool legacy = cJSON_GetObjectItem(json, "color_temp") ||
                        cJSON_GetObjectItem(json, "color");

    zbc_curveset_t cs;
    memset(&cs, 0, sizeof(cs));
    bool ok = !legacy &&
              curve_from_json(cJSON_GetObjectItem(json, "level"), &cs.level) &&
              chroma_from_json(cJSON_GetObjectItem(json, "chroma"), &cs.chroma);
    cJSON_Delete(json);
    if (legacy) {
        return send_error(req, "400 Bad Request",
                          "color_temp and color are now one path; send \"chroma\" instead");
    }
    if (!ok) {
        return send_error(req, "400 Bad Request", "malformed curve");
    }

    esp_err_t err = curve_db_set(ieee, ep, &cs);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_error(req, "400 Bad Request",
                          "points must be 2-6 (a path 2-8), ascending, from 1 % to 100 %");
    }
    if (err == ESP_ERR_NO_MEM) {
        return send_error(req, "507 Insufficient Storage", "no room for another curve");
    }
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "could not store the curve");
    }
    /* A new curve is somebody deciding what colour this light should be. */
    lighting_note_manual_color(ieee, ep);
    return send_ok(req, "200 OK");
}

/* Dispatches every /api/devices/... request after pulling out the IEEE. */
static esp_err_t device_dispatch(httpd_req_t *req)
{
    /* Anything that changes a light, so the log ties a lamp's behaviour back to
     * the request that asked for it. GETs are the poll and would drown it. */
    if (req->method != HTTP_GET) {
        ESP_LOGI(TAG, "<- %s", req->uri);
    }
    char ieee_str[24];
    if (!uri_segment(req->uri, 2, ieee_str, sizeof(ieee_str))) {
        return send_error(req, "400 Bad Request", "missing device address");
    }
    uint8_t ieee[ZBC_IEEE_LEN];
    if (!zbc_str_to_ieee(ieee_str, ieee)) {
        return send_error(req, "400 Bad Request", "malformed device address");
    }

    char tail[16];
    bool has_tail = uri_segment(req->uri, 5, tail, sizeof(tail));

    /* Every route with a tail is /api/devices/{ieee}/ep/{ep}/{tail}. */
    int ep = 0;
    if (has_tail) {
        char ep_str[8];
        if (!uri_segment(req->uri, 4, ep_str, sizeof(ep_str))) {
            return send_error(req, "400 Bad Request", "missing endpoint");
        }
        ep = atoi(ep_str);
        if (ep <= 0 || ep > 240) {
            return send_error(req, "400 Bad Request", "endpoint out of range");
        }
    }

    if (req->method == HTTP_POST && has_tail) {
        if (strcmp(tail, "onoff") == 0) {
            /* A wired light with no pin cannot switch for want of a pin, not for
             * anything the Zigbee stack did - send_command_result would say 502. */
            const uint16_t light_id = zbc_light_id_from_ieee(ieee);
            if (light_id && light_db_get(light_id, NULL) && local_light_state(light_id) < 0) {
                return send_error(req, "409 Conflict", "this light has no pin yet - pick one under Wiring");
            }
            return device_onoff_post(req, ieee, (uint8_t)ep);
        }
        if (strcmp(tail, "level") == 0) {
            return device_level_post(req, ieee, (uint8_t)ep);
        }
        if (strcmp(tail, "color_temp") == 0) {
            return device_color_temp_post(req, ieee, (uint8_t)ep);
        }
        if (strcmp(tail, "color") == 0) {
            return device_color_post(req, ieee, (uint8_t)ep);
        }
        if (strcmp(tail, "curves") == 0) {
            return device_curves_post(req, ieee, (uint8_t)ep);
        }
    }
    if (req->method == HTTP_GET && has_tail && strcmp(tail, "curves") == 0) {
        return device_curves_get(req, ieee, (uint8_t)ep);
    }
    if (req->method == HTTP_DELETE && has_tail && strcmp(tail, "curves") == 0) {
        curve_db_clear(ieee, (uint8_t)ep);   /* absent is already the desired state */
        return send_ok(req, "200 OK");
    }
    if (req->method == HTTP_PATCH && !has_tail) {
        return device_patch(req, ieee);
    }
    if (req->method == HTTP_DELETE && !has_tail) {
        return device_delete(req, ieee);
    }
    return send_error(req, "404 Not Found", "no such device route");
}

/* ---------------------------------------------------------------- groups */

/* A group is a coordinator-side list of (device, endpoint) pairs. Commands are
 * fanned out over the ordinary per-device routes rather than sent as a ZCL
 * group-cast, so nothing has to be re-applied to a lamp that is reset and
 * rejoins - see CLAUDE.md. */

/* zbc_al_expose_t on the wire. */
static const char *const AL_EXPOSE_NAMES[] = { "off", "mode_select", "endpoint" };
/* zbc_turn_on_t, in order. */
static const char *const TURN_ON_NAMES[] = { "last", "adaptive", "full" };
_Static_assert(sizeof(TURN_ON_NAMES) / sizeof(TURN_ON_NAMES[0]) == ZBC_TURN_ON_COUNT,
               "a name for every zbc_turn_on_t");

static cJSON *group_to_json(const zbc_group_t *g, int64_t now)
{
    cJSON *jg = cJSON_CreateObject();
    cJSON_AddNumberToObject(jg, "id", g->id);
    cJSON_AddStringToObject(jg, "name", g->name);

    cJSON *members = cJSON_CreateArray();
    int known = 0, on_count = 0;
    for (int m = 0; m < g->member_count; m++) {
        char ieee[19];
        zbc_ieee_to_str(g->members[m].ieee, ieee);

        cJSON *jm = cJSON_CreateObject();
        cJSON_AddStringToObject(jm, "ieee", ieee);
        cJSON_AddNumberToObject(jm, "ep", g->members[m].ep);

        zbc_device_t          dev;
        const zbc_endpoint_t *e = NULL;
        if (lighting_get_device(g->members[m].ieee, &dev)) {
            e = lighting_find_endpoint(&dev, g->members[m].ep);
            bool fresh = dev.last_seen_us > 0 && (now - dev.last_seen_us) < OFFLINE_AFTER_US;
            cJSON_AddStringToObject(jm, "name", dev.name);
            cJSON_AddBoolToObject(jm, "online", dev.online && fresh);
        } else {
            /* Unpairing drops members, so this only happens in a race. */
            cJSON_AddStringToObject(jm, "name", ieee);
            cJSON_AddBoolToObject(jm, "online", false);
        }

        cJSON_AddBoolToObject(jm, "brightness", e && (e->features & ZBC_FEAT_LEVEL));
        if (!e || e->on_off == ZBC_UNKNOWN) {
            cJSON_AddNullToObject(jm, "on");
        } else {
            cJSON_AddBoolToObject(jm, "on", e->on_off == 1);
            known++;
            on_count += e->on_off == 1;
        }
        if (!e || e->level == ZBC_UNKNOWN) {
            cJSON_AddNullToObject(jm, "brightness_pct");
        } else {
            cJSON_AddNumberToObject(jm, "brightness_pct", zbc_level_to_pct(e->level));
        }
        cJSON_AddItemToArray(members, jm);
    }
    cJSON_AddItemToObject(jg, "members", members);

    /* On/off is derived from the lamps, so a light switched elsewhere shows up. */
    if (known == 0) {
        cJSON_AddNullToObject(jg, "on");
    } else {
        cJSON_AddBoolToObject(jg, "on", on_count > 0);
    }
    /* Brightness is the value the group was last commanded to, not an average of
     * its members: lamps settle a percent or two apart, and an average would
     * never agree with what the slider asked for. */
    if (g->last_pct == ZBC_UNKNOWN) {
        /* Nothing commanded since the last reboot, or since a sweep ended and gave
         * up its aim. The brightest lit member is then the best answer - still
         * not an average, so the slider's exact-equality reconciliation keeps
         * working, and once anything is commanded last_pct takes over again. */
        int derived = -1;
        lighting_group_state(g->id, NULL, &derived);
        if (derived < 0) {
            cJSON_AddNullToObject(jg, "brightness_pct");
        } else {
            cJSON_AddNumberToObject(jg, "brightness_pct", derived);
        }
    } else {
        cJSON_AddNumberToObject(jg, "brightness_pct", g->last_pct);
    }

    /* What a Matter controller's "turn on" does to this room when it is dark. */
    cJSON_AddStringToObject(jg, "turn_on", TURN_ON_NAMES[al_db_turn_on(g->id)]);

    lighting_al_status_t al;
    if (lighting_group_al_status(g->id, &al)) {
        cJSON *ja = cJSON_AddObjectToObject(jg, "adaptive");
        cJSON_AddStringToObject(ja, "expose", al.expose < ZBC_AL_EXPOSE_COUNT ? AL_EXPOSE_NAMES[al.expose] : "off");
        cJSON_AddBoolToObject(ja, "active", al.active);
        cJSON_AddStringToObject(ja, "reason", al.reason);
        /* What the sun says now, shown before anything is switched on - null
         * until there is a clock and a location to work it out from. */
        if (al.pct < 0) {
            cJSON_AddNullToObject(ja, "pct");
            cJSON_AddNullToObject(ja, "offset_k");
        } else {
            cJSON_AddNumberToObject(ja, "pct", al.pct);
            cJSON_AddNumberToObject(ja, "offset_k", al.offset_k);
        }
    }
    return jg;
}

/* GET /api/groups */
/* False when the table could not be copied; nothing was written then. */
static bool stream_groups(json_stream_t *js)
{
    zbc_group_t *groups = calloc(CONFIG_ZBC_MAX_GROUPS, sizeof(zbc_group_t));
    if (!groups) {
        return false;
    }
    const size_t  n     = group_db_snapshot(groups, CONFIG_ZBC_MAX_GROUPS);
    const int64_t now   = esp_timer_get_time();
    bool          first = true;
    json_stream_raw(js, "[");
    for (size_t i = 0; i < n; i++) {
        json_stream_element(js, &first, group_to_json(&groups[i], now));
    }
    json_stream_raw(js, "]");
    free(groups);
    return true;
}

static esp_err_t groups_get(httpd_req_t *req)
{
    json_stream_t js;
    if (!json_stream_begin(&js, req)) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    json_stream_raw(&js, "{\"groups\":");
    if (!stream_groups(&js)) {
        return json_stream_fail(&js);
    }
    json_stream_raw(&js, "}");
    return json_stream_end(&js);
}

/* --------------------------------------------------------------- GPIO buttons */

/* Names rather than enum numbers on the wire: a button's configuration is the
 * one thing here somebody will read with curl and want to understand. */
static const char *const COND_NAMES[]   = { "always", "off", "on", "below", "above" };
static const char *const ACTION_NAMES[] = { "none", "on", "off", "toggle", "set",
                                            "dim_up", "dim_down", "dim_cycle", "adaptive" };
static const char *const EVENT_NAMES[]  = { "short", "long", "double", "triple" };
_Static_assert(sizeof(EVENT_NAMES) / sizeof(EVENT_NAMES[0]) == ZBC_EV_COUNT,
               "one JSON name per gesture, in button_db.h order");
static const char *const PULL_NAMES[]   = { "none", "up", "down" };
static const char *const INPUT_NAMES[]  = { "button", "switch" };

static int name_index(const char *const *names, int n, const cJSON *item)
{
    if (!cJSON_IsString(item)) {
        return -1;
    }
    for (int i = 0; i < n; i++) {
        if (strcmp(names[i], item->valuestring) == 0) {
            return i;
        }
    }
    return -1;
}

static cJSON *button_to_json(const zbc_button_t *b)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "id", b->id);
    cJSON_AddStringToObject(obj, "name", b->name);
    cJSON_AddStringToObject(obj, "source", b->src == ZBC_BTN_SRC_ZIGBEE ? "zigbee" : "gpio");
    if (b->src == ZBC_BTN_SRC_ZIGBEE && button_db_has_source(b)) {
        char ieee[19];
        zbc_ieee_to_str(b->ieee, ieee);
        cJSON_AddStringToObject(obj, "ieee", ieee);
        cJSON_AddNumberToObject(obj, "ep", b->zb_ep);
        cJSON_AddNumberToObject(obj, "key", b->zb_key);
    } else {
        cJSON_AddNullToObject(obj, "ieee");
        cJSON_AddNullToObject(obj, "ep");
        cJSON_AddNullToObject(obj, "key");
    }
    if (b->gpio < 0) {
        cJSON_AddNullToObject(obj, "gpio");
    } else {
        cJSON_AddNumberToObject(obj, "gpio", b->gpio);
    }
    cJSON_AddBoolToObject(obj, "inverted", b->inverted);
    cJSON_AddStringToObject(obj, "pull", PULL_NAMES[b->pull <= ZBC_PULL_DOWN ? b->pull : 0]);
    cJSON_AddStringToObject(obj, "input", INPUT_NAMES[b->input < ZBC_INPUT_COUNT ? b->input : 0]);

    /* The live reading: how a wiring mistake shows on the page. */
    bool pressed = false;
    int  level   = 0;
    if (buttons_pin_state(b->id, &pressed, &level)) {
        cJSON_AddBoolToObject(obj, "pressed", pressed);
        cJSON_AddNumberToObject(obj, "level", level);
    } else {
        cJSON_AddNullToObject(obj, "pressed");
        cJSON_AddNullToObject(obj, "level");
    }
    /* The same job for a remote: "nobody has pressed it yet" and "it is pointed
     * at the wrong key" are otherwise the same picture. */
    const int64_t ago = buttons_last_event_ms(b->id);
    if (ago >= 0) {
        cJSON_AddNumberToObject(obj, "last_event_ms", (double)ago);
    } else {
        cJSON_AddNullToObject(obj, "last_event_ms");
    }

    cJSON *events = cJSON_AddObjectToObject(obj, "events");
    for (int e = 0; e < ZBC_EV_COUNT; e++) {
        cJSON *rules = cJSON_AddArrayToObject(events, EVENT_NAMES[e]);
        int n = 0;
        const zbc_btn_rule_t *list = button_db_rules((zbc_button_t *)b, e, &n);
        for (int r = 0; r < n; r++) {
            const zbc_btn_rule_t *rule = &list[r];
            cJSON *ro = cJSON_CreateObject();
            cJSON_AddNumberToObject(ro, "group", rule->group_id);
            if (rule->ep != 0) {
                char ieee[19];
                zbc_ieee_to_str(rule->ieee, ieee);
                cJSON_AddStringToObject(ro, "ieee", ieee);
                cJSON_AddNumberToObject(ro, "ep", rule->ep);
            } else {
                cJSON_AddNullToObject(ro, "ieee");
                cJSON_AddNullToObject(ro, "ep");
            }
            cJSON_AddBoolToObject(ro, "and", (rule->flags & ZBC_RULE_AND) != 0);
            cJSON_AddStringToObject(ro, "when", COND_NAMES[rule->cond]);
            cJSON_AddNumberToObject(ro, "arg", rule->cond_arg);
            cJSON_AddStringToObject(ro, "do", ACTION_NAMES[rule->action]);
            cJSON_AddNumberToObject(ro, "value", rule->action_arg);
            cJSON_AddItemToArray(rules, ro);
        }
    }
    return obj;
}

/* False when the table could not be copied; nothing was written then. One
 * button at a time, as the devices are: the whole table is about 4 KB, and
 * this runs inside the deepest request the page makes. */
static bool stream_buttons(json_stream_t *js)
{
    uint16_t     ids[CONFIG_ZBC_MAX_BUTTONS];
    const size_t n = button_db_list_ids(ids, CONFIG_ZBC_MAX_BUTTONS);
    zbc_button_t *b = malloc(sizeof(*b));
    if (!b) {
        return false;
    }
    bool first = true;
    json_stream_raw(js, "[");
    for (size_t i = 0; i < n; i++) {
        /* Deleted since the ids were taken: left out, as it would be a moment later. */
        if (button_db_get(ids[i], b)) {
            json_stream_element(js, &first, button_to_json(b));
        }
    }
    json_stream_raw(js, "]");
    free(b);
    return true;
}

static esp_err_t buttons_get(httpd_req_t *req)
{
    json_stream_t js;
    if (!json_stream_begin(&js, req)) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    json_stream_raw(&js, "{\"buttons\":");
    if (!stream_buttons(&js)) {
        return json_stream_fail(&js);
    }
    json_stream_raw(&js, "}");
    return json_stream_end(&js);
}

/* POST /api/buttons  {"name": "Kitchen switch"} */
static esp_err_t buttons_post(httpd_req_t *req)
{
    cJSON *json = read_json_body(req);
    if (!json) {
        return ESP_FAIL;
    }
    const char *name = name_from_json(json);
    if (!name) {
        cJSON_Delete(json);
        return send_error(req, "400 Bad Request", "name is required");
    }
    uint16_t  id  = 0;
    esp_err_t err = button_db_create(name, &id);
    cJSON_Delete(json);

    if (err == ESP_ERR_NO_MEM) {
        return send_error(req, "507 Insufficient Storage", "no room for another button");
    }
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "could not create the button");
    }

    zbc_button_t b;
    if (!button_db_get(id, &b)) {
        return send_error(req, "500 Internal Server Error", "button vanished");
    }
    buttons_reload();
    return send_json(req, button_to_json(&b), "201 Created");
}

/* One rule from the request body. `first` says it opens its gesture, which has
 * nothing before it to join. Returns a reason on refusal, NULL on success. */
static const char *rule_from_json(const cJSON *item, zbc_btn_rule_t *r, bool first)
{
    if (!cJSON_IsObject(item)) {
        return "a rule has to be an object";
    }
    memset(r, 0, sizeof(*r));

    const cJSON *group = cJSON_GetObjectItem(item, "group");
    r->group_id = cJSON_IsNumber(group) && group->valueint > 0 ? (uint16_t)group->valueint : 0;

    /* One light instead of a room, named the way the Lights page names it. */
    const cJSON *jieee = cJSON_GetObjectItem(item, "ieee");
    if (jieee && !cJSON_IsNull(jieee)) {
        const cJSON *jep = cJSON_GetObjectItem(item, "ep");
        uint8_t      ieee[ZBC_IEEE_LEN];
        if (r->group_id != 0) {
            return "a rule acts on a room or on one light, not both";
        }
        if (!cJSON_IsString(jieee) || !zbc_str_to_ieee(jieee->valuestring, ieee)) {
            return "ieee has to be a 0x-prefixed 64-bit address";
        }
        if (!cJSON_IsNumber(jep) || jep->valueint < 1 || jep->valueint > 240) {
            return "ep has to be an endpoint number";
        }
        memcpy(r->ieee, ieee, ZBC_BTN_IEEE_LEN);
        r->ep = (uint8_t)jep->valueint;
    }

    const int cond = name_index(COND_NAMES, ZBC_COND_COUNT, cJSON_GetObjectItem(item, "when"));
    const int act  = name_index(ACTION_NAMES, ZBC_ACT_COUNT, cJSON_GetObjectItem(item, "do"));
    if (cond < 0) {
        return "when must be always, on, off, below or above";
    }
    if (act < 0) {
        return "do must be one of none, on, off, toggle, set, dim_up, dim_down, dim_cycle, adaptive";
    }
    r->cond   = (uint8_t)cond;
    r->action = (uint8_t)act;

    if (cJSON_IsTrue(cJSON_GetObjectItem(item, "and"))) {
        if (first) {
            return "the first rule of a gesture has nothing to join with \"and\"";
        }
        r->flags |= ZBC_RULE_AND;
    }

    const cJSON *arg = cJSON_GetObjectItem(item, "arg");
    const cJSON *val = cJSON_GetObjectItem(item, "value");
    const int argv = cJSON_IsNumber(arg) ? arg->valueint : 0;
    const int valv = cJSON_IsNumber(val) ? val->valueint : 0;
    if (argv < 0 || argv > 100 || valv < 0 || valv > 100) {
        return "percentages run from 0 to 100";
    }
    r->cond_arg   = (uint8_t)argv;
    r->action_arg = (uint8_t)valv;

    /* A rule that points at a room or a light nobody can name would be a button
     * that quietly does nothing. Say so now rather than after it is wired to a
     * wall. A remote's endpoint is no light: it has nothing to switch. */
    if (r->group_id != 0 && !group_db_get(r->group_id, NULL)) {
        return "that rule names a room that does not exist";
    }
    if (r->ep != 0) {
        zbc_device_t          dev;
        const zbc_endpoint_t *e = lighting_get_device(r->ieee, &dev) ? lighting_find_endpoint(&dev, r->ep)
                                                                     : NULL;
        if (!e || !(e->features & ZBC_FEAT_ONOFF) || (e->features & ZBC_FEAT_SWITCH)) {
            return "that rule names a light that does not exist";
        }
        if (r->action == ZBC_ACT_ADAPTIVE) {
            return "Adaptive Lighting is a mode of a room, not of one light";
        }
    }
    return NULL;
}

/* Fills `out` from the request body, leaving whatever the body does not mention
 * as it was. Returns a reason on refusal, NULL on success. */
static const char *button_from_json(const cJSON *json, zbc_button_t *out, uint16_t id)
{
    const cJSON *name = cJSON_GetObjectItem(json, "name");
    if (cJSON_IsString(name)) {
        if (name->valuestring[0] == '\0') {
            return "a button needs a name";
        }
        strlcpy(out->name, name->valuestring, sizeof(out->name));
    }

    /* The source decides which half of the record means anything, so it is read
     * first and the other half is zeroed - a button switched from a pin to a
     * remote must not keep claiming the pin. */
    const cJSON *source = cJSON_GetObjectItem(json, "source");
    if (source) {
        if (!cJSON_IsString(source)) {
            return "source must be gpio or zigbee";
        }
        if (strcmp(source->valuestring, "zigbee") == 0) {
            if (out->src != ZBC_BTN_SRC_ZIGBEE) {
                out->src      = ZBC_BTN_SRC_ZIGBEE;
                out->gpio     = -1;
                out->inverted = false;
                out->pull     = ZBC_PULL_NONE;
            }
        } else if (strcmp(source->valuestring, "gpio") == 0) {
            if (out->src != ZBC_BTN_SRC_GPIO) {
                out->src   = ZBC_BTN_SRC_GPIO;
                out->gpio  = -1;
                out->pull  = ZBC_PULL_UP;
                out->inverted = true;
                memset(out->ieee, 0, ZBC_BTN_IEEE_LEN);
                out->zb_ep  = 0;
                out->zb_key = 0;
            }
        } else {
            return "source must be gpio or zigbee";
        }
    }

    if (out->src == ZBC_BTN_SRC_ZIGBEE) {
        /* All three arrive together or not at all: they are one identity, and
         * half of one would point at a key nobody ever pressed. */
        const cJSON *jieee = cJSON_GetObjectItem(json, "ieee");
        const cJSON *jep   = cJSON_GetObjectItem(json, "ep");
        const cJSON *jkey  = cJSON_GetObjectItem(json, "key");
        if (jieee && cJSON_IsNull(jieee)) {
            memset(out->ieee, 0, ZBC_BTN_IEEE_LEN);
            out->zb_ep  = 0;
            out->zb_key = 0;
        } else if (jieee || jep || jkey) {
            uint8_t ieee[ZBC_IEEE_LEN];
            if (!cJSON_IsString(jieee) || !zbc_str_to_ieee(jieee->valuestring, ieee)) {
                return "ieee has to be a 0x-prefixed 64-bit address";
            }
            if (!cJSON_IsNumber(jep) || jep->valueint < 1 || jep->valueint > 240) {
                return "ep has to be an endpoint number";
            }
            const int key = cJSON_IsNumber(jkey) ? jkey->valueint : 0;
            if (key < 0 || key >= ZB_REMOTE_KEY_MAX) {
                return "key is out of range";
            }
            memcpy(out->ieee, ieee, ZBC_BTN_IEEE_LEN);
            out->zb_ep  = (uint8_t)jep->valueint;
            out->zb_key = (uint8_t)key;
            if (!device_db_get(ieee, NULL)) {
                return "that button names a remote that is not paired";
            }
            if (button_db_key_taken(out->ieee, out->zb_ep, out->zb_key, id)) {
                return "another button already uses that key";
            }
        }
        /* The pin fields are meaningless now, and valid_button says so. */
        out->gpio = -1;
    } else {
        const cJSON *gpio = cJSON_GetObjectItem(json, "gpio");
        if (cJSON_IsNull(gpio)) {
            out->gpio = -1;
        } else if (cJSON_IsNumber(gpio)) {
            const int pin = gpio->valueint;
            const char *why = buttons_pin_problem(pin);
            if (why) {
                return why;
            }
            zbc_thermal_cfg_t th;
            settings_thermal(&th);
            if (pin >= 0 && pin == th.gpio) {
                return "that pin reads the overheat sensor";
            }
            out->gpio = (int8_t)pin;
        }

        const cJSON *inverted = cJSON_GetObjectItem(json, "inverted");
        if (cJSON_IsBool(inverted)) {
            out->inverted = cJSON_IsTrue(inverted);
        }

        const cJSON *pull = cJSON_GetObjectItem(json, "pull");
        if (pull) {
            const int p = name_index(PULL_NAMES, 3, pull);
            if (p < 0) {
                return "pull must be none, up or down";
            }
            out->pull = (uint8_t)p;
        }
        const cJSON *input = cJSON_GetObjectItem(json, "input");
        if (input) {
            const int k = name_index(INPUT_NAMES, ZBC_INPUT_COUNT, input);
            if (k < 0) {
                return "input must be button or switch";
            }
            out->input = (uint8_t)k;
        }
    }

    const cJSON *events = cJSON_GetObjectItem(json, "events");
    if (cJSON_IsObject(events)) {
        /* The pool is packed again gesture by gesture, in order: a gesture the
         * body leaves out keeps its rules, copied from the record as it stood. */
        zbc_button_t was = *out;
        int          at  = 0;
        for (int e = 0; e < ZBC_EV_COUNT; e++) {
            const cJSON *list = cJSON_GetObjectItem(events, EVENT_NAMES[e]);
            if (!list) {
                int n = 0;
                const zbc_btn_rule_t *keep = button_db_rules(&was, e, &n);
                if (at + n > CONFIG_ZBC_BUTTON_RULES) {
                    return "too many rules on one button";
                }
                memcpy(&out->rules[at], keep, (size_t)n * sizeof(*keep));
                out->rule_count[e] = (uint8_t)n;
                at += n;
                continue;
            }
            if (!cJSON_IsArray(list)) {
                return "each event is a list of rules";
            }
            if (at + cJSON_GetArraySize(list) > CONFIG_ZBC_BUTTON_RULES) {
                return "too many rules on one button";
            }
            out->rule_count[e] = 0;

            int          sweeps = 0;       /* dimming rules in the case being read */
            const cJSON *item   = NULL;
            cJSON_ArrayForEach(item, list) {
                zbc_btn_rule_t *r   = &out->rules[at];
                const char     *why = rule_from_json(item, r, out->rule_count[e] == 0);
                if (why) {
                    return why;
                }
                if (!(r->flags & ZBC_RULE_AND)) {
                    sweeps = 0;
                }
                if (e == ZBC_EV_LONG && (r->action == ZBC_ACT_DIM_UP || r->action == ZBC_ACT_DIM_DOWN ||
                                         r->action == ZBC_ACT_DIM_CYCLE) &&
                    ++sweeps > ZBC_BTN_MAX_SWEEPS) {
                    return "one long press can dim at most four rooms or lights";
                }
                out->rule_count[e]++;
                at++;
            }
        }
        /* Nothing stale past the counts: the record is stored whole. */
        memset(&out->rules[at], 0, (size_t)(CONFIG_ZBC_BUTTON_RULES - at) * sizeof(out->rules[0]));
    }

    if (out->src == ZBC_BTN_SRC_GPIO && button_db_pin_taken(out->gpio, id)) {
        return "another button already uses that pin";
    }
    /* A button would read a pin a light is driving - each registry refuses the
     * other's pins, see light_from_json. */
    if (out->src == ZBC_BTN_SRC_GPIO && light_db_pin_taken(out->gpio, 0)) {
        return "a wired light already uses that pin";
    }
    return NULL;
}

/* PATCH /api/buttons/{id} - the whole record. The editor saves everything at
 * once, so there is nothing to be gained from a field at a time. */
static esp_err_t button_patch(httpd_req_t *req, uint16_t id)
{
    zbc_button_t b;
    if (!button_db_get(id, &b)) {
        return send_error(req, "404 Not Found", "unknown button");
    }

    cJSON *json = read_json_body(req);
    if (!json) {
        return ESP_FAIL;
    }
    const char *why = button_from_json(json, &b, id);
    cJSON_Delete(json);

    if (why) {
        return send_error(req, strstr(why, "already uses") ? "409 Conflict" : "400 Bad Request", why);
    }

    esp_err_t err = button_db_set(id, &b);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_error(req, "400 Bad Request", "that button configuration does not make sense");
    }
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "could not store the button");
    }

    /* Re-read the pins now rather than at the next reboot: someone changing a
     * pin is almost certainly standing over the board with a wire in hand. */
    buttons_reload();
    return send_json(req, button_to_json(&b), "200 OK");
}

/* POST /api/buttons/{id}/test  {"event": "short"|"long"|"double"|"triple"} */
static esp_err_t button_test_post(httpd_req_t *req, uint16_t id)
{
    cJSON *json = NULL;
    if (req->content_len > 0 && !(json = read_json_body(req))) {
        return ESP_FAIL;                   /* answered already; nothing is fired */
    }
    int ev = ZBC_EV_SHORT, hold_ms = 0;
    if (json) {
        ev = name_index(EVENT_NAMES, ZBC_EV_COUNT, cJSON_GetObjectItem(json, "event"));
        /* How long to hold, for a long press. The sweep and the stop that ends
         * it are the whole of what a long press does, so a test that does not
         * hold tests none of it. */
        const cJSON *hold = cJSON_GetObjectItem(json, "hold_ms");
        if (cJSON_IsNumber(hold)) {
            hold_ms = hold->valueint;
        }
        cJSON_Delete(json);
        if (ev < 0) {
            return send_error(req, "400 Bad Request", "event must be short, long or double");
        }
    }
    esp_err_t err = buttons_fire(id, (zbc_btn_event_t)ev, hold_ms);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_error(req, "404 Not Found", "unknown button");
    }
    if (err != ESP_OK) {
        return send_error(req, "503 Service Unavailable", "the button task is not ready");
    }
    return send_ok(req, "202 Accepted");
}

static esp_err_t button_dispatch(httpd_req_t *req)
{
    if (req->method != HTTP_GET) {
        ESP_LOGI(TAG, "<- %s", req->uri);
    }
    char id_str[8];
    if (!uri_segment(req->uri, 2, id_str, sizeof(id_str))) {
        if (req->method == HTTP_GET) {
            return buttons_get(req);
        }
        if (req->method == HTTP_POST) {
            return buttons_post(req);
        }
        return send_error(req, "404 Not Found", "no such button route");
    }
    int id = atoi(id_str);
    if (id <= 0 || id > 0xffff) {
        return send_error(req, "400 Bad Request", "malformed button id");
    }

    char tail[16];
    bool has_tail = uri_segment(req->uri, 3, tail, sizeof(tail));

    if (req->method == HTTP_POST && has_tail && strcmp(tail, "test") == 0) {
        return button_test_post(req, (uint16_t)id);
    }
    if (req->method == HTTP_PATCH && !has_tail) {
        return button_patch(req, (uint16_t)id);
    }
    if (req->method == HTTP_DELETE && !has_tail) {
        if (button_db_remove((uint16_t)id) == ESP_ERR_NOT_FOUND) {
            return send_error(req, "404 Not Found", "unknown button");
        }
        buttons_reload();                  /* hands the pin back */
        return send_ok(req, "200 OK");
    }
    return send_error(req, "404 Not Found", "no such button route");
}

/* --------------------------------------------------------------- wired lights */

/* A wired light is created and wired here, and otherwise used through the
 * ordinary device routes under its made-up address - switching, curves, group
 * membership, rename and delete. See light_db.h. */

/* POST /api/lights  {"name": "Porch"} -> {"id", "ieee", "ep"} */
static esp_err_t lights_post(httpd_req_t *req)
{
    cJSON *json = read_json_body(req);
    if (!json) {
        return ESP_FAIL;
    }
    const char *name = name_from_json(json);
    if (!name) {
        cJSON_Delete(json);
        return send_error(req, "400 Bad Request", "name is required");
    }
    uint16_t  id  = 0;
    esp_err_t err = light_db_create(name, &id);
    cJSON_Delete(json);

    if (err == ESP_ERR_NO_MEM) {
        return send_error(req, "507 Insufficient Storage", "no room for another wired light");
    }
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "could not create the light");
    }

    uint8_t ieee[ZBC_IEEE_LEN];
    char    ieee_str[19];
    zbc_light_ieee(id, ieee);
    zbc_ieee_to_str(ieee, ieee_str);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddStringToObject(root, "ieee", ieee_str);
    cJSON_AddNumberToObject(root, "ep", ZBC_LIGHT_EP);
    return send_json(req, root, "201 Created");
}

/* The index of a string among `names`, or -1. */
static int light_name_index(const cJSON *item, const char *const names[], int count)
{
    for (int i = 0; cJSON_IsString(item) && i < count; i++) {
        if (strcmp(item->valuestring, names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

/* Why the chip could not run every PWM light once `cand` is stored for light
 * `id`, or NULL. The same plan local_lights_reload runs, so nothing is accepted
 * here that would then sit dark. */
static const char *pwm_plan_problem(const zbc_light_t *cand, uint16_t id)
{
    zbc_light_t    all[CONFIG_ZBC_MAX_LIGHTS];
    const size_t   n                            = light_db_snapshot(all, CONFIG_ZBC_MAX_LIGHTS);
    uint16_t       hz[CONFIG_ZBC_MAX_LIGHTS]    = { 0 };
    uint8_t        chans[CONFIG_ZBC_MAX_LIGHTS] = { 0 };
    zbc_pwm_slot_t slot[CONFIG_ZBC_MAX_LIGHTS];
    uint16_t       timer_hz[ZBC_PWM_TIMERS];
    for (size_t i = 0; i < n; i++) {
        const zbc_light_t *l  = all[i].id == id ? cand : &all[i];
        const uint8_t      ch = zbc_light_pwm_channels(l->kind);
        /* Only a light with every pin chosen takes its channels, as in reload. */
        if (ch && l->gpio >= 0 && (ch < 2 || l->gpio_warm >= 0) && (ch < 3 || l->gpio_neutral >= 0)) {
            hz[i]    = l->pwm_hz;
            chans[i] = ch;
        }
    }
    return zbc_pwm_plan(hz, chans, n, slot, timer_hz);
}

/* A kelvin field, if present. Returns a reason on refusal. */
static const char *kelvin_from_json(const cJSON *json, const char *field, uint16_t *out)
{
    const cJSON *k = cJSON_GetObjectItem(json, field);
    if (!k) {
        return NULL;
    }
    if (!cJSON_IsNumber(k) || k->valueint < ZBC_LIGHT_KELVIN_MIN || k->valueint > ZBC_LIGHT_KELVIN_MAX) {
        return "a colour temperature must be 1000-10000 K";
    }
    *out = (uint16_t)k->valueint;
    return NULL;
}

/* Fills `out` from the request body, leaving whatever the body does not mention
 * as it was. Returns a reason on refusal, NULL on success. */
static const char *light_from_json(const cJSON *json, zbc_light_t *out, uint16_t id)
{
    const cJSON *name = cJSON_GetObjectItem(json, "name");
    if (cJSON_IsString(name)) {
        if (name->valuestring[0] == '\0') {
            return "a light needs a name";
        }
        strlcpy(out->name, name->valuestring, sizeof(out->name));
    }

    const cJSON *gpio = cJSON_GetObjectItem(json, "gpio");
    if (cJSON_IsNull(gpio)) {
        out->gpio = -1;
    } else if (cJSON_IsNumber(gpio)) {
        const char *why = local_light_pin_problem(gpio->valueint);
        if (why) {
            return why;
        }
        out->gpio = (int8_t)gpio->valueint;
    } else if (gpio) {
        return "gpio must be a pin number or null";
    }

    const cJSON *low = cJSON_GetObjectItem(json, "active_low");
    if (cJSON_IsBool(low)) {
        out->active_low = cJSON_IsTrue(low);
    }

    const cJSON *boot = cJSON_GetObjectItem(json, "power_on");
    if (boot) {
        const int i = light_name_index(boot, LIGHT_BOOT_NAMES, ZBC_LIGHT_BOOT_COUNT);
        if (i < 0) {
            return "power_on must be off, on or last";
        }
        out->boot = (uint8_t)i;
    }

    const cJSON *kind = cJSON_GetObjectItem(json, "kind");
    if (kind) {
        const int i = light_name_index(kind, LIGHT_KIND_NAMES, ZBC_LIGHT_KIND_COUNT);
        if (i < 0) {
            return "kind must be onoff, dim, cwww or cwnwww";
        }
        out->kind = (uint8_t)i;
    }

    const cJSON *warm = cJSON_GetObjectItem(json, "gpio_warm");
    if (cJSON_IsNull(warm)) {
        out->gpio_warm = -1;
    } else if (cJSON_IsNumber(warm)) {
        const char *why = local_light_pin_problem(warm->valueint);
        if (why) {
            return why;
        }
        out->gpio_warm = (int8_t)warm->valueint;
    } else if (warm) {
        return "gpio_warm must be a pin number or null";
    }

    const cJSON *neutral = cJSON_GetObjectItem(json, "gpio_neutral");
    if (cJSON_IsNull(neutral)) {
        out->gpio_neutral = -1;
    } else if (cJSON_IsNumber(neutral)) {
        const char *why = local_light_pin_problem(neutral->valueint);
        if (why) {
            return why;
        }
        out->gpio_neutral = (int8_t)neutral->valueint;
    } else if (neutral) {
        return "gpio_neutral must be a pin number or null";
    }

    const cJSON *hz = cJSON_GetObjectItem(json, "pwm_hz");
    if (hz) {
        if (!cJSON_IsNumber(hz) || hz->valueint < (int)ZBC_PWM_HZ_MIN || hz->valueint > (int)ZBC_PWM_HZ_MAX) {
            return "pwm_hz must be 100-19531";
        }
        out->pwm_hz = (uint16_t)hz->valueint;
    }

    const char *kwhy = kelvin_from_json(json, "cold_kelvin", &out->cold_k);
    if (!kwhy) {
        kwhy = kelvin_from_json(json, "warm_kelvin", &out->warm_k);
    }
    if (!kwhy) {
        kwhy = kelvin_from_json(json, "neutral_kelvin", &out->neutral_k);
    }
    if (kwhy) {
        return kwhy;
    }
    if (out->warm_k >= out->cold_k) {
        return "the warm white has to be warmer - fewer kelvin - than the cold white";
    }
    const bool three = out->kind == ZBC_LIGHT_CNW;
    if (three && (out->neutral_k <= out->warm_k || out->neutral_k >= out->cold_k)) {
        return "the neutral white has to lie between the warm and the cold white";
    }

    const cJSON *mix = cJSON_GetObjectItem(json, "mix");
    if (mix) {
        const int i = light_name_index(mix, LIGHT_MIX_NAMES, ZBC_MIX_COUNT);
        if (i < 0) {
            return "mix must be constant, full or custom";
        }
        out->mix = (uint8_t)i;
    }

    /* {"cold": {"points": [[x, y], ...]} | null, "neutral": ..., "warm": ...};
     * a channel left out keeps its curve. */
    const cJSON *curves = cJSON_GetObjectItem(json, "mix_curves");
    if (curves && !cJSON_IsObject(curves)) {
        return "mix_curves must be an object of cold, neutral and warm";
    }
    for (int c = 0; curves && c < ZBC_PWM_MAX_LIGHT_CH; c++) {
        const cJSON *item = cJSON_GetObjectItem(curves, LIGHT_CH_NAMES[c]);
        if (!item) {
            continue;
        }
        zbc_curve_t     parsed;
        zbc_mix_curve_t mc = { 0 };
        bool            ok = curve_from_json(item, &parsed) && parsed.n <= ZBC_MIX_MAX_POINTS;
        for (int k = 0; ok && k < parsed.n; k++) {
            ok      = parsed.y[k] <= 100;
            mc.x[k] = parsed.x[k];
            mc.y[k] = (uint8_t)parsed.y[k];
        }
        mc.n = parsed.n;
        if (!ok || !zbc_mix_curve_valid(&mc)) {
            return "a mixing curve has 2-6 points, from 1 to 100 and ascending, each 0-100 %";
        }
        out->curve[c] = mc;
    }

    const bool tunable = zbc_light_tunable(out->kind);
    if (tunable && out->gpio >= 0 && out->gpio == out->gpio_warm) {
        return "the cold and warm white need two different pins";
    }
    if (three && out->gpio_neutral >= 0 && (out->gpio_neutral == out->gpio || out->gpio_neutral == out->gpio_warm)) {
        return "the neutral white needs a pin of its own";
    }

    /* A pin is driven by one light or read by one button, never both: a button
     * on a light's pin would read its own light, and a light on a button's pin
     * would short the button. */
    if (light_db_pin_taken(out->gpio, id)) {
        return "another light already uses that pin";
    }
    if (button_db_pin_taken(out->gpio, 0)) {
        return "a button already uses that pin";
    }
    if (tunable && light_db_pin_taken(out->gpio_warm, id)) {
        return "another light already uses the warm white pin";
    }
    if (tunable && button_db_pin_taken(out->gpio_warm, 0)) {
        return "a button already uses the warm white pin";
    }
    if (three && light_db_pin_taken(out->gpio_neutral, id)) {
        return "another light already uses the neutral white pin";
    }
    if (three && button_db_pin_taken(out->gpio_neutral, 0)) {
        return "a button already uses the neutral white pin";
    }
    return pwm_plan_problem(out, id);
}

/* PATCH /api/lights/{id}  {"name", "gpio", "active_low", "power_on"}, any subset */
static esp_err_t light_patch(httpd_req_t *req, uint16_t id)
{
    zbc_light_t l;
    if (!light_db_get(id, &l)) {
        return send_error(req, "404 Not Found", "unknown light");
    }
    cJSON *json = read_json_body(req);
    if (!json) {
        return ESP_FAIL;
    }
    const char *why = light_from_json(json, &l, id);
    cJSON_Delete(json);
    if (why) {
        /* Conflicts with something else on the board, rather than a bad value. */
        const bool conflict = strstr(why, "already uses") || strstr(why, "no PWM");
        return send_error(req, conflict ? "409 Conflict" : "400 Bad Request", why);
    }

    esp_err_t err = light_db_set(id, &l);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_error(req, "400 Bad Request", "that light configuration does not make sense");
    }
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "could not store the light");
    }
    /* Now rather than at the next reboot, for the reason buttons_reload is. */
    local_lights_reload();
    return send_ok(req, "200 OK");
}

static esp_err_t light_dispatch(httpd_req_t *req)
{
    ESP_LOGI(TAG, "<- %s", req->uri);
    char id_str[8];
    if (!uri_segment(req->uri, 2, id_str, sizeof(id_str))) {
        if (req->method == HTTP_POST) {
            return lights_post(req);
        }
        return send_error(req, "404 Not Found", "no such light route");
    }
    int id = atoi(id_str);
    if (id <= 0 || id > 0xffff) {
        return send_error(req, "400 Bad Request", "malformed light id");
    }
    char tail[16];
    if (req->method == HTTP_PATCH && !uri_segment(req->uri, 3, tail, sizeof(tail))) {
        return light_patch(req, (uint16_t)id);
    }
    return send_error(req, "404 Not Found", "no such light route");
}

/* The last few presses from any paired remote, newest first.
 *
 * A remote cannot be configured from its cluster list: an Aqara wireless switch
 * advertises no client clusters at all, and the two halves of a rocker are one
 * endpoint told apart only by the command they send. So which key somebody means
 * is settled by them pressing it, and this is what the editor watches while it
 * asks them to. `seq` never repeats, which is how a client tells a new press
 * from the same one polled twice. */
static cJSON *remotes_array(void)
{
    zb_remote_log_t log[ZBC_REMOTE_LOG_LEN];
    const size_t n = zb_recent_remote_events(log, ZBC_REMOTE_LOG_LEN);
    const int64_t now = esp_timer_get_time();

    cJSON *array = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        char ieee[19];
        zbc_ieee_to_str(log[i].ieee, ieee);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "seq", log[i].seq);
        cJSON_AddStringToObject(o, "ieee", ieee);
        cJSON_AddNumberToObject(o, "ep", log[i].ep);
        cJSON_AddNumberToObject(o, "key", log[i].key);
        cJSON_AddStringToObject(o, "event", remote_event_name((zb_remote_ev_t)log[i].ev));
        cJSON_AddNumberToObject(o, "ago_ms", (double)((now - log[i].at_us) / 1000));
        /* What the press decoded from: a repeated tsn is the same frame twice,
         * and `group` says whether it came by binding or group-cast. */
        if (log[i].cluster != 0xFFFF) {
            cJSON_AddNumberToObject(o, "cluster", log[i].cluster);
            cJSON_AddNumberToObject(o, "cmd", log[i].cmd);
            cJSON_AddNumberToObject(o, "tsn", log[i].tsn);
            char hex[3 * sizeof(log[i].payload) + 1] = "";
            size_t hn = 0;
            const size_t kept = log[i].payload_len < sizeof(log[i].payload)
                                    ? log[i].payload_len : sizeof(log[i].payload);
            for (size_t b = 0; b < kept; b++) {
                hn += (size_t)snprintf(hex + hn, sizeof(hex) - hn, b ? " %02x" : "%02x",
                                       log[i].payload[b]);
            }
            cJSON_AddStringToObject(o, "payload", hex);
            cJSON_AddNumberToObject(o, "payload_len", log[i].payload_len);
        }
        if (log[i].group != 0xFFFF) {
            cJSON_AddNumberToObject(o, "group", log[i].group);
        }
        /* The name the device list shows, so the editor can say which remote it
         * heard from without a second lookup. */
        zbc_device_t dev;
        if (device_db_get(log[i].ieee, &dev)) {
            cJSON_AddStringToObject(o, "name", dev.name[0] ? dev.name : dev.model);
        }
        cJSON_AddItemToArray(array, o);
    }
    return array;
}

/* GET /api/remotes */
/* The device a diagnostic entry is about: its IEEE and the name the device list
 * shows, null for either the registry does not know. Static because a device
 * row is large and httpd serves one request at a time. */
static void add_device_ref(cJSON *o, const char *key, const uint8_t *ieee, uint16_t short_addr)
{
    static zbc_device_t dev;
    uint8_t             found[ZBC_IEEE_LEN];
    if (!ieee && device_db_ieee_by_short(short_addr, found)) {
        ieee = found;
    }
    if (!ieee) {
        cJSON_AddNullToObject(o, key);
        return;
    }
    char str[19];
    zbc_ieee_to_str(ieee, str);
    cJSON *d = cJSON_AddObjectToObject(o, key);
    cJSON_AddStringToObject(d, "ieee", str);
    if (device_db_get(ieee, &dev)) {
        cJSON_AddStringToObject(d, "name", dev.name);
    } else {
        cJSON_AddNullToObject(d, "name");
    }
}

/* A parent by short address, as a person reads it: "coordinator", the router's
 * name and address, or null for none. */
static void add_parent_ref(cJSON *o, const char *key, uint16_t short_addr)
{
    static zbc_device_t dev;
    if (short_addr == ZB_PARENT_UNKNOWN) {
        cJSON_AddNullToObject(o, key);
        return;
    }
    if (short_addr == 0x0000) {
        cJSON_AddStringToObject(o, key, "coordinator");
        return;
    }
    uint8_t ieee[ZBC_IEEE_LEN];
    char    text[ZBC_NAME_LEN + 10];
    if (device_db_ieee_by_short(short_addr, ieee) && device_db_get(ieee, &dev)) {
        snprintf(text, sizeof(text), "%s (0x%04x)", dev.name, short_addr);
    } else {
        snprintf(text, sizeof(text), "0x%04x", short_addr);
    }
    cJSON_AddStringToObject(o, key, text);
}

static esp_err_t remotes_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "remotes", remotes_array());

    /* Every decoded command frame, oldest first - what a whole turn of a dial
     * looked like, Stops and all. Here only, not in /api/state: the page polls
     * that every two seconds and has no use for it. Static because it is 1.5 KB
     * and httpd serves one request at a time. */
    static zb_remote_frame_t frames[ZBC_REMOTE_FRAMES_LEN];
    const size_t  fn  = zb_recent_remote_frames(frames, ZBC_REMOTE_FRAMES_LEN);
    const int64_t now = esp_timer_get_time();
    cJSON *farr = cJSON_CreateArray();
    for (size_t i = 0; i < fn; i++) {
        char addr[7];
        snprintf(addr, sizeof(addr), "0x%04x", frames[i].short_addr);
        char hex[3 * sizeof(frames[i].payload) + 1] = "";
        size_t hn = 0;
        const size_t kept = frames[i].payload_len < sizeof(frames[i].payload)
                                ? frames[i].payload_len : sizeof(frames[i].payload);
        for (size_t b = 0; b < kept; b++) {
            hn += (size_t)snprintf(hex + hn, sizeof(hex) - hn, b ? " %02x" : "%02x",
                                   frames[i].payload[b]);
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "ago_ms", (double)((now - frames[i].at_us) / 1000));
        cJSON_AddStringToObject(o, "short_addr", addr);
        cJSON_AddNumberToObject(o, "ep", frames[i].ep);
        cJSON_AddNumberToObject(o, "cluster", frames[i].cluster);
        cJSON_AddNumberToObject(o, "cmd", frames[i].cmd);
        cJSON_AddNumberToObject(o, "tsn", frames[i].tsn);
        cJSON_AddStringToObject(o, "payload", hex);
        cJSON_AddItemToArray(farr, o);
    }
    cJSON_AddItemToObject(root, "frames", farr);

    /* Which parent each device moved to, and when - see zb_parent_changes. Here
     * with the frames, for the same reason. */
    static zb_parent_note_t moves[ZB_PARENT_LOG_LEN];
    const size_t mn   = zb_parent_changes(moves, ZB_PARENT_LOG_LEN);
    cJSON       *marr = cJSON_CreateArray();
    for (size_t i = 0; i < mn; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "ago_s", (double)((now - moves[i].at_us) / 1000000));
        cJSON_AddStringToObject(o, "what", moves[i].what ? moves[i].what : "?");
        char addr[7];
        snprintf(addr, sizeof(addr), "0x%04x", moves[i].short_addr);
        cJSON_AddStringToObject(o, "short_addr", addr);
        add_device_ref(o, "device", moves[i].have_ieee ? moves[i].ieee : NULL, moves[i].short_addr);
        add_parent_ref(o, "parent", moves[i].parent);
        add_parent_ref(o, "was", moves[i].was);
        cJSON_AddItemToArray(marr, o);
    }
    cJSON_AddItemToObject(root, "parent_changes", marr);
    return send_json(req, root, "200 OK");
}

/* GET /api/state - everything the poll loop needs, in one request.
 *
 * The separate /api/status, /api/devices and /api/groups stay as the documented
 * REST surface. The UI wants all three together, and on a single-threaded
 * server each would wait for the one before, each costing airtime. */
/* What the page compares to decide whether its tables are current: the change
 * counter, and the number of devices still fresh - the one thing the page
 * derives (the "offline" badge, ten minutes after the last frame) that can
 * change with nothing else moving. Compared for equality only. */
static void state_rev_string(char *out, size_t len)
{
    snprintf(out, len, "%lu-%u", (unsigned long)zbc_state_rev(),
             (unsigned)device_db_count_fresh(OFFLINE_AFTER_US));
}

static esp_err_t state_get(httpd_req_t *req)
{
    char rev[24];
    state_rev_string(rev, sizeof(rev));

    /* `?rev=` is what the page saw last time. Nothing new since: answer with
     * the status and the access state, which carry the parts that move on
     * their own - the pairing countdown, the Wi-Fi signal, the failsafe - and
     * leave the tables out. This is most polls, and it is what turns a 2 s
     * poll from a rebuild of every table into a few hundred bytes. */
    char query[64], seen[24];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "rev", seen, sizeof(seen)) == ESP_OK &&
        strcmp(seen, rev) == 0) {
        /* Streamed like the full answer: the status is most of this body, and
         * the access state is read from the request headers before the first
         * chunk goes out - see json_stream_t. */
        cJSON        *auth = auth_json(req);
        json_stream_t js;
        if (!json_stream_begin(&js, req)) {
            cJSON_Delete(auth);
            return send_error(req, "500 Internal Server Error", "out of memory");
        }
        bool first = true;
        json_stream_raw(&js, "{");
        json_stream_key(&js, &first, "status");
        stream_status(&js, false);
        json_stream_member(&js, &first, "auth", auth);
        json_stream_member(&js, &first, "rev", cJSON_CreateString(rev));
        json_stream_member(&js, &first, "unchanged", cJSON_CreateTrue());
        json_stream_raw(&js, "}");
        return json_stream_end(&js);
    }

    /* The whole answer, written as it is built - see json_stream_t. The access
     * state first: it reads the key from the request headers, and the server
     * discards those once the first chunk has gone out. */
    cJSON        *auth = auth_json(req);
    json_stream_t js;
    if (!json_stream_begin(&js, req)) {
        cJSON_Delete(auth);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    json_stream_raw(&js, "{\"status\":");
    stream_status(&js, false);
    json_stream_raw(&js, ",\"devices\":");
    stream_devices(&js);
    json_stream_raw(&js, ",\"groups\":");
    if (!stream_groups(&js)) {
        cJSON_Delete(auth);
        return json_stream_fail(&js);
    }
    /* The buttons ride along whole rather than as a summary: their live pin
     * reading has to be polled anyway, and carrying the rules too means the
     * editor opens off what the page already has. */
    json_stream_raw(&js, ",\"buttons\":");
    if (!stream_buttons(&js)) {
        cJSON_Delete(auth);
        return json_stream_fail(&js);
    }
    /* Here rather than on a poll of its own: esp_http_server answers one request
     * at a time and each one costs airtime the mesh needs, and the capture panel
     * in the button editor is watching this the whole time it is open. */
    json_stream_raw(&js, ",\"remotes\":");
    json_stream_value(&js, remotes_array());
    /* So the "open to everyone" and failsafe banners follow the poll instead of
     * costing a request of their own. */
    json_stream_raw(&js, ",\"auth\":");
    json_stream_value(&js, auth);
    /* Taken before the tables were built, so a change that lands while they
     * are being serialised makes the next poll fetch them again rather than
     * be missed. */
    json_stream_raw(&js, ",\"rev\":");
    json_stream_value(&js, cJSON_CreateString(rev));
    json_stream_raw(&js, "}");
    return json_stream_end(&js);
}

/* POST /api/groups  {"name": "Living room"} */
static esp_err_t groups_post(httpd_req_t *req)
{
    cJSON *json = read_json_body(req);
    if (!json) {
        return ESP_FAIL;
    }
    const char *name = name_from_json(json);
    if (!name) {
        cJSON_Delete(json);
        return send_error(req, "400 Bad Request", "name is required");
    }
    uint16_t  id  = 0;
    esp_err_t err = group_db_create(name, &id);
    cJSON_Delete(json);

    if (err == ESP_ERR_NO_MEM) {
        return send_error(req, "507 Insufficient Storage", "no space for another room");
    }
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "could not create the room");
    }

    zbc_group_t g;
    if (!group_db_get(id, &g)) {
        return send_error(req, "500 Internal Server Error", "room vanished");
    }
    return send_json(req, group_to_json(&g, esp_timer_get_time()), "201 Created");
}

/* PATCH /api/groups/{id}  {"name": "Kitchen", "adaptive": "off"|"mode_select"|"endpoint",
 *                         "turn_on": "last"|"adaptive"|"full"}
 *
 * Any of the keys on its own. A move into or out of Mode Select reaches Matter
 * controllers only after a restart - a cluster cannot be added to an endpoint that
 * is running, and rebuilding the endpoint would change the id controllers know the
 * light by - so the answer says when one is needed. */
static esp_err_t group_patch(httpd_req_t *req, uint16_t id)
{
    cJSON *json = read_json_body(req);
    if (!json) {
        return ESP_FAIL;
    }
    const bool   has_name = cJSON_GetObjectItem(json, "name") != NULL;
    const char  *name     = name_from_json(json);
    const cJSON *adaptive = cJSON_GetObjectItem(json, "adaptive");
    const cJSON *turn_on  = cJSON_GetObjectItem(json, "turn_on");
    if ((!has_name && !adaptive && !turn_on) || (has_name && !name)) {
        cJSON_Delete(json);
        return send_error(req, "400 Bad Request", "name, adaptive or turn_on is required");
    }
    const int expose = adaptive ? name_index(AL_EXPOSE_NAMES, ZBC_AL_EXPOSE_COUNT, adaptive) : -1;
    if (adaptive && expose < 0) {
        cJSON_Delete(json);
        return send_error(req, "400 Bad Request", "adaptive must be off, mode_select or endpoint");
    }
    const int on_means = turn_on ? name_index(TURN_ON_NAMES, ZBC_TURN_ON_COUNT, turn_on) : -1;
    if (turn_on && on_means < 0) {
        cJSON_Delete(json);
        return send_error(req, "400 Bad Request", "turn_on must be last, adaptive or full");
    }
    if (!group_db_get(id, NULL)) {
        cJSON_Delete(json);
        return send_error(req, "404 Not Found", "unknown room");
    }
    esp_err_t err = name ? group_db_rename(id, name) : ESP_OK;
    if (err == ESP_OK && expose >= 0) {
        err = lighting_group_set_adaptive_expose(id, (uint8_t)expose);
    }
    if (err == ESP_OK && on_means >= 0) {
        err = al_db_set_turn_on(id, (uint8_t)on_means);
    }
    cJSON_Delete(json);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_error(req, "404 Not Found", "unknown room");
    }
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "could not store the room");
    }

    bool restart = false;
    if (expose >= 0) {
        matter_status_t *st = malloc(sizeof(*st));
        if (st) {
            matter_bridge_get_status(st);
            for (int i = 0; i < st->groups; i++) {
                if (st->group_list[i].group_id == id) {
                    restart = st->group_list[i].mode_select_live != (expose == ZBC_AL_EXPOSE_MODE_SELECT);
                }
            }
            free(st);
        }
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "restart_needed", restart);
    return send_json(req, root, "200 OK");
}

/* POST /api/groups/{id}/members  {"ieee": "0x…", "ep": 1} */
static esp_err_t group_members_post(httpd_req_t *req, uint16_t id)
{
    cJSON *json = read_json_body(req);
    if (!json) {
        return ESP_FAIL;
    }
    cJSON *jieee = cJSON_GetObjectItem(json, "ieee");
    cJSON *jep   = cJSON_GetObjectItem(json, "ep");
    uint8_t ieee[ZBC_IEEE_LEN];
    if (!cJSON_IsString(jieee) || !zbc_str_to_ieee(jieee->valuestring, ieee) ||
        !cJSON_IsNumber(jep) || jep->valueint <= 0 || jep->valueint > 240) {
        cJSON_Delete(json);
        return send_error(req, "400 Bad Request", "ieee and ep are required");
    }
    uint8_t ep = (uint8_t)jep->valueint;
    cJSON_Delete(json);

    /* A group is a set of things that can be switched, so the endpoint has to
     * exist and speak On/Off. Dimming is optional - see group_level_post. */
    zbc_device_t dev;
    if (!lighting_get_device(ieee, &dev)) {
        return send_error(req, "404 Not Found", "unknown device");
    }
    const zbc_endpoint_t *e = lighting_find_endpoint(&dev, ep);
    if (!e || !(e->features & ZBC_FEAT_ONOFF)) {
        return send_error(req, "409 Conflict", "that endpoint cannot be switched");
    }

    esp_err_t err = group_db_add_member(id, ieee, ep);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_error(req, "404 Not Found", "unknown room");
    }
    if (err == ESP_ERR_NO_MEM) {
        return send_error(req, "507 Insufficient Storage", "no room for another light");
    }
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "could not add the light");
    }
    return send_ok(req, "200 OK");
}

/* POST /api/groups/{id}/level  {"pct": 1-100} */
static esp_err_t group_level_post(httpd_req_t *req, uint16_t id)
{
    cJSON *json = read_json_body(req);
    if (!json) {
        return ESP_FAIL;
    }
    cJSON *pct = cJSON_GetObjectItem(json, "pct");
    if (!cJSON_IsNumber(pct) || pct->valueint < 1 || pct->valueint > 100) {
        cJSON_Delete(json);
        return send_error(req, "400 Bad Request", "pct must be 1-100");
    }
    uint16_t dsec = transition_ds_from_json(json);
    int      want = pct->valueint;
    cJSON_Delete(json);

    /* "Light this room to N %" - each member's curve decides its own share. */
    esp_err_t err = lighting_group_set_brightness(id, want, dsec);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_error(req, "404 Not Found", "unknown room");
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return send_error(req, "409 Conflict", "no light in this room can dim");
    }
    if (err != ESP_OK) {
        return send_error(req, "502 Bad Gateway", "no light in this room took the command");
    }
    return send_ok(req, "202 Accepted");
}

/* POST /api/groups/{id}/onoff  {"state": "on"|"off"|"toggle"} */
static esp_err_t group_onoff_post(httpd_req_t *req, uint16_t id)
{
    cJSON *json = read_json_body(req);
    if (!json) {
        return ESP_FAIL;
    }
    int cmd = onoff_from_json(json);
    cJSON_Delete(json);
    if (cmd < 0) {
        return send_error(req, "400 Bad Request", "state must be on, off or toggle");
    }

    esp_err_t err = lighting_group_set_onoff(id, (zb_onoff_cmd_t)cmd, settings_transition_ds());
    if (err == ESP_ERR_NOT_FOUND) {
        return send_error(req, "404 Not Found", "unknown room");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error(req, "409 Conflict", "this room has no lights in it");
    }
    if (err != ESP_OK) {
        return send_error(req, "502 Bad Gateway", "no light in this room took the command");
    }
    return send_ok(req, "202 Accepted");
}

/* POST /api/groups/{id}/adaptive  {"state": "on"|"off"}, on when absent
 *
 * The Adaptive Lighting button. On starts the mode - and re-enters it on a group
 * that is already lit - and off ends it, leaving the lights where they are. */
static esp_err_t group_adaptive_post(httpd_req_t *req, uint16_t id)
{
    bool on = true;
    if (req->content_len > 0) {
        cJSON *json = read_json_body(req);
        if (!json) {
            return ESP_FAIL;
        }
        const cJSON *state = cJSON_GetObjectItem(json, "state");
        const bool   valid = !state || (cJSON_IsString(state) && (strcmp(state->valuestring, "on") == 0 ||
                                                                 strcmp(state->valuestring, "off") == 0));
        on = !state || (valid && strcmp(state->valuestring, "on") == 0);
        cJSON_Delete(json);
        if (!valid) {
            return send_error(req, "400 Bad Request", "state must be on or off");
        }
    }
    if (!on) {
        if (!group_db_get(id, NULL)) {
            return send_error(req, "404 Not Found", "unknown room");
        }
        lighting_group_stop_adaptive(id);
        return send_ok(req, "202 Accepted");
    }

    esp_err_t err = lighting_group_start_adaptive(id, settings_transition_ds());
    if (err == ESP_ERR_NOT_FOUND) {
        return send_error(req, "404 Not Found", "unknown room");
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return send_error(req, "409 Conflict", "Adaptive Lighting is off for this room");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error(req, "409 Conflict", "this room has no lights in it");
    }
    if (err != ESP_OK) {
        return send_error(req, "502 Bad Gateway", "no light in this room took the command");
    }
    return send_ok(req, "202 Accepted");
}

/* An optional integer field of a room's day. False only for one that is present
 * and not a number in range; *out is left alone when absent. */
static bool al_field(const cJSON *obj, const char *key, int lo, int hi, int *out)
{
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    if (!v) {
        return true;
    }
    if (!cJSON_IsNumber(v) || v->valuedouble < lo || v->valuedouble > hi) {
        return false;
    }
    *out = v->valueint;
    return true;
}

/* "HH:MM" or null (none) onto *out; left alone when absent. */
static bool al_time_field(const cJSON *obj, const char *key, int16_t *out)
{
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    if (!v) {
        return true;
    }
    if (cJSON_IsNull(v)) {
        *out = ZBC_AL_NO_TIME;
        return true;
    }
    unsigned h, m;
    char     extra;
    if (!cJSON_IsString(v) || sscanf(v->valuestring, "%2u:%2u%c", &h, &m, &extra) != 2 || h > 23 || m > 59) {
        return false;
    }
    *out = (int16_t)(h * 60 + m);
    return true;
}

/* {"earliest": "HH:MM"|null, "latest": "HH:MM"|null}, either; null clears both. */
static bool al_window_field(const cJSON *al, const char *key, int16_t w[2])
{
    const cJSON *o = cJSON_GetObjectItem(al, key);
    if (!o) {
        return true;
    }
    if (cJSON_IsNull(o)) {
        w[0] = w[1] = ZBC_AL_NO_TIME;
        return true;
    }
    return cJSON_IsObject(o) && al_time_field(o, "earliest", &w[0]) && al_time_field(o, "latest", &w[1]);
}

static bool json_whole(const cJSON *v, int lo, int hi)
{
    return cJSON_IsNumber(v) && v->valuedouble >= lo && v->valuedouble <= hi &&
           v->valuedouble == (double)v->valueint;
}

/* [[x, value], ...], the points of a shape between its ends, or [] for none;
 * left alone when absent. The order is al_db_day_ok's to check. */
static bool al_points_field(const cJSON *al, const char *key, int lo, int hi, uint8_t *n, uint8_t *xs, int16_t *ys)
{
    const cJSON *a = cJSON_GetObjectItem(al, key);
    if (!a) {
        return true;
    }
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) > ZBC_AL_SHAPE_POINTS) {
        return false;
    }
    uint8_t      k = 0;
    const cJSON *pt;
    cJSON_ArrayForEach(pt, a) {
        if (!cJSON_IsArray(pt) || cJSON_GetArraySize(pt) != 2 || !json_whole(cJSON_GetArrayItem(pt, 0), 2, 99) ||
            !json_whole(cJSON_GetArrayItem(pt, 1), lo, hi)) {
            return false;
        }
        xs[k] = (uint8_t)cJSON_GetArrayItem(pt, 0)->valueint;
        ys[k] = (int16_t)cJSON_GetArrayItem(pt, 1)->valueint;
        k++;
    }
    *n = k;
    return true;
}

static void add_hhmm(cJSON *obj, const char *key, int16_t m)
{
    if (m == ZBC_AL_NO_TIME) {
        cJSON_AddNullToObject(obj, key);
        return;
    }
    char hhmm[12];
    snprintf(hhmm, sizeof(hhmm), "%02d:%02d", m / 60, m % 60);
    cJSON_AddStringToObject(obj, key, hhmm);
}

static void add_window(cJSON *al, const char *key, const int16_t w[2])
{
    cJSON *o = cJSON_AddObjectToObject(al, key);
    add_hhmm(o, "earliest", w[0]);
    add_hhmm(o, "latest", w[1]);
}

static void add_points(cJSON *al, const char *key, uint8_t n, const uint8_t *xs, const int16_t *ys)
{
    cJSON *a = cJSON_AddArrayToObject(al, key);
    for (uint8_t i = 0; i < n; i++) {
        cJSON *pt = cJSON_CreateArray();
        cJSON_AddItemToArray(pt, cJSON_CreateNumber(xs[i]));
        cJSON_AddItemToArray(pt, cJSON_CreateNumber(ys[i]));
        cJSON_AddItemToArray(a, pt);
    }
}

/* A room's day, any subset of its fields, onto *day. False, with the reason in
 * *why, for anything out of range; *day is then not to be used. */
static bool al_day_from_json(const cJSON *al, zbc_al_profile_t *day, const char **why)
{
    int k_mid = day->k_midnight, k_noon = day->k_noon, k_base = day->k_base;
    int p_mid = day->pct_midnight, p_noon = day->pct_noon;
    if (!cJSON_IsObject(al) ||
        !al_field(al, "k_midnight", -ZBC_AL_OFFSET_K_LIMIT, ZBC_AL_OFFSET_K_LIMIT, &k_mid) ||
        !al_field(al, "k_noon", -ZBC_AL_OFFSET_K_LIMIT, ZBC_AL_OFFSET_K_LIMIT, &k_noon) ||
        !al_field(al, "k_base", ZBC_AL_BASE_K_MIN, ZBC_AL_BASE_K_MAX, &k_base) ||
        !al_field(al, "pct_midnight", 1, 100, &p_mid) || !al_field(al, "pct_noon", 1, 100, &p_noon)) {
        *why = "offsets must be -3000 to 3000 K, the base 1500-8000 K and brightness 1-100 %";
        return false;
    }
    day->k_midnight   = (int16_t)k_mid;
    day->k_noon       = (int16_t)k_noon;
    day->k_base       = (uint16_t)k_base;
    day->pct_midnight = (uint8_t)p_mid;
    day->pct_noon     = (uint8_t)p_noon;

    if (!al_window_field(al, "sunrise", day->rise_win) || !al_window_field(al, "sunset", day->set_win)) {
        *why = "sunrise and sunset take earliest and latest, each \"HH:MM\" or null";
        return false;
    }
    int16_t pct_y[ZBC_AL_SHAPE_POINTS];
    for (int i = 0; i < ZBC_AL_SHAPE_POINTS; i++) {
        pct_y[i] = day->pct_y[i];
    }
    if (!al_points_field(al, "pct_points", 1, 100, &day->n_pct, day->pct_x, pct_y) ||
        !al_points_field(al, "k_points", -ZBC_AL_OFFSET_K_LIMIT, ZBC_AL_OFFSET_K_LIMIT, &day->n_k, day->k_x,
                         day->k_y)) {
        *why = "pct_points and k_points take up to 4 [x, value] pairs: x 2-99, brightness 1-100 %, "
               "colour shift -3000 to 3000 K";
        return false;
    }
    for (int i = 0; i < ZBC_AL_SHAPE_POINTS; i++) {
        day->pct_y[i] = (uint8_t)pct_y[i];
    }
    if (!al_db_day_ok(day)) {
        *why = "a limit's earliest must not be after its latest, and points must go up in x";
        return false;
    }
    return true;
}

static void add_al_day(cJSON *al, const zbc_al_profile_t *day)
{
    cJSON_AddNumberToObject(al, "k_midnight", day->k_midnight);
    cJSON_AddNumberToObject(al, "k_noon", day->k_noon);
    cJSON_AddNumberToObject(al, "k_base", day->k_base);
    cJSON_AddNumberToObject(al, "pct_midnight", day->pct_midnight);
    cJSON_AddNumberToObject(al, "pct_noon", day->pct_noon);
    cJSON_AddNumberToObject(al, "offset_limit_k", ZBC_AL_OFFSET_K_LIMIT);
    cJSON_AddNumberToObject(al, "base_min_k", ZBC_AL_BASE_K_MIN);
    cJSON_AddNumberToObject(al, "base_max_k", ZBC_AL_BASE_K_MAX);
    /* The day stretched to clock times, and its shape between midnight and noon. */
    add_window(al, "sunrise", day->rise_win);
    add_window(al, "sunset", day->set_win);
    int16_t pct_y[ZBC_AL_SHAPE_POINTS];
    for (int i = 0; i < ZBC_AL_SHAPE_POINTS; i++) {
        pct_y[i] = day->pct_y[i];
    }
    add_points(al, "pct_points", day->n_pct, day->pct_x, pct_y);
    add_points(al, "k_points", day->n_k, day->k_x, day->k_y);
    cJSON_AddNumberToObject(al, "max_points", ZBC_AL_SHAPE_POINTS);
}

/* The zone the limits are read in, as the page sent it, with its offset now. */
static void add_time_zone(cJSON *root, time_t now)
{
    char tz[ZBC_TZ_MAX_LEN + 1], tz_name[ZBC_TZ_NAME_LEN];
    if (settings_tz(tz, sizeof(tz), tz_name, sizeof(tz_name))) {
        cJSON *z = cJSON_AddObjectToObject(root, "time_zone");
        cJSON_AddStringToObject(z, "posix", tz);
        if (tz_name[0]) {
            cJSON_AddStringToObject(z, "name", tz_name);
        } else {
            cJSON_AddNullToObject(z, "name");
        }
        int32_t off = 0;
        settings_tz_offset((int64_t)now, &off);
        cJSON_AddNumberToObject(z, "offset_s", off);
    } else {
        cJSON_AddNullToObject(root, "time_zone");
    }
}

/* GET /api/groups/{id}/day, and the answer to a POST there: the room's Adaptive
 * Lighting day, the sun's day around now as the room uses it (sunrise and sunset
 * moved into its limits: al_rise, al_set), the zone the limits are read in, and
 * what the room is sent now - from the firmware's own arithmetic, which the page
 * draws as the dot on its mirror of the day. */
static esp_err_t group_day_send(httpd_req_t *req, uint16_t id)
{
    zbc_al_profile_t day;
    al_db_day(id, &day);
    cJSON *root = cJSON_CreateObject();
    add_al_day(cJSON_AddObjectToObject(root, "day"), &day);
    const time_t now = time(NULL);
    add_time_zone(root, now);
    zbc_sun_day_t        real, used;
    lighting_al_status_t st;
    if (now < 1735689600) {
        /* Before SNTP has answered the clock counts up from 1970. */
        cJSON_AddNullToObject(root, "time_utc");
        cJSON_AddNullToObject(root, "sun");
        cJSON_AddNullToObject(root, "now");
    } else {
        cJSON_AddNumberToObject(root, "time_utc", (double)now);
        if (lighting_al_day(id, (int64_t)now, &real, &used)) {
            cJSON *sun = cJSON_AddObjectToObject(root, "sun");
            cJSON_AddNumberToObject(sun, "rise", (double)real.rise);
            cJSON_AddNumberToObject(sun, "noon", (double)real.noon);
            cJSON_AddNumberToObject(sun, "set", (double)real.set);
            cJSON_AddNumberToObject(sun, "polar", real.polar);
            cJSON_AddNumberToObject(sun, "al_rise", (double)used.rise);
            cJSON_AddNumberToObject(sun, "al_set", (double)used.set);
        } else {
            cJSON_AddNullToObject(root, "sun");
        }
        if (lighting_group_al_status(id, &st) && st.pct >= 0) {
            cJSON *an = cJSON_AddObjectToObject(root, "now");
            cJSON_AddNumberToObject(an, "pct", st.pct);
            cJSON_AddNumberToObject(an, "offset_k", st.offset_k);
        } else {
            cJSON_AddNullToObject(root, "now");
        }
    }
    return send_json(req, root, "200 OK");
}

/* POST /api/groups/{id}/day  {"pct_midnight": 20, "k_noon": 500, "sunrise": {...}, ...}
 * Any subset of the fields GET answers under "day", onto the room's day. */
static esp_err_t group_day_post(httpd_req_t *req, uint16_t id)
{
    if (!group_db_get(id, NULL)) {
        return send_error(req, "404 Not Found", "unknown room");
    }
    cJSON *json = read_json_body(req);
    if (!json) {
        return ESP_FAIL;
    }
    zbc_al_profile_t day;
    al_db_day(id, &day);
    const char *why = NULL;
    const bool  ok  = al_day_from_json(json, &day, &why);
    cJSON_Delete(json);
    if (!ok) {
        return send_error(req, "400 Bad Request", why);
    }
    /* The limits are local clock times. Without a zone they would do nothing,
     * and a box that says it has them and quietly ignores them is worse than a
     * refusal. */
    if (al_db_day_has_limits(&day) && !settings_tz(NULL, 0, NULL, 0)) {
        return send_error(req, "400 Bad Request",
                          "sunrise and sunset limits are local clock times: send time_zone to /api/settings first");
    }
    const esp_err_t err = lighting_group_set_adaptive_day(id, &day);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_error(req, "404 Not Found", "unknown room");
    }
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "could not store the room's Adaptive Lighting day");
    }
    return group_day_send(req, id);
}

/* Dispatches every /api/groups... request. The collection routes carry no id;
 * /api/groups/{id}/members/{ieee}/{ep} is the longest form we parse. */
static esp_err_t group_dispatch(httpd_req_t *req)
{
    if (req->method != HTTP_GET) {
        ESP_LOGI(TAG, "<- %s", req->uri);
    }
    char id_str[8];
    if (!uri_segment(req->uri, 2, id_str, sizeof(id_str))) {
        if (req->method == HTTP_GET) {
            return groups_get(req);
        }
        if (req->method == HTTP_POST) {
            return groups_post(req);
        }
        return send_error(req, "404 Not Found", "no such group route");
    }
    int id = atoi(id_str);
    if (id <= 0 || id > 0xffff) {
        return send_error(req, "400 Bad Request", "malformed group id");
    }

    char tail[16];
    bool has_tail = uri_segment(req->uri, 3, tail, sizeof(tail));

    if (req->method == HTTP_POST && has_tail) {
        if (strcmp(tail, "level") == 0) {
            return group_level_post(req, (uint16_t)id);
        }
        if (strcmp(tail, "onoff") == 0) {
            return group_onoff_post(req, (uint16_t)id);
        }
        if (strcmp(tail, "members") == 0) {
            return group_members_post(req, (uint16_t)id);
        }
        if (strcmp(tail, "adaptive") == 0) {
            return group_adaptive_post(req, (uint16_t)id);
        }
        if (strcmp(tail, "day") == 0) {
            return group_day_post(req, (uint16_t)id);
        }
    }
    if (req->method == HTTP_GET && has_tail && strcmp(tail, "day") == 0) {
        if (!group_db_get((uint16_t)id, NULL)) {
            return send_error(req, "404 Not Found", "unknown room");
        }
        return group_day_send(req, (uint16_t)id);
    }
    if (req->method == HTTP_PATCH && !has_tail) {
        return group_patch(req, (uint16_t)id);
    }
    if (req->method == HTTP_DELETE && !has_tail) {
        if (group_db_remove((uint16_t)id) == ESP_ERR_NOT_FOUND) {
            return send_error(req, "404 Not Found", "unknown room");
        }
        /* The rules that named it stay, but unassigned: a button keeps its
         * shape and the UI says "pick a room" rather than silently losing the
         * work someone did setting it up. */
        button_db_forget_group((uint16_t)id);
        lighting_group_forget_adaptive((uint16_t)id);
        /* The id is given out again: the WLED interface must not stream to
         * whatever room is made next. */
        if (settings_wled_room() == (uint16_t)id) {
            settings_set_wled(settings_wled_on(), 0, settings_wled_others_off());
            wled_reload();
        }
        return send_ok(req, "200 OK");
    }
    /* DELETE /api/groups/{id}/members/{ieee}/{ep} */
    if (req->method == HTTP_DELETE && has_tail && strcmp(tail, "members") == 0) {
        char ieee_str[24], ep_str[8];
        uint8_t ieee[ZBC_IEEE_LEN];
        if (!uri_segment(req->uri, 4, ieee_str, sizeof(ieee_str)) ||
            !zbc_str_to_ieee(ieee_str, ieee) ||
            !uri_segment(req->uri, 5, ep_str, sizeof(ep_str))) {
            return send_error(req, "400 Bad Request", "missing light address or endpoint");
        }
        int ep = atoi(ep_str);
        if (ep <= 0 || ep > 240) {
            return send_error(req, "400 Bad Request", "endpoint out of range");
        }
        if (group_db_remove_member((uint16_t)id, ieee, (uint8_t)ep) == ESP_ERR_NOT_FOUND) {
            return send_error(req, "404 Not Found", "unknown room or light");
        }
        return send_ok(req, "200 OK");
    }
    return send_error(req, "404 Not Found", "no such group route");
}

/* Started with 3072 bytes: esp_restart() can confirm an image on trial on this
 * stack (ota.c's confirm_on_restart), which writes flash and logs. */
static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));  /* let the response reach the browser */
    esp_restart();
}

/* GET /api/settings */
static esp_err_t settings_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    /* Whether this build has /api/backup and /api/restore: the page shows the
     * Backup card only when it does. */
#if CONFIG_ZBC_CONFIG_BACKUP
    cJSON_AddBoolToObject(root, "backup", true);
#else
    cJSON_AddBoolToObject(root, "backup", false);
#endif
    cJSON_AddNumberToObject(root, "transition_ms", settings_transition_ds() * 100);
    cJSON_AddNumberToObject(root, "transition_max_ms", ZBC_TRANSITION_DS_MAX * 100);
    cJSON_AddBoolToObject(root, "wifi_sleep", settings_wifi_sleep());
    cJSON_AddBoolToObject(root, "warm_off", settings_warm_off());
#if CONFIG_ZBC_WLED
    /* Absent from a build without the interface: the page shows its card only
     * when this is here. */
    cJSON *wl = cJSON_AddObjectToObject(root, "wled");
    cJSON_AddBoolToObject(wl, "enabled", settings_wled_on());
    cJSON_AddBoolToObject(wl, "others_off", settings_wled_others_off());
    if (settings_wled_room()) {
        cJSON_AddNumberToObject(wl, "room", settings_wled_room());
    } else {
        cJSON_AddNullToObject(wl, "room");
    }
#endif
    /* The channel the network is on, or the one it will be formed on. */
    zb_netinfo_t zb;
    zb_get_netinfo(&zb);
    const uint8_t zb_ch = settings_zigbee_channel();
    cJSON_AddNumberToObject(root, "zigbee_channel",
                            zb.network_up ? zb.channel : zb_ch ? zb_ch : CONFIG_ZBC_ZIGBEE_CHANNEL);
    cJSON_AddStringToObject(root, "name", settings_name());
    /* Derived from the name, and the caller needs it: it is the address the box
     * comes back on after the restart below. */
    cJSON_AddStringToObject(root, "hostname", settings_hostname());
    cJSON_AddNumberToObject(root, "name_max_len", ZBC_COORD_NAME_LEN - 1);

    /* Where Adaptive Lighting's sun is. Each room's day is its own
     * (/api/groups/{id}/day). No timezone here: the sun model works in UTC, and
     * the page shows these timestamps in the viewer's own local time. */
    int32_t    lat = 0, lon = 0;
    const bool located = settings_location(&lat, &lon);
    if (located) {
        cJSON *loc = cJSON_AddObjectToObject(root, "location");
        cJSON_AddNumberToObject(loc, "lat", lat / 1e6);
        cJSON_AddNumberToObject(loc, "lon", lon / 1e6);
    } else {
        cJSON_AddNullToObject(root, "location");
    }
    const time_t now = time(NULL);
    add_time_zone(root, now);

    zbc_thermal_cfg_t th;
    settings_thermal(&th);
    cJSON *t = cJSON_AddObjectToObject(root, "thermal");
    if (th.gpio < 0) {
        cJSON_AddNullToObject(t, "gpio");
    } else {
        cJSON_AddNumberToObject(t, "gpio", th.gpio);
    }
    cJSON_AddNumberToObject(t, "b", th.b);
    cJSON_AddNumberToObject(t, "r0_ohm", th.r0_ohm);
    cJSON_AddNumberToObject(t, "rs_ohm", th.rs_ohm);
    cJSON_AddStringToObject(t, "ntc_to", th.ntc_to_3v3 ? "3v3" : "gnd");
    cJSON_AddNumberToObject(t, "trip_c", th.trip_c);
    cJSON *pins = cJSON_AddArrayToObject(t, "adc_pins");
    for (int pin = 0; pin < SOC_GPIO_PIN_COUNT; pin++) {
        if (thermal_pin_is_adc(pin)) {
            cJSON_AddItemToArray(pins, cJSON_CreateNumber(pin));
        }
    }

    zbc_sun_day_t day;
    if (now < 1735689600) {
        /* Before SNTP has answered the clock counts up from 1970. */
        cJSON_AddNullToObject(root, "time_utc");
        cJSON_AddNullToObject(root, "sun");
    } else {
        cJSON_AddNumberToObject(root, "time_utc", (double)now);
        if (located && zbc_sun_day((int64_t)now, lat / 1e6, lon / 1e6, &day)) {
            cJSON *sun = cJSON_AddObjectToObject(root, "sun");
            cJSON_AddNumberToObject(sun, "rise", (double)day.rise);
            cJSON_AddNumberToObject(sun, "noon", (double)day.noon);
            cJSON_AddNumberToObject(sun, "set", (double)day.set);
            cJSON_AddNumberToObject(sun, "polar", day.polar);
        } else {
            cJSON_AddNullToObject(root, "sun");
        }
    }
    return send_json(req, root, "200 OK");
}

/* {"gpio": 3|null, "b": 3350, "r0_ohm": 10000, "rs_ohm": 10000, "ntc_to": "gnd"|"3v3",
 * "trip_c": 75}, any subset, onto *out. NULL when it is all acceptable. */
static bool thermal_num(const cJSON *obj, const char *key, long lo, long hi, long *out, bool *bad)
{
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    if (!v) {
        return false;
    }
    if (!cJSON_IsNumber(v) || v->valuedouble < lo || v->valuedouble > hi) {
        *bad = true;
        return false;
    }
    *out = (long)v->valuedouble;
    return true;
}

static const char *thermal_from_json(const cJSON *j, zbc_thermal_cfg_t *out)
{
    if (!cJSON_IsObject(j)) {
        return "thermal must be an object";
    }
    bool bad = false;
    long v   = 0;
    if (thermal_num(j, "b", ZBC_THERMAL_B_MIN, ZBC_THERMAL_B_MAX, &v, &bad)) out->b = (uint16_t)v;
    if (bad) return "b must be 1000-10000";
    if (thermal_num(j, "r0_ohm", ZBC_THERMAL_OHM_MIN, ZBC_THERMAL_OHM_MAX, &v, &bad)) out->r0_ohm = (uint32_t)v;
    if (bad) return "r0_ohm must be 100-1000000";
    if (thermal_num(j, "rs_ohm", ZBC_THERMAL_OHM_MIN, ZBC_THERMAL_OHM_MAX, &v, &bad)) out->rs_ohm = (uint32_t)v;
    if (bad) return "rs_ohm must be 100-1000000";
    if (thermal_num(j, "trip_c", ZBC_THERMAL_TRIP_MIN_C, ZBC_THERMAL_TRIP_MAX_C, &v, &bad)) out->trip_c = (uint8_t)v;
    if (bad) return "trip_c must be 40-120";

    const cJSON *to = cJSON_GetObjectItem(j, "ntc_to");
    if (to) {
        if (!cJSON_IsString(to) || (strcmp(to->valuestring, "gnd") != 0 && strcmp(to->valuestring, "3v3") != 0)) {
            return "ntc_to must be \"gnd\" or \"3v3\"";
        }
        out->ntc_to_3v3 = strcmp(to->valuestring, "3v3") == 0;
    }

    const cJSON *gpio = cJSON_GetObjectItem(j, "gpio");
    if (cJSON_IsNull(gpio)) {
        out->gpio = -1;
    } else if (gpio) {
        if (!cJSON_IsNumber(gpio)) {
            return "gpio must be a pin number or null";
        }
        const int pin = gpio->valueint;
        if (!thermal_pin_is_adc(pin)) {
            return "the overheat sensor needs an analog pin (GPIO 0-6)";
        }
        const char *why = buttons_pin_problem(pin);
        if (why) {
            return why;
        }
#if CONFIG_ZBC_UNLOCK_GPIO >= 0
        if (pin == CONFIG_ZBC_UNLOCK_GPIO) {
            return "that pin is watched for the failsafe unlock";
        }
#endif
        if (light_db_pin_taken(pin, 0)) {
            return "a wired light already uses that pin";
        }
        if (button_db_pin_taken(pin, 0)) {
            return "a button already uses that pin";
        }
        out->gpio = (int8_t)pin;
    }
    return NULL;
}

/* POST /api/settings  {"transition_ms": 400, "name": "Living Room Hub"}
 *
 * Both keys are optional and independent, so the fade time can still be saved
 * on its own - it takes effect at once, where a new name needs the restart. */
static esp_err_t settings_post(httpd_req_t *req)
{
    cJSON *json = read_json_body(req);
    if (!json) {
        return ESP_FAIL;
    }
    cJSON *ms   = cJSON_GetObjectItem(json, "transition_ms");
    cJSON *name = cJSON_GetObjectItem(json, "name");
    cJSON *loc  = cJSON_GetObjectItem(json, "location");
    cJSON *wsl  = cJSON_GetObjectItem(json, "wifi_sleep");
    cJSON *warm = cJSON_GetObjectItem(json, "warm_off");
    cJSON *thj  = cJSON_GetObjectItem(json, "thermal");
    cJSON *zch  = cJSON_GetObjectItem(json, "zigbee_channel");
    cJSON *wlj  = cJSON_GetObjectItem(json, "wled");
    cJSON *tzj  = cJSON_GetObjectItem(json, "time_zone");
    if (!ms && !name && !loc && !wsl && !warm && !thj && !zch && !wlj && !tzj) {
        cJSON_Delete(json);
        return send_error(req, "400 Bad Request",
                          "transition_ms, name, location, time_zone, wifi_sleep, warm_off, thermal, zigbee_channel "
                          "or wled is required (a room's Adaptive Lighting day is /api/groups/{id}/day)");
    }
#if CONFIG_ZBC_WLED
    /* The WLED interface: {"enabled": bool, "room": id | null, "others_off": bool},
     * any of them. */
    bool     wled_on     = settings_wled_on();
    uint16_t wled_room   = settings_wled_room();
    bool     wled_others = settings_wled_others_off();
    if (wlj) {
        const cJSON *en = cJSON_GetObjectItem(wlj, "enabled");
        const cJSON *rm = cJSON_GetObjectItem(wlj, "room");
        const cJSON *ot = cJSON_GetObjectItem(wlj, "others_off");
        if (!cJSON_IsObject(wlj) || (en && !cJSON_IsBool(en)) || (ot && !cJSON_IsBool(ot)) ||
            (rm && !cJSON_IsNull(rm) && (!cJSON_IsNumber(rm) || rm->valueint < 1 || rm->valueint > 0xFFFF ||
                                         !group_db_get((uint16_t)rm->valueint, NULL)))) {
            cJSON_Delete(json);
            return send_error(req, "400 Bad Request",
                              "wled takes enabled and others_off (true or false) and room (a room's id, or null)");
        }
        if (en) {
            wled_on = cJSON_IsTrue(en);
        }
        if (ot) {
            wled_others = cJSON_IsTrue(ot);
        }
        if (rm) {
            wled_room = cJSON_IsNull(rm) ? 0 : (uint16_t)rm->valueint;
        }
    }
#else
    if (wlj) {
        cJSON_Delete(json);
        return send_error(req, "400 Bad Request", "this build has no WLED interface");
    }
#endif
    if (zch && (!cJSON_IsNumber(zch) || zch->valuedouble != (double)zch->valueint || zch->valueint < 11 ||
                zch->valueint > 26)) {
        cJSON_Delete(json);
        return send_error(req, "400 Bad Request", "zigbee_channel must be 11-26");
    }
    /* A move needs the network that is to move; refused before anything else is
     * stored, so a refusal changes nothing. */
    const int zb_channel = zch ? zch->valueint : 0;
    if (zch) {
        zb_netinfo_t zb;
        zb_get_netinfo(&zb);
        if (!zb.network_up) {
            cJSON_Delete(json);
            return send_error(req, "409 Conflict", "the Zigbee network is not up");
        }
    }
    /* Checked in full before anything is stored, like the rest. */
    zbc_thermal_cfg_t th;
    settings_thermal(&th);
    if (thj) {
        const char *why = thermal_from_json(thj, &th);
        if (why) {
            cJSON_Delete(json);
            return send_error(req, "400 Bad Request", why);
        }
    }
    if (wsl && !cJSON_IsBool(wsl)) {
        cJSON_Delete(json);
        return send_error(req, "400 Bad Request", "wifi_sleep must be true or false");
    }
    if (warm && !cJSON_IsBool(warm)) {
        cJSON_Delete(json);
        return send_error(req, "400 Bad Request", "warm_off must be true or false");
    }
    if (ms && (!cJSON_IsNumber(ms) || ms->valueint < 0 || ms->valueint > ZBC_TRANSITION_DS_MAX * 100)) {
        cJSON_Delete(json);
        return send_error(req, "400 Bad Request", "transition_ms must be 0-10000");
    }
    if (name && !cJSON_IsString(name)) {
        cJSON_Delete(json);
        return send_error(req, "400 Bad Request", "name must be a string");
    }
    /* Here, with the rest, and not only when it is stored below: by then the
     * location, the zone and the overheat settings have been written, and a
     * refused name would leave them changed behind a 400. */
    if (name && !settings_name_ok(name->valuestring)) {
        cJSON_Delete(json);
        return send_error(req, "400 Bad Request",
                          "the name needs at least one letter or digit, and at most 31 characters");
    }

    /* Adaptive Lighting's location and day, both checked in full before either is
     * stored - and before the rename below, which restarts the box. null forgets
     * the location. */
    bool    set_loc = false, forget_loc = false;
    int32_t lat_udeg = 0, lon_udeg = 0;
    if (loc && !cJSON_IsNull(loc)) {
        const cJSON *la = cJSON_GetObjectItem(loc, "lat");
        const cJSON *lo = cJSON_GetObjectItem(loc, "lon");
        if (!cJSON_IsNumber(la) || !cJSON_IsNumber(lo) || la->valuedouble < -90.0 || la->valuedouble > 90.0 ||
            lo->valuedouble < -180.0 || lo->valuedouble > 180.0) {
            cJSON_Delete(json);
            return send_error(req, "400 Bad Request", "lat must be -90 to 90 and lon -180 to 180");
        }
        lat_udeg = (int32_t)(la->valuedouble * 1e6 + (la->valuedouble < 0 ? -0.5 : 0.5));
        lon_udeg = (int32_t)(lo->valuedouble * 1e6 + (lo->valuedouble < 0 ? -0.5 : 0.5));
        set_loc  = true;
    } else if (loc) {
        forget_loc = true;
    }
    /* The zone the limits are read in: {"posix": "CET-1CEST,M3.5.0,M10.5.0/3",
     * "name": "Europe/Berlin"}, or null to forget it. */
    bool        set_tz = false, forget_tz = false;
    const char *tz_posix = NULL, *tz_name = NULL;
    if (tzj && !cJSON_IsNull(tzj)) {
        const cJSON *px = cJSON_GetObjectItem(tzj, "posix");
        const cJSON *nm = cJSON_GetObjectItem(tzj, "name");
        tz_posix        = cJSON_IsString(px) ? px->valuestring : NULL;
        tz_name         = cJSON_IsString(nm) ? nm->valuestring : NULL;
        if (!cJSON_IsObject(tzj) || !tz_posix || (nm && !cJSON_IsNull(nm) && !tz_name) ||
            !settings_tz_ok(tz_posix, tz_name)) {
            cJSON_Delete(json);
            return send_error(req, "400 Bad Request",
                              "time_zone takes posix (a POSIX TZ rule such as CET-1CEST,M3.5.0,M10.5.0/3) and "
                              "name (such as Europe/Berlin)");
        }
        set_tz = true;
    } else if (tzj) {
        forget_tz = true;
    }
    /* The rooms' limits are local clock times. Without a zone they would do
     * nothing, and a box that says it has them and quietly ignores them is worse
     * than a refusal. */
    if (forget_tz && al_db_any_limits()) {
        cJSON_Delete(json);
        return send_error(req, "409 Conflict",
                          "a room's Adaptive Lighting day has sunrise or sunset limits, which are local clock "
                          "times: clear them before forgetting the time zone");
    }

    if (set_tz || forget_tz) {
        if (settings_set_tz(tz_posix, tz_name) != ESP_OK) {
            cJSON_Delete(json);
            return send_error(req, "500 Internal Server Error", "could not store the time zone");
        }
    }
    if (set_loc || forget_loc) {
        if (settings_set_location(set_loc, lat_udeg, lon_udeg) != ESP_OK) {
            cJSON_Delete(json);
            return send_error(req, "500 Internal Server Error", "could not store the location");
        }
    }
    if (loc || tzj) {
        lighting_al_wake();
        /* A room page shows what the mode would send now. */
        zbc_state_bump();
    }
    if (thj) {
        if (settings_set_thermal(&th) != ESP_OK) {
            cJSON_Delete(json);
            return send_error(req, "500 Internal Server Error", "could not store the overheat settings");
        }
        thermal_reload();
    }

    /* Renaming restarts the box, so only do it when something actually changed -
     * a dialog that saves on every close would otherwise reboot on every close.
     * Both are compared, not just the name: an empty name means "back to the
     * default", and for a box literally called "Elixcon Light Hub" that moves
     * the hostname from elixcon-light-hub to zigbee while the name stands
     * still. Comparing what settings.c ended up with covers every such case
     * without this handler having to know the rule. */
    bool rename = false;
    if (name) {
        char was_name[ZBC_COORD_NAME_LEN];
        char was_host[ZBC_COORD_NAME_LEN];
        strlcpy(was_name, settings_name(), sizeof(was_name));
        strlcpy(was_host, settings_hostname(), sizeof(was_host));

        esp_err_t err = settings_set_name(name->valuestring);
        if (err == ESP_ERR_INVALID_ARG) {
            cJSON_Delete(json);
            return send_error(req, "400 Bad Request",
                              "the name needs at least one letter or digit, and at most 31 characters");
        }
        if (err != ESP_OK) {
            cJSON_Delete(json);
            return send_error(req, "500 Internal Server Error", "could not store the name");
        }
        rename = strcmp(was_name, settings_name()) != 0 || strcmp(was_host, settings_hostname()) != 0;
    }

    uint16_t   ds        = ms ? (uint16_t)(ms->valueint / 100) : settings_transition_ds();
    const bool set_sleep = wsl != NULL;
    const bool sleep_on  = wsl ? cJSON_IsTrue(wsl) : settings_wifi_sleep();
    const bool set_warm  = warm != NULL;
    const bool warm_on   = warm ? cJSON_IsTrue(warm) : settings_warm_off();
    cJSON_Delete(json);

    /* Read by lighting.c on the next switch-off; nothing to apply. */
    if (set_warm && settings_set_warm_off(warm_on) != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "could not store the setting");
    }

#if CONFIG_ZBC_WLED
    if (wlj) {
        /* others_off is read when a stream starts; a running one is left as it is. */
        if (settings_set_wled(wled_on, wled_room, wled_others) != ESP_OK) {
            return send_error(req, "500 Internal Server Error", "could not store the setting");
        }
        wled_reload();
    }
#endif

    if (set_sleep) {
        if (settings_set_wifi_sleep(sleep_on) != ESP_OK) {
            return send_error(req, "500 Internal Server Error", "could not store the setting");
        }
        /* Takes effect at once, so the radio counters can be compared both
         * ways without a reboot between them. */
        wifi_manager_set_modem_sleep(sleep_on);
    }

    if (ms) {
        if (settings_set_transition_ds(ds) != ESP_OK) {
            return send_error(req, "500 Internal Server Error", "could not store the setting");
        }
        /* Move-to commands carry the new value from here on, but switching a lamp
         * uses the lamp's own attribute, so push that out now. */
        zb_apply_onoff_transition(ds);
    }

    /* Last, since it is the one that can still be refused: the network moves in
     * the next ten seconds, and a rename's restart below would not stop it - the
     * channel is stored first, and the boot puts the coordinator back on it. */
    if (zb_channel) {
        esp_err_t err = zb_move_channel((uint8_t)zb_channel);
        if (err == ESP_ERR_INVALID_STATE) {
            return send_error(req, "409 Conflict", "a channel move or survey is in progress");
        }
        if (err != ESP_OK) {
            return send_error(req, "500 Internal Server Error", "could not start the channel move");
        }
    }

    /* The response carries the derived hostname, which is the only place the
     * caller can learn the address to come back to. Send it before rebooting. */
    esp_err_t sent = settings_get(req);
    if (rename) {
        ESP_LOGI(TAG, "Renamed, restarting");
        xTaskCreate(reboot_task, "reboot", 3072, NULL, 5, NULL);
    }
    return sent;
}

static esp_err_t wifi_scan_get(httpd_req_t *req)
{
    wifi_ap_record_t *records = NULL;
    uint16_t          count   = 0;
    esp_err_t         err     = wifi_manager_scan(&records, &count);
    if (err != ESP_OK) {
        return send_error(req, "503 Service Unavailable", "scan failed");
    }

    cJSON *array = cJSON_CreateArray();
    for (uint16_t i = 0; i < count; i++) {
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", (const char *)records[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", records[i].rssi);
        /* Which channel, so a Zigbee channel can be chosen against what is
         * actually on the air rather than against a guess. */
        cJSON_AddNumberToObject(ap, "channel", records[i].primary);
        /* A 40 MHz network covers the channel next to it as well. */
        if (records[i].second == WIFI_SECOND_CHAN_ABOVE || records[i].second == WIFI_SECOND_CHAN_BELOW) {
            cJSON_AddStringToObject(ap, "second", records[i].second == WIFI_SECOND_CHAN_ABOVE ? "above" : "below");
        } else {
            cJSON_AddNullToObject(ap, "second");
        }
        cJSON_AddBoolToObject(ap, "open", records[i].authmode == WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(array, ap);
    }
    free(records);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "networks", array);
    return send_json(req, root, "200 OK");
}

/* POST /api/zigbee/scan starts a channel survey (zb_channel_survey_start); GET
 * says how it is going and what it found. The page ranks the channels itself,
 * from this and /api/wifi/scan. */
static esp_err_t zigbee_scan_post(httpd_req_t *req)
{
    ESP_LOGI(TAG, "<- %s", req->uri);
    if (zb_channel_survey_start() != ESP_OK) {
        return send_error(req, "409 Conflict",
                          "the Zigbee network is not up, or a channel move or survey is in progress");
    }
    return send_ok(req, "202 Accepted");
}

static esp_err_t zigbee_scan_get(httpd_req_t *req)
{
    zb_survey_t sv;
    zb_get_channel_survey(&sv);
    static const char *const STATES[] = { "idle", "running", "done", "failed" };
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", STATES[sv.state <= ZB_SURVEY_FAILED ? sv.state : 0]);
    if (sv.error) {
        cJSON_AddStringToObject(root, "error", sv.error);
    } else {
        cJSON_AddNullToObject(root, "error");
    }
    cJSON_AddNumberToObject(root, "passes", sv.passes);
    if (sv.state == ZB_SURVEY_IDLE) {
        cJSON_AddNullToObject(root, "s_ago");
    } else {
        cJSON_AddNumberToObject(root, "s_ago", (double)((esp_timer_get_time() - sv.at_us) / 1000000));
    }
    /* Each scan's reading as it came: one burst can lift a channel's highest by
     * 40 dB, and the page judges by the median. */
    cJSON *chs = cJSON_AddArrayToObject(root, "channels");
    for (int i = 0; i < ZB_SURVEY_CHANNELS; i++) {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddNumberToObject(c, "channel", 11 + i);
        cJSON *dbm = cJSON_AddArrayToObject(c, "dbm");
        for (int k = 0; k < sv.ch[i].samples && k < ZB_SURVEY_PASSES; k++) {
            cJSON_AddItemToArray(dbm, cJSON_CreateNumber(sv.ch[i].dbm[k]));
        }
        cJSON_AddItemToArray(chs, c);
    }
    cJSON *nets = cJSON_AddArrayToObject(root, "networks");
    for (int i = 0; i < sv.networks && i < ZB_SURVEY_NETWORKS; i++) {
        char hex[19];
        snprintf(hex, sizeof(hex), "0x%016llx", (unsigned long long)sv.net[i].ext_pan_id);
        cJSON *n = cJSON_CreateObject();
        cJSON_AddNumberToObject(n, "channel", sv.net[i].channel);
        cJSON_AddNumberToObject(n, "pan_id", sv.net[i].pan_id);
        cJSON_AddStringToObject(n, "ext_pan_id", hex);
        cJSON_AddBoolToObject(n, "permit_join", sv.net[i].permit_join);
        cJSON_AddItemToArray(nets, n);
    }
    return send_json(req, root, "200 OK");
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    cJSON *json = read_json_body(req);
    if (!json) {
        return ESP_FAIL;
    }
    cJSON *ssid = cJSON_GetObjectItem(json, "ssid");
    cJSON *pass = cJSON_GetObjectItem(json, "password");
    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0') {
        cJSON_Delete(json);
        return send_error(req, "400 Bad Request", "ssid is required");
    }

    esp_err_t err = wifi_manager_set_credentials(ssid->valuestring,
                                                 cJSON_IsString(pass) ? pass->valuestring : "");
    cJSON_Delete(json);
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "could not store credentials");
    }

    ESP_LOGI(TAG, "New Wi-Fi credentials stored, restarting");
    send_ok(req, "200 OK");
    xTaskCreate(reboot_task, "reboot", 3072, NULL, 5, NULL);
    return ESP_OK;
}

/* POST /api/wifi/resume - ends a button-started setup hotspot early, restarting
 * onto the network that is still stored. */
/* POST /api/wifi/test {"ssid", "password"} - tries a network from the setup
 * hotspot before storing it (see wifi_manager_test); GET /api/wifi/test says how
 * it went. The page polls the GET: the hotspot hops to the network's channel as
 * soon as the check associates, so a phone may miss an answer or two. */
static esp_err_t wifi_test_post(httpd_req_t *req)
{
    cJSON *json = read_json_body(req);
    if (!json) {
        return ESP_FAIL;
    }
    const cJSON *ssid = cJSON_GetObjectItem(json, "ssid");
    const cJSON *pass = cJSON_GetObjectItem(json, "password");
    esp_err_t    err  = cJSON_IsString(ssid)
                            ? wifi_manager_test(ssid->valuestring, cJSON_IsString(pass) ? pass->valuestring : "")
                            : ESP_ERR_INVALID_ARG;
    cJSON_Delete(json);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_error(req, "400 Bad Request", "a network name of up to 32 characters, and a password of up to 64");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error(req, "409 Conflict", "a network can only be checked from the setup hotspot");
    }
    if (err == ESP_ERR_NOT_FINISHED) {
        return send_error(req, "409 Conflict", "a check is already running");
    }
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "the check could not start");
    }
    return send_ok(req, "202 Accepted");
}

static esp_err_t wifi_test_get(httpd_req_t *req)
{
    static const char *const NAMES[] = {
        [WIFI_TEST_IDLE] = "idle",           [WIFI_TEST_RUNNING] = "checking",
        [WIFI_TEST_JOINED] = "joined",       [WIFI_TEST_WRONG_PASSWORD] = "wrong_password",
        [WIFI_TEST_NOT_FOUND] = "not_found", [WIFI_TEST_NO_ADDRESS] = "no_address",
        [WIFI_TEST_FAILED] = "failed",       [WIFI_TEST_NOT_SAVED] = "not_saved",
    };
    wifi_test_status_t t;
    wifi_manager_test_status(&t);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", NAMES[t.state]);
    cJSON_AddStringToObject(root, "ssid", t.ssid);
    cJSON_AddNumberToObject(root, "reason", t.reason);
    if (t.ip[0]) {
        cJSON_AddStringToObject(root, "ip", t.ip);
    } else {
        cJSON_AddNullToObject(root, "ip");
    }
    return send_json(req, root, "200 OK");
}

static esp_err_t wifi_resume_post(httpd_req_t *req)
{
    wifi_mgr_status_t wifi;
    wifi_manager_get_status(&wifi);
    if (!wifi.temporary) {
        return send_error(req, "409 Conflict", "the setup hotspot was not started by the button");
    }
    ESP_LOGI(TAG, "Leaving the setup hotspot for \"%s\"", wifi.home_ssid);
    send_ok(req, "200 OK");
    xTaskCreate(reboot_task, "reboot", 3072, NULL, 5, NULL);
    return ESP_OK;
}

#if CONFIG_ZBC_CRASH_TEST
/* The crash CONFIG_ZBC_CRASH_TEST offers, in a task of its own so the answer gets
 * out first and the record names a task that is not the web server. */
static void crash_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    if (arg) {
        abort();
    }
    /* Below RAM, ROM and flash: a load access fault, with this as mtval. Through
     * a volatile, or the compiler sees the constant and warns instead. */
    volatile uintptr_t bad = 0x10;
    (void)*(volatile uint32_t *)bad;
    vTaskDelete(NULL);
}
#endif

/* POST /api/restart - a restart and nothing else; everything stored stays. An
 * image on trial is confirmed on the way out once the station has joined
 * (ota.c's confirm_on_restart), so a restart then is not a failed update. Before
 * that it would be: the bootloader would roll back to the updater, which looks
 * exactly like a crash, so it is refused instead. */
static esp_err_t restart_post(httpd_req_t *req)
{
    if (ota_pending_verify() && !wifi_manager_station_joined()) {
        return send_error(req, "409 Conflict",
                          "the new firmware is still on trial - it is confirmed once Wi-Fi is up, or after five minutes");
    }
#if CONFIG_ZBC_CRASH_TEST
    /* {"crash": "abort" | "fault"}: restart by crashing, to test crash_log.c. A
     * crash does not pass through confirm_on_restart, so an image on trial is
     * refused even once the station has joined. */
    cJSON *json = NULL;
    if (req->content_len > 0 && !(json = read_json_body(req))) {
        return ESP_FAIL;                   /* answered already */
    }
    if (json) {
        const cJSON *crash = cJSON_GetObjectItem(json, "crash");
        const bool   want_abort = cJSON_IsString(crash) && strcmp(crash->valuestring, "abort") == 0;
        const bool   want_fault = cJSON_IsString(crash) && strcmp(crash->valuestring, "fault") == 0;
        cJSON_Delete(json);
        if (want_abort || want_fault) {
            if (ota_pending_verify()) {
                return send_error(req, "409 Conflict", "the new firmware is still on trial");
            }
            ESP_LOGW(TAG, "Crash requested over HTTP (%s)", want_abort ? "abort" : "fault");
            send_ok(req, "200 OK");
            xTaskCreate(crash_task, "crash", 3072, want_abort ? (void *)1 : NULL, 5, NULL);
            return ESP_OK;
        }
    }
#endif
    ESP_LOGW(TAG, "Restart requested over HTTP");
    send_ok(req, "200 OK");
    xTaskCreate(reboot_task, "reboot", 3072, NULL, 5, NULL);
    return ESP_OK;
}

/* POST /api/factory_reset  {"scope": "all" | "zigbee"}
 *
 * "zigbee" forms a new network, on the channel the Settings page last moved it
 * to, without taking the Wi-Fi credentials with it, so the coordinator comes
 * back on the network instead of in the setup portal. */
static esp_err_t factory_reset_post(httpd_req_t *req)
{
    bool   zigbee_only = false;
    cJSON *json        = NULL;
    /* A body that could not be read has been answered, and must not fall through
     * to the default - which is the reset that erases everything. */
    if (req->content_len > 0 && !(json = read_json_body(req))) {
        return ESP_FAIL;
    }
    if (json) {
        cJSON     *scope = cJSON_GetObjectItem(json, "scope");
        const bool known = !scope || (cJSON_IsString(scope) && (strcmp(scope->valuestring, "all") == 0 ||
                                                                strcmp(scope->valuestring, "zigbee") == 0));
        zigbee_only      = known && scope && strcmp(scope->valuestring, "zigbee") == 0;
        cJSON_Delete(json);
        /* A scope this build does not know is not read as "everything". */
        if (!known) {
            return send_error(req, "400 Bad Request", "scope must be all or zigbee");
        }
    }
    /* Leaving the network needs the stack, which the setup hotspot never starts
     * (and the fallback AP stops): refused before anything is erased, rather than
     * half a reset and then a crash on the stack's lock. */
    if (!zb_is_running()) {
        return send_error(req, "409 Conflict",
                          "the Zigbee network is not running in the setup hotspot - reset it once the "
                          "coordinator is back on its Wi-Fi");
    }

    ESP_LOGW(TAG, "%s reset requested over HTTP", zigbee_only ? "Zigbee network" : "Factory");
    send_ok(req, "200 OK");
    if (!zigbee_only) {
        wifi_manager_erase_credentials();
        /* Before the fabric goes: every Matter lamp is asked to forget us while
         * we can still talk to it, for a few seconds and no longer. */
        mctl_forget_all(4000);
        mlight_db_erase();
        /* A Zigbee-only reset keeps the controllers: they lose the groups with
         * the lamps, and gain the new ones as they are made. */
        matter_bridge_erase();
        /* With the Wi-Fi gone the box comes back as the setup portal, which is
         * open anyway - and a key nobody can use any more is not worth keeping. */
        auth_erase();
        /* Not part of the Zigbee network, so a network-only reset keeps them. */
        light_db_erase();
        local_lights_reload();
    }
    group_db_erase();
    al_db_erase();
    curve_db_erase();
    button_db_erase();
    buttons_reload();
    zb_factory_reset();  /* leaves the Zigbee network and reboots */
    xTaskCreate(reboot_task, "reboot", 3072, NULL, 5, NULL);
    return ESP_OK;
}

/* ---- Configuration backup (config_backup.h) ---- */

#if CONFIG_ZBC_CONFIG_BACKUP

/* The value of `key` in the query string, with a "%2C" read as the comma it
 * is - what a browser's URLSearchParams makes of "wifi,auth". Empty when the
 * key is absent; false only for one too long to be a list of sections. */
static bool query_list(httpd_req_t *req, const char *key, char *out, size_t size)
{
    char query[96];
    out[0] = '\0';
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return true;
    }
    const esp_err_t err = httpd_query_key_value(query, key, out, size);
    if (err == ESP_ERR_HTTPD_RESULT_TRUNC) {
        return false;
    }
    if (err != ESP_OK) {
        out[0] = '\0';
        return true;
    }
    char *w = out;
    for (const char *r = out; *r; r++) {
        if (r[0] == '%' && r[1] == '2' && (r[2] == 'C' || r[2] == 'c')) {
            *w++ = ',';
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
    return true;
}

/* The download's way out: the json_stream buffer, sent a chunk at a time. */
static esp_err_t backup_sink(void *ctx, const char *text, size_t len)
{
    json_stream_t *js = ctx;
    while (len) {
        if (js->len == JSON_STREAM_BUF) {
            json_stream_flush(js);
        }
        const size_t take = MIN(len, JSON_STREAM_BUF - js->len);
        memcpy(js->buf + js->len, text, take);
        js->len += take;
        text += take;
        len -= take;
    }
    return js->failed ? ESP_FAIL : ESP_OK;
}

/* GET /api/backup?omit=wifi,auth - the configuration as a file, streamed: one
 * table in memory at a time. */
static esp_err_t backup_get(httpd_req_t *req)
{
    char    list[32];
    uint8_t omit;
    if (!query_list(req, "omit", list, sizeof(list)) || !config_backup_parse_secrets(list, &omit)) {
        return send_error(req, "400 Bad Request", "omit takes wifi and auth");
    }
    json_stream_t js;
    if (!json_stream_begin(&js, req)) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    httpd_resp_set_type(req, "application/x-ndjson");
    /* Kept until the first chunk goes out, which is inside this function. */
    char disposition[80];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s-config.jsonl\"", settings_hostname());
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    if (config_backup_export(omit, backup_sink, &js) != ESP_OK) {
        /* Cut short, never ended cleanly: a file without its last line is
         * one the restore refuses, rather than one that restores half. */
        return json_stream_fail(&js);
    }
    return json_stream_end(&js);
}

/* The largest file any build writes is ~30 KB (a full registry, every table). */
#define RESTORE_MAX_LEN (64 * 1024)

typedef struct {
    httpd_req_t *req;
    size_t       left;
    int          timeouts;
} restore_body_t;

static int restore_read(void *ctx, char *buf, size_t len)
{
    restore_body_t *b = ctx;
    while (b->left) {
        const int n = httpd_req_recv(b->req, buf, MIN(len, b->left));
        /* Bounded, as read_json_body is: one server task, and a client that
         * stops sending must not hold it. */
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++b->timeouts < 3) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        b->left -= (size_t)n;
        return n;
    }
    return 0;
}

/* POST /api/restore?keep=wifi,auth - a file from GET /api/backup, as the raw
 * body. Staged, then the box restarts and the boot carries it out. */
static esp_err_t restore_post(httpd_req_t *req)
{
    char    list[32];
    uint8_t keep;
    if (!query_list(req, "keep", list, sizeof(list)) || !config_backup_parse_secrets(list, &keep)) {
        return send_error(req, "400 Bad Request", "keep takes wifi and auth");
    }
    /* Whether the devices' addresses still hold is a question about the
     * network that is up; clearing them on the same network would leave every
     * remote unknown until it is paired again. */
    zb_netinfo_t zb;
    zb_get_netinfo(&zb);
    if (!zb.network_up) {
        return send_error(req, "409 Conflict", "the Zigbee network is not up yet");
    }
    if (req->content_len <= 0) {
        return send_error(req, "400 Bad Request", "no file");
    }
    if (req->content_len > RESTORE_MAX_LEN) {
        return send_error(req, "413 Payload Too Large", "the file is larger than any configuration file");
    }

    restore_body_t  body = { .req = req, .left = req->content_len };
    cb_result_t     res;
    const esp_err_t err  = config_backup_stage(restore_read, &body, req->content_len, keep, &res);
    if (err != ESP_OK) {
        /* Read to the end before answering: a client still sending when the
         * answer comes reports a broken connection instead of the reason. */
        char sink[128];
        while (restore_read(&body, sink, sizeof(sink)) > 0) {
        }
        const char *status = err == ESP_ERR_INVALID_ARG            ? "400 Bad Request"
                           : err == ESP_ERR_NVS_NOT_ENOUGH_SPACE   ? "507 Insufficient Storage"
                           : err == ESP_ERR_TIMEOUT                ? "408 Request Timeout"
                                                                   : "500 Internal Server Error";
        return send_error(req, status, res.error[0] ? res.error : esp_err_to_name(err));
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddBoolToObject(r, "addresses_kept", res.addresses_kept);
    cJSON_AddNumberToObject(r, "entries", res.entries);
    cJSON_AddNumberToObject(r, "skipped", res.skipped);
    cJSON_AddBoolToObject(r, "wifi", res.wifi);
    cJSON_AddBoolToObject(r, "auth", res.auth);
    ESP_LOGW(TAG, "Configuration restore staged over HTTP; restarting to apply it");
    send_json(req, r, "200 OK");
    xTaskCreate(reboot_task, "reboot", 3072, NULL, 5, NULL);
    return ESP_OK;
}

#endif /* CONFIG_ZBC_CONFIG_BACKUP */

/* The Matter lamps' side of /api/matter: our fabric, each device with its
 * subscription, and the pairing in progress. */
static void add_controller_json(cJSON *root, uint32_t now_ms)
{
    static mctl_status_t cs;       /* static for the same reason as matter_get's */
    mctl_get_status(&cs);
    const int64_t now_us = (int64_t)now_ms * 1000;
    char hex[19];
    cJSON *c = cJSON_AddObjectToObject(root, "controller");
    cJSON_AddBoolToObject(c, "running", cs.running);
    cJSON_AddNumberToObject(c, "fabric_index", cs.fabric_index);
    cJSON_AddStringToObject(c, "fabric_error", cs.fabric_error);
    snprintf(hex, sizeof(hex), "%016llX", (unsigned long long)cs.own_node_id);
    cJSON_AddStringToObject(c, "node_id", cs.fabric_index ? hex : "");
    cJSON_AddNumberToObject(c, "commands_sent", cs.commands_sent);
    cJSON_AddNumberToObject(c, "commands_failed", cs.commands_failed);
    cJSON_AddNumberToObject(c, "commands_dropped", cs.commands_dropped);
    cJSON_AddNumberToObject(c, "gate_timeouts", cs.gate_timeouts);
    cJSON_AddNumberToObject(c, "chip_stack_unused", cs.chip_stack_unused);
    cJSON *nodes = cJSON_AddArrayToObject(c, "nodes");
    for (int i = 0; i < cs.nodes; i++) {
        const mctl_node_status_t *n = &cs.node[i];
        cJSON *o = cJSON_CreateObject();
        snprintf(hex, sizeof(hex), "%016llX", (unsigned long long)n->node_id);
        cJSON_AddStringToObject(o, "node_id", hex);
        cJSON_AddNumberToObject(o, "lamps", n->lamps);
        cJSON_AddBoolToObject(o, "online", n->online);
        cJSON_AddBoolToObject(o, "subscribed", n->subscribed);
        cJSON_AddNumberToObject(o, "drops", n->drops);
        cJSON_AddNumberToObject(o, "reports", n->reports);
        cJSON_AddNumberToObject(o, "last_seen_s_ago",
                                n->last_seen_us ? (double)((now_us - n->last_seen_us) / 1000000) : -1);
        cJSON_AddNumberToObject(o, "down_s", n->down_since_us ? (double)((now_us - n->down_since_us) / 1000000) : 0);
        snprintf(hex, sizeof(hex), "0x%08" PRIx32, n->last_error);
        cJSON_AddStringToObject(o, "last_error", n->last_error ? hex : "");
        cJSON_AddNumberToObject(o, "heap_low_up", n->heap_low_up);
        cJSON_AddItemToArray(nodes, o);
    }
    static const char *const PAIR_STATES[] = { "idle", "running", "done", "failed" };
    cJSON *p = cJSON_AddObjectToObject(c, "pairing");
    cJSON_AddStringToObject(p, "state", PAIR_STATES[cs.pair_state < 4 ? cs.pair_state : 0]);
    cJSON_AddStringToObject(p, "step", cs.pair_step);
    cJSON_AddStringToObject(p, "error", cs.pair_error);
    snprintf(hex, sizeof(hex), "%016llX", (unsigned long long)cs.pair_node_id);
    cJSON_AddStringToObject(p, "node_id", cs.pair_node_id ? hex : "");
    cJSON_AddNumberToObject(p, "lamps", cs.pair_lamps);
    cJSON_AddNumberToObject(p, "s_ago", cs.pair_ago_s);
    /* Where the heap went during the last pairing, step by step. */
    cJSON *hl = cJSON_AddObjectToObject(p, "heap_low");
    for (int i = 0; i < MCTL_PAIR_STEPS; i++) {
        if (cs.pair_heap_low[i] && cs.pair_step_names[i]) {
            cJSON_AddNumberToObject(hl, cs.pair_step_names[i], cs.pair_heap_low[i]);
        }
    }
}

/* GET /api/matter - what a controller needs to add the bridge, and whether one
 * has. `running` is false in the setup portal and on a board whose partition
 * table predates Matter. */
static esp_err_t matter_get(httpd_req_t *req)
{
    /* Static: close to 1 KB, and httpd serves one request at a time. */
    static matter_status_t st;
    matter_bridge_get_status(&st);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "running", st.running);
    cJSON_AddNumberToObject(root, "fabrics", st.fabric_count);
    /* Who they are. A controller can add more than one fabric, so "2" on its own
     * does not say whether that is two ecosystems or one that took two. */
    cJSON *list = cJSON_AddArrayToObject(root, "fabric_list");
    for (int i = 0; i < st.fabric_listed; i++) {
        cJSON *f = cJSON_CreateObject();
        char   vid[7];
        snprintf(vid, sizeof(vid), "0x%04X", st.fabrics[i].vendor_id);
        cJSON_AddNumberToObject(f, "index", st.fabrics[i].index);
        cJSON_AddStringToObject(f, "vendor_id", vid);
        cJSON_AddStringToObject(f, "label", st.fabrics[i].label);
        /* This box's own, for the Matter lamps: not a controller. */
        cJSON_AddBoolToObject(f, "ours", st.fabrics[i].ours);
        /* Whether the controller can see the bridge, and how soon after boot it
         * could again. `kept` is what CHIP resumes after a restart; the intervals
         * are the controller's, and `resume_tries` counts failed attempts. */
        const matter_fabric_t *fb = &st.fabrics[i];
        cJSON *subs = cJSON_AddObjectToObject(f, "subscriptions");
        cJSON_AddNumberToObject(subs, "live", fb->subs_live);
        cJSON_AddNumberToObject(subs, "kept", fb->subs_kept);
        if (fb->subs_kept) {
            cJSON_AddNumberToObject(subs, "min_interval_s", fb->sub_min_s);
            cJSON_AddNumberToObject(subs, "max_interval_s", fb->sub_max_s);
            cJSON_AddNumberToObject(subs, "resume_tries", fb->resume_tries);
        }
        if (fb->live_after_s >= 0) {
            cJSON_AddNumberToObject(subs, "live_after_s", fb->live_after_s);
        } else {
            cJSON_AddNullToObject(subs, "live_after_s");
        }
        cJSON_AddItemToArray(list, f);
    }
    cJSON_AddBoolToObject(root, "window_open", st.window_open);
    cJSON_AddNumberToObject(root, "groups", st.groups);
    /* Per group: the group's state, Matter's copy of it, and the write between
     * them - so "the controller does not follow" says which of the three is off. */
    cJSON *gl = cJSON_AddArrayToObject(root, "group_list");
    for (int i = 0; i < st.groups; i++) {
        const matter_group_t *d = &st.group_list[i];
        cJSON *g = cJSON_CreateObject();
        cJSON_AddNumberToObject(g, "group", d->group_id);
        cJSON_AddNumberToObject(g, "endpoint", d->endpoint_id);
        cJSON_AddNumberToObject(g, "group_on", d->group_on);
        cJSON_AddNumberToObject(g, "group_pct", d->group_pct);
        cJSON_AddNumberToObject(g, "matter_on", d->matter_on);
        cJSON_AddNumberToObject(g, "matter_level", d->matter_level);
        cJSON_AddBoolToObject(g, "held", d->held);
        cJSON_AddNumberToObject(g, "writes", d->writes);
        cJSON_AddStringToObject(g, "last_err", d->last_err ? esp_err_to_name(d->last_err) : "");
        /* Adaptive Lighting: how the group asks to be exposed, what the running
         * endpoints actually have - Mode Select only changes at a restart - and
         * the mode as each side sees it. */
        cJSON_AddStringToObject(g, "expose", d->expose < ZBC_AL_EXPOSE_COUNT ? AL_EXPOSE_NAMES[d->expose] : "off");
        cJSON_AddBoolToObject(g, "mode_select_live", d->mode_select_live);
        cJSON_AddNumberToObject(g, "al_endpoint", d->al_endpoint);
        cJSON_AddNumberToObject(g, "al_active", d->al_active);
        cJSON_AddNumberToObject(g, "al_matter_on", d->al_matter_on);
        cJSON_AddNumberToObject(g, "matter_mode", d->matter_mode);
        cJSON_AddItemToArray(gl, g);
    }
    cJSON_AddStringToObject(root, "manual_code", st.manual_code);
    cJSON_AddStringToObject(root, "qr_code", st.qr_code);
    /* The last commands controllers sent, oldest first, as they arrived - which
     * two controllers disagreeing about the same light comes down to. */
    const uint32_t now  = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t seen = st.commands_seen;
    const uint32_t kept = seen < MATTER_RECENT_COMMANDS ? seen : MATTER_RECENT_COMMANDS;
    cJSON *cl = cJSON_AddArrayToObject(root, "commands");
    for (uint32_t i = seen - kept; i != seen; i++) {
        const matter_command_t *c = &st.commands[i % MATTER_RECENT_COMMANDS];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "ago_ms", now - c->at_ms);
        cJSON_AddNumberToObject(o, "fabric", c->fabric);
        cJSON_AddNumberToObject(o, "endpoint", c->endpoint);
        cJSON_AddNumberToObject(o, "cluster", c->cluster);
        cJSON_AddNumberToObject(o, "cmd", c->command);
        if (c->level) {
            cJSON_AddNumberToObject(o, "level", c->level);
        } else {
            cJSON_AddNullToObject(o, "level");
        }
        if (c->transition_ds != MATTER_NO_TRANSITION) {
            cJSON_AddNumberToObject(o, "transition_ds", c->transition_ds);
        } else {
            cJSON_AddNullToObject(o, "transition_ds");
        }
        cJSON_AddItemToArray(cl, o);
    }
    add_controller_json(root, now);
    return send_json(req, root, "200 OK");
}

/* POST /api/matter/lamps/pair  {"code": "3497-011-2332" | "MT:..."}
 * The code of a device in pairing mode - Apple Home's "Turn on pairing mode" gives
 * one out. 202 and the progress in GET /api/matter -> controller.pairing. */
static esp_err_t matter_pair_post(httpd_req_t *req)
{
    cJSON *json = read_json_body(req);
    if (!json) {
        return ESP_FAIL;
    }
    const cJSON *code = cJSON_GetObjectItem(json, "code");
    esp_err_t    err  = cJSON_IsString(code) ? mctl_pair(code->valuestring) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(json);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_error(req, "400 Bad Request", "that is not a Matter pairing code");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error(req, "409 Conflict",
                          mctl_running() ? "a pairing is already running" : "Matter is not running");
    }
    if (err == MCTL_ERR_NO_FABRIC_SLOT) {
        return send_error(req, "409 Conflict",
                          "this box has no Matter fabric left for its lamps - remove a controller first");
    }
    if (err == MCTL_ERR_BRIDGE_BUSY) {
        return send_error(req, "409 Conflict", "a controller is adding this box right now - try again in a minute");
    }
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    return send_ok(req, "202 Accepted");
}

/* POST /api/matter/nodes/{node}/rescan - reads a paired device's endpoints
 * again; lamps added to a hub after it was paired appear only this way. */
static esp_err_t matter_rescan_post(httpd_req_t *req, const char *node_str)
{
    char *end = NULL;
    const unsigned long long node = strtoull(node_str, &end, 16);
    if (!end || *end != '\0' || node == 0) {
        return send_error(req, "400 Bad Request", "malformed node id");
    }
    esp_err_t err = mctl_rescan_node(node);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_error(req, "404 Not Found", "no lamp of that device is paired");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error(req, "409 Conflict", "Matter is not running");
    }
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    return send_ok(req, "202 Accepted");
}

/* POST /api/matter/window - the window opened at boot closes after 15 minutes;
 * this is how a controller gets another chance without a reboot. */
static esp_err_t matter_window_post(httpd_req_t *req)
{
    esp_err_t err = matter_bridge_open_window();
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error(req, "409 Conflict", "Matter is not running");
    }
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    return send_ok(req, "200 OK");
}

/* POST /api/matter/reset - forgets every controller, keeps everything else. */
/* ------------------------------------------------ Zigbee device firmware */

/* /api/zigbee/ota                 GET     the image, its target and the transfer
 * /api/zigbee/ota/image           POST    multipart upload of one OTA file
 *                                 DELETE  forget the image
 * /api/zigbee/ota/notify          POST    {"ieee": "..."} names the device the
 *                                         image is for and asks it to ask; {}
 *                                         asks every listening device, and
 *                                         names none
 * /api/zigbee/ota/target          POST    {"ieee": "..." | null}, without asking
 * /api/zigbee/ota/ask             POST    {"ieee": "..."} makes that device say
 *                                         what it runs; offers nothing, needs
 *                                         no image
 *
 * The browser fetches images from the Zigbee2MQTT index and strips their
 * wrappers; this end only takes a plain OTA file. See zb_ota.h. */
static esp_err_t zigbee_ota_dispatch(httpd_req_t *req)
{
    if (req->method != HTTP_GET) {
        ESP_LOGI(TAG, "<- %s", req->uri);
    }
    char what[16];
    if (!uri_segment(req->uri, 3, what, sizeof(what))) {
        if (req->method != HTTP_GET) {
            return send_error(req, "405 Method Not Allowed", "GET only");
        }
        return send_json(req, zb_ota_status_json(), "200 OK");
    }

    if (strcmp(what, "image") == 0 && (req->method == HTTP_POST || req->method == HTTP_DELETE)) {
        const char *why;
        if (!zb_ota_begin_write(&why)) {
            return send_error(req, "409 Conflict", why);
        }
        /* The stack reads the image through a mapping of the flash about to
         * be erased; it lets go first. zb_ota_image refuses from begin_write
         * on, so nothing re-attaches the old mapping in between. */
        zb_ota_server_detach();
        esp_err_t sent;
        if (req->method == HTTP_POST) {
            sent = zb_ota_handle_upload(req);
        } else {
            zb_ota_forget();
            sent = send_ok(req, "200 OK");
        }
        zb_ota_end_write();
        zb_ota_server_attach();   /* ESP_ERR_INVALID_STATE: no network yet, network-up attaches */
        return sent;
    }

    const bool ask = strcmp(what, "ask") == 0;
    if ((strcmp(what, "notify") == 0 || strcmp(what, "target") == 0 || ask) && req->method == HTTP_POST) {
        const bool notify = what[0] == 'n';
        uint8_t    ieee[ZBC_IEEE_LEN];
        bool       named = false;
        if (req->content_len > 0) {
            cJSON *body = read_json_body(req);
            if (!body) {
                return ESP_OK;
            }
            const cJSON *j = cJSON_GetObjectItem(body, "ieee");
            if (cJSON_IsString(j)) {
                named = zbc_str_to_ieee(j->valuestring, ieee);
                if (!named) {
                    cJSON_Delete(body);
                    return send_error(req, "400 Bad Request", "malformed device address");
                }
            } else if (j && !cJSON_IsNull(j)) {
                cJSON_Delete(body);
                return send_error(req, "400 Bad Request", "ieee must be a device address or null");
            }
            cJSON_Delete(body);
        }
        if (named && zbc_light_id_from_ieee(ieee)) {
            return send_error(req, "400 Bad Request", "a wired light has no Zigbee firmware");
        }
        if (named && !device_db_get(ieee, NULL)) {
            return send_error(req, "404 Not Found", "unknown device");
        }
        if (ask) {
            if (!named) {
                return send_error(req, "400 Bad Request", "ieee is required");
            }
            esp_err_t err = zb_ota_server_ask(ieee);
            if (err == ESP_ERR_INVALID_STATE) {
                return send_error(req, "409 Conflict", "the Zigbee network is not up");
            }
            if (err == ZB_ERR_UNREACHABLE) {
                return send_error(req, "409 Conflict", "the device has not joined this network yet");
            }
            if (err != ESP_OK) {
                return send_error(req, "500 Internal Server Error", "the stack refused the Image Notify");
            }
            return send_json(req, zb_ota_status_json(), "202 Accepted");
        }
        const void *ptr;
        uint32_t    len;
        if (named && !zb_ota_image(&ptr, &len)) {
            return send_error(req, "409 Conflict", "no image is stored");
        }
        if (!notify) {
            zb_ota_set_target(named ? ieee : NULL);
            return send_json(req, zb_ota_status_json(), "200 OK");
        }
        if (named) {
            zb_ota_set_target(ieee);
        }
        esp_err_t err = zb_ota_server_notify(named ? ieee : NULL);
        if (err == ESP_ERR_INVALID_STATE) {
            return send_error(req, "409 Conflict", "the Zigbee network is not up");
        }
        if (err == ESP_ERR_NOT_FOUND) {
            return send_error(req, "404 Not Found", "unknown device");
        }
        if (err == ZB_ERR_UNREACHABLE) {
            return send_error(req, "409 Conflict", "the device has not joined this network yet");
        }
        if (err != ESP_OK) {
            return send_error(req, "500 Internal Server Error", "the stack refused the Image Notify");
        }
        return send_json(req, zb_ota_status_json(), "202 Accepted");
    }
    return send_error(req, "404 Not Found", "no such route");
}

/* /api/updater/image (POST, multipart elixcon_light_hub_updater.bin) and /api/updater/run
 * (POST) - see ota.h. */
static esp_err_t updater_dispatch(httpd_req_t *req)
{
    ESP_LOGI(TAG, "<- %s", req->uri);
    char what[8];
    if (!uri_segment(req->uri, 2, what, sizeof(what))) {
        return send_error(req, "404 Not Found", "no such route");
    }
    if (strcmp(what, "image") == 0) {
        return ota_handle_updater_image(req);
    }
    if (strcmp(what, "run") == 0) {
        return ota_handle_updater_run(req);
    }
    return send_error(req, "404 Not Found", "no such route");
}

static esp_err_t matter_reset_post(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Matter reset requested over HTTP");
    /* Our fabric goes with the rest: every Matter lamp is asked to forget us
     * first, while that is still possible. The rows stay, as needing pairing
     * again, so groups and curves keep naming them. */
    mctl_forget_all(4000);
    esp_err_t err = matter_bridge_erase();
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    send_ok(req, "200 OK");
    xTaskCreate(reboot_task, "reboot", 3072, NULL, 5, NULL);
    return ESP_OK;
}

/* /api/matter/window         POST  opens a commissioning window for a controller
 * /api/matter/reset          POST  forgets every controller and our own fabric
 * /api/matter/lamps/pair     POST  pairs a Matter lamp from its code
 * /api/matter/nodes/{n}/rescan POST reads a paired device's endpoints again */
static esp_err_t matter_post_dispatch(httpd_req_t *req)
{
    ESP_LOGI(TAG, "<- %s", req->uri);
    char a[16], b[24], c[16];
    if (!uri_segment(req->uri, 2, a, sizeof(a))) {
        return send_error(req, "404 Not Found", "no such Matter route");
    }
    if (strcmp(a, "window") == 0) {
        return matter_window_post(req);
    }
    if (strcmp(a, "reset") == 0) {
        return matter_reset_post(req);
    }
    if (strcmp(a, "lamps") == 0 && uri_segment(req->uri, 3, b, sizeof(b)) && strcmp(b, "pair") == 0) {
        return matter_pair_post(req);
    }
    if (strcmp(a, "nodes") == 0 && uri_segment(req->uri, 3, b, sizeof(b)) &&
        uri_segment(req->uri, 4, c, sizeof(c)) && strcmp(c, "rescan") == 0) {
        return matter_rescan_post(req, b);
    }
    return send_error(req, "404 Not Found", "no such Matter route");
}

/* ------------------------------------------------------------- /api/auth */

/* GET /api/auth - not guarded: it is how a locked page learns that a button has
 * been pressed five times, and it says nothing a locked caller should not know. */
static esp_err_t auth_state_get(httpd_req_t *req)
{
    return send_json(req, auth_json(req), "200 OK");
}

static esp_err_t send_session(httpd_req_t *req, const zbc_auth_key_t *k, const char *shown)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", k->id);
    cJSON_AddStringToObject(root, "name", k->name);
    cJSON_AddBoolToObject(root, "issued", shown != NULL);
    if (shown) {
        cJSON_AddStringToObject(root, "key", shown);
    }
    return send_json(req, root, shown ? "201 Created" : "200 OK");
}

/* POST /api/auth/session  {"name": "Safari on macOS"} or {"key": "K7M2Q-..."}
 *
 * Not guarded, because it does its own check and has to answer a locked caller
 * who types a key in. Three ways through:
 *
 *  - with "key": a key somebody wrote down. The cookie becomes that same key,
 *    so signing in on a phone and a laptop does not use up two of the slots;
 *  - already carrying a valid key: that key, and nothing is created;
 *  - otherwise, while access is open: a new browser key, shown once in the
 *    response so it can be written down. That closes the failsafe window, which
 *    has done its job the moment somebody is in. */
static esp_err_t auth_session_post(httpd_req_t *req)
{
    char name[ZBC_AUTH_NAME_LEN] = "";
    char typed[64]               = "";
    bool has_typed               = false;
    if (req->content_len > 0) {
        cJSON *json = read_json_body(req);
        if (!json) {
            return ESP_FAIL;
        }
        cJSON *n = cJSON_GetObjectItem(json, "name");
        cJSON *t = cJSON_GetObjectItem(json, "key");
        if (cJSON_IsString(n)) {
            strlcpy(name, n->valuestring, sizeof(name));
        }
        if (cJSON_IsString(t)) {
            strlcpy(typed, t->valuestring, sizeof(typed));
            has_typed = true;
        }
        cJSON_Delete(json);
    }

    char           cookie[96];
    char           canon[ZBC_AUTH_KEY_CANON_LEN];
    zbc_auth_key_t k;

    if (has_typed) {
        const uint16_t id = auth_check(typed);
        if (!id || !auth_get(id, &k)) {
            ESP_LOGW(TAG, "Sign-in with a key that is not known here");
            return send_error(req, "401 Unauthorized", "That key is not known here");
        }
        auth_touch(id);
        auth_normalize(typed, canon);
        auth_http_set_cookie(req, cookie, sizeof(cookie), canon);
        ESP_LOGI(TAG, "A browser signed in with key %u (\"%s\")", k.id, k.name);
        return send_session(req, &k, NULL);
    }

    bool     from_cookie = false;
    uint16_t id          = auth_http_presented_key(req, &from_cookie, canon);
    if (id && auth_get(id, &k)) {
        auth_touch(id);
        if (!from_cookie) {
            auth_http_set_cookie(req, cookie, sizeof(cookie), canon);
        }
        return send_session(req, &k, NULL);
    }
    if (access_mode() == ACCESS_LOCKED) {
        return send_error(req, "401 Unauthorized", "locked");
    }

    char shown[ZBC_AUTH_KEY_SHOWN_LEN];
    esp_err_t err = auth_create(ZBC_KEY_BROWSER, name, shown, &id);
    if (err == ESP_ERR_NO_MEM) {
        return send_error(req, "409 Conflict",
                          "every key slot is in use - revoke one under Settings, Access");
    }
    if (err != ESP_OK || !auth_get(id, &k)) {
        return send_error(req, "500 Internal Server Error", "could not store the key");
    }
    auth_failsafe_close();
    auth_canonical(shown, canon);
    auth_http_set_cookie(req, cookie, sizeof(cookie), canon);
    return send_session(req, &k, shown);
}

/* GET /api/auth/keys */
static esp_err_t auth_keys_get(httpd_req_t *req)
{
    zbc_auth_key_t keys[CONFIG_ZBC_MAX_AUTH_KEYS];
    const size_t   n    = auth_snapshot(keys, CONFIG_ZBC_MAX_AUTH_KEYS);
    const uint16_t mine = auth_http_presented_key(req, NULL, NULL);
    const int64_t  now  = esp_timer_get_time();

    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(root, "keys");
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", keys[i].id);
        cJSON_AddStringToObject(o, "kind", keys[i].kind == ZBC_KEY_API ? "api" : "browser");
        cJSON_AddStringToObject(o, "name", keys[i].name);
        /* Since boot: there is no wall clock, and this never touches flash. */
        cJSON_AddNumberToObject(o, "last_used_s_ago",
                                keys[i].last_used_us ? (double)((now - keys[i].last_used_us) / 1000000) : -1);
        cJSON_AddBoolToObject(o, "current", keys[i].id == mine);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddNumberToObject(root, "max", CONFIG_ZBC_MAX_AUTH_KEYS);
    return send_json(req, root, "200 OK");
}

/* POST /api/auth/keys  {"name": "Home Assistant"} - the key is in the response
 * and nowhere else, ever. */
static esp_err_t auth_keys_post(httpd_req_t *req)
{
    char name[ZBC_AUTH_NAME_LEN] = "";
    if (req->content_len > 0) {
        cJSON *json = read_json_body(req);
        if (!json) {
            return ESP_FAIL;
        }
        cJSON *n = cJSON_GetObjectItem(json, "name");
        if (cJSON_IsString(n)) {
            strlcpy(name, n->valuestring, sizeof(name));
        }
        cJSON_Delete(json);
    }

    char           shown[ZBC_AUTH_KEY_SHOWN_LEN];
    uint16_t       id;
    zbc_auth_key_t k;
    esp_err_t err = auth_create(ZBC_KEY_API, name, shown, &id);
    if (err == ESP_ERR_NO_MEM) {
        return send_error(req, "409 Conflict", "every key slot is in use - revoke one first");
    }
    if (err != ESP_OK || !auth_get(id, &k)) {
        return send_error(req, "500 Internal Server Error", "could not store the key");
    }
    return send_session(req, &k, shown);
}

/* DELETE /api/auth/keys/{id} */
static esp_err_t auth_key_delete(httpd_req_t *req, uint16_t id)
{
    const uint16_t mine = auth_http_presented_key(req, NULL, NULL);
    if (auth_revoke(id) == ESP_ERR_NOT_FOUND) {
        return send_error(req, "404 Not Found", "unknown key");
    }
    char cookie[96];
    if (id == mine) {
        auth_http_set_cookie(req, cookie, sizeof(cookie), NULL);
    }
    /* The mode, so the UI can say so when that was the last key and the
     * coordinator is now open to everyone. */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "mode", ACCESS_NAMES[access_mode()]);
    return send_json(req, root, "200 OK");
}

/* Everything under /api/auth/ but the session, which is registered on its own
 * and unguarded. */
static esp_err_t auth_dispatch(httpd_req_t *req)
{
    if (req->method != HTTP_GET) {
        ESP_LOGI(TAG, "<- %s", req->uri);
    }
    char what[8];
    if (!uri_segment(req->uri, 2, what, sizeof(what))) {
        return send_error(req, "404 Not Found", "no such auth route");
    }
    if (strcmp(what, "lock") == 0 && req->method == HTTP_POST) {
        /* Ends the failsafe early, for somebody who is already in. */
        auth_failsafe_close();
        return send_ok(req, "200 OK");
    }
    if (strcmp(what, "keys") != 0) {
        return send_error(req, "404 Not Found", "no such auth route");
    }
    char id_str[8];
    if (!uri_segment(req->uri, 3, id_str, sizeof(id_str))) {
        if (req->method == HTTP_GET) {
            return auth_keys_get(req);
        }
        if (req->method == HTTP_POST) {
            return auth_keys_post(req);
        }
    } else if (req->method == HTTP_DELETE) {
        int id = atoi(id_str);
        if (id <= 0 || id > 0xffff) {
            return send_error(req, "400 Bad Request", "malformed key id");
        }
        return auth_key_delete(req, (uint16_t)id);
    }
    return send_error(req, "404 Not Found", "no such auth route");
}

/* Sends captive-portal probes to the setup page instead of a bare 404. */
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    wifi_mgr_status_t wifi;
    wifi_manager_get_status(&wifi);
    if (wifi.mode == WIFI_MGR_MODE_AP && req->method == HTTP_GET) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        return httpd_resp_sendstr(req, "");
    }
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"error\":\"not found\"}");
}

/* ------------------------------------------------------------- lifecycle */

#if CONFIG_ZBC_WLED
/* WLED's JSON API, for the WLED interface (wled.c): GET /json, /json/info,
 * /json/state, /json/si, /json/cfg, /json/eff(ects), /json/pal(ettes),
 * /json/fxdata and /json/nodes - what Hyperion, HyperHDR and LedFx read, and
 * what a client that found the interface by mDNS asked for at once;
 * POST or PUT /json/state (or /json, /json/si) with live, on and bri. Not
 * guarded - a WLED client sends no key - so it answers nothing at all while the
 * interface is off, and while it is on it can do no more than a WLED could:
 * switch the one room and stream to it. */
static esp_err_t wled_json_dispatch(httpd_req_t *req)
{
    if (!settings_wled_on()) {
        return send_error(req, "404 Not Found", "the WLED interface is off");
    }
    char path[24] = "";
    const char *u = req->uri + strlen("/json");
    size_t      n = 0;
    if (*u == '/') {
        u++;
    }
    while (u[n] && u[n] != '?' && u[n] != '/' && n < sizeof(path) - 1) {
        path[n] = u[n];
        n++;
    }
    path[n] = '\0';

    if (req->method == HTTP_GET) {
        cJSON *root = NULL;
        if (!path[0] || strcmp(path, "si") == 0) {
            root = cJSON_CreateObject();
            cJSON_AddItemToObject(root, "state", wled_state_json());
            cJSON_AddItemToObject(root, "info", wled_info_json());
            if (!path[0]) {
                cJSON_AddItemToObject(root, "effects", cJSON_CreateStringArray((const char *[]){ "Solid" }, 1));
                cJSON_AddItemToObject(root, "palettes", cJSON_CreateStringArray((const char *[]){ "Default" }, 1));
            }
        } else if (strcmp(path, "info") == 0) {
            root = wled_info_json();
        } else if (strcmp(path, "state") == 0) {
            root = wled_state_json();
        } else if (strcmp(path, "cfg") == 0) {
            root = wled_cfg_json();
        } else if (strcmp(path, "eff") == 0 || strcmp(path, "effects") == 0) {
            root = cJSON_CreateStringArray((const char *[]){ "Solid" }, 1);
        } else if (strcmp(path, "pal") == 0 || strcmp(path, "palettes") == 0) {
            root = cJSON_CreateStringArray((const char *[]){ "Default" }, 1);
        } else if (strcmp(path, "fxdata") == 0) {
            /* One entry per effect, its controls; Solid has none. */
            root = cJSON_CreateStringArray((const char *[]){ "" }, 1);
        } else if (strcmp(path, "nodes") == 0) {
            root = cJSON_CreateObject();
            cJSON_AddArrayToObject(root, "nodes");
        }
        return root ? send_json(req, root, "200 OK") : send_error(req, "404 Not Found", "not part of this WLED");
    }

    const bool state = !path[0] || strcmp(path, "state") == 0 || strcmp(path, "si") == 0;
    if (!state && strcmp(path, "cfg") != 0) {
        return send_error(req, "404 Not Found", "not part of this WLED");
    }
    cJSON *body = read_json_body(req);
    if (!body) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "<- %s %s", req->method == HTTP_PUT ? "PUT" : "POST", req->uri);
    /* /json/cfg: LedFx may write its sync settings back. There are none here. */
    const bool verbose = state && wled_apply_state(body);
    cJSON_Delete(body);
    if (verbose) {
        return send_json(req, wled_state_json(), "200 OK");
    }
    cJSON *ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "success", true);
    return send_json(req, ok, "200 OK");
}

#if CONFIG_ZBC_WLED_WS
/* WLED's websocket, with CONFIG_ZBC_WLED_WS. The WLED apps (iOS, Android) show
 * a device online only while /ws is connected; a WLED sends {state, info} as it
 * opens and again after every command it is sent there, which is what this
 * does. Nothing is pushed unasked - the app's view is as fresh as its last
 * command. Not guarded, like /json, and refused while the interface is off. */
static esp_err_t wled_ws_send_state(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "state", wled_state_json());
    cJSON_AddItemToObject(root, "info", wled_info_json());
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)text, .len = strlen(text) };
    const esp_err_t err = httpd_ws_send_frame(req, &f);
    free(text);
    return err;
}

/* Right after the handshake (the handler is not called for it). */
static esp_err_t wled_ws_opened(httpd_req_t *req)
{
    if (!settings_wled_on()) {
        return ESP_FAIL;                   /* closes the socket */
    }
    return wled_ws_send_state(req);
}

static esp_err_t wled_ws_handler(httpd_req_t *req)
{
    if (!settings_wled_on()) {
        return ESP_FAIL;
    }
    httpd_ws_frame_t f = { 0 };
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if (err != ESP_OK || f.len == 0) {
        return err;
    }
    /* Only the frame's header has been read. A payload left on the socket would
     * be parsed as the next frame's header, so a frame this does not take ends
     * the connection rather than leaving the stream out of step. */
    if (f.type != HTTPD_WS_TYPE_TEXT || f.len > MAX_BODY_LEN) {
        return ESP_FAIL;
    }
    char *buf = malloc(f.len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    f.payload = (uint8_t *)buf;
    err = httpd_ws_recv_frame(req, &f, f.len);
    if (err == ESP_OK) {
        buf[f.len] = '\0';
        cJSON *body = cJSON_Parse(buf);
        if (body) {
            wled_apply_state(body);
            cJSON_Delete(body);
        }
    }
    free(buf);
    return err == ESP_OK ? wled_ws_send_state(req) : err;
}
#endif
#endif /* CONFIG_ZBC_WLED */

esp_err_t http_server_start(void)
{
    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    /* A hard cap, not a hint: a route past it simply fails to register. 49 in
     * the table below (two fewer without CONFIG_ZBC_CONFIG_BACKUP, three fewer
     * without CONFIG_ZBC_WLED), one more with CONFIG_ZBC_WLED_WS. */
    config.max_uri_handlers = 52;
    config.lru_purge_enable = true;
    config.uri_match_fn     = httpd_uri_match_wildcard;
    config.stack_size       = 6144;
    /* The default 5 s gives up on a firmware upload that stalls for a moment
     * on the antenna the 802.15.4 radio is also using. */
    config.recv_wait_timeout = 20;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "could not start HTTP server");

/* The real handler rides in user_ctx and guarded() calls it - see guarded(). */
#define GUARDED(u, m, h) { .uri = (u), .method = (m), .handler = guarded, .user_ctx = (void *)(h) }

    static const httpd_uri_t routes[] = {
        /* The only three routes a locked caller can reach, besides WLED's below
         * while that interface is on. The page itself, so it can draw the lock
         * screen; the access state, so that screen can see a button being
         * pressed; and the session, which checks for itself. First, because
         * httpd takes the first match and the /api/auth/ wildcard below matches
         * these too. */
        { .uri = "/",                  .method = HTTP_GET,    .handler = watched, .user_ctx = (void *)index_get },
        { .uri = "/api/auth",          .method = HTTP_GET,    .handler = watched, .user_ctx = (void *)auth_state_get },
        { .uri = "/api/auth/session",  .method = HTTP_POST,   .handler = watched, .user_ctx = (void *)auth_session_post },
#if CONFIG_ZBC_WLED
        /* WLED's API, for its clients: no key, and nothing while the interface
         * is off - see wled_json_dispatch. */
        { .uri = "/json*",             .method = HTTP_GET,    .handler = watched, .user_ctx = (void *)wled_json_dispatch },
        { .uri = "/json*",             .method = HTTP_POST,   .handler = watched, .user_ctx = (void *)wled_json_dispatch },
        { .uri = "/json*",             .method = HTTP_PUT,    .handler = watched, .user_ctx = (void *)wled_json_dispatch },
#endif
#if CONFIG_ZBC_WLED_WS
        { .uri = "/ws",                .method = HTTP_GET,    .handler = watched, .user_ctx = (void *)wled_ws_handler,
          .is_websocket = true, .ws_post_handshake_cb = wled_ws_opened },
#endif

        GUARDED("/api/auth/*",        HTTP_GET,    auth_dispatch),
        GUARDED("/api/auth/*",        HTTP_POST,   auth_dispatch),
        GUARDED("/api/auth/*",        HTTP_DELETE, auth_dispatch),
        GUARDED("/api/status",        HTTP_GET,    status_get),
        GUARDED("/api/state",         HTTP_GET,    state_get),
        GUARDED("/api/devices",       HTTP_GET,    devices_get),
        GUARDED("/api/devices/*",     HTTP_GET,    device_dispatch),
        GUARDED("/api/devices/*",     HTTP_POST,   device_dispatch),
        GUARDED("/api/devices/*",     HTTP_PATCH,  device_dispatch),
        GUARDED("/api/devices/*",     HTTP_DELETE, device_dispatch),
        GUARDED("/api/groups*",       HTTP_GET,    group_dispatch),
        GUARDED("/api/groups*",       HTTP_POST,   group_dispatch),
        GUARDED("/api/groups*",       HTTP_PATCH,  group_dispatch),
        GUARDED("/api/groups*",       HTTP_DELETE, group_dispatch),
        GUARDED("/api/lights*",       HTTP_POST,   light_dispatch),
        GUARDED("/api/lights*",       HTTP_PATCH,  light_dispatch),
        GUARDED("/api/remotes",       HTTP_GET,    remotes_get),
        GUARDED("/api/buttons*",      HTTP_GET,    button_dispatch),
        GUARDED("/api/buttons*",      HTTP_POST,   button_dispatch),
        GUARDED("/api/buttons*",      HTTP_PATCH,  button_dispatch),
        GUARDED("/api/buttons*",      HTTP_DELETE, button_dispatch),
        GUARDED("/api/permit_join",   HTTP_POST,   permit_join_post),
        GUARDED("/api/settings",      HTTP_GET,    settings_get),
        GUARDED("/api/settings",      HTTP_POST,   settings_post),
        GUARDED("/api/wifi/scan",     HTTP_GET,    wifi_scan_get),
        GUARDED("/api/wifi",          HTTP_POST,   wifi_post),
        GUARDED("/api/wifi/resume",   HTTP_POST,   wifi_resume_post),
        GUARDED("/api/wifi/test",     HTTP_POST,   wifi_test_post),
        GUARDED("/api/wifi/test",     HTTP_GET,    wifi_test_get),
        GUARDED("/api/factory_reset", HTTP_POST,   factory_reset_post),
#if CONFIG_ZBC_CONFIG_BACKUP
        GUARDED("/api/backup",        HTTP_GET,    backup_get),
        GUARDED("/api/restore",       HTTP_POST,   restore_post),
#endif
        GUARDED("/api/restart",       HTTP_POST,   restart_post),
        GUARDED("/api/matter",        HTTP_GET,    matter_get),
        GUARDED("/api/matter/*",      HTTP_POST,   matter_post_dispatch),
        GUARDED("/api/update/prepare", HTTP_POST,  ota_handle_prepare),
        GUARDED("/api/updater/*",     HTTP_POST,   updater_dispatch),
        GUARDED("/api/zigbee/scan",   HTTP_GET,    zigbee_scan_get),
        GUARDED("/api/zigbee/scan",   HTTP_POST,   zigbee_scan_post),
        GUARDED("/api/zigbee/ota*",   HTTP_GET,    zigbee_ota_dispatch),
        GUARDED("/api/zigbee/ota*",   HTTP_POST,   zigbee_ota_dispatch),
        GUARDED("/api/zigbee/ota*",   HTTP_DELETE, zigbee_ota_dispatch),
        /* Outside /api on purpose: this is the path ESPHome's web_server OTA
         * platform uses, so the same upload works against either. The
         * coordinator cannot write the partition it runs from, so this hands
         * over to the updater - see ota.h. */
        GUARDED("/update",            HTTP_POST,   ota_handle_handoff),
    };
#undef GUARDED
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &routes[i]), TAG,
                            "could not register %s", routes[i].uri);
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found);

    ESP_LOGI(TAG, "Web interface listening on port %d", config.server_port);
    return ESP_OK;
}

void http_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
