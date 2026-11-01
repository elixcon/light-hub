#include "http_wired.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "btn_gesture.h"
#include "button_db.h"
#include "curve_db.h"
#include "http_util.h"
#include "light_math.h"
#include "local_lights.h"
#include "ntc.h"
#include "settings.h"
#include "thermal.h"

static const char *TAG = "http";

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

void wired_light_to_json(cJSON *d, uint16_t light_id, const zbc_light_t *lp)
{
    const zbc_light_t light = *lp;
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

bool curve_from_json(const cJSON *obj, zbc_curve_t *out)
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

const char *button_pin_problem(int pin)
{
    const char *why = buttons_pin_problem(pin);
    if (why) {
        return why;
    }
    zbc_thermal_cfg_t th;
    settings_thermal(&th);
    if (pin >= 0 && pin == th.gpio) {
        return "that pin reads the overheat sensor";
    }
    return NULL;
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
        const int i = name_index(LIGHT_BOOT_NAMES, ZBC_LIGHT_BOOT_COUNT, boot);
        if (i < 0) {
            return "power_on must be off, on or last";
        }
        out->boot = (uint8_t)i;
    }

    const cJSON *kind = cJSON_GetObjectItem(json, "kind");
    if (kind) {
        const int i = name_index(LIGHT_KIND_NAMES, ZBC_LIGHT_KIND_COUNT, kind);
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
        const int i = name_index(LIGHT_MIX_NAMES, ZBC_MIX_COUNT, mix);
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

esp_err_t light_dispatch(httpd_req_t *req)
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

/* Overheat protection. On every poll, like the rest of the status, so the
 * temperature moves without a revision bump; a change of state bumps one. */
cJSON *thermal_json(void)
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
cJSON *wired_json(void)
{
    local_lights_fade_stats_t f;
    local_lights_fade_stats(&f);
    cJSON *w = cJSON_CreateObject();
    cJSON_AddNumberToObject(w, "fade_ticks", f.ticks);
    cJSON_AddNumberToObject(w, "fade_late", f.late);
    cJSON_AddNumberToObject(w, "fade_max_gap_ms", f.max_gap_ms);
    return w;
}
