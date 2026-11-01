#include "http_util.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "auth.h"
#include "auth_http.h"
#include "heap_watch.h"
#include "wifi_manager.h"

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
