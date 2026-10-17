#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "colorspace.h"
#include "curve_db.h"
#include "state_rev.h"

static const char *TAG = "curve_db";

#define CURVE_NVS_NAMESPACE "zbc"
/* In config_backup.c's list too: a backup carries this table under this key. */
#define CURVE_NVS_KEY       "curves"
#define CURVE_BLOB_VERSION  2

typedef struct {
    uint32_t version;
    uint16_t count;
    uint16_t reserved;
} curve_blob_header_t;

static zbc_curveset_t    s_curves[CONFIG_ZBC_MAX_CURVES];
static SemaphoreHandle_t s_lock;

#define CDB_LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define CDB_UNLOCK() xSemaphoreGiveRecursive(s_lock)

/* ------------------------------------------------------------------ maths */

/* >>> host-test region: pchip <<< */
/* Fritsch-Carlson monotone cubic Hermite interpolation.
 *
 * The two tangents of a segment are the average of the secants either side,
 * except where the secants change sign (a turning point, tangent 0) or where the
 * average would be steep enough for the cubic to overshoot - the classic bound is
 * three times the smaller neighbouring secant. That clamp is the whole trick: it
 * is what keeps the curve inside the box its control points define, so a
 * brightness ramp can never dip or bulge past the values the user placed. */
float zbc_pchip(const uint8_t *xs, const float *ys, int n, float t)
{
    if (n < 2) {
        return n == 1 ? ys[0] : 0.0f;
    }
    if (t <= xs[0]) {
        return ys[0];
    }
    if (t >= xs[n - 1]) {
        return ys[n - 1];
    }

    int i = 0;
    while (i < n - 2 && t > xs[i + 1]) {
        i++;
    }

    const float h = (float)xs[i + 1] - (float)xs[i];
    const float d = (ys[i + 1] - ys[i]) / h;

    /* Secants of the neighbouring segments, falling back to this one at the ends
     * so the curve leaves the first point and meets the last one cleanly. */
    const float d_prev = i > 0 ? (ys[i] - ys[i - 1]) / ((float)xs[i] - (float)xs[i - 1]) : d;
    const float d_next = i < n - 2 ? (ys[i + 2] - ys[i + 1]) / ((float)xs[i + 2] - (float)xs[i + 1]) : d;

    float m0, m1;
    if (i == 0) {
        m0 = d;
    } else if (d_prev * d <= 0.0f) {
        m0 = 0.0f;
    } else {
        m0 = (d_prev + d) / 2.0f;
        const float limit = 3.0f * fminf(fabsf(d_prev), fabsf(d));
        if (fabsf(m0) > limit) {
            m0 = m0 > 0.0f ? limit : -limit;
        }
    }
    if (i == n - 2) {
        m1 = d;
    } else if (d * d_next <= 0.0f) {
        m1 = 0.0f;
    } else {
        m1 = (d + d_next) / 2.0f;
        const float limit = 3.0f * fminf(fabsf(d), fabsf(d_next));
        if (fabsf(m1) > limit) {
            m1 = m1 > 0.0f ? limit : -limit;
        }
    }

    const float s  = (t - (float)xs[i]) / h;
    const float s2 = s * s;
    const float s3 = s2 * s;
    return (2.0f * s3 - 3.0f * s2 + 1.0f) * ys[i]
         + (s3 - 2.0f * s2 + s) * h * m0
         + (-2.0f * s3 + 3.0f * s2) * ys[i + 1]
         + (s3 - s2) * h * m1;
}
/* >>> end host-test region: pchip <<< */

static float curve_eval(const zbc_curve_t *c, int input_pct)
{
    float ys[ZBC_CURVE_MAX_POINTS];
    for (int i = 0; i < c->n; i++) {
        ys[i] = (float)c->y[i];
    }
    return zbc_pchip(c->x, ys, c->n, (float)input_pct);
}

/* Where one stop sits on the chromaticity diagram, whichever kind it is. This is
 * the whole trick behind blending warmth into colour: both kinds name a point in
 * the same space, so a segment joining them is just a line. */
static void stop_to_xy(const zbc_chroma_path_t *p, int i, float *x, float *y)
{
    if (p->kind[i] == ZBC_STOP_KELVIN) {
        zbc_kelvin_to_xy(p->kelvin[i], x, y);
    } else {
        zbc_hs_to_xy(p->hue[i], p->sat[i], x, y);
    }
}

/* Hue and saturation across a run of colour stops, [a, a+m).
 *
 * Hue is an angle, so consecutive stops are unwrapped onto a continuous line
 * before interpolation - otherwise a path from 350 to 10 degrees sweeps the long
 * way round the wheel through green instead of crossing red. */
static void hs_run_eval(const zbc_chroma_path_t *p, int a, int m, int input_pct,
                        int *out_hue, int *out_sat)
{
    float hues[ZBC_CHROMA_MAX_STOPS], sats[ZBC_CHROMA_MAX_STOPS];
    hues[0] = (float)p->hue[a];
    sats[0] = (float)p->sat[a];
    for (int i = 1; i < m; i++) {
        float delta = (float)p->hue[a + i] - (float)p->hue[a + i - 1];
        while (delta > 180.0f) {
            delta -= 360.0f;
        }
        while (delta < -180.0f) {
            delta += 360.0f;
        }
        hues[i] = hues[i - 1] + delta;
        sats[i] = (float)p->sat[a + i];
    }

    float h = fmodf(zbc_pchip(&p->x[a], hues, m, (float)input_pct), 360.0f);
    if (h < 0.0f) {
        h += 360.0f;
    }
    float s = zbc_pchip(&p->x[a], sats, m, (float)input_pct);

    *out_hue = (int)lroundf(h) % 360;
    *out_sat = (int)lroundf(s < 0.0f ? 0.0f : (s > 100.0f ? 100.0f : s));
}

static uint16_t to_zcl_xy(float v)
{
    float s = v * 65535.0f;
    return (uint16_t)(s < 0.0f ? 0.0f : (s > 65535.0f ? 65535.0f : s));
}

/* One input percentage to one colour, by the segment rule in curve_db.h. */
static void chroma_eval(const zbc_chroma_path_t *p, int input_pct, zbc_lamp_target_t *out)
{
    if (p->n < 2) {
        return;
    }

    /* Landing exactly on a warmth stop is a warmth, whichever segment claims it.
     *
     * A stop is where two segments meet, and where their kinds differ it has two
     * readings at once: a stop at 10 % between a colour at 1 % and warmth above
     * is both the far end of the blend and the near end of the warmth run. The
     * colour is the same point either way - the path passes exactly through
     * every stop - but the command is not. MoveToColorTemperature leaves the
     * lamp in the mode the rest of that run will use and keeps the lamp's own
     * white calibration; the xy reading puts it in colour mode for that one
     * step, and the next step out of it costs a change of kind, which is always
     * sent whatever the deadbands say. */
    for (int j = 0; j < p->n; j++) {
        if (p->x[j] == input_pct && p->kind[j] == ZBC_STOP_KELVIN) {
            out->out    = ZBC_OUT_CT;
            out->kelvin = p->kelvin[j];
            return;
        }
    }

    /* The bracketing segment, found the same way zbc_pchip finds it. */
    int i = 0;
    while (i < p->n - 2 && input_pct > p->x[i + 1]) {
        i++;
    }

    if (p->kind[i] == p->kind[i + 1]) {
        /* Widen to the whole run of that kind. A run is contiguous, so no stop of
         * the other kind can sit inside the series being interpolated, and the
         * tangents at a run's ends fall back to the segment secant exactly as
         * zbc_pchip already does at the ends of a curve. */
        int a = i, b = i + 1;
        while (a > 0 && p->kind[a - 1] == p->kind[i]) {
            a--;
        }
        while (b < p->n - 1 && p->kind[b + 1] == p->kind[i]) {
            b++;
        }
        const int m = b - a + 1;

        if (p->kind[i] == ZBC_STOP_KELVIN) {
            float ys[ZBC_CHROMA_MAX_STOPS];
            for (int j = 0; j < m; j++) {
                ys[j] = (float)p->kelvin[a + j];
            }
            out->out    = ZBC_OUT_CT;
            out->kelvin = (int)lroundf(zbc_pchip(&p->x[a], ys, m, (float)input_pct));
        } else {
            out->out = ZBC_OUT_HS;
            hs_run_eval(p, a, m, input_pct, &out->hue, &out->sat);
        }
        return;
    }

    /* A segment that changes kind. Kelvin and hue have nothing in common, so the
     * blend happens in CIE xy and the segment is a straight line between the two
     * endpoints' chromaticities. A pchip over two knots is that line anyway: with
     * both tangents equal to the secant, the Hermite cubic degenerates to it. */
    float x0, y0, x1, y1;
    stop_to_xy(p, i,     &x0, &y0);
    stop_to_xy(p, i + 1, &x1, &y1);

    const float span = (float)p->x[i + 1] - (float)p->x[i];
    float t = span > 0.0f ? ((float)input_pct - (float)p->x[i]) / span : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);

    out->out   = ZBC_OUT_XY;
    out->cie_x = to_zcl_xy(x0 + (x1 - x0) * t);
    out->cie_y = to_zcl_xy(y0 + (y1 - y0) * t);
}

/* --------------------------------------------------------------- validation */

/* `max` differs between the two tables: a level curve holds six points, a colour
 * path eight, because warmth and colour share one budget. */
static bool valid_x(const uint8_t *x, uint8_t n, uint8_t max)
{
    if (n < 2 || n > max) {
        return false;
    }
    if (x[0] != 1 || x[n - 1] != 100) {
        return false;
    }
    for (int i = 1; i < n; i++) {
        if (x[i] <= x[i - 1]) {
            return false;  /* a repeated or descending x divides by zero below */
        }
    }
    return true;
}

static bool valid_curve(const zbc_curve_t *c, uint16_t y_min, uint16_t y_max)
{
    if (c->n == 0) {
        return true;
    }
    if (!valid_x(c->x, c->n, ZBC_CURVE_MAX_POINTS)) {
        return false;
    }
    for (int i = 0; i < c->n; i++) {
        if (c->y[i] < y_min || c->y[i] > y_max) {
            return false;
        }
    }
    return true;
}

static bool valid_chroma(const zbc_chroma_path_t *p)
{
    if (p->n == 0) {
        return true;
    }
    if (!valid_x(p->x, p->n, ZBC_CHROMA_MAX_STOPS)) {
        return false;
    }
    for (int i = 0; i < p->n; i++) {
        if (p->kind[i] == ZBC_STOP_KELVIN) {
            if (p->kelvin[i] < 1000 || p->kelvin[i] > 10000) {
                return false;
            }
        } else if (p->kind[i] == ZBC_STOP_COLOR) {
            if (p->hue[i] > 359 || p->sat[i] > 100) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ table */

static zbc_curveset_t *find(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    for (int i = 0; i < CONFIG_ZBC_MAX_CURVES; i++) {
        if (s_curves[i].in_use && s_curves[i].ep == ep &&
            memcmp(s_curves[i].ieee, ieee, ZBC_IEEE_LEN) == 0) {
            return &s_curves[i];
        }
    }
    return NULL;
}

static bool set_is_empty(const zbc_curveset_t *c)
{
    return c->level.n == 0 && c->chroma.n == 0;
}

static void reset_runtime(zbc_curveset_t *c)
{
    c->last_input_pct = ZBC_UNKNOWN;
    c->sent_kelvin    = ZBC_UNKNOWN;
    c->sent_hue       = ZBC_UNKNOWN;
    c->sent_sat       = ZBC_UNKNOWN;
    c->sent_x         = ZBC_UNKNOWN;
    c->sent_y         = ZBC_UNKNOWN;
    c->sent_mode      = ZBC_OUT_NONE;
    c->sent_off       = ZBC_UNKNOWN;
}

static esp_err_t cdb_save_locked(void)
{
    zbc_state_bump();          /* the page shows this table */
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CURVE_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    size_t used = 0;
    for (int i = 0; i < CONFIG_ZBC_MAX_CURVES; i++) {
        if (s_curves[i].in_use) {
            used++;
        }
    }

    size_t blob_size = sizeof(curve_blob_header_t) + used * sizeof(zbc_curveset_t);
    uint8_t *blob = calloc(1, blob_size);
    if (!blob) {
        nvs_close(nvs);
        return ESP_ERR_NO_MEM;
    }

    curve_blob_header_t hdr = { .version = CURVE_BLOB_VERSION, .count = (uint16_t)used };
    memcpy(blob, &hdr, sizeof(hdr));

    zbc_curveset_t *out = (zbc_curveset_t *)(blob + sizeof(hdr));
    for (int i = 0; i < CONFIG_ZBC_MAX_CURVES; i++) {
        if (!s_curves[i].in_use) {
            continue;
        }
        *out = s_curves[i];
        reset_runtime(out);  /* what was last on air is not a property of the curve */
        out++;
    }

    err = nvs_set_blob(nvs, CURVE_NVS_KEY, blob, blob_size);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    free(blob);
    nvs_close(nvs);
    return err;
}

bool curve_db_blob_ok(const void *blob, size_t len)
{
    curve_blob_header_t hdr;
    if (!blob || len < sizeof(hdr)) {
        return false;
    }
    memcpy(&hdr, blob, sizeof(hdr));
    return hdr.version == CURVE_BLOB_VERSION && len == sizeof(hdr) + (size_t)hdr.count * sizeof(zbc_curveset_t);
}

static void cdb_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open(CURVE_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    size_t blob_size = 0;
    esp_err_t err = nvs_get_blob(nvs, CURVE_NVS_KEY, NULL, &blob_size);
    if (err != ESP_OK || blob_size < sizeof(curve_blob_header_t)) {
        nvs_close(nvs);
        return;
    }

    uint8_t *blob = malloc(blob_size);
    if (!blob) {
        nvs_close(nvs);
        return;
    }
    err = nvs_get_blob(nvs, CURVE_NVS_KEY, blob, &blob_size);
    nvs_close(nvs);
    if (err != ESP_OK) {
        free(blob);
        return;
    }

    if (!curve_db_blob_ok(blob, blob_size)) {
        ESP_LOGW(TAG, "Stored curves are from an incompatible build, discarding");
        free(blob);
        return;
    }
    curve_blob_header_t hdr;
    memcpy(&hdr, blob, sizeof(hdr));

    const zbc_curveset_t *in = (const zbc_curveset_t *)(blob + sizeof(hdr));
    int loaded = 0;
    for (int i = 0; i < hdr.count && loaded < CONFIG_ZBC_MAX_CURVES; i++) {
        s_curves[loaded]        = in[i];
        s_curves[loaded].in_use = true;
        reset_runtime(&s_curves[loaded]);
        loaded++;
    }
    free(blob);
    ESP_LOGI(TAG, "Restored curves for %d light(s) from NVS", loaded);
}

esp_err_t curve_db_init(void)
{
    if (s_lock) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    memset(s_curves, 0, sizeof(s_curves));
    CDB_LOCK();
    cdb_load();
    CDB_UNLOCK();
    return ESP_OK;
}

bool curve_db_get(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, zbc_curveset_t *out)
{
    CDB_LOCK();
    zbc_curveset_t *c = find(ieee, ep);
    if (c && out) {
        *out = *c;
    }
    bool found = c != NULL;
    CDB_UNLOCK();
    return found;
}

esp_err_t curve_db_set(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, const zbc_curveset_t *in)
{
    if (!in) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Level may map to 0, which means "switch this lamp off"; kelvin is only
     * sanity-checked here and clamped to the lamp's own range when it is used. */
    if (!valid_curve(&in->level, 0, 100) || !valid_chroma(&in->chroma)) {
        return ESP_ERR_INVALID_ARG;
    }

    CDB_LOCK();
    zbc_curveset_t *c = find(ieee, ep);
    if (set_is_empty(in)) {
        /* Nothing left to store; give the row back rather than keep an empty one. */
        if (c) {
            memset(c, 0, sizeof(*c));
            esp_err_t err = cdb_save_locked();
            CDB_UNLOCK();
            return err;
        }
        CDB_UNLOCK();
        return ESP_OK;
    }

    if (!c) {
        for (int i = 0; i < CONFIG_ZBC_MAX_CURVES; i++) {
            if (!s_curves[i].in_use) {
                c = &s_curves[i];
                break;
            }
        }
        if (!c) {
            CDB_UNLOCK();
            ESP_LOGE(TAG, "Curve table full (%d)", CONFIG_ZBC_MAX_CURVES);
            return ESP_ERR_NO_MEM;
        }
    }

    memset(c, 0, sizeof(*c));
    c->in_use = true;
    memcpy(c->ieee, ieee, ZBC_IEEE_LEN);
    c->ep     = ep;
    c->level  = in->level;
    c->chroma = in->chroma;
    reset_runtime(c);

    esp_err_t err = cdb_save_locked();
    CDB_UNLOCK();
    return err;
}

esp_err_t curve_db_clear(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    CDB_LOCK();
    zbc_curveset_t *c = find(ieee, ep);
    if (!c) {
        CDB_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    memset(c, 0, sizeof(*c));
    esp_err_t err = cdb_save_locked();
    CDB_UNLOCK();
    return err;
}

bool curve_db_active(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep,
                     bool *level, bool *chroma)
{
    CDB_LOCK();
    zbc_curveset_t *c = find(ieee, ep);
    bool any = false;
    if (level) {
        *level = c && c->level.n > 0;
    }
    if (chroma) {
        *chroma = c && c->chroma.n > 0;
    }
    if (c) {
        any = !set_is_empty(c);
    }
    CDB_UNLOCK();
    return any;
}

/* A level curve's output for one input, as the whole percent that is sent. */
static int level_out_pct(const zbc_curve_t *c, int input_pct)
{
    int p = (int)lroundf(curve_eval(c, input_pct));
    return p < 0 ? 0 : (p > 100 ? 100 : p);
}

void curve_db_eval(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, int input_pct,
                   zbc_lamp_target_t *out)
{
    memset(out, 0, sizeof(*out));
    out->pct = input_pct;

    CDB_LOCK();
    zbc_curveset_t *c = find(ieee, ep);
    if (c) {
        if (c->level.n) {
            out->pct = level_out_pct(&c->level, input_pct);
        }
        chroma_eval(&c->chroma, input_pct, out);
    }
    CDB_UNLOCK();
}

int curve_db_input_for(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, int level_pct)
{
    int best = level_pct;
    CDB_LOCK();
    const zbc_curveset_t *c = find(ieee, ep);
    if (c && c->level.n) {
        int best_miss = 1000;
        for (int in = 1; in <= 100; in++) {
            const int miss = abs(level_out_pct(&c->level, in) - level_pct);
            if (miss <= best_miss) {
                best      = in;
                best_miss = miss;
            }
        }
    }
    CDB_UNLOCK();
    return best;
}

int curve_db_input_pct(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    CDB_LOCK();
    zbc_curveset_t *c = find(ieee, ep);
    int pct = c ? c->last_input_pct : ZBC_UNKNOWN;
    CDB_UNLOCK();
    return pct;
}

void curve_db_note_sent(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, int input_pct, bool off)
{
    CDB_LOCK();
    zbc_curveset_t *c = find(ieee, ep);
    if (c) {
        c->last_input_pct = (int16_t)input_pct;
        c->sent_off       = off ? 1 : 0;
        zbc_state_bump();             /* input_pct is what the slider binds to */
    }
    CDB_UNLOCK();
}

void curve_db_forget_sent(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    CDB_LOCK();
    zbc_curveset_t *c = find(ieee, ep);
    if (c) {
        const int16_t keep = c->last_input_pct;
        reset_runtime(c);
        c->last_input_pct = keep;
    }
    CDB_UNLOCK();
}

void curve_db_note_color_sent(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep,
                              const zbc_lamp_target_t *t)
{
    CDB_LOCK();
    zbc_curveset_t *c = find(ieee, ep);
    if (c) {
        /* Only the channel that was actually used is recorded, and the others are
         * forgotten. Leaving a stale value behind is what would let the next
         * command through a deadband it should have been stopped by - or hold it
         * back against a colour the lamp is no longer showing. */
        c->sent_kelvin = ZBC_UNKNOWN;
        c->sent_hue    = ZBC_UNKNOWN;
        c->sent_sat    = ZBC_UNKNOWN;
        c->sent_x      = ZBC_UNKNOWN;
        c->sent_y      = ZBC_UNKNOWN;
        switch (t->out) {
        case ZBC_OUT_CT:
            c->sent_kelvin = t->kelvin;
            break;
        case ZBC_OUT_HS:
            c->sent_hue = (int16_t)t->hue;
            c->sent_sat = (int16_t)t->sat;
            break;
        case ZBC_OUT_XY:
            c->sent_x = t->cie_x;
            c->sent_y = t->cie_y;
            break;
        default:
            break;
        }
        c->sent_mode = (int8_t)t->out;
    }
    CDB_UNLOCK();
}

void curve_db_forget_device(const uint8_t ieee[ZBC_IEEE_LEN])
{
    bool changed = false;
    CDB_LOCK();
    for (int i = 0; i < CONFIG_ZBC_MAX_CURVES; i++) {
        if (s_curves[i].in_use && memcmp(s_curves[i].ieee, ieee, ZBC_IEEE_LEN) == 0) {
            memset(&s_curves[i], 0, sizeof(s_curves[i]));
            changed = true;
        }
    }
    if (changed) {
        cdb_save_locked();
    }
    CDB_UNLOCK();
}

esp_err_t curve_db_erase(void)
{
    CDB_LOCK();
    memset(s_curves, 0, sizeof(s_curves));
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CURVE_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_erase_key(nvs, CURVE_NVS_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    CDB_UNLOCK();
    return err;
}
