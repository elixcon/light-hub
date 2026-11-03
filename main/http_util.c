#include "http_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "auth.h"
#include "auth_http.h"
#include "crash_log.h"
#include "heap_watch.h"
#include "ota.h"
#include "wifi_manager.h"

static const char *TAG = "http";

extern const uint8_t index_html_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_gz_end");

/* ------------------------------------------------------------- utilities */

esp_err_t send_json(httpd_req_t *req, cJSON *root, const char *status)
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
bool json_stream_begin(json_stream_t *js, httpd_req_t *req)
{
    *js = (json_stream_t){ .req = req, .buf = malloc(JSON_STREAM_BUF) };
    if (!js->buf) {
        return false;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return true;
}

void json_stream_flush(json_stream_t *js)
{
    if (js->len && !js->failed && httpd_resp_send_chunk(js->req, js->buf, js->len) != ESP_OK) {
        js->failed = true;
    }
    js->total += js->len;
    js->len = 0;
}

void json_stream_raw(json_stream_t *js, const char *text)
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
bool json_stream_item(json_stream_t *js, const char *sep, cJSON *item)
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
void json_stream_value(json_stream_t *js, cJSON *item)
{
    if (!json_stream_item(js, "", item)) {
        json_stream_raw(js, "null");
    }
}

/* An array element, with the comma when it is not the first. An element that
 * could not be built is left out, never written as null - the page reads a
 * device's fields without checking for one. */
void json_stream_element(json_stream_t *js, bool *first, cJSON *item)
{
    if (json_stream_item(js, *first ? "" : ",", item)) {
        *first = false;
    }
}

/* An object member's name, with the comma when it is not the first. The braces
 * are the caller's and the value follows, so a member can be left out - or be
 * an object streamed in turn - without the separators going wrong. */
void json_stream_key(json_stream_t *js, bool *first, const char *name)
{
    json_stream_raw(js, *first ? "\"" : ",\"");
    json_stream_raw(js, name);
    json_stream_raw(js, "\":");
    *first = false;
}

/* A member whose value is one small tree, built and freed here. */
void json_stream_member(json_stream_t *js, bool *first, const char *name, cJSON *value)
{
    json_stream_key(js, first, name);
    json_stream_value(js, value);
}

/* A table could not be copied. Before anything went out that is an ordinary
 * error answer; after, the response is cut off, so the page keeps what it
 * showed and polls again - an empty list would read as every device gone. */
esp_err_t json_stream_fail(json_stream_t *js)
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

esp_err_t json_stream_end(json_stream_t *js)
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

esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", message);
    return send_json(req, root, status);
}

esp_err_t send_ok(httpd_req_t *req, const char *status)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root, status);
}

void add_kelvin(cJSON *obj, const char *name, int32_t mireds)
{
    if (mireds <= 0) {
        cJSON_AddNullToObject(obj, name);
    } else {
        cJSON_AddNumberToObject(obj, name, 1000000 / mireds);
    }
}

/* Reads the request body and parses it as JSON. Returns NULL on any problem,
 * having already answered the request. */
cJSON *read_json_body(httpd_req_t *req)
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
bool uri_segment(const char *uri, int index, char *out, size_t out_size)
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

const char *const ACCESS_NAMES[] = { "locked", "setup", "open", "failsafe" };

/* Whether a request with no key gets in, and why. The setup portal is open on
 * purpose: the person on its network is within Wi-Fi range of the board, the
 * captive-portal browsers phones open throw cookies away, and a lock on the one
 * page that gets the box onto a network would be a lock on the only way back. */
access_mode_t access_mode(void)
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

/* Every route but three is registered through this, with the real handler in
 * user_ctx - so a route added later cannot forget to check, which a line at the
 * top of each handler could. */
esp_err_t guarded(httpd_req_t *req)
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
esp_err_t watched(httpd_req_t *req)
{
    heap_watch_request_begin();
    const esp_err_t err = ((route_fn_t)req->user_ctx)(req);
    heap_watch_request_end(req);
    return err;
}

const char *name_from_json(const cJSON *json)
{
    const cJSON *name = cJSON_GetObjectItem(json, "name");
    return cJSON_IsString(name) && name->valuestring[0] != '\0' ? name->valuestring : NULL;
}

int name_index(const char *const *names, int n, const cJSON *item)
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

/* ------------------------------------------------------- routes both serve */

/* Started with 3072 bytes: esp_restart() can confirm an image on trial on this
 * stack (ota.c's confirm_on_restart), which writes flash and logs. */
void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));  /* let the response reach the browser */
    esp_restart();
}

cJSON *auth_json(httpd_req_t *req)
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

cJSON *wifi_json(void)
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

esp_err_t index_get(httpd_req_t *req)
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

/* GET /api/auth - not guarded: it is how a locked page learns that a button has
 * been pressed five times, and it says nothing a locked caller should not know. */
esp_err_t auth_state_get(httpd_req_t *req)
{
    return send_json(req, auth_json(req), "200 OK");
}

esp_err_t send_session(httpd_req_t *req, const zbc_auth_key_t *k, const char *shown)
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
esp_err_t auth_session_post(httpd_req_t *req)
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

esp_err_t wifi_scan_get(httpd_req_t *req)
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

esp_err_t wifi_post(httpd_req_t *req)
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
esp_err_t wifi_test_post(httpd_req_t *req)
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
        char why[64];
        snprintf(why, sizeof(why), "the check could not start (%s)", esp_err_to_name(err));
        return send_error(req, "500 Internal Server Error", why);
    }
    return send_ok(req, "202 Accepted");
}

esp_err_t wifi_test_get(httpd_req_t *req)
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

esp_err_t wifi_resume_post(httpd_req_t *req)
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

/* /api/updater/image (POST, multipart elixcon_light_hub_updater.bin) and /api/updater/run
 * (POST) - see ota.h. */
esp_err_t updater_dispatch(httpd_req_t *req)
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

/* Sends captive-portal probes to the setup page instead of a bare 404. */
esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
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

/* This firmware and the updater beside it. */
cJSON *ota_json(void)
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
    return o;
}

void stream_firmware_status(json_stream_t *js, bool *first)
{
    json_stream_member(js, first, "uptime_s", cJSON_CreateNumber((double)(esp_timer_get_time() / 1000000)));
    /* Why this boot happened, so "the board seems to have crashed" can be told
     * from a power cut - and where the last crash was, since there is no core
     * dump partition and the panic handler prints only to the serial port. */
    json_stream_member(js, first, "reset_reason",
                       cJSON_CreateString(crash_log_reset_reason_name(esp_reset_reason())));
    json_stream_member(js, first, "last_crash", crash_log_json());
    json_stream_member(js, first, "version", cJSON_CreateString(esp_app_get_description()->version));
    /* Unique per image, where `version` is only the git description - two
     * builds from one uncommitted tree share it. The UI reloads itself when this
     * changes, so it has to change whenever the firmware does. */
    char build[17];
    esp_app_get_elf_sha256(build, sizeof(build));
    json_stream_member(js, first, "build", cJSON_CreateString(build));
}

bool rev_unchanged(httpd_req_t *req, const char *rev)
{
    char query[64], seen[24];
    return httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
           httpd_query_key_value(query, "rev", seen, sizeof(seen)) == ESP_OK && strcmp(seen, rev) == 0;
}

bool reset_scope_from_body(httpd_req_t *req, bool *zigbee_only, esp_err_t *ret)
{
    *zigbee_only = false;
    if (req->content_len == 0) {
        return true;
    }
    /* A body that could not be read has been answered, and must not fall through
     * to the default - which is the reset that erases everything. */
    cJSON *json = read_json_body(req);
    if (!json) {
        *ret = ESP_FAIL;
        return false;
    }
    const cJSON *scope = cJSON_GetObjectItem(json, "scope");
    const bool   known = !scope || (cJSON_IsString(scope) && (strcmp(scope->valuestring, "all") == 0 ||
                                                              strcmp(scope->valuestring, "zigbee") == 0));
    *zigbee_only = known && scope && strcmp(scope->valuestring, "zigbee") == 0;
    cJSON_Delete(json);
    /* A scope this build does not know is not read as "everything". */
    if (!known) {
        *ret = send_error(req, "400 Bad Request", "scope must be all or zigbee");
        return false;
    }
    return true;
}
