/* The updater's web server: its page, its status, signing in, POST /update, and
 * converting a device that still has another firmware's layout.
 *
 * Access follows the coordinator's rules, less the failsafe window (the button
 * sampler is not here): open in the setup hotspot and while no key exists,
 * otherwise a key. The cookie is the coordinator's own, because both answer on
 * the same address - a browser signed in to one is signed in to the other. A
 * device being converted has another firmware's NVS, so no keys, so it is open.
 */
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "auth.h"
#include "auth_http.h"
#include "convert.h"
#include "ota.h"
#include "updater.h"
#include "wifi_manager.h"

static const char *TAG = "updater_http";

extern const char page_start[] asm("_binary_page_html_start");
extern const char page_end[] asm("_binary_page_html_end");

static bool setup_or_open(void)
{
    return wifi_manager_mode() == WIFI_MGR_MODE_AP || auth_key_count() == 0;
}

static bool authorized(httpd_req_t *req)
{
    const uint16_t id = auth_http_presented_key(req, NULL, NULL);
    if (id) {
        auth_touch(id);
        return true;
    }
    return setup_or_open();
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

/* A whole small body, NUL-terminated. False when it is empty, too big or cut short. */
static bool read_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len <= 0 || (size_t)req->content_len >= cap) {
        return false;
    }
    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n <= 0) {
            return false;
        }
        got += n;
    }
    buf[got] = '\0';
    return true;
}

/* The string value of `key` in a small flat JSON object, with \" \\ and \/
 * unescaped - enough for a Wi-Fi password, which may contain either. Not a JSON
 * parser: the page is the only client. */
static bool json_string(const char *json, const char *key, char *out, size_t cap)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) {
        return false;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    if (*p++ != '"') {
        return false;
    }
    size_t n = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            c = *p++;
            if (c != '"' && c != '\\' && c != '/') {
                return false;
            }
        }
        if (n + 1 >= cap) {
            return false;
        }
        out[n++] = c;
    }
    if (*p != '"') {
        return false;
    }
    out[n] = '\0';
    return true;
}

static esp_err_t page_get(httpd_req_t *req)
{
    updater_touch();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* A text embed carries a trailing NUL that is not part of the page. */
    return httpd_resp_send(req, page_start, page_end - page_start - 1);
}

/* GET /api/status - unguarded, like the coordinator's /api/auth: the
 * coordinator's page polls it to see the handover happen, and it says nothing
 * a locked caller should not know. `updater: true` is what tells the two apart. */
static esp_err_t status_get(httpd_req_t *req)
{
    const char *app = updater_app_version();
    char        body[640];
    snprintf(body, sizeof(body),
             "{\"updater\":true,\"reason\":\"%s\",\"message\":\"%s\",\"version\":\"%.32s\","
             "\"app_version\":%s%.32s%s,\"authorized\":%s,\"setup\":%s,\"convert\":%s}",
             updater_reason(), updater_message(), esp_app_get_description()->version,
             app ? "\"" : "", app ? app : "null", app ? "\"" : "",
             authorized(req) ? "true" : "false",
             wifi_manager_mode() == WIFI_MGR_MODE_AP ? "true" : "false",
             convert_needed() ? "true" : "false");
    return send_json(req, "200 OK", body);
}

/* POST /api/auth/session  {"key": "K7M2Q-..."}. Typing a key in is the one way
 * through here; creating keys is the coordinator's business. */
static esp_err_t session_post(httpd_req_t *req)
{
    char body[160];
    if (!read_body(req, body, sizeof(body))) {
        return send_json(req, "400 Bad Request", "{\"error\":\"expected {\\\"key\\\": ...}\"}");
    }
    char     typed[64];
    char     canon[ZBC_AUTH_KEY_CANON_LEN];
    uint16_t id = 0;
    if (!json_string(body, "key", typed, sizeof(typed)) || !(id = auth_check(typed))) {
        ESP_LOGW(TAG, "Sign-in with a key that is not known here");
        return send_json(req, "401 Unauthorized", "{\"error\":\"That key is not known here\"}");
    }
    auth_touch(id);
    auth_normalize(typed, canon);
    static char cookie[96];   /* httpd keeps the pointer until the response is sent */
    auth_http_set_cookie(req, cookie, sizeof(cookie), canon);
    return send_json(req, "200 OK", "{\"ok\":true}");
}

/* POST /update - ESPHome's shape, as in the coordinator. Refused on another
 * firmware's layout: the app partition there is that firmware's slot, and a
 * coordinator written into it would run against a table it was not built for. */
static esp_err_t update_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        httpd_resp_set_hdr(req, "X-Ota-Error", "This browser is not signed in to the coordinator");
        return send_json(req, "401 Unauthorized", "{\"error\":\"locked\"}");
    }
    if (convert_needed()) {
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_set_hdr(req, "X-Ota-Error", "convert this device's flash layout first - open the updater page");
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "Update Failed!");
    }
    updater_upload_started();
    esp_err_t err = ota_handle_update(req);
    updater_upload_finished();
    return err;
}

/* GET /api/convert - what converting this device would take, and how far it got. */
static esp_err_t convert_get(httpd_req_t *req)
{
    static char body[2048];   /* the httpd task is the only caller */
    convert_status_json(body, sizeof(body));
    return send_json(req, "200 OK", body);
}

/* POST /api/convert  {"ssid": "...", "password": "...", "confirm": "CONVERT"}
 * The confirmation is spelled out so that nothing but a person who has read the
 * warning on the page starts a rewrite of the bootloader. */
static esp_err_t convert_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_json(req, "401 Unauthorized", "{\"error\":\"locked\"}");
    }
    char body[320];
    char ssid[33]    = "";
    char pass[65]    = "";
    char confirm[16] = "";
    if (!read_body(req, body, sizeof(body)) || !json_string(body, "confirm", confirm, sizeof(confirm)) ||
        strcmp(confirm, "CONVERT") != 0) {
        return send_json(req, "400 Bad Request", "{\"error\":\"expected {\\\"confirm\\\": \\\"CONVERT\\\"}\"}");
    }
    json_string(body, "ssid", ssid, sizeof(ssid));
    json_string(body, "password", pass, sizeof(pass));
    if (pass[0] && strlen(pass) < 8) {
        return send_json(req, "400 Bad Request", "{\"error\":\"a Wi-Fi password has at least 8 characters\"}");
    }
    if (convert_start(ssid, pass) != ESP_OK) {
        return send_json(req, "409 Conflict", "{\"error\":\"this device cannot be converted now - see GET /api/convert\"}");
    }
    ESP_LOGW(TAG, "Conversion started from the web page");
    return send_json(req, "202 Accepted", "{\"ok\":true}");
}

/* In the setup hotspot every GET goes to the page, which is what makes a phone
 * open it by itself - the same arrangement as the coordinator's. */
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t code)
{
    (void)code;
    if (wifi_manager_mode() == WIFI_MGR_MODE_AP && req->method == HTTP_GET) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        return httpd_resp_sendstr(req, "");
    }
    return send_json(req, "404 Not Found", "{\"error\":\"not found - this is the updater\"}");
}

esp_err_t updater_http_start(void)
{
    httpd_config_t config    = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers  = 8;
    config.lru_purge_enable  = true;
    config.stack_size        = 6144;
    /* As in the coordinator: an upload that stalls for a moment is not a failed one. */
    config.recv_wait_timeout = 20;

    httpd_handle_t server = NULL;
    esp_err_t      err    = httpd_start(&server, &config);
    if (err != ESP_OK) {
        return err;
    }
    static const httpd_uri_t routes[] = {
        { .uri = "/",                 .method = HTTP_GET,  .handler = page_get },
        { .uri = "/api/status",       .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/auth/session", .method = HTTP_POST, .handler = session_post },
        { .uri = "/api/convert",      .method = HTTP_GET,  .handler = convert_get },
        { .uri = "/api/convert",      .method = HTTP_POST, .handler = convert_post },
        { .uri = "/update",           .method = HTTP_POST, .handler = update_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);
    ESP_LOGI(TAG, "Updater page listening on port %d", config.server_port);
    return ESP_OK;
}
