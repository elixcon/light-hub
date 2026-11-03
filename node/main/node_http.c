#include "node_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "button_db.h"
#include "crash_log.h"
#include "device_db.h"
#include "http_util.h"
#include "http_wired.h"
#include "light_db.h"
#include "light_math.h"
#include "local_lights.h"
#include "node_buttons.h"
#include "node_main.h"
#include "node_proto.h"
#include "node_store.h"
#include "node_zb.h"
#include "ota.h"
#include "settings.h"
#include "state_rev.h"
#include "wifi_manager.h"

static const char *TAG = "node_http";

static const char *const MODE_NAMES[]  = { "zigbee", "station", "hotspot", "unpaired" };

/* ---------------------------------------------------------------- the status */

static cJSON *zigbee_json(void)
{
    node_zb_stats_t z = { 0 };
    node_zb_stats(&z);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "network_up", z.joined);
    cJSON_AddBoolToObject(o, "rx_on", z.rx_on);
    if (z.joined) {
        cJSON_AddNumberToObject(o, "short_addr", z.short_addr);
        cJSON_AddNumberToObject(o, "pan_id", z.pan_id);
        cJSON_AddNumberToObject(o, "channel", z.channel);
    }
    if (z.joined && z.parent_short != 0xFFFF) {
        cJSON *p = cJSON_AddObjectToObject(o, "parent");
        cJSON_AddNumberToObject(p, "short_addr", z.parent_short);
        cJSON_AddBoolToObject(p, "coordinator", z.parent_short == 0);
        cJSON_AddNumberToObject(p, "lqi", z.parent_lqi);
        cJSON_AddNumberToObject(p, "rssi", z.parent_rssi);
        cJSON_AddNumberToObject(p, "join_floor", z.join_floor);
        cJSON_AddNumberToObject(p, "rejoins_failed", z.rejoins_failed);
        cJSON_AddNumberToObject(p, "joined_after_ms", z.joined_after_ms);
    }
    /* What the page's poll asks of any Zigbee status. A node never opens a network. */
    cJSON_AddNumberToObject(o, "permit_join_remaining_s", 0);
    return o;
}

static void stream_status(json_stream_t *js)
{
    bool first = true;
    json_stream_raw(js, "{");
    json_stream_member(js, &first, "name", cJSON_CreateString(settings_name()));
    json_stream_member(js, &first, "wifi", wifi_json());
    json_stream_member(js, &first, "zigbee", zigbee_json());
    cJSON *n = cJSON_CreateObject();
    cJSON_AddStringToObject(n, "mode", MODE_NAMES[node_mode()]);
    cJSON_AddBoolToObject(n, "paired", node_store_paired());
    cJSON_AddBoolToObject(n, "hub_down", node_buttons_hub_down());
    cJSON_AddNumberToObject(n, "window_left_s", node_window_left_s());
    json_stream_member(js, &first, "node", n);
    json_stream_member(js, &first, "thermal", thermal_json());
    json_stream_member(js, &first, "wired", wired_json());
    stream_firmware_status(js, &first);
    json_stream_member(js, &first, "ota", ota_json());
    json_stream_raw(js, "}");
}

static esp_err_t status_get(httpd_req_t *req)
{
    json_stream_t js;
    if (!json_stream_begin(&js, req)) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    stream_status(&js);
    return json_stream_end(&js);
}

/* ------------------------------------------------------------------ the lights */

static cJSON *light_to_json(const zbc_light_t *l)
{
    uint8_t ieee[ZBC_IEEE_LEN];
    char    ieee_str[19];
    zbc_light_ieee(l->id, ieee);
    zbc_ieee_to_str(ieee, ieee_str);

    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "ieee", ieee_str);
    cJSON_AddNullToObject(d, "short_addr");
    cJSON_AddStringToObject(d, "name", l->name);
    cJSON_AddStringToObject(d, "manufacturer", "");
    cJSON_AddStringToObject(d, "model", "");
    cJSON_AddStringToObject(d, "source", "gpio");
    wired_light_to_json(d, l->id, l);
    cJSON_AddBoolToObject(d, "online", true);
    cJSON_AddNumberToObject(d, "last_seen_s_ago", 0);

    zbc_local_state_t st = { .on = -1, .level = -1, .mireds = -1 };
    local_light_get(l->id, &st);
    const bool dims    = zbc_light_pwm_channels(l->kind) > 0;
    const bool tunable = zbc_light_tunable(l->kind);

    cJSON *eps = cJSON_AddArrayToObject(d, "endpoints");
    cJSON *ep  = cJSON_CreateObject();
    cJSON_AddItemToArray(eps, ep);
    cJSON_AddNumberToObject(ep, "ep", ZBC_LIGHT_EP);
    cJSON_AddNumberToObject(ep, "device_id", 0);
    cJSON *feat = cJSON_AddArrayToObject(ep, "features");
    cJSON_AddItemToArray(feat, cJSON_CreateString("onoff"));
    if (dims) {
        cJSON_AddItemToArray(feat, cJSON_CreateString("brightness"));
    }
    if (tunable) {
        cJSON_AddItemToArray(feat, cJSON_CreateString("color_temp"));
    }
    if (st.on < 0) {
        cJSON_AddNullToObject(ep, "on");
    } else {
        cJSON_AddBoolToObject(ep, "on", st.on > 0);
    }
    if (dims && st.level >= 0) {
        cJSON_AddNumberToObject(ep, "brightness_pct", zbc_level_to_pct(st.level));
    } else {
        cJSON_AddNullToObject(ep, "brightness_pct");
    }
    if (tunable) {
        add_kelvin(ep, "kelvin", st.mireds);
        cJSON_AddNumberToObject(ep, "min_kelvin", l->warm_k);
        cJSON_AddNumberToObject(ep, "max_kelvin", l->cold_k);
        cJSON_AddStringToObject(ep, "color_mode", "color_temp");
    } else {
        cJSON_AddNullToObject(ep, "kelvin");
        cJSON_AddNullToObject(ep, "min_kelvin");
        cJSON_AddNullToObject(ep, "max_kelvin");
        cJSON_AddNullToObject(ep, "color_mode");
    }
    cJSON_AddNullToObject(ep, "hue");
    cJSON_AddNullToObject(ep, "sat");
    cJSON *curves = cJSON_AddObjectToObject(ep, "curves");
    cJSON_AddBoolToObject(curves, "level", false);
    cJSON_AddBoolToObject(curves, "chroma", false);
    cJSON_AddNullToObject(ep, "input_pct");
    cJSON_AddArrayToObject(ep, "reporting");
    return d;
}

static void stream_lights(json_stream_t *js)
{
    zbc_light_t *lights = malloc(CONFIG_ZBC_MAX_LIGHTS * sizeof(*lights));
    json_stream_raw(js, "[");
    bool first = true;
    if (lights) {
        const int n = (int)light_db_snapshot(lights, CONFIG_ZBC_MAX_LIGHTS);
        for (int i = 0; i < n; i++) {
            json_stream_element(js, &first, light_to_json(&lights[i]));
        }
        free(lights);
    }
    json_stream_raw(js, "]");
}

/* A wired light's address, or 0. */
static uint16_t light_of_uri(httpd_req_t *req)
{
    char    s[24];
    uint8_t ieee[ZBC_IEEE_LEN];
    if (!uri_segment(req->uri, 2, s, sizeof(s)) || !zbc_str_to_ieee(s, ieee)) {
        return 0;
    }
    return zbc_light_id_from_ieee(ieee);
}

static esp_err_t light_result(httpd_req_t *req, esp_err_t err)
{
    if (err == ESP_OK) {
        return send_ok(req, "202 Accepted");
    }
    if (err == ESP_ERR_NOT_ALLOWED) {
        return send_error(req, "409 Conflict", "the board is too hot; wired lights are held off");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error(req, "409 Conflict", "this light has no pin yet - pick one under Wiring");
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return send_error(req, "409 Conflict", "this light does not support that");
    }
    return send_error(req, "404 Not Found", "unknown light");
}

/* /api/devices/{ieee}[/ep/1/{onoff,level,color_temp}]: the wired lights, as the hub's
 * editor reaches them. Curves and rooms are the hub's, so they are not here. */
static esp_err_t device_dispatch(httpd_req_t *req)
{
    const uint16_t id = light_of_uri(req);
    if (!id || !light_db_get(id, NULL)) {
        return send_error(req, "404 Not Found", "unknown light");
    }
    char tail[16];
    if (!uri_segment(req->uri, 5, tail, sizeof(tail))) {
        if (req->method == HTTP_DELETE) {
            light_db_remove(id);
            local_lights_reload();
            return send_ok(req, "200 OK");
        }
        if (req->method == HTTP_PATCH) {
            cJSON *json = read_json_body(req);
            if (!json) {
                return ESP_FAIL;
            }
            const char *name = name_from_json(json);
            esp_err_t err = name ? light_db_rename(id, name) : ESP_ERR_INVALID_ARG;
            cJSON_Delete(json);
            return err == ESP_OK ? send_ok(req, "200 OK") : send_error(req, "400 Bad Request", "name is required");
        }
        return send_error(req, "404 Not Found", "no such route");
    }
    if (req->method != HTTP_POST) {
        return send_error(req, "404 Not Found", "no such route");
    }
    cJSON *json = read_json_body(req);
    if (!json) {
        return ESP_FAIL;
    }
    const uint16_t ds  = settings_transition_ds();
    esp_err_t      err = ESP_ERR_NOT_SUPPORTED;
    if (strcmp(tail, "onoff") == 0) {
        const cJSON *state = cJSON_GetObjectItem(json, "state");
        const char  *s     = cJSON_IsString(state) ? state->valuestring : "";
        if (strcmp(s, "toggle") == 0) {
            err = local_light_set(id, ZB_ONOFF_TOGGLE);
        } else if (strcmp(s, "on") == 0) {
            err = local_light_on(id, ds);
        } else if (strcmp(s, "off") == 0) {
            err = local_light_off(id, ds);
        }
    } else if (strcmp(tail, "level") == 0) {
        const cJSON *pct = cJSON_GetObjectItem(json, "pct");
        if (cJSON_IsNumber(pct) && pct->valueint >= 1 && pct->valueint <= 100) {
            err = local_light_on_at(id, zbc_pct_to_level(pct->valueint), ds);
        }
    } else if (strcmp(tail, "color_temp") == 0) {
        const cJSON *k = cJSON_GetObjectItem(json, "kelvin");
        if (cJSON_IsNumber(k) && k->valueint >= ZBC_LIGHT_KELVIN_MIN && k->valueint <= ZBC_LIGHT_KELVIN_MAX) {
            err = local_light_set_ct(id, (uint16_t)(1000000 / k->valueint), ds);
        }
    }
    cJSON_Delete(json);
    return light_result(req, err);
}

/* ----------------------------------------------------------------- the buttons */

/* A button here is a pin and how it is wired: the rules are the hub's. The one thing
 * stored beside them is the light to toggle when the hub cannot be reached, kept as an
 * ordinary rule. */
static cJSON *button_to_json(const zbc_button_t *b)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", b->id);
    cJSON_AddStringToObject(o, "name", b->name);
    cJSON_AddStringToObject(o, "source", "gpio");
    button_wiring_to_json(o, b);
    bool pressed = false;
    int  level   = 0;
    if (node_buttons_pin_state(b->id, &pressed, &level)) {
        cJSON_AddBoolToObject(o, "pressed", pressed);
        cJSON_AddNumberToObject(o, "level", level);
    } else {
        cJSON_AddNullToObject(o, "pressed");
        cJSON_AddNullToObject(o, "level");
    }
    const uint16_t fb = node_buttons_fallback(b);
    if (fb) {
        cJSON_AddNumberToObject(o, "fallback", fb);
    } else {
        cJSON_AddNullToObject(o, "fallback");
    }
    /* How the hub's rule editor will name it: the key of this node's control endpoint. */
    cJSON_AddNumberToObject(o, "key", b->id <= NODE_MAX_KEYS ? b->id - 1 : -1);
    return o;
}

static void stream_buttons(json_stream_t *js)
{
    uint16_t ids[CONFIG_ZBC_MAX_BUTTONS];
    const int n = (int)button_db_list_ids(ids, CONFIG_ZBC_MAX_BUTTONS);
    zbc_button_t *b = malloc(sizeof(*b));
    json_stream_raw(js, "[");
    bool first = true;
    for (int i = 0; b && i < n; i++) {
        if (button_db_get(ids[i], b) && b->src == ZBC_BTN_SRC_GPIO) {
            json_stream_element(js, &first, button_to_json(b));
        }
    }
    free(b);
    json_stream_raw(js, "]");
}

static const char *button_from_json(const cJSON *json, zbc_button_t *b, uint16_t id)
{
    const char *name = name_from_json(json);
    if (name) {
        strlcpy(b->name, name, sizeof(b->name));
    }
    const char *why = button_wiring_from_json(json, b);
    if (why) {
        return why;
    }
    if (cJSON_IsNumber(cJSON_GetObjectItem(json, "gpio"))) {
        if (light_db_pin_taken(b->gpio, 0)) {
            return "a light drives that pin";
        }
        if (button_db_pin_taken(b->gpio, id)) {
            return "another button reads that pin";
        }
    }
    const cJSON *fb = cJSON_GetObjectItem(json, "fallback");
    if (fb) {
        /* Replaces whatever rules were here: on a node they mean only this. A board
         * that used to be a hub keeps them until somebody sets this. */
        memset(b->rule_count, 0, sizeof(b->rule_count));
        if (cJSON_IsNumber(fb)) {
            const uint16_t light = (uint16_t)fb->valueint;
            if (!light_db_get(light, NULL)) {
                return "unknown light";
            }
            zbc_btn_rule_t r = { .cond = ZBC_COND_ALWAYS, .action = ZBC_ACT_TOGGLE, .ep = ZBC_LIGHT_EP };
            zbc_light_ieee(light, r.ieee);
            b->rules[0] = r;
            b->rule_count[ZBC_EV_SHORT] = 1;
        }
    }
    return NULL;
}

static esp_err_t button_dispatch(httpd_req_t *req)
{
    char id_str[8];
    if (!uri_segment(req->uri, 2, id_str, sizeof(id_str))) {
        if (req->method == HTTP_GET) {
            json_stream_t js;
            if (!json_stream_begin(&js, req)) {
                return send_error(req, "500 Internal Server Error", "out of memory");
            }
            json_stream_raw(&js, "{\"buttons\":");
            stream_buttons(&js);
            json_stream_raw(&js, "}");
            return json_stream_end(&js);
        }
        cJSON *json = read_json_body(req);
        if (!json) {
            return ESP_FAIL;
        }
        const char *name = name_from_json(json);
        uint16_t    id   = 0;
        esp_err_t   err  = name ? button_db_create(name, &id) : ESP_ERR_INVALID_ARG;
        cJSON_Delete(json);
        if (err == ESP_ERR_INVALID_ARG) {
            return send_error(req, "400 Bad Request", "name is required");
        }
        if (err != ESP_OK) {
            return send_error(req, "507 Insufficient Storage", "no room for another button");
        }
        node_buttons_reload();
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "id", id);
        return send_json(req, root, "201 Created");
    }

    const uint16_t id = (uint16_t)atoi(id_str);
    zbc_button_t  *b  = malloc(sizeof(*b));
    if (!b) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    esp_err_t ret;
    if (!id || !button_db_get(id, b)) {
        ret = send_error(req, "404 Not Found", "unknown button");
    } else if (req->method == HTTP_DELETE) {
        button_db_remove(id);
        node_buttons_reload();
        ret = send_ok(req, "200 OK");
    } else if (req->method == HTTP_PATCH) {
        cJSON *json = read_json_body(req);
        if (!json) {
            free(b);
            return ESP_FAIL;
        }
        const char *why = button_from_json(json, b, id);
        cJSON_Delete(json);
        if (why) {
            ret = send_error(req, "400 Bad Request", why);
        } else if (button_db_set(id, b) != ESP_OK) {
            ret = send_error(req, "400 Bad Request", "that button configuration does not make sense");
        } else {
            node_buttons_reload();
            ret = send_json(req, button_to_json(b), "200 OK");
        }
    } else {
        ret = send_error(req, "404 Not Found", "no such route");
    }
    free(b);
    return ret;
}

/* -------------------------------------------------------------------- the state */

static esp_err_t state_get(httpd_req_t *req)
{
    char rev[16];
    snprintf(rev, sizeof(rev), "%lu", (unsigned long)zbc_state_rev());

    const bool unchanged = rev_unchanged(req, rev);
    /* First: it reads the request's headers, gone once a chunk has been sent. */
    cJSON        *auth = auth_json(req);
    json_stream_t js;
    if (!json_stream_begin(&js, req)) {
        cJSON_Delete(auth);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    bool first = true;
    json_stream_raw(&js, "{");
    json_stream_key(&js, &first, "status");
    stream_status(&js);
    if (!unchanged) {
        json_stream_key(&js, &first, "devices");
        stream_lights(&js);
        json_stream_key(&js, &first, "buttons");
        stream_buttons(&js);
        json_stream_raw(&js, ",\"groups\":[],\"remotes\":[]");
    }
    json_stream_member(&js, &first, "auth", auth);
    json_stream_member(&js, &first, "rev", cJSON_CreateString(rev));
    if (unchanged) {
        json_stream_member(&js, &first, "unchanged", cJSON_CreateTrue());
    }
    json_stream_raw(&js, "}");
    return json_stream_end(&js);
}

static esp_err_t devices_get(httpd_req_t *req)
{
    json_stream_t js;
    if (!json_stream_begin(&js, req)) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    json_stream_raw(&js, "{\"devices\":");
    stream_lights(&js);
    json_stream_raw(&js, "}");
    return json_stream_end(&js);
}

/* ---------------------------------------------------------- the node's own routes */

static esp_err_t settings_get_(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", settings_name());
    cJSON_AddStringToObject(o, "hostname", settings_hostname());
    cJSON_AddBoolToObject(o, "wifi_sleep", settings_wifi_sleep());
    return send_json(req, o, "200 OK");
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));   /* let the answer reach the browser */
    node_restart_into((node_ask_t)(intptr_t)arg, 0);
    vTaskDelete(NULL);
}

static void restart_later(node_ask_t ask)
{
    xTaskCreate(restart_task, "restart", 3072, (void *)(intptr_t)ask, 5, NULL);
}

static esp_err_t ok_then_restart(httpd_req_t *req, node_ask_t ask)
{
    send_ok(req, "200 OK");
    restart_later(ask);
    return ESP_OK;
}

static esp_err_t settings_post_(httpd_req_t *req)
{
    cJSON *json = read_json_body(req);
    if (!json) {
        return ESP_FAIL;
    }
    const char *name = name_from_json(json);
    esp_err_t   err  = ESP_OK;
    if (name) {
        err = settings_name_ok(name) ? settings_set_name(name) : ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(json);
    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", "that name cannot be used");
    }
    if (name) {
        /* The hostname comes from it, read once at boot. Back on Wi-Fi: this page is open. */
        restart_later(NODE_ASK_WIFI);
    }
    return settings_get_(req);
}

/* POST /api/node/close: done configuring, so back to Zigbee alone now rather than when
 * the idle time runs out. */
static esp_err_t node_close_post(httpd_req_t *req)
{
    return ok_then_restart(req, NODE_ASK_NONE);
}

/* POST /api/node/leave: forget the Zigbee network, to pair with another hub. */
static esp_err_t node_leave_post(httpd_req_t *req)
{
    node_store_set_paired(false);
    node_store_forget_claim();   /* the next boot erases the network, before the stack starts */
    return ok_then_restart(req, NODE_ASK_NONE);
}

/* DELETE /api/wifi: forget the stored network. A paired node goes back to Zigbee alone,
 * and opens its hotspot the next time the hub asks. An unpaired one opens it now. */
static esp_err_t wifi_forget(httpd_req_t *req)
{
    if (wifi_manager_erase_credentials() != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "could not forget the network");
    }
    return ok_then_restart(req, NODE_ASK_NONE);
}

/* POST /api/factory_reset: as new, with the scopes a hub's has. No stack is needed, so
 * unlike a hub's it also works from the setup hotspot. */
static esp_err_t factory_reset_post(httpd_req_t *req)
{
    bool      zigbee_only;
    esp_err_t answered;
    if (!reset_scope_from_body(req, &zigbee_only, &answered)) {
        return answered;
    }
    if (zigbee_only) {
        return node_leave_post(req);
    }
    node_store_ask_wipe();
    return ok_then_restart(req, NODE_ASK_NONE);
}

/* Restarts keep Wi-Fi: the page asking is open. */
static esp_err_t restart_post(httpd_req_t *req)
{
    return ok_then_restart(req, NODE_ASK_WIFI);
}

/* Every route counts as somebody using the page, which keeps Wi-Fi open. So the page
 * stops polling while its tab is hidden (stopPollingWhileHidden). */
static esp_err_t touched(httpd_req_t *req)
{
    node_touch();
    return guarded(req);
}

static esp_err_t update_post(httpd_req_t *req)
{
    node_touch();
    return ota_handle_handoff(req);
}

esp_err_t node_http_start(void)
{
    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 32;
    config.lru_purge_enable = true;
    config.uri_match_fn     = httpd_uri_match_wildcard;
    config.stack_size       = 6144;
    config.recv_wait_timeout = 20;

    httpd_handle_t server;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "could not start the HTTP server");

#define R(u, m, h) { .uri = (u), .method = (m), .handler = touched, .user_ctx = (void *)(h) }
    static const httpd_uri_t routes[] = {
        { .uri = "/",                  .method = HTTP_GET,  .handler = watched, .user_ctx = (void *)index_get },
        { .uri = "/api/auth",          .method = HTTP_GET,  .handler = watched, .user_ctx = (void *)auth_state_get },
        { .uri = "/api/auth/session",  .method = HTTP_POST, .handler = watched, .user_ctx = (void *)auth_session_post },
        { .uri = "/update",            .method = HTTP_POST, .handler = update_post },
        R("/api/status",          HTTP_GET,    status_get),
        R("/api/state",           HTTP_GET,    state_get),
        R("/api/devices",         HTTP_GET,    devices_get),
        R("/api/devices/*",       HTTP_POST,   device_dispatch),
        R("/api/devices/*",       HTTP_PATCH,  device_dispatch),
        R("/api/devices/*",       HTTP_DELETE, device_dispatch),
        R("/api/lights*",         HTTP_POST,   light_dispatch),
        R("/api/lights*",         HTTP_PATCH,  light_dispatch),
        R("/api/buttons*",        HTTP_GET,    button_dispatch),
        R("/api/buttons*",        HTTP_POST,   button_dispatch),
        R("/api/buttons*",        HTTP_PATCH,  button_dispatch),
        R("/api/buttons*",        HTTP_DELETE, button_dispatch),
        R("/api/settings",        HTTP_GET,    settings_get_),
        R("/api/settings",        HTTP_POST,   settings_post_),
        R("/api/wifi/scan",       HTTP_GET,    wifi_scan_get),
        R("/api/wifi",            HTTP_POST,   wifi_post),
        R("/api/wifi",            HTTP_DELETE, wifi_forget),
        R("/api/factory_reset",   HTTP_POST,   factory_reset_post),
        R("/api/wifi/resume",     HTTP_POST,   wifi_resume_post),
        R("/api/wifi/test",       HTTP_POST,   wifi_test_post),
        R("/api/wifi/test",       HTTP_GET,    wifi_test_get),
        R("/api/restart",         HTTP_POST,   restart_post),
        R("/api/node/close",      HTTP_POST,   node_close_post),
        R("/api/node/leave",      HTTP_POST,   node_leave_post),
        R("/api/update/prepare",  HTTP_POST,   ota_handle_prepare),
        R("/api/updater/*",       HTTP_POST,   updater_dispatch),
    };
#undef R
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        if (httpd_register_uri_handler(server, &routes[i]) != ESP_OK) {
            ESP_LOGE(TAG, "Could not register %s", routes[i].uri);
        }
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);
    return ESP_OK;
}
