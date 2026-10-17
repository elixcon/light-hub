#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "al_db.h"
#include "curve_db.h"
#include "heap_watch.h"
#include "light_db.h"
#include "lighting.h"
#include "local_lights.h"
#include "matter_ctl.h"
#include "mlight_db.h"
#include "settings.h"
#include "state_rev.h"
#include "sun.h"

static const char *TAG = "lighting";

/* How far a mapped colour value has to move before it is worth another radio
 * transmission. Without these a one-percent slider step on a six-lamp group puts
 * eighteen commands on air instead of six; the colour follows in steps too small
 * for the eye to notice. */
#define CT_DEADBAND_K    40
#define HUE_DEADBAND_DEG 3
#define SAT_DEADBAND_PCT 3

/* The same, for a sweep. A held button walks a whole colour curve in a couple of
 * seconds, and the ordinary deadbands would let a colour command out beside
 * every level command, doubling the traffic where it is already tightest: the
 * lamps' reports then come back out of order and the light lags the finger.
 * Coarse deadbands cut the colour to a few steps across the sweep - warmth is
 * not what anybody watches while holding a dimmer. */
#define CT_DEADBAND_RAMP_K    800
#define HUE_DEADBAND_RAMP_DEG 20
#define SAT_DEADBAND_RAMP_PCT 20

/* The same again for a blend across a colour/warmth boundary, measured in CIE
 * xy. Sized against the sRGB triangle to be about as perceptible as the pair
 * above: its perimeter is 1.554 in xy, so 3 degrees of hue is 0.011-0.014
 * (712-918 units of 65536); the mean distance from white to a primary is 0.304,
 * so 3 % saturation is 0.008-0.010 (533-643 units). The hue/saturation test is
 * "either moved enough", so the effective threshold is the smaller, and this
 * matches it. Coarse is seven times it, as 20 is to 3.
 *
 * Looser than CT_DEADBAND_K (about 194 of these units) on purpose: MacAdam
 * ellipses are smallest near the white point, so the same xy distance is far
 * more visible in a white than in a saturated colour. */
#define XY_DEADBAND       512
#define XY_DEADBAND_RAMP 3584

/* >>> host-test region: alstate <<< */
/* Adaptive Lighting's decisions, kept free of everything but plain C so
 * test/test_alstate.c can drive them: whether a group's mode is doing anything,
 * what colour a lamp gets, and whether a minute's drift is worth a command. */

#define AL_NO_OFFSET     (-32768)
/* The kelvin a shift has to move before it is sent again. The same figure as
 * CT_DEADBAND_K: a whole day's swing is a couple of thousand kelvin, so this is
 * a command every few minutes at the steepest and none for hours at the flat. */
#define AL_DEADBAND_K    40

typedef enum {
    AL_OFF = 0,        /* the mode is not running */
    AL_ON,
    AL_NO_CLOCK,       /* running, waiting for SNTP */
    AL_NO_LOCATION,    /* running, waiting for somebody to enter a location */
} al_reason_t;

typedef struct {
    bool     used;
    uint16_t group_id;
    bool     running;          /* started, and not yet ended by an off or a hand */
    int16_t  last_pct;         /* -1 nothing sent since it started */
    int32_t  last_offset_k;    /* AL_NO_OFFSET nothing sent since it started */
    uint8_t  reason;           /* the al_reason_t last logged */
} al_group_t;

/* Whether a group's mode is changing its lights. A mode that is running without a
 * clock or a location still counts as running - a controller that started it
 * must not see it switch itself off - it just has nothing to send yet, and the
 * clock is reported first because a location is no use without one. */
static al_reason_t al_reason(bool running, bool clock_ok, bool located)
{
    if (!running) {
        return AL_OFF;
    }
    if (!clock_ok) {
        return AL_NO_CLOCK;
    }
    if (!located) {
        return AL_NO_LOCATION;
    }
    return AL_ON;
}

static bool al_reason_active(al_reason_t r)
{
    return r == AL_ON;
}

/* The colour temperature a lamp is sent while it is shifted, or 0 to leave its
 * colour alone. A warmth curve's kelvin is shifted; a lamp that does colour
 * temperature but whose curve says nothing about colour is shifted from the
 * base; a curve that asks for a colour, or a blend towards one, is not touched -
 * a hue is not a temperature. The caller still clamps to the lamp's own range. */
static int al_shifted_kelvin(bool curve_ct, bool curve_none, int curve_k, bool lamp_ct,
                             int base_k, int offset_k)
{
    if (!lamp_ct || offset_k == AL_NO_OFFSET) {
        return 0;
    }
    int k;
    if (curve_ct) {
        k = curve_k + offset_k;
    } else if (curve_none) {
        k = base_k + offset_k;
    } else {
        return 0;
    }
    return k < 1000 ? 1000 : (k > 20000 ? 20000 : k);
}

/* Whether a tick has anything to send: a new percentage, a first send, or a
 * shift that has moved past the deadband. */
static bool al_needs_send(int pct, int last_pct, int offset_k, int last_offset_k)
{
    if (pct != last_pct || last_offset_k == AL_NO_OFFSET) {
        return true;
    }
    return abs(offset_k - last_offset_k) >= AL_DEADBAND_K;
}

/* The value the day's shape gives at a phase. With no points between the ends it
 * is the straight line from the midnight value to the noon value, zbc_al_mix -
 * the two-value profile exactly, so a day that has never had a shape drawn does
 * what it always did. With points, the monotone cubic every curve here uses,
 * through the midnight value at x 1, the points, and the noon value at x 100;
 * sunrise and sunset are x 50.5. */
static int al_shape(double phase, int at_midnight, int at_noon, int n, const uint8_t *xs, const int16_t *ys)
{
    if (n <= 0) {
        return zbc_al_mix(phase, at_midnight, at_noon);
    }
    if (n > ZBC_AL_SHAPE_POINTS) {
        n = ZBC_AL_SHAPE_POINTS;
    }
    if (phase < -1.0) {
        phase = -1.0;
    } else if (phase > 1.0) {
        phase = 1.0;
    }
    uint8_t x[ZBC_AL_SHAPE_POINTS + 2];
    float   y[ZBC_AL_SHAPE_POINTS + 2];
    x[0] = 1;
    y[0] = (float)at_midnight;
    for (int i = 0; i < n; i++) {
        x[i + 1] = xs[i];
        y[i + 1] = (float)ys[i];
    }
    x[n + 1] = 100;
    y[n + 1] = (float)at_noon;
    return (int)lroundf(zbc_pchip(x, y, n + 2, (float)(1.0 + 99.0 * (phase + 1.0) / 2.0)));
}
/* >>> end host-test region: alstate <<< */

/* Adaptive Lighting's colour shift, as it travels with a brightness command to
 * every member of a group: the offset the group's day gives now, and the base
 * that day shifts a lamp with no warmth curve from. Both are the group's, so a
 * lamp in two rooms is shifted by whichever room sent the command. */
typedef struct {
    int32_t  offset_k;     /* AL_NO_OFFSET: no shift, the lamp's own curve */
    uint16_t base_k;
} al_shift_t;

#define AL_NO_SHIFT ((al_shift_t){ .offset_k = AL_NO_OFFSET, .base_k = 0 })

_Static_assert(ZBC_AL_SHAPE_POINTS + 2 == ZBC_CURVE_MAX_POINTS,
               "the page edits a shape with the curve editor, which allows ZBC_CURVE_MAX_POINTS");

/* Which groups are running the mode and what the last tick sent them. The set of
 * running groups is also stored (al_db_set_running, by the Adaptive Lighting task
 * alone) and resumed at boot; what was sent is not. Written from the HTTP,
 * the buttons, the Matter bridge and the Adaptive Lighting task, and every access
 * is a few assignments, so a spinlock rather than a mutex. */
static al_group_t   s_al[CONFIG_ZBC_MAX_GROUPS];
static portMUX_TYPE s_al_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_al_task;

/* Why the task was woken before the minute was up, as notification bits: to tick
 * now (a day, the place or the zone changed), or only to store the running set (a
 * start or an end, which send their own commands). */
#define AL_WAKE_TICK  (1u << 0)
#define AL_WAKE_STORE (1u << 1)

static void al_wake(uint32_t why)
{
    if (s_al_task) {
        xTaskNotify(s_al_task, why, eSetBits);
    }
}

/* Called with s_al_mux held. */
static al_group_t *al_slot_locked(uint16_t id, bool create)
{
    al_group_t *spare = NULL;
    for (size_t i = 0; i < sizeof(s_al) / sizeof(s_al[0]); i++) {
        if (s_al[i].used && s_al[i].group_id == id) {
            return &s_al[i];
        }
        if (!spare && !s_al[i].used) {
            spare = &s_al[i];
        }
    }
    if (!create || !spare) {
        return NULL;
    }
    memset(spare, 0, sizeof(*spare));
    spare->used          = true;
    spare->group_id      = id;
    spare->last_pct      = -1;
    spare->last_offset_k = AL_NO_OFFSET;
    return spare;
}

/* SNTP has answered. Any time before 2025 is the clock counting up from 1970. */
static bool al_clock_ok(void)
{
    return time(NULL) >= 1735689600;
}

/* The sun's day around `now`, as it is and as a group's mode uses it: sunrise
 * and sunset moved into the limits of that group's day. The limits are local clock times, so
 * without a time zone they do nothing - read as UTC they would be an hour or two
 * out. The zone's offset is the one at the day's noon: a clock change happens in
 * the small hours, and a limit falls either side of noon. */
static bool al_day(int64_t now, const zbc_al_profile_t *prof, zbc_sun_day_t *real, zbc_sun_day_t *used)
{
    int32_t lat, lon;
    if (!settings_location(&lat, &lon) || !zbc_sun_day(now, lat / 1e6, lon / 1e6, real)) {
        return false;
    }
    *used = *real;
    int32_t off;
    if (settings_tz_offset(real->noon, &off)) {
        zbc_sun_stretch(used, off, prof->rise_win, prof->set_win);
    }
    return true;
}

/* What Adaptive Lighting asks of a group at this moment, from the group's own
 * day. False without a clock or a place. */
static bool al_target(uint16_t group_id, int *pct, al_shift_t *shift)
{
    if (!al_clock_ok()) {
        return false;
    }
    zbc_al_profile_t prof;
    al_db_day(group_id, &prof);
    const int64_t now = (int64_t)time(NULL);
    zbc_sun_day_t real, day;
    if (!al_day(now, &prof, &real, &day)) {
        return false;
    }
    const double phase = zbc_sun_phase(now, &day);
    int16_t      pct_y[ZBC_AL_SHAPE_POINTS];
    for (int i = 0; i < ZBC_AL_SHAPE_POINTS; i++) {
        pct_y[i] = prof.pct_y[i];
    }
    const int p = al_shape(phase, prof.pct_midnight, prof.pct_noon, prof.n_pct, prof.pct_x, pct_y);
    *pct            = p < 1 ? 1 : (p > 100 ? 100 : p);
    shift->offset_k = al_shape(phase, prof.k_midnight, prof.k_noon, prof.n_k, prof.k_x, prof.k_y);
    shift->base_k   = prof.k_base;
    return true;
}

static al_reason_t group_al_reason(uint16_t id, al_group_t *copy)
{
    al_group_t snap = { .last_pct = -1, .last_offset_k = AL_NO_OFFSET };
    portENTER_CRITICAL(&s_al_mux);
    const al_group_t *g = al_slot_locked(id, false);
    if (g) {
        snap = *g;
    }
    portEXIT_CRITICAL(&s_al_mux);
    if (copy) {
        *copy = snap;
    }
    return al_reason(snap.running, al_clock_ok(), settings_location(NULL, NULL));
}

static void al_note_sent(uint16_t id, int pct, int offset_k)
{
    portENTER_CRITICAL(&s_al_mux);
    al_group_t *g = al_slot_locked(id, false);
    if (g) {
        g->last_pct      = (int16_t)pct;
        g->last_offset_k = offset_k;
    }
    portEXIT_CRITICAL(&s_al_mux);
}

/* Marks a group's mode running with nothing sent yet - a start and a resume alike.
 * Nothing is sent here either. */
static esp_err_t al_begin(uint16_t id)
{
    if (!group_db_get(id, NULL)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (al_db_expose(id) == ZBC_AL_EXPOSE_OFF) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    portENTER_CRITICAL(&s_al_mux);
    al_group_t *g = al_slot_locked(id, true);
    if (g) {
        g->running       = true;
        g->last_pct      = -1;
        g->last_offset_k = AL_NO_OFFSET;
    }
    portEXIT_CRITICAL(&s_al_mux);
    if (!g) {
        return ESP_ERR_NO_MEM;
    }
    zbc_state_bump();
    return ESP_OK;
}

/* Ends a group's mode, and says whether there was one to end. Nothing is sent: the
 * lights stay where they are. */
static bool al_end(uint16_t id)
{
    bool was = false;
    portENTER_CRITICAL(&s_al_mux);
    al_group_t *g = al_slot_locked(id, false);
    if (g && g->running) {
        was              = true;
        g->running       = false;
        g->last_pct      = -1;
        g->last_offset_k = AL_NO_OFFSET;
    }
    portEXIT_CRITICAL(&s_al_mux);
    if (was) {
        zbc_state_bump();
        al_wake(AL_WAKE_STORE);
    }
    return was;
}

/* A hand on the group ends the mode: somebody wants the lights the way they set
 * them, and a mode that pulled them back a minute later would fight them. */
static void al_note_manual(uint16_t id, const char *what)
{
    if (al_end(id)) {
        ESP_LOGI(TAG, "Adaptive Lighting ended for group %u: %s by hand", id, what);
    }
}

static bool al_any_running(void)
{
    bool any = false;
    portENTER_CRITICAL(&s_al_mux);
    for (size_t i = 0; i < sizeof(s_al) / sizeof(s_al[0]) && !any; i++) {
        any = s_al[i].used && s_al[i].running;
    }
    portEXIT_CRITICAL(&s_al_mux);
    return any;
}

/* The same for one light, which ends it for every group that light is in. */
static void al_note_manual_endpoint(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, const char *what)
{
    /* The common case is that no group is running the mode, and a slider drag
     * must not copy the group table on every step for nothing. */
    if (!al_any_running()) {
        return;
    }
    zbc_group_t *groups = calloc(CONFIG_ZBC_MAX_GROUPS, sizeof(*groups));
    if (!groups) {
        return;
    }
    const size_t n = group_db_snapshot(groups, CONFIG_ZBC_MAX_GROUPS);
    for (size_t i = 0; i < n; i++) {
        for (int m = 0; m < groups[i].member_count; m++) {
            if (groups[i].members[m].ep == ep &&
                memcmp(groups[i].members[m].ieee, ieee, ZBC_IEEE_LEN) == 0) {
                al_note_manual(groups[i].id, what);
                break;
            }
        }
    }
    free(groups);
}

/* What this module last said to an endpoint, and when.
 *
 * `pct` is the brightness to come back to when the lamp is switched on again:
 * fading off drives its level to zero, so the level it had before has to be
 * held here or it is simply gone. `sent_us` and `sent_off` are what the fade-in
 * decision reads - see endpoint_is_dark.
 *
 * Runtime only, and not worth persisting: after a reboot the lamp's own level
 * and its own report are as good a guess as any. */
static struct {
    bool    used;
    uint8_t ieee[ZBC_IEEE_LEN];
    uint8_t ep;
    uint8_t pct;          /* 0 until one is known - see recall_level */
    int64_t sent_us;      /* when we last told this endpoint anything */
    bool    sent_off;     /* ...and whether what we told it was an off */
} s_restore[CONFIG_ZBC_MAX_DEVICES * CONFIG_ZBC_MAX_ENDPOINTS_PER_DEVICE + CONFIG_ZBC_MAX_MATTER_LIGHTS];

static int restore_slot(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, bool create)
{
    int spare = -1;
    for (size_t i = 0; i < sizeof(s_restore) / sizeof(s_restore[0]); i++) {
        if (s_restore[i].used && s_restore[i].ep == ep &&
            memcmp(s_restore[i].ieee, ieee, ZBC_IEEE_LEN) == 0) {
            return (int)i;
        }
        if (spare < 0 && !s_restore[i].used) {
            spare = (int)i;
        }
    }
    if (!create || spare < 0) {
        return -1;
    }
    s_restore[spare].used = true;
    memcpy(s_restore[spare].ieee, ieee, ZBC_IEEE_LEN);
    s_restore[spare].ep = ep;
    return spare;
}

void lighting_forget_device(const uint8_t ieee[ZBC_IEEE_LEN])
{
    for (size_t i = 0; i < sizeof(s_restore) / sizeof(s_restore[0]); i++) {
        if (s_restore[i].used && memcmp(s_restore[i].ieee, ieee, ZBC_IEEE_LEN) == 0) {
            memset(&s_restore[i], 0, sizeof(s_restore[i]));
        }
    }
}

static void remember_level(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, int pct)
{
    const int i = restore_slot(ieee, ep, true);
    if (i >= 0) {
        s_restore[i].pct = (uint8_t)pct;
    }
}

static int recall_level(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    /* 0 is "nothing remembered": note_sent creates the slot too, for the lit/off
     * belief, and leaves pct at 0, which read as a level would switch the lamp
     * on at level 1. Nothing remembers 0: a curve output of 0 is an off and
     * returns before remember_level. */
    const int i = restore_slot(ieee, ep, false);
    return i >= 0 && s_restore[i].pct > 0 ? s_restore[i].pct : -1;
}

/* What we last told this endpoint to do. Called from the out_* transports
 * below, after the command, so a refused one leaves the belief where it was -
 * and so a command path added later cannot forget to record it. */
static void note_sent(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, bool lit)
{
    const int i = restore_slot(ieee, ep, true);
    if (i >= 0) {
        s_restore[i].sent_us  = esp_timer_get_time();
        s_restore[i].sent_off = !lit;
    }
}

/* >>> host-test region: fadein <<< */
/* How long a report is given to arrive before the lamp's own word is believed
 * again. A slider dragged across a lamp that is off sends its next step long
 * before the On/Off report of the first one gets back, and presetting to the
 * bottom on every one of those steps is a dip down the whole length of the
 * drag. Generous: On/Off reports have a minimum interval of 0. */
#define LIT_REPORT_LAG_US 2000000LL

/* The level a warm off leaves a lamp at while it is dark: 1 %, which is what
 * off_endpoint dims to before the colour and the off. */
#define PARKED_LEVEL 3

/* Whether a brightness for this endpoint has to start at the bottom of the
 * lamp's range.
 *
 * A lamp switched off with OffWithEffect keeps its CurrentLevel, so a plain
 * move-to-level with on/off lights it at the level it held and then ramps: asked
 * for 10 % after an evening at full, it flares to full and slides down, which
 * looks like an overshoot being corrected. The preset gives the ramp somewhere
 * to start.
 *
 * A lamp already at the bottom needs none - every lamp a warm off parked at
 * PARKED_LEVEL: the move lights it there and ramps. The preset is a frame on the
 * critical path of every switch-on, and any frame may cost the stack's 1.6 s
 * retry. A Hue LST002 lit this way still takes the colour that follows.
 *
 * The lamp's own report is the authority, with two exceptions. Our own off is
 * newer than a report that has not caught up (a Hue reports off only once the
 * 0.8 s effect has run), and a lamp that says it is lit is never preset: that
 * would be a visible dip, and means our off was lost or something else switched
 * it back on.
 *
 * Nothing sent yet reads as `sent_off` false at time 0, far enough in the past
 * for the lamp's own word to decide - the right answer after a restart. */
static bool zbc_should_fade_in(bool fade, int reported_on, int level,
                               bool sent_off, int64_t sent_us, int64_t now_us)
{
    if (!fade || level == ZBC_UNKNOWN || level <= PARKED_LEVEL) {
        return false;      /* nothing to fade from, or already at the bottom */
    }
    if (sent_off) {
        return reported_on != 1;
    }
    return reported_on == 0 && now_us - sent_us > LIT_REPORT_LAG_US;
}

/* How long what we last told a lamp outranks what it last said, when deciding
 * what a press means. Longer than LIT_REPORT_LAG_US, because an off takes far
 * longer to be heard than an on: a warm off is three gated frames, a Hue reports
 * off only once its 0.8 s effect has run, and a lamp whose first settle read was
 * skipped (it had reported something else meanwhile) is read again only 3.7 s
 * after the last frame. Six seconds leaves room for one 1.6 s APS retry on top. */
#define OWN_WORD_US 6000000LL

/* Whether an endpoint is lit, as a decision has to read it: on (1), off (0) or
 * not known (ZBC_UNKNOWN).
 *
 * The lamp's own word, unless we told it something so recently that its word
 * cannot have caught up - then what we told it. Read from the report alone, a
 * room switched off three seconds ago still has a member saying "on", so the
 * next press switches it off again instead of on, and an off sent to a lamp
 * already dark is the warm off's dim to PARKED_LEVEL: it lights the lamp, then
 * switches it off.
 *
 * `sent_us` 0 is nothing sent since the restart. */
static int zbc_believed_on(int reported_on, bool sent_off, int64_t sent_us, int64_t now_us)
{
    if (sent_us > 0 && now_us - sent_us < OWN_WORD_US) {
        return sent_off ? 0 : 1;
    }
    return reported_on;
}
/* >>> end host-test region: fadein <<< */

static bool endpoint_is_dark(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep,
                             const zbc_endpoint_t *e, uint16_t transition_ds)
{
    const int i = restore_slot(ieee, ep, false);
    return zbc_should_fade_in(transition_ds > 0, e->on_off, e->level,
                              i >= 0 && s_restore[i].sent_off,
                              i >= 0 ? s_restore[i].sent_us : 0,
                              esp_timer_get_time());
}

/* See zbc_believed_on. A wired light's state is its own and never lags; it
 * goes through the same rule because its commands are recorded the same way. */
static int endpoint_lit(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, const zbc_endpoint_t *e)
{
    const int i = restore_slot(ieee, ep, false);
    return zbc_believed_on(e->on_off, i >= 0 && s_restore[i].sent_off,
                           i >= 0 ? s_restore[i].sent_us : 0, esp_timer_get_time());
}

/* >>> host-test region: levels <<< */
int zbc_level_to_pct(int level)
{
    int pct = (level * 100 + 126) / 254;
    return pct < 1 ? 1 : (pct > 100 ? 100 : pct);
}

uint8_t zbc_pct_to_level(int pct)
{
    int level = (pct * 254 + 50) / 100;
    return (uint8_t)(level < 1 ? 1 : (level > 254 ? 254 : level));
}
/* >>> end host-test region: levels <<< */

const zbc_endpoint_t *lighting_find_endpoint(const zbc_device_t *dev, uint8_t ep)
{
    for (int i = 0; i < dev->ep_count; i++) {
        if (dev->eps[i].ep == ep) {
            return &dev->eps[i];
        }
    }
    return NULL;
}

/* Which of the three transports a light is behind. The one test, so a fourth
 * cannot be added in one place and forgotten in another. */
typedef enum {
    LIGHT_ZIGBEE = 0,
    LIGHT_WIRED,
    LIGHT_MATTER,
} light_kind_t;

static light_kind_t light_kind(const uint8_t ieee[ZBC_IEEE_LEN], uint16_t *id)
{
    uint16_t n = zbc_light_id_from_ieee(ieee);
    if (n) {
        *id = n;
        return LIGHT_WIRED;
    }
    n = zbc_mlight_id_from_ieee(ieee);
    *id = n;
    return n ? LIGHT_MATTER : LIGHT_ZIGBEE;
}

/* A Matter lamp's row as a Zigbee lamp of the same abilities would have it: the
 * row from mlight_db, the state from its subscription. */
static bool matter_device(uint16_t id, const uint8_t ieee[ZBC_IEEE_LEN], zbc_device_t *out)
{
    zbc_mlight_t row;
    if (!mlight_db_get(id, &row)) {
        return false;
    }
    if (!out) {
        return true;
    }
    memset(out, 0, sizeof(*out));
    out->in_use = true;
    memcpy(out->ieee, ieee, ZBC_IEEE_LEN);
    strlcpy(out->name, row.name, sizeof(out->name));
    strlcpy(out->manufacturer, row.vendor, sizeof(out->manufacturer));
    strlcpy(out->model, row.product, sizeof(out->model));
    out->ep_count = 1;

    mctl_state_t st = { .on_off = ZBC_UNKNOWN, .level = ZBC_UNKNOWN, .ct_mireds = ZBC_UNKNOWN,
                        .hue = ZBC_UNKNOWN, .saturation = ZBC_UNKNOWN, .color_mode = ZBC_UNKNOWN };
    mctl_get(id, &st);
    out->online       = st.online;
    out->last_seen_us = st.last_seen_us;

    zbc_endpoint_t *e = &out->eps[0];
    e->ep            = ZBC_LIGHT_EP;
    e->features      = row.features;
    e->on_off        = st.on_off;
    e->level         = st.level;
    e->ct_mireds     = st.ct_mireds;
    e->ct_min_mireds = row.ct_min_mireds ? row.ct_min_mireds : ZBC_UNKNOWN;
    e->ct_max_mireds = row.ct_max_mireds ? row.ct_max_mireds : ZBC_UNKNOWN;
    e->hue           = st.hue;
    e->saturation    = st.saturation;
    e->color_mode    = st.color_mode;
    return true;
}

bool lighting_get_device(const uint8_t ieee[ZBC_IEEE_LEN], zbc_device_t *out)
{
    uint16_t           id   = 0;
    const light_kind_t kind = light_kind(ieee, &id);
    if (kind == LIGHT_ZIGBEE) {
        return device_db_get(ieee, out);
    }
    if (kind == LIGHT_MATTER) {
        return matter_device(id, ieee, out);
    }
    zbc_light_t l;
    if (!light_db_get(id, &l)) {
        return false;
    }
    if (!out) {
        return true;
    }

    /* The row a Zigbee lamp that can only switch would have, so everything that
     * reads a device - the group fan-out, the switch point, the JSON - takes it
     * without knowing the difference. */
    memset(out, 0, sizeof(*out));
    out->in_use = true;
    memcpy(out->ieee, ieee, ZBC_IEEE_LEN);
    strlcpy(out->name, l.name, sizeof(out->name));
    const bool tunable = zbc_light_tunable(l.kind);
    const bool dims    = tunable || l.kind == ZBC_LIGHT_DIM;
    if (l.kind == ZBC_LIGHT_CNW && l.gpio >= 0 && l.gpio_neutral >= 0 && l.gpio_warm >= 0) {
        snprintf(out->model, sizeof(out->model), "GPIO %d + %d + %d", l.gpio, l.gpio_neutral, l.gpio_warm);
    } else if (l.kind == ZBC_LIGHT_CWWW && l.gpio >= 0 && l.gpio_warm >= 0) {
        snprintf(out->model, sizeof(out->model), "GPIO %d + %d", l.gpio, l.gpio_warm);
    } else if (!tunable && l.gpio >= 0) {
        snprintf(out->model, sizeof(out->model), "GPIO %d", l.gpio);
    }
    zbc_local_state_t st;
    const bool        live = local_light_get(id, &st);
    out->online       = live;
    out->last_seen_us = esp_timer_get_time();   /* a pin is never stale */
    out->ep_count     = 1;

    /* The row a Zigbee lamp of the same abilities would have: an on/off light
     * can only switch, a dimmable one dims, and a tunable white one dims and does
     * colour temperature between its cold and warm LEDs' own. */
    zbc_endpoint_t *e = &out->eps[0];
    e->ep            = ZBC_LIGHT_EP;
    e->features      = tunable ? (ZBC_FEAT_ONOFF | ZBC_FEAT_LEVEL | ZBC_FEAT_COLOR_TEMP)
                       : dims  ? (ZBC_FEAT_ONOFF | ZBC_FEAT_LEVEL)
                               : ZBC_FEAT_ONOFF;
    e->on_off        = live ? st.on : ZBC_UNKNOWN;
    e->level         = live && dims ? st.level : ZBC_UNKNOWN;
    e->ct_mireds     = live && tunable ? st.mireds : ZBC_UNKNOWN;
    e->ct_min_mireds = tunable ? (int32_t)(1000000 / l.cold_k) : ZBC_UNKNOWN;
    e->ct_max_mireds = tunable ? (int32_t)(1000000 / l.warm_k) : ZBC_UNKNOWN;
    e->hue           = ZBC_UNKNOWN;
    e->saturation    = ZBC_UNKNOWN;
    e->color_mode    = tunable ? 2 : ZBC_UNKNOWN;
    return true;
}

#if CONFIG_ZBC_WLED

/* ------------------------------------------------- entertainment streams
 *
 * A room's Hue colour lamps driven by a stream of pixels, through Philips'
 * entertainment frames (zb_ent_*: one frame for every lamp, outside the gate).
 * The source is the WLED interface (wled.c); what a stream means is decided
 * here. Measured on an LST002, a lamp in a stream answers reads with the state
 * it had before the stream, holds the last frame when the stream ends, and
 * brings its attributes up to date only then - so while a stream runs, nothing
 * here may act on what the registry says about its lamps, and at the end the
 * deadbands must forget what they think the lamps are showing. */

/* >>> host-test region: stream <<< */

/* How often the lamps are sent the latest pixels, and how long they glide to
 * each frame: the same, so each glide ends as the next frame arrives (Bifrost's
 * choice; Hue's own clients glide 160 ms). Measured smooth on an LST002. */
#define STREAM_TICK_MS        50
/* The longest a stream outlives its source: a WLED timeout of 255 means none at
 * all, and a client that crashed would otherwise hold the room for ever. */
#define STREAM_MAX_SILENCE_US (60LL * 1000 * 1000)
/* A timeout the source did not give (DDP has none): WLED's own default. */
#define STREAM_DEFAULT_US     2500000LL
#define STREAM_BRI_MAX        2047

/* One pixel as a frame carries it, done as Bifrost does it, which is known to
 * look right on Hue lamps: the RGB taken as linear wide-gamut primaries, the
 * chromaticity from that gamut's RGB-to-XYZ matrix, and the brightness as the
 * share of the brightest this chromaticity can be - which for linear RGB is
 * simply the largest channel. So a pure blue is a full blue, not the 5 % of
 * white its luminance is. `master` scales it (WLED's `bri`).
 *
 * Black is brightness 0, where Bifrost never goes below 1: at 1 an LST002 keeps
 * a faint glow of the last colour, at 0 it is dark and the next colour comes
 * back at once (both seen 2026-10-09) - and a film's letterbox is meant to be
 * dark. Black leaves x and y alone, so a lamp dimmed to nothing keeps the colour
 * it had rather than gliding through white on its way down. */
static void stream_rgb_to_light(uint8_t r, uint8_t g, uint8_t b, uint8_t master,
                                uint16_t *bri, float *x, float *y)
{
    const uint8_t  mx     = r > g ? (r > b ? r : b) : (g > b ? g : b);
    *bri = (uint16_t)(((uint32_t)mx * master * STREAM_BRI_MAX + 255u * 255u / 2) / (255u * 255u));
    if (mx == 0) {
        return;
    }
    const float R = r / 255.0f, G = g / 255.0f, B = b / 255.0f;
    const float X = 0.7164f * R + 0.1010f * G + 0.1468f * B;
    const float Y = 0.2587f * R + 0.7247f * G + 0.0166f * B;
    const float Z = 0.0000f * R + 0.0512f * G + 0.7740f * B;
    const float s = X + Y + Z;
    *x = X / s;
    *y = Y / s;
}

/* What a source's timeout comes to: its own, else WLED's default, and never
 * more than STREAM_MAX_SILENCE_US. */
static int64_t stream_timeout_us(int64_t asked_us)
{
    if (asked_us <= 0) {
        return STREAM_DEFAULT_US;
    }
    return asked_us > STREAM_MAX_SILENCE_US ? STREAM_MAX_SILENCE_US : asked_us;
}

/* Whether a packet is taken. After a hand has ended a stream (`*override`),
 * packets are dropped for as long as they keep coming: the source is still
 * playing, and taking it back would undo what the hand just did. Once it has
 * paused for its own timeout - the TV switched off, the client stopped - the
 * next packet is a new stream, as with WLED's live override. */
static bool stream_take(bool *override, int64_t *last_rx_us, int64_t now_us, int64_t timeout_us)
{
    const bool quiet = now_us - *last_rx_us >= timeout_us;
    *last_rx_us = now_us;
    if (*override && !quiet) {
        return false;
    }
    *override = false;
    return true;
}

/* Whether a lamp takes entertainment frames: a Hue, which has done since 2017,
 * and one with colour - a white-ambiance Hue cannot be in an entertainment area.
 * By manufacturer, because the registry does not keep the clusters a lamp has,
 * and a lamp paired before this was written is not asked again. */
static bool stream_is_hue(const char *manufacturer)
{
    return strncmp(manufacturer, "Signify", 7) == 0 || strncmp(manufacturer, "Philips", 7) == 0;
}

/* >>> end host-test region: stream <<< */

/* A lamp of the stream, and what to put back when it ends: what the registry
 * said before the stream (the last true word it will give until afterwards). */
typedef struct {
    uint8_t ieee[ZBC_IEEE_LEN];
    uint8_t ep;
    int8_t  was_on;       /* 1, 0, ZBC_UNKNOWN */
    int16_t input_pct;    /* to come back to; ZBC_UNKNOWN if nothing says */
    int8_t  color_mode;   /* ZCL ColorMode, ZBC_UNKNOWN */
    int32_t ct_mireds;
    int16_t hue, sat;     /* ZCL 0-254 */
} stream_lamp_t;

/* A light of the room that cannot take part, switched off for the stream
 * (settings_wled_others_off): kept dark while it runs, and switched back on
 * after it if it was lit before. */
typedef struct {
    uint8_t ieee[ZBC_IEEE_LEN];
    uint8_t ep;
    int8_t  was_on;       /* 1, or ZBC_UNKNOWN: a light known dark is not touched */
} stream_dark_t;

/* One stream at a time. Starting and ending are serialised by s_stream_lock (a
 * mutex: they send commands); the pixels, the lamp sets and the counters by
 * s_stream_mux, a spinlock, because stream_holds is asked by every command. */
static struct {
    bool          active;
    uint32_t      gen;                 /* which stream the tick task belongs to */
    uint16_t      group_id;
    uint8_t       n;
    stream_lamp_t lamp[ZB_ENT_MAX_LAMPS];
    uint8_t       n_dark;
    stream_dark_t dark[CONFIG_ZBC_MAX_GROUP_MEMBERS];
    uint8_t       rgb[ZB_ENT_MAX_LAMPS][3];
    uint8_t       master;
    int64_t       last_rx_us;
    int64_t       timeout_us;
    bool          override;
    int64_t       ovr_rx_us;           /* the last packet dropped, or taken */
    int64_t       ovr_timeout_us;
    int64_t       retry_after_us;      /* a stream that could not start is not retried per packet */
    lighting_stream_status_t st;
} s_stream;
static portMUX_TYPE      s_stream_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_stream_lock;
static StaticSemaphore_t s_stream_lock_buf;

typedef enum {
    STREAM_NONE = 0,
    STREAM_LAMP,          /* takes the frames */
    STREAM_DARK,          /* switched off for the stream */
} stream_role_t;

/* What the running stream has of an endpoint, with s_stream_mux held; `*at` is
 * its index in the lamp or dark set. */
static stream_role_t stream_role_locked(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, int *at)
{
    for (int i = 0; s_stream.active && i < s_stream.n; i++) {
        if (s_stream.lamp[i].ep == ep && memcmp(s_stream.lamp[i].ieee, ieee, ZBC_IEEE_LEN) == 0) {
            *at = i;
            return STREAM_LAMP;
        }
    }
    for (int i = 0; s_stream.active && i < s_stream.n_dark; i++) {
        if (s_stream.dark[i].ep == ep && memcmp(s_stream.dark[i].ieee, ieee, ZBC_IEEE_LEN) == 0) {
            *at = i;
            return STREAM_DARK;
        }
    }
    return STREAM_NONE;
}

/* Whether a command must not reach this endpoint: it is a lamp of the stream,
 * or a light the stream switched off. Asked by every transport below, so
 * nothing automatic can fight a stream - an Adaptive Lighting tick above all,
 * which would light a dark member of a room it finds lit. The public entry
 * points take the endpoint back first (stream_hand_*), so a hand never meets
 * this. */
static bool stream_holds(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    int at;
    portENTER_CRITICAL(&s_stream_mux);
    const bool held = stream_role_locked(ieee, ep, &at) != STREAM_NONE;
    portEXIT_CRITICAL(&s_stream_mux);
    return held;
}

static void stream_hand_group(uint16_t group_id);
static void stream_hand_endpoint(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep);

#else

/* Without CONFIG_ZBC_WLED there are no streams: nothing is held, and a hand
 * has nothing to end. */
static inline bool stream_holds(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    (void)ieee;
    (void)ep;
    return false;
}

static inline void stream_hand_group(uint16_t group_id)
{
    (void)group_id;
}

static inline void stream_hand_endpoint(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    (void)ieee;
    (void)ep;
}

#endif /* CONFIG_ZBC_WLED */

/* The one place a light command picks its transport: each of these is one zb_*
 * call, or the local_light_* or mctl_* call that mirrors it; zb_coordinator.c
 * knows neither wired lights nor Matter lamps exist, and no zb_* call is made
 * anywhere else in this file. A wired light has no hue or xy, so those two
 * refuse it - set_brightness_ex never reaches them for one.
 *
 * Also the one place the lit/off belief is recorded, so a command path added
 * later records it without knowing it exists. The colour transports record
 * nothing - a colour says nothing about whether the lamp is lit. And the one
 * place a lamp in a stream is kept out of reach (stream_holds). */
static esp_err_t switch_endpoint(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, zb_onoff_cmd_t cmd)
{
    if (stream_holds(ieee, ep)) {
        return LIGHTING_ERR_STREAMING;
    }
    uint16_t        id  = 0;
    const light_kind_t k = light_kind(ieee, &id);
    const esp_err_t err = k == LIGHT_WIRED  ? local_light_set(id, cmd)
                        : k == LIGHT_MATTER ? mctl_set_onoff(id, cmd)
                                            : zb_set_onoff(ieee, ep, cmd);
    /* An unresolved toggle leaves no belief: only the lamp knows which way it
     * went, and its report is how we hear of it. */
    if (err == ESP_OK && cmd != ZB_ONOFF_TOGGLE) {
        note_sent(ieee, ep, cmd == ZB_ONOFF_ON);
    }
    return err;
}

static esp_err_t out_set_level(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint8_t level, uint16_t ds)
{
    if (stream_holds(ieee, ep)) {
        return LIGHTING_ERR_STREAMING;
    }
    uint16_t        id  = 0;
    const light_kind_t k = light_kind(ieee, &id);
    const esp_err_t err = k == LIGHT_WIRED  ? local_light_set_level(id, level, ds)
                        : k == LIGHT_MATTER ? mctl_set_level(id, level, ds)
                                            : zb_set_level(ieee, ep, level, ds);
    if (err == ESP_OK) {
        note_sent(ieee, ep, true);
    }
    return err;
}

static esp_err_t out_fade_in(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint8_t level, uint16_t ds)
{
    if (stream_holds(ieee, ep)) {
        return LIGHTING_ERR_STREAMING;
    }
    uint16_t        id  = 0;
    const light_kind_t k = light_kind(ieee, &id);
    const esp_err_t err = k == LIGHT_WIRED  ? local_light_fade_in(id, level, ds)
                        : k == LIGHT_MATTER ? mctl_fade_in(id, level, ds)
                                            : zb_fade_in(ieee, ep, level, ds);
    if (err == ESP_OK) {
        note_sent(ieee, ep, true);
    }
    return err;
}

/* `ds` reaches a wired light only: OffWithEffect has no transition field, and a
 * Matter lamp's mirror of it fades as the Zigbee one does. */
static esp_err_t out_fade_off(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint16_t ds)
{
    if (stream_holds(ieee, ep)) {
        return LIGHTING_ERR_STREAMING;
    }
    uint16_t        id  = 0;
    const light_kind_t k = light_kind(ieee, &id);
    const esp_err_t err = k == LIGHT_WIRED  ? local_light_fade_off(id, ds)
                        : k == LIGHT_MATTER ? mctl_fade_off(id)
                                            : zb_fade_off(ieee, ep);
    if (err == ESP_OK) {
        note_sent(ieee, ep, false);
    }
    return err;
}

/* A wired light's glide (local_light_glide); there is no Zigbee or Matter
 * mirror, so anything else is refused - glides() keeps it from being asked. */
#define GLIDE_STEPS LOCAL_LIGHT_GLIDE_POINTS

static esp_err_t out_glide(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, const uint8_t *level,
                           const uint16_t *mireds, uint16_t ds, bool off)
{
    if (stream_holds(ieee, ep)) {
        return LIGHTING_ERR_STREAMING;
    }
    uint16_t id = 0;
    if (light_kind(ieee, &id) != LIGHT_WIRED) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const esp_err_t err = local_light_glide(id, level, mireds, GLIDE_STEPS, ds, off);
    if (err == ESP_OK) {
        note_sent(ieee, ep, !off);
    }
    return err;
}

static esp_err_t out_set_ct(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint16_t mireds, uint16_t ds)
{
    if (stream_holds(ieee, ep)) {
        return LIGHTING_ERR_STREAMING;
    }
    uint16_t           id = 0;
    const light_kind_t k  = light_kind(ieee, &id);
    return k == LIGHT_WIRED  ? local_light_set_ct(id, mireds, ds)
         : k == LIGHT_MATTER ? mctl_set_ct(id, mireds, ds)
                             : zb_set_color_temp(ieee, ep, mireds, ds);
}

static esp_err_t out_set_hs(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint8_t hue, uint8_t sat, uint16_t ds)
{
    if (stream_holds(ieee, ep)) {
        return LIGHTING_ERR_STREAMING;
    }
    uint16_t           id = 0;
    const light_kind_t k  = light_kind(ieee, &id);
    return k == LIGHT_WIRED  ? ESP_ERR_NOT_SUPPORTED          /* white LEDs only */
         : k == LIGHT_MATTER ? mctl_set_hs(id, hue, sat, ds)
                             : zb_set_color_hs(ieee, ep, hue, sat, ds);
}

static esp_err_t out_set_xy(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint16_t x, uint16_t y, uint16_t ds)
{
    if (stream_holds(ieee, ep)) {
        return LIGHTING_ERR_STREAMING;
    }
    uint16_t           id = 0;
    const light_kind_t k  = light_kind(ieee, &id);
    return k == LIGHT_WIRED  ? ESP_ERR_NOT_SUPPORTED
         : k == LIGHT_MATTER ? mctl_set_xy(id, x, y, ds)
                             : zb_set_color_xy(ieee, ep, x, y, ds);
}

/* The colour and the off as one gated command - see off_endpoint. A wired light
 * never gets here: it takes its colour while dark and needs no pairing. */
static esp_err_t out_warm_off(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint16_t mireds, bool plain_off)
{
    if (stream_holds(ieee, ep)) {
        return LIGHTING_ERR_STREAMING;
    }
    uint16_t           id  = 0;
    const light_kind_t k   = light_kind(ieee, &id);
    const esp_err_t    err = k == LIGHT_MATTER ? mctl_warm_off(id, mireds, plain_off)
                                               : zb_warm_off(ieee, ep, mireds, plain_off);
    if (err == ESP_OK) {
        /* The one off that is not switch_endpoint's or out_fade_off's. It follows
         * the prewarm dim, which recorded "lit", so the order is what leaves the
         * belief right. */
        note_sent(ieee, ep, false);
    }
    return err;
}

static esp_err_t out_level_stop(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    if (stream_holds(ieee, ep)) {
        return LIGHTING_ERR_STREAMING;
    }
    uint16_t           id = 0;
    const light_kind_t k  = light_kind(ieee, &id);
    if (k == LIGHT_WIRED) {
        /* An on/off light has no ramp to stop, and that is not a failure. */
        esp_err_t err = local_light_stop(id);
        return err == ESP_ERR_NOT_SUPPORTED ? ESP_OK : err;
    }
    if (k == LIGHT_MATTER) {
        esp_err_t err = mctl_level_stop(id);
        return err == ESP_ERR_NOT_SUPPORTED ? ESP_OK : err;
    }
    return zb_level_stop(ieee, ep);
}

/* The lamp's own physical limits, in kelvin. zb_set_color_temp does not clamp,
 * so a curve that runs to 2000 K on a lamp that stops at 2700 K has to be
 * pulled in here rather than sent and silently ignored. */
static int clamp_kelvin(const zbc_endpoint_t *e, int kelvin)
{
    if (e->ct_max_mireds > 0) {                       /* warmest = fewest kelvin */
        int min_k = 1000000 / e->ct_max_mireds;
        if (kelvin < min_k) {
            kelvin = min_k;
        }
    }
    if (e->ct_min_mireds > 0) {                       /* coolest = most kelvin */
        int max_k = 1000000 / e->ct_min_mireds;
        if (kelvin > max_k) {
            kelvin = max_k;
        }
    }
    return kelvin;
}

/* >>> host-test region: prewarm <<< */
/* The colour temperature to leave a lamp at when it is switched off, in mireds,
 * or 0 for "send nothing". Warmest is the largest mired value, hence
 * ct_max_mireds.
 *
 * Nothing is sent for an endpoint without colour temperature (a plain dimmer, a
 * relay, a colour-only lamp), nor for one whose limits never arrived: there is
 * no warmest to go to, and a guessed 370 would be refused by some lamps and
 * obeyed by others.
 *
 * Nor for a lamp already there - the common case, since a warmth curve warms a
 * lamp as it dims - which would cost three commands and a visible dip to arrive
 * where the lamp already is. Compared in kelvin against the same deadband as
 * every other colour decision here.
 *
 * Nor for a lamp that is not `lit` - dark, on its way out after our own off, or
 * of unknown state. The warmth has to reach a lit lamp anyway, and the dim in
 * front of it is a move-to-level *with on/off*: sent to a dark lamp it lights
 * it at 1 % for the off to switch out again. Its colour is no guide either: a
 * lamp that reported something during its last off is not re-read until 3.7 s
 * after it, so a second off inside that time still sees the colour from
 * before the first and finds it well short of warmest. */
static uint16_t prewarm_mireds(bool enabled, bool lit, const zbc_endpoint_t *e)
{
    if (!enabled || !lit || !(e->features & ZBC_FEAT_COLOR_TEMP) || e->ct_max_mireds <= 0 ||
        e->ct_max_mireds > UINT16_MAX) {
        return 0;
    }
    if (e->ct_mireds > 0 &&
        abs(1000000 / (int)e->ct_mireds - 1000000 / (int)e->ct_max_mireds) < CT_DEADBAND_K) {
        return 0;
    }
    return (uint16_t)e->ct_max_mireds;
}
/* >>> end host-test region: prewarm <<< */

/* Adaptive Lighting shifts the curve rather than replacing it, so what the curve
 * says at a brightness is where the shift starts from. The CT branch of
 * send_curve_color then clamps and deadbands it like any other colour
 * temperature, and curve_db records the shifted value - which is what lets the
 * curve's own colour go out again the moment the shift stops. */
static void al_shift_target(const zbc_endpoint_t *e, zbc_lamp_target_t *t, al_shift_t al)
{
    if (al.offset_k == AL_NO_OFFSET) {
        return;
    }
    const int k = al_shifted_kelvin(t->out == ZBC_OUT_CT, t->out == ZBC_OUT_NONE, t->kelvin,
                                    (e->features & ZBC_FEAT_COLOR_TEMP) != 0, al.base_k, (int)al.offset_k);
    if (k > 0) {
        t->out    = ZBC_OUT_CT;
        t->kelvin = k;
    }
}

/* A wired light with curves switches on and off by gliding along them
 * (local_light_glide): the brightness asked of it moves on smootherstep and the
 * light shows what its level curve and warmth path give at each point on the
 * way - ESPHome's light transitions, which run every output's formula on one
 * brightness. Two lights in one room then fade as one light would: one dark below
 * 40 % stays dark until the room's glide passes 40 % and leaves before it, and
 * every light comes on warm and dim and goes out that way. Faded from black on its
 * own instead, a light lingers at its dimmest duties for the first and last 150 ms
 * of the fade, and LED spots with drivers of their own show that as a cold tint
 * and a flicker.
 *
 * Only with curves to glide along. Without, a glide is smootherstep in level,
 * which lingers near black longer than local_light_fade_in's ease in light does. */
static bool glides(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, const zbc_endpoint_t *e)
{
    return zbc_light_id_from_ieee(ieee) != 0 && (e->features & ZBC_FEAT_LEVEL) &&
           curve_db_active(ieee, ep, NULL, NULL);
}

/* What the light shows at GLIDE_STEPS evenly spaced points as the brightness asked
 * of it goes from `from` to `to` (input percent; 0 is dark, in the colour of 1).
 * False, and `mireds` not to be used, unless the way is warmth all along - a
 * wired light takes no other colour. */
static bool glide_points(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, const zbc_endpoint_t *e,
                         int from, int to, al_shift_t al,
                         uint8_t level[GLIDE_STEPS], uint16_t mireds[GLIDE_STEPS])
{
    bool warmth = (e->features & ZBC_FEAT_COLOR_TEMP) != 0;
    for (int i = 0; i < GLIDE_STEPS; i++) {
        const int x = (int)lround(from + (double)(to - from) * i / (GLIDE_STEPS - 1));
        zbc_lamp_target_t t;
        curve_db_eval(ieee, ep, x > 0 ? x : 1, &t);
        al_shift_target(e, &t, al);
        level[i] = x > 0 && t.pct > 0 ? zbc_pct_to_level(t.pct) : 0;
        if (t.out == ZBC_OUT_CT) {
            mireds[i] = (uint16_t)(1000000 / clamp_kelvin(e, t.kelvin));
        } else {
            warmth = false;
        }
    }
    return warmth;
}

/* On from dark to the brightness `input_pct` asks for, ending at `level` (the
 * restore level a switch-on comes back at; 0 for the curve's own). The colour
 * it ends in is recorded as sent: the caller's own colour command would only
 * replace the glide's way there with a straight line to the same place. */
static esp_err_t glide_on(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, const zbc_endpoint_t *e,
                          int input_pct, uint8_t level, uint16_t transition_ds, al_shift_t al)
{
    uint8_t  lv[GLIDE_STEPS];
    uint16_t mr[GLIDE_STEPS];
    const bool warmth = glide_points(ieee, ep, e, 0, input_pct, al, lv, mr);
    if (level > 0) {
        lv[GLIDE_STEPS - 1] = level;
    }
    const esp_err_t err = out_glide(ieee, ep, lv, warmth ? mr : NULL, transition_ds, false);
    if (err == ESP_OK && warmth) {
        const zbc_lamp_target_t end = { .out = ZBC_OUT_CT, .kelvin = 1000000 / mr[GLIDE_STEPS - 1] };
        curve_db_note_color_sent(ieee, ep, &end);
    }
    return err;
}

/* Off, gliding down from the brightness last asked for; without one to start
 * from, the plain fade out. The colour it ends in - the path's bottom - is
 * recorded as sent, since that is what the light now holds. */
static esp_err_t glide_off(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, const zbc_endpoint_t *e,
                           uint16_t transition_ds)
{
    const int from = curve_db_input_pct(ieee, ep);
    if (from <= 0) {
        return out_fade_off(ieee, ep, transition_ds);
    }
    uint8_t  lv[GLIDE_STEPS];
    uint16_t mr[GLIDE_STEPS];
    const bool      warmth = glide_points(ieee, ep, e, from, 0, AL_NO_SHIFT, lv, mr);
    const esp_err_t err    = out_glide(ieee, ep, lv, warmth ? mr : NULL, transition_ds, true);
    if (err == ESP_OK && warmth) {
        const zbc_lamp_target_t end = { .out = ZBC_OUT_CT, .kelvin = 1000000 / mr[GLIDE_STEPS - 1] };
        curve_db_note_color_sent(ieee, ep, &end);
    }
    return err;
}

/* Switch an endpoint off, leaving it at its warmest white on the way out when
 * the setting asks for it, so that whatever switches it on next - this
 * coordinator, a remote, a Matter controller - lights it warm. Lamps keep their
 * colour across an off.
 *
 * The colour has to reach the lamp while it is still lit: a Hue LST002 ignores a
 * colour command sent while it is dark, with or without the Color Control
 * ExecuteIfOff option.
 *
 * The colour and the off are one gated command (zb_warm_off), not two calls: the
 * gate drains ZBC_CH_LEVEL before ZBC_CH_COLOR, so an off queued beside a colour
 * goes out first and the colour arrives too late. */
static esp_err_t off_endpoint(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep,
                              const zbc_endpoint_t *e, uint16_t transition_ds)
{
    /* Asked before anything below records this off, which would make every lamp
     * read as dark. */
    const uint16_t mireds = e ? prewarm_mireds(settings_warm_off(), endpoint_lit(ieee, ep, e) == 1, e)
                              : 0;
    const bool     fade   = transition_ds > 0;
    const bool     glide  = fade && e && glides(ieee, ep, e);

    if (mireds == 0) {
        return glide  ? glide_off(ieee, ep, e, transition_ds)
             : fade   ? out_fade_off(ieee, ep, transition_ds)
                      : switch_endpoint(ieee, ep, ZB_ONOFF_OFF);
    }

    /* A wired light needs none of the dance: local_light_set_ct moves its mired
     * ramp and never its level, so it takes a colour while it is dark and the
     * change simply runs under the fade-out. It must *not* be dimmed either -
     * its restore level lives in local_lights.c, where a dim would overwrite the
     * level it comes back at, the same trap set_brightness_ex is avoided for.
     * The off goes first: a colour that finds the light fading out travels with
     * that fade, warming as the light dims, rather than on a clock of its own. */
    esp_err_t err;
    if (zbc_light_id_from_ieee(ieee) != 0) {
        err = glide  ? glide_off(ieee, ep, e, transition_ds)
            : fade   ? out_fade_off(ieee, ep, transition_ds)
                     : switch_endpoint(ieee, ep, ZB_ONOFF_OFF);
        out_set_ct(ieee, ep, mireds, transition_ds);
    } else {
        /* Down to the bottom first - not to hide the colour change: the gate sends
         * the colour when this frame is confirmed, one round trip in, so it lands
         * near full brightness, and the off cuts the dim short a round trip later.
         * The dim leaves the lamp parked at PARKED_LEVEL while dark, and that is
         * load-bearing: a Hue LST002 that went dark at its old level ignores the
         * colour its next switch-on sends and stays at its warmest; parked, it
         * takes it. It also spares that switch-on its preset - see
         * zbc_should_fade_in. Only with a fade: a dip on the way out is not what
         * Instant means.
         *
         * Not through set_brightness_ex, which records what it sends as the level
         * to come back to - the lamp would return at 1 %. The callers have
         * already taken the restore level. */
        if (fade && (e->features & ZBC_FEAT_LEVEL)) {
            out_set_level(ieee, ep, PARKED_LEVEL, transition_ds);
        }
        err = out_warm_off(ieee, ep, mireds, !fade);
        if (err == ZB_ERR_UNREACHABLE || err == LIGHTING_ERR_STREAMING) {
            return err;           /* nothing to fall back to: it is not there, or not ours */
        }
        if (err != ESP_OK) {
            /* Being switched off is what was asked for; the warmth is a nicety
             * on top of it, and must never be what stops it happening. */
            ESP_LOGW(TAG, "Warm-off for ep %d refused (0x%x), switching off plainly", ep, err);
            return fade ? out_fade_off(ieee, ep, transition_ds) : switch_endpoint(ieee, ep, ZB_ONOFF_OFF);
        }
    }

    /* Truthful, because the lamp was lit and did as it was told: the curve's own
     * colour goes out again the moment something asks for a different one. */
    if (err == ESP_OK) {
        const zbc_lamp_target_t t = { .out = ZBC_OUT_CT, .kelvin = 1000000 / mireds };
        curve_db_note_color_sent(ieee, ep, &t);
    }
    return err;
}

/* >>> host-test region: switchpoint <<< */
/* Whether a switch-only endpoint needs an On/Off for this brightness step.
 *
 * A slider drag is a stream of requests, and without this each would put an On
 * on air for a relay that is already on. The latch (`sent_off`: 1 off, 0 on,
 * ZBC_UNKNOWN) makes a drag cost one command at the crossing. The reported state
 * is consulted too, so a relay switched by hand is put right on the next move;
 * while a report lags behind a command this can repeat it, and the command gate
 * collapses the repeats. */
static bool switch_should_send(bool want_on, int sent_off, int reported_on)
{
    if (sent_off != (want_on ? 0 : 1)) {
        return true;                          /* includes a latch that is unknown */
    }
    return reported_on != ZBC_UNKNOWN && reported_on != (want_on ? 1 : 0);
}
/* >>> end host-test region: switchpoint <<< */

/* An endpoint that can only switch has no level to map a brightness onto, so
 * its level curve is read as a switch point: an output above 0 means on. That is
 * the same thing a curve dropping to 0 already means for a dimmable lamp - "dark
 * below here" - and it keeps the setting in curve_db without growing its blob.
 * With no curve the endpoint ignores brightness and follows only on/off. */
static esp_err_t set_switch_point(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep,
                                  const zbc_endpoint_t *e, int input_pct)
{
    zbc_curveset_t cs;
    if (!curve_db_get(ieee, ep, &cs) || cs.level.n == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    zbc_lamp_target_t t;
    curve_db_eval(ieee, ep, input_pct, &t);
    const bool want_on = t.pct > 0;

    esp_err_t err = ESP_OK;
    if (switch_should_send(want_on, cs.sent_off, e->on_off)) {
        err = switch_endpoint(ieee, ep, want_on ? ZB_ONOFF_ON : ZB_ONOFF_OFF);
    }
    /* Only latched once it went out, or a refused command is never retried. */
    if (err == ESP_OK) {
        curve_db_note_sent(ieee, ep, input_pct, !want_on);
    }
    return err;
}

/* The level to come back to, from the lamp's own report - only when nothing has
 * been commanded through here, and only if that report is of a lit lamp. */
static void remember_reported_level(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep,
                                    const zbc_endpoint_t *e)
{
    if (recall_level(ieee, ep) < 0 &&
        e->level != ZBC_UNKNOWN && e->level > 1 && e->on_off != 0) {
        remember_level(ieee, ep, zbc_level_to_pct(e->level));
    }
}

/* Whether a colour command went out, warning when it was refused - but not for
 * a lamp that is not answering, which the coordinator has already said once. */
static bool colour_sent(esp_err_t err, const char *what, uint8_t ep)
{
    if (err != ESP_OK && err != ZB_ERR_UNREACHABLE && err != LIGHTING_ERR_STREAMING) {
        ESP_LOGW(TAG, "%s for ep %d refused (0x%x)", what, ep, err);
    }
    return err == ESP_OK;
}

/* The one colour command a target may be worth, and the deadbands that decide
 * whether it is worth it. Shared by a brightness request and by a switch-on,
 * which has to be able to put a colour back - see restore_curve_color. Returns
 * true when something went out, which is what the caller records. */
static bool send_curve_color(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep,
                             const zbc_endpoint_t *e, zbc_lamp_target_t *t,
                             const zbc_curveset_t *cs, bool have_curves,
                             uint16_t transition_ds, bool coarse)
{
    /* One colour command at most: a target names exactly one output.
     *
     * A change of output kind is always sent, whatever the deadbands say. The
     * three channels track independently, so at a boundary each is compared
     * against a stale value of its own and both gates fall open - a slider
     * jittering across a stop would send a colour command every step. And the
     * lamp has to be told to leave colour-temperature mode, which a deadband
     * must not suppress. */
    const bool mode_changed = !have_curves || cs->sent_mode != (int8_t)t->out;
    bool sent_color = false;

    switch (t->out) {
    case ZBC_OUT_CT:
        if (e->features & ZBC_FEAT_COLOR_TEMP) {
            /* clamp_kelvin, because zb_set_color_temp does not. */
            int kelvin = clamp_kelvin(e, t->kelvin);
            if (mode_changed || cs->sent_kelvin == ZBC_UNKNOWN ||
                abs(kelvin - (int)cs->sent_kelvin) >= (coarse ? CT_DEADBAND_RAMP_K : CT_DEADBAND_K)) {
                if (colour_sent(out_set_ct(ieee, ep, (uint16_t)(1000000 / kelvin), transition_ds),
                                "Colour temperature", ep)) {
                    t->kelvin   = kelvin;              /* what actually went out */
                    sent_color = true;
                }
            }
        }
        break;

    case ZBC_OUT_HS:
        if (e->features & ZBC_FEAT_ANY_COLOR) {
            int dh = !mode_changed && cs->sent_hue != ZBC_UNKNOWN
                         ? abs(t->hue - (int)cs->sent_hue) : 999;
            if (dh > 180) {
                dh = 360 - dh;                        /* hue is a circle */
            }
            int ds_ = !mode_changed && cs->sent_sat != ZBC_UNKNOWN
                          ? abs(t->sat - (int)cs->sent_sat) : 999;
            if (dh >= (coarse ? HUE_DEADBAND_RAMP_DEG : HUE_DEADBAND_DEG) ||
                ds_ >= (coarse ? SAT_DEADBAND_RAMP_PCT : SAT_DEADBAND_PCT)) {
                uint8_t zcl_hue = (uint8_t)((t->hue % 360) * 254 / 360);
                uint8_t zcl_sat = (uint8_t)(t->sat * 254 / 100);
                if (colour_sent(out_set_hs(ieee, ep, zcl_hue, zcl_sat, transition_ds), "Colour", ep)) {
                    sent_color = true;
                }
            }
        }
        break;

    case ZBC_OUT_XY:
        if (e->features & ZBC_FEAT_ANY_COLOR) {
            /* int64: a whole-diagram move is ~49000 units, and squaring that
             * overflows 32 bits before the two terms are even added. */
            const int64_t dx = (int64_t)t->cie_x - cs->sent_x;
            const int64_t dy = (int64_t)t->cie_y - cs->sent_y;
            const int64_t lim = coarse ? XY_DEADBAND_RAMP : XY_DEADBAND;
            if (mode_changed || cs->sent_x == ZBC_UNKNOWN || cs->sent_y == ZBC_UNKNOWN ||
                dx * dx + dy * dy >= lim * lim) {
                if (colour_sent(out_set_xy(ieee, ep, t->cie_x, t->cie_y, transition_ds),
                                "Blended colour", ep)) {
                    sent_color = true;
                }
            }
        }
        break;

    case ZBC_OUT_NONE:
    default:
        break;
    }

    return sent_color;
}

/* How long a colour gets when it accompanies a switch-on.
 *
 * The gate holds the colour command until the level frame carrying the same
 * intent is confirmed, one round trip later (24-130 ms, more with retries).
 * Given the level's own transition it would finish that much after the ramp,
 * and for that gap the lamp sits at its final brightness in a colour it has not
 * reached. Cold reads as brighter than warm, so a lamp coming up to a warm
 * target appears to overshoot and be pulled back.
 *
 * The colour moving with the level is the point - dim and warm, brightening and
 * cooling together. So the colour gets the level's transition less the round
 * trip, and lands no later than the level. A fade too short to give a round trip
 * back becomes a snap: that early in a short ramp the lamp is at a few percent
 * and the change cannot be seen. */
#define COLOUR_LEAD_DS 1      /* one gate round trip, rounded up generously */

static uint16_t switch_on_colour_ds(uint16_t transition_ds)
{
    return transition_ds > COLOUR_LEAD_DS ? (uint16_t)(transition_ds - COLOUR_LEAD_DS) : 0;
}

/* Put the colour back when an endpoint is switched on.
 *
 * off_endpoint may have moved the lamp to its warmest on the way out; a
 * switch-on that restored only the level would leave it there, and nothing else
 * would move it, since a colour command is only sent when a brightness request
 * computes one.
 *
 * The curve is read at the brightness the switch-on is for, which is what the
 * lamp would show had it never been switched off. The deadbands mean this
 * usually sends nothing; only a lamp actually somewhere else, as after a
 * warm-off, gets a command. A lamp with no colour path keeps the warmth.
 *
 * It follows the level command rather than leading it, so the lamp lights warm
 * and moves to its colour over the transition. `from_dark` picks the transition
 * by the one rule switch_on_colour_ds states, so this path and
 * set_brightness_ex's look the same: a group switch-on takes one or the other
 * depending on whether `last_pct` is known, which is runtime only.
 *
 * `input_pct` is the brightness the switch-on is for, decided by the caller: the
 * one last asked for, or - after a restart, when nothing has been - the one it
 * brings the lamp back at. */
static void restore_curve_color(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep,
                                const zbc_endpoint_t *e, int input_pct,
                                uint16_t transition_ds, bool from_dark)
{
    zbc_curveset_t cs;
    if (!curve_db_get(ieee, ep, &cs) || input_pct <= 0) {
        return;
    }

    zbc_lamp_target_t t;
    curve_db_eval(ieee, ep, input_pct, &t);
    const uint16_t ds = from_dark ? switch_on_colour_ds(transition_ds) : transition_ds;
    if (send_curve_color(ieee, ep, e, &t, &cs, true, ds, false)) {
        curve_db_note_color_sent(ieee, ep, &t);
    }
}

/* `coarse` widens the colour deadbands for a sweep; see above. Whether a lamp
 * needs the fade-in preset is not a property of the request but of the lamp,
 * and is decided per endpoint by endpoint_is_dark. `al` is Adaptive Lighting's
 * shift, AL_NO_SHIFT when it is not running for the group this command is for. */
static esp_err_t set_brightness_ex(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep,
                                   int input_pct, uint16_t transition_ds, bool coarse,
                                   al_shift_t al)
{
    zbc_device_t dev;
    if (!lighting_get_device(ieee, &dev)) {
        return ESP_ERR_NOT_FOUND;
    }
    const zbc_endpoint_t *e = lighting_find_endpoint(&dev, ep);
    if (!e) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!(e->features & ZBC_FEAT_LEVEL)) {
        return set_switch_point(ieee, ep, e, input_pct);
    }

    zbc_lamp_target_t t;
    curve_db_eval(ieee, ep, input_pct, &t);
    al_shift_target(e, &t, al);

    zbc_curveset_t cs;
    bool have_curves = curve_db_get(ieee, ep, &cs);

    /* A level curve may map the bottom of the range to nothing at all. Level 1
     * would leave the lamp dimly lit, so this has to become an Off command -
     * that is what lets one lamp drop out of a group while the others carry on. */
    if (t.pct == 0) {
        esp_err_t err = ESP_OK;
        if (!have_curves || cs.sent_off != 1) {
            /* Only if nothing was ever commanded - see the note on the on/off
             * path below. The last non-zero request already recorded itself. */
            remember_reported_level(ieee, ep, e);
            /* Fade out rather than cut, for the same reason as above. */
            err = off_endpoint(ieee, ep, e, transition_ds);
        }
        /* Only latched once it went out, as in set_switch_point: latched for a
         * lamp that was not answering, the next request below its threshold
         * after it came back would find sent_off set and never send the Off. */
        if (err == ESP_OK) {
            curve_db_note_sent(ieee, ep, input_pct, true);
        }
        return err;
    }

    /* Remember where this lamp is meant to sit, so switching it off and on
     * again brings it back here rather than to full. Taken from the command
     * rather than from a read, which lags and reads 0 right after a fade out. */
    remember_level(ieee, ep, t.pct);

    /* A lamp that is dark gets the preset that gives the ramp somewhere to
     * start, whether it was asked for a brightness or asked to come on: a plain
     * move to a level lights it at the level it held before - see
     * endpoint_is_dark, which is also what keeps a slider drag out of this. */
    const bool from_dark = endpoint_is_dark(ieee, ep, e, transition_ds);
    if (from_dark && glides(ieee, ep, e)) {
        /* Level and colour in one, the colour recorded by glide_on. */
        const esp_err_t err = glide_on(ieee, ep, e, input_pct, 0, transition_ds, al);
        if (err == ESP_OK) {
            curve_db_note_sent(ieee, ep, input_pct, false);
        }
        return err;
    }
    esp_err_t  err = from_dark
                         ? out_fade_in(ieee, ep, zbc_pct_to_level(t.pct), transition_ds)
                         : out_set_level(ieee, ep, zbc_pct_to_level(t.pct), transition_ds);
    /* A colour accompanying a switch-on travels with the level but gets a round
     * trip less to do it in - see switch_on_colour_ds, which restore_curve_color
     * uses too. It must not be sent any earlier: between the preset and the ramp
     * it reaches a lamp that has been on for a millisecond, which ignores it (as
     * with ExecuteIfOff and the warm-off ordering). */
    const bool sent_color =
        send_curve_color(ieee, ep, e, &t, &cs, have_curves,
                         from_dark ? switch_on_colour_ds(transition_ds) : transition_ds,
                         coarse);

    /* A level that did not go out - a lamp that has stopped answering, above
     * all - is not the brightness the lamp was asked for, and the page binds to
     * this one. */
    if (err == ESP_OK) {
        curve_db_note_sent(ieee, ep, input_pct, false);
    }
    if (sent_color) {
        curve_db_note_color_sent(ieee, ep, &t);
    }
    return err;
}

esp_err_t lighting_set_brightness(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep,
                                  int input_pct, uint16_t transition_ds)
{
    al_note_manual_endpoint(ieee, ep, "dimmed");
    stream_hand_endpoint(ieee, ep);
    return set_brightness_ex(ieee, ep, input_pct, transition_ds, false, AL_NO_SHIFT);
}

/* Switching a lamp with a fade.
 *
 * On/Off commands carry no transition, and the Level Control attributes meant
 * to supply one are advisory: an OSRAM PAR16 stores OnOffTransitionTime,
 * OnTransitionTime and OffTransitionTime, reads them back correctly, and still
 * snaps. Ramping the level needs nothing beyond Level Control.
 *
 * On is a move-to-level-with-on-off up to the remembered level. Off is not the
 * mirror image: a ramp to zero leaves a Hue LST002 faintly lit at level 1, so it
 * is OffWithEffect (zb_fade_off), which fades and switches off in one command.
 *
 * Without the hand on a stream, for a stream switching the lights of its room
 * that cannot take part; lighting_set_onoff is this and the hand. */
static esp_err_t set_onoff_ex(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, zb_onoff_cmd_t cmd)
{
    const uint16_t ds = settings_transition_ds();

    zbc_device_t          dev;
    const zbc_endpoint_t *e = NULL;
    if (lighting_get_device(ieee, &dev)) {
        e = lighting_find_endpoint(&dev, ep);
    }

    /* A fade has to know which way it is going, so a toggle is resolved here.
     * With no state to resolve it from, the lamp decides and gets no fade. */
    const int lit = e ? endpoint_lit(ieee, ep, e) : ZBC_UNKNOWN;
    if (cmd == ZB_ONOFF_TOGGLE && lit != ZBC_UNKNOWN) {
        cmd = lit == 1 ? ZB_ONOFF_OFF : ZB_ONOFF_ON;
    }

    bool fade = ds > 0 && e && (e->features & ZBC_FEAT_LEVEL) && cmd != ZB_ONOFF_TOGGLE;
    esp_err_t err;
    /* Read before the level command goes out, because sending one records that
     * the endpoint is lit - ask afterwards and the answer is always "no". The
     * colour that follows needs it. */
    bool from_dark = false;
    /* The brightness this switch-on is for, as the curves read it: the one last
     * asked for, or - nothing asked since a restart - the one the lamp comes
     * back at below. The colour is put back at it, and it is what is recorded. */
    int input_pct = curve_db_input_pct(ieee, ep);

    if (!fade) {
        /* An unresolved toggle is deliberately not warmed: which way it went is
         * the lamp's decision, and this must not run on a lamp coming on. It
         * leaves no belief behind either, for the same reason: only the lamp
         * knows which way it went, and its report is the way to hear of it. */
        err = cmd == ZB_ONOFF_OFF ? off_endpoint(ieee, ep, e, 0)
                                  : switch_endpoint(ieee, ep, cmd);
    } else if (cmd == ZB_ONOFF_OFF) {
        /* What the lamp reports is a sample of the end of a ramp, and losing the
         * last report leaves the figure a few percent low; used as the restore
         * level, the next off would record that in turn. So the commanded value
         * wins: it is what the user chose, and `lighting_set_brightness` has
         * already recorded it. The lamp's report is only a fallback for a level
         * nobody set through here, as after a restart. */
        remember_reported_level(ieee, ep, e);
        err = off_endpoint(ieee, ep, e, ds);
    } else {
        int pct = recall_level(ieee, ep);
        if (pct < 0) {
            /* Nothing commanded since the restart (this table does not survive one),
             * so the lamp's own level - unless switching off has already
             * flattened it, in which case come back at full.
             *
             * "Flattened" is the whole bottom of the range, not just level 1:
             * off_endpoint parks the lamp at PARKED_LEVEL before a warm off, and a
             * lamp left that way would otherwise come back as a 1 % glow. */
            const bool flattened = e->level == ZBC_UNKNOWN || e->level <= PARKED_LEVEL;
            pct = flattened ? 100 : zbc_level_to_pct(e->level);
        }
        /* The brightness asked for, read back through the level curve from the
         * level the lamp is coming back at - "back at full" when it was
         * flattened. `pct` is always an output here: a level the lamp reported,
         * or one remembered from its report. */
        if (input_pct <= 0) {
            input_pct = curve_db_input_for(ieee, ep, pct);
        }
        /* Give the ramp somewhere to start. OffWithEffect leaves CurrentLevel where
         * it was, so without this the lamp is asked to move to the level it
         * already holds and comes on at full instantly. Only when the lamp really
         * is dark and not already at the bottom: endpoint_is_dark decides both.
         *
         * zb_fade_in sends the two frames as one command: as two calls the gate
         * would treat the ramp as superseding the preset, which would then never
         * go out. */
        from_dark = endpoint_is_dark(ieee, ep, e, ds);
        if (from_dark && input_pct > 0 && glides(ieee, ep, e)) {
            /* restore_curve_color below then finds the glide's colour recorded
             * and has nothing to add. */
            err = glide_on(ieee, ep, e, input_pct, zbc_pct_to_level(pct), ds, AL_NO_SHIFT);
        } else {
            err = from_dark ? out_fade_in(ieee, ep, zbc_pct_to_level(pct), ds)
                            : out_set_level(ieee, ep, zbc_pct_to_level(pct), ds);
        }
    }

    /* Before the bookkeeping: this reads the input pct that note_sent rewrites. */
    if (err == ESP_OK && cmd == ZB_ONOFF_ON && e) {
        restore_curve_color(ieee, ep, e, input_pct, ds, from_dark);
    }
    if (err == ESP_OK && cmd != ZB_ONOFF_TOGGLE) {
        curve_db_note_sent(ieee, ep, input_pct, cmd == ZB_ONOFF_OFF);
    }
    return err;
}

esp_err_t lighting_set_onoff(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, zb_onoff_cmd_t cmd)
{
    stream_hand_endpoint(ieee, ep);
    return set_onoff_ex(ieee, ep, cmd);
}

bool lighting_group_any_on(const zbc_group_t *g)
{
    for (int m = 0; m < g->member_count; m++) {
        zbc_device_t dev;
        if (!lighting_get_device(g->members[m].ieee, &dev)) {
            continue;
        }
        const zbc_endpoint_t *e = lighting_find_endpoint(&dev, g->members[m].ep);
        if (e && endpoint_lit(g->members[m].ieee, g->members[m].ep, e) == 1) {
            return true;
        }
    }
    return false;
}

static esp_err_t group_brightness(uint16_t group_id, int input_pct, uint16_t transition_ds,
                                  bool coarse, bool switching_on, al_shift_t al)
{
    zbc_group_t g;
    if (!group_db_get(group_id, &g)) {
        return ESP_ERR_NOT_FOUND;
    }

    /* Members that cannot dim are skipped rather than failing the whole group: a
     * mixed group still has a working brightness slider for the lamps that have
     * one, and the rest keep following its on/off. */
    int sent = 0, dimmable = 0;
    for (int m = 0; m < g.member_count; m++) {
        esp_err_t err = set_brightness_ex(g.members[m].ieee, g.members[m].ep,
                                          input_pct, transition_ds, coarse, al);
        if (err == ESP_ERR_NOT_SUPPORTED && switching_on) {
            /* A switch-only member with no curve has no brightness to follow,
             * and "the rest keep following its on/off" is precisely this call:
             * skipping it here would leave it dark every time the group came
             * back on, while the off path switches it off. */
            err = lighting_set_onoff(g.members[m].ieee, g.members[m].ep, ZB_ONOFF_ON);
        }
        if (err == ESP_ERR_NOT_SUPPORTED || err == ESP_ERR_NOT_FOUND) {
            continue;
        }
        dimmable++;
        if (err == ESP_OK) {
            sent++;
        }
    }

    if (dimmable == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (sent == 0) {
        /* Not ESP_ERR_INVALID_STATE, which the API answers as "this room has no
         * lights in it" - a room whose lamps are all unplugged has lights. */
        return ESP_FAIL;
    }
    group_db_note_level(group_id, input_pct);
    return ESP_OK;
}

/* Every public way of setting a group's brightness is somebody's hand - the web
 * UI, a button, a remote, a Matter controller - so each one stops Adaptive
 * Lighting for the group. Its own ticks call group_brightness() directly. */
esp_err_t lighting_group_set_brightness(uint16_t group_id, int input_pct, uint16_t transition_ds)
{
    al_note_manual(group_id, "dimmed");
    stream_hand_group(group_id);
    return group_brightness(group_id, input_pct, transition_ds, false, false, AL_NO_SHIFT);
}

/* A brightness that is also a switch-on: a member that can only switch is
 * switched on rather than skipped, as lighting_group_set_onoff does when it
 * re-applies last_pct. */
esp_err_t lighting_group_switch_on_to(uint16_t group_id, int input_pct, uint16_t transition_ds)
{
    al_note_manual(group_id, "dimmed");
    stream_hand_group(group_id);
    return group_brightness(group_id, input_pct, transition_ds, false, true, AL_NO_SHIFT);
}

esp_err_t lighting_group_set_onoff(uint16_t group_id, zb_onoff_cmd_t cmd, uint16_t transition_ds)
{
    stream_hand_group(group_id);
    zbc_group_t g;
    if (!group_db_get(group_id, &g)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (g.member_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    if (cmd == ZB_ONOFF_TOGGLE) {
        /* Resolved once for the whole group: sending Toggle to each member would
         * split a half-lit group instead of switching it. */
        cmd = lighting_group_any_on(&g) ? ZB_ONOFF_OFF : ZB_ONOFF_ON;
    }

    /* Switching a group off ends its Adaptive Lighting mode. A plain on neither
     * starts it nor uses the sun's values: that is what the mode's own button is
     * for, and a switch that sometimes came on at the sun's brightness would be a
     * switch nobody could predict. */
    if (cmd == ZB_ONOFF_OFF && al_end(group_id)) {
        ESP_LOGI(TAG, "Adaptive Lighting ended for group %u: switched off", group_id);
    }

    /* Switching a group back on re-applies its brightness rather than sending a
     * bare On, so the curves get their say: a lamp whose curve puts it below its
     * threshold stays dark instead of coming back at whatever level it held. */
    if (cmd == ZB_ONOFF_ON && g.last_pct != ZBC_UNKNOWN) {
        return group_brightness(group_id, g.last_pct, transition_ds, false, true, AL_NO_SHIFT);
    }

    int sent = 0;
    for (int m = 0; m < g.member_count; m++) {
        /* Through the per-light path so members fade the same way they would
         * on their own; it also keeps the curve bookkeeping in one place. */
        if (lighting_set_onoff(g.members[m].ieee, g.members[m].ep, cmd) == ESP_OK) {
            sent++;
        }
    }
    /* ESP_ERR_INVALID_STATE is kept for the empty room above; see group_brightness. */
    return sent > 0 ? ESP_OK : ESP_FAIL;
}

bool lighting_group_state(uint16_t group_id, bool *on, int *pct)
{
    zbc_group_t g;
    if (!group_db_get(group_id, &g)) {
        return false;
    }

    const bool any_on = lighting_group_any_on(&g);
    int        p      = g.last_pct != ZBC_UNKNOWN ? g.last_pct : -1;

    if (p < 0) {
        /* Nothing has been commanded since the reboot - last_pct is deliberately
         * not persisted. The brightest member that is lit is the best available
         * guess, and it is what a dim sweep has to start from, or holding a
         * button after a power cut would yank the room to one step above zero. */
        for (int m = 0; m < g.member_count; m++) {
            zbc_device_t dev;
            if (!lighting_get_device(g.members[m].ieee, &dev)) {
                continue;
            }
            const zbc_endpoint_t *e = lighting_find_endpoint(&dev, g.members[m].ep);
            if (e && e->on_off == 1 && e->level != ZBC_UNKNOWN && e->level > 0) {
                const int mp = zbc_level_to_pct(e->level);
                if (mp > p) {
                    p = mp;
                }
            }
        }
    }

    if (on) {
        *on = any_on;
    }
    if (pct) {
        *pct = p;
    }
    return true;
}

bool lighting_group_known_off(uint16_t group_id)
{
    zbc_group_t g;
    if (!group_db_get(group_id, &g)) {
        return true;
    }
    bool known_off = false;
    for (int m = 0; m < g.member_count; m++) {
        zbc_device_t dev;
        if (!lighting_get_device(g.members[m].ieee, &dev)) {
            continue;
        }
        const zbc_endpoint_t *e = lighting_find_endpoint(&dev, g.members[m].ep);
        const int lit = e ? endpoint_lit(g.members[m].ieee, g.members[m].ep, e) : ZBC_UNKNOWN;
        if (lit == 1) {
            return false;
        }
        if (lit == 0) {
            known_off = true;
        }
    }
    return known_off;
}

/* The arithmetic of a nudge, for a room and a light alike - see
 * lighting_group_nudge in lighting.h. */
esp_err_t lighting_target_nudge(const lighting_target_t *t, int delta_pct,
                                uint16_t transition_ds, bool coarse, int *landed_pct)
{
    bool on  = false;
    int  pct = -1;
    if (!lighting_target_state(t, &on, &pct)) {
        return ESP_ERR_NOT_FOUND;
    }

    int base;
    if (!on) {
        /* A step up from dark starts at the bottom, so holding a button
         * brings it up gently rather than restoring whatever it held last. */
        base = 0;
    } else if (pct >= 0) {
        base = pct;
    } else {
        /* Lit, but nothing knows how brightly - the light's level, or every
         * member's, is still unread. Start from the middle so a step in either direction does
         * something; the read that follows this command settles it, and the next
         * step of a sweep works from a real number. */
        base = 50;
    }

    if (base == 0 && delta_pct < 0) {
        if (landed_pct) {
            *landed_pct = 1;        /* already as low as it goes: a rail */
        }
        return ESP_OK;
    }

    int target = base + delta_pct;
    if (target < 1) {
        target = 1;
    } else if (target > 100) {
        target = 100;
    }

    /* Already there. A button held at the top of its range would otherwise put
     * a command per lamp on air several times a second for no change at all -
     * the same reason the colour deadbands above exist. The caller still gets
     * the landing point, so it can see it has hit a rail and turn round. */
    if (target == base) {
        if (landed_pct) {
            *landed_pct = target;
        }
        return ESP_OK;
    }

    esp_err_t err = coarse ? lighting_target_ramp_to(t, target, transition_ds)
                           : lighting_target_set_brightness(t, target, transition_ds);
    if (err == ESP_OK && landed_pct) {
        *landed_pct = target;
    }
    return err;
}

esp_err_t lighting_group_nudge(uint16_t group_id, int delta_pct,
                               uint16_t transition_ds, bool coarse, int *landed_pct)
{
    const lighting_target_t t = { .group_id = group_id };
    return lighting_target_nudge(&t, delta_pct, transition_ds, coarse, landed_pct);
}

/* One step of a sweep. Coarse colour deadbands, because a held button walks the
 * whole of a warmth curve in a couple of seconds and the ordinary ones would put
 * a colour command on air beside every single level command. */
esp_err_t lighting_group_ramp_to(uint16_t group_id, int input_pct, uint16_t transition_ds)
{
    al_note_manual(group_id, "dimmed");
    stream_hand_group(group_id);
    return group_brightness(group_id, input_pct, transition_ds, true, false, AL_NO_SHIFT);
}

esp_err_t lighting_group_stop_ramp(uint16_t group_id, int ended_pct)
{
    stream_hand_group(group_id);
    zbc_group_t g;
    if (!group_db_get(group_id, &g)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (ended_pct < 1) {
        ended_pct = 1;
    } else if (ended_pct > 100) {
        ended_pct = 100;
    }

    /* Two commands, and both are needed, because Stop cannot be relied on: an
     * OSRAM PAR16 accepts Stop and StopWithOnOff, answers both successfully, and
     * runs on to the rail regardless (a Hue stops on the first Stop).
     *
     * So the sweep is ended by commanding where it got to. A new level command
     * supersedes a move in progress on any lamp with Level Control, and unlike
     * Stop it puts the lamp exactly where the coordinator thinks it is.
     *
     * Stop still goes first, for the lamps that honour it: it halts the motion
     * at once instead of a fifth of a second later. The order does not matter if
     * they cross - either way the lamp ends stopped at the right level - so this
     * is not the two-commands-with-a-gap race OffWithEffect avoids. */
    for (int m = 0; m < g.member_count; m++) {
        out_level_stop(g.members[m].ieee, g.members[m].ep);
    }

    /* Through the ordinary path, so the curves get their say at the landing
     * point: a member whose curve puts it below its threshold is switched off
     * here, which is something a lamp ramping its own level linearly can never
     * work out for itself. It also records `ended_pct` as the group's
     * brightness, so the next hold carries on from the light in front of you. */
    return lighting_group_set_brightness(group_id, ended_pct, 2);
}

/* ------------------------------------------------------ a room or one light */

static const zbc_endpoint_t *target_endpoint(const lighting_target_t *t, zbc_device_t *dev)
{
    return lighting_get_device(t->ieee, dev) ? lighting_find_endpoint(dev, t->ep) : NULL;
}

bool lighting_target_state(const lighting_target_t *t, bool *on, int *pct)
{
    if (t->ep == 0) {
        return lighting_group_state(t->group_id, on, pct);
    }
    zbc_device_t          dev;
    const zbc_endpoint_t *e = target_endpoint(t, &dev);
    if (!e) {
        return false;
    }
    const bool lit = endpoint_lit(t->ieee, t->ep, e) == 1;

    /* The brightness asked for, as a room's is the one it was last commanded:
     * a lamp's report lags a ramp, and a sweep that started from it would jump.
     * curve_db holds it for a light with a curve, and survives an off; the
     * restore level holds it for one without, where what was asked for is what
     * the lamp was told. After a restart neither knows, and the lamp's own
     * level is the best guess, read back through the curve like the rest. */
    int p = curve_db_input_pct(t->ieee, t->ep);
    if (p <= 0) {
        const int restore = recall_level(t->ieee, t->ep);
        if (restore > 0) {
            p = curve_db_input_for(t->ieee, t->ep, restore);
        } else if (lit && e->level != ZBC_UNKNOWN && e->level > 0) {
            p = curve_db_input_for(t->ieee, t->ep, zbc_level_to_pct(e->level));
        } else if (lit && !(e->features & ZBC_FEAT_LEVEL)) {
            p = 100;          /* on is all a switch can be */
        } else {
            p = ZBC_UNKNOWN;
        }
    }
    if (on) {
        *on = lit;
    }
    if (pct) {
        *pct = p;
    }
    return true;
}

bool lighting_target_known_off(const lighting_target_t *t)
{
    if (t->ep == 0) {
        return lighting_group_known_off(t->group_id);
    }
    zbc_device_t          dev;
    const zbc_endpoint_t *e = target_endpoint(t, &dev);
    return !e || endpoint_lit(t->ieee, t->ep, e) == 0;
}

esp_err_t lighting_target_set_onoff(const lighting_target_t *t, zb_onoff_cmd_t cmd,
                                    uint16_t transition_ds)
{
    if (t->ep == 0) {
        return lighting_group_set_onoff(t->group_id, cmd, transition_ds);
    }
    return lighting_set_onoff(t->ieee, t->ep, cmd);
}

esp_err_t lighting_target_set_brightness(const lighting_target_t *t, int input_pct,
                                         uint16_t transition_ds)
{
    if (t->ep == 0) {
        return lighting_group_set_brightness(t->group_id, input_pct, transition_ds);
    }
    return lighting_set_brightness(t->ieee, t->ep, input_pct, transition_ds);
}

esp_err_t lighting_target_ramp_to(const lighting_target_t *t, int input_pct, uint16_t transition_ds)
{
    if (t->ep == 0) {
        return lighting_group_ramp_to(t->group_id, input_pct, transition_ds);
    }
    /* A hand on the light, as lighting_set_brightness says, with the coarse
     * colour deadbands of a sweep. */
    al_note_manual_endpoint(t->ieee, t->ep, "dimmed");
    stream_hand_endpoint(t->ieee, t->ep);
    return set_brightness_ex(t->ieee, t->ep, input_pct, transition_ds, true, AL_NO_SHIFT);
}

esp_err_t lighting_target_stop_ramp(const lighting_target_t *t, int ended_pct)
{
    if (t->ep == 0) {
        return lighting_group_stop_ramp(t->group_id, ended_pct);
    }
    if (ended_pct < 1) {
        ended_pct = 1;
    } else if (ended_pct > 100) {
        ended_pct = 100;
    }
    /* Stop, then the level the sweep reckons it reached - the pair a room's
     * members get, for the same reasons: see lighting_group_stop_ramp. */
    stream_hand_endpoint(t->ieee, t->ep);
    out_level_stop(t->ieee, t->ep);
    return lighting_set_brightness(t->ieee, t->ep, ended_pct, 2);
}

esp_err_t lighting_set_color_temp(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint16_t mireds,
                                  uint16_t transition_ds)
{
    al_note_manual_endpoint(ieee, ep, "colour temperature changed");
    stream_hand_endpoint(ieee, ep);
    zbc_device_t dev;
    if (lighting_get_device(ieee, &dev)) {
        const zbc_endpoint_t *e = lighting_find_endpoint(&dev, ep);
        if (e && mireds > 0) {
            /* zb_set_color_temp does not clamp, and a lamp told to go past its
             * own limit silently ignores the command. */
            mireds = (uint16_t)(1000000 / clamp_kelvin(e, 1000000 / mireds));
        }
    }
    return out_set_ct(ieee, ep, mireds, transition_ds);
}

esp_err_t lighting_set_color_hs(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint8_t hue,
                                uint8_t saturation, uint16_t transition_ds)
{
    if (zbc_light_id_from_ieee(ieee)) {
        return ESP_ERR_NOT_SUPPORTED;   /* white LEDs only */
    }
    al_note_manual_endpoint(ieee, ep, "colour changed");
    stream_hand_endpoint(ieee, ep);
    return out_set_hs(ieee, ep, hue, saturation, transition_ds);
}

#if CONFIG_ZBC_WLED

/* ------------------------------------- entertainment streams: the session */

/* Recursive: ending a stream switches lamps through the public paths, which
 * ask stream_hand_* - harmless once the stream is marked ended, and safe if a
 * later change asks while it is not. */
static void stream_lock(void)
{
    static portMUX_TYPE once = portMUX_INITIALIZER_UNLOCKED;
    taskENTER_CRITICAL(&once);
    if (!s_stream_lock) {
        s_stream_lock = xSemaphoreCreateRecursiveMutexStatic(&s_stream_lock_buf);
    }
    taskEXIT_CRITICAL(&once);
    xSemaphoreTakeRecursive(s_stream_lock, portMAX_DELAY);
}

static void stream_unlock(void)
{
    xSemaphoreGiveRecursive(s_stream_lock);
}

size_t lighting_stream_lamps(uint16_t group_id, zb_ent_lamp_t *out, size_t max)
{
    zbc_group_t g;
    if (!group_db_get(group_id, &g)) {
        return 0;
    }
    size_t n = 0;
    for (int m = 0; m < g.member_count && n < max && n < ZB_ENT_MAX_LAMPS; m++) {
        uint16_t id;
        if (light_kind(g.members[m].ieee, &id) != LIGHT_ZIGBEE) {
            continue;
        }
        zbc_device_t dev;
        if (!device_db_get(g.members[m].ieee, &dev)) {
            continue;
        }
        const zbc_endpoint_t *e = lighting_find_endpoint(&dev, g.members[m].ep);
        if (!e || !(e->features & ZBC_FEAT_ANY_COLOR) || !stream_is_hue(dev.manufacturer)) {
            continue;
        }
        memcpy(out[n].ieee, g.members[m].ieee, ZBC_IEEE_LEN);
        out[n].ep = g.members[m].ep;
        n++;
    }
    return n;
}

/* What a lamp is to go back to when the stream ends, taken before its first
 * frame: afterwards the lamp says nothing true until the stream is over. */
static void stream_snapshot(const zb_ent_lamp_t *lamp, stream_lamp_t *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->ieee, lamp->ieee, ZBC_IEEE_LEN);
    out->ep         = lamp->ep;
    out->was_on     = ZBC_UNKNOWN;
    out->input_pct  = ZBC_UNKNOWN;
    out->color_mode = ZBC_UNKNOWN;
    out->ct_mireds  = ZBC_UNKNOWN;
    out->hue        = ZBC_UNKNOWN;
    out->sat        = ZBC_UNKNOWN;

    zbc_device_t dev;
    if (!lighting_get_device(lamp->ieee, &dev)) {
        return;
    }
    const zbc_endpoint_t *e = lighting_find_endpoint(&dev, lamp->ep);
    if (!e) {
        return;
    }
    out->was_on     = (int8_t)endpoint_lit(lamp->ieee, lamp->ep, e);
    out->color_mode = e->color_mode;
    out->ct_mireds  = e->ct_mireds;
    out->hue        = e->hue;
    out->sat        = e->saturation;
    /* The brightness asked for last, which is what the slider shows; else the
     * one the lamp reported, read back through its curve. */
    int pct = curve_db_input_pct(lamp->ieee, lamp->ep);
    if (pct <= 0 && e->level != ZBC_UNKNOWN && e->level > 0) {
        pct = curve_db_input_for(lamp->ieee, lamp->ep, zbc_level_to_pct(e->level));
    }
    out->input_pct = (int16_t)(pct > 0 ? pct : ZBC_UNKNOWN);
}

/* A lamp back to how it was before the stream. Dark: switched off as any lamp is
 * (with the warm-off, if set - the lamp is lit, so it applies). Lit: its colour,
 * unless its own colour path will send one, then its brightness, curves and all
 * - or, in a room running Adaptive Lighting, the mode's brightness and shift for
 * now (`al_pct` above 0), which is what the room would show had it not streamed.
 * A lamp nothing was known about is switched off: lights left in a film's last
 * colour are the worse surprise. */
static void stream_restore_lamp(const stream_lamp_t *l, int al_pct, al_shift_t al)
{
    zbc_device_t dev;
    if (!lighting_get_device(l->ieee, &dev)) {
        return;
    }
    const zbc_endpoint_t *e = lighting_find_endpoint(&dev, l->ep);
    if (!e) {
        return;
    }
    const uint16_t ds = settings_transition_ds();
    if (l->was_on != 1) {
        off_endpoint(l->ieee, l->ep, e, ds);
        return;
    }
    zbc_curveset_t cs;
    const bool path = curve_db_get(l->ieee, l->ep, &cs) && cs.chroma.n > 0;
    if (!path) {
        if (l->color_mode == 2 && l->ct_mireds > 0) {
            out_set_ct(l->ieee, l->ep, (uint16_t)l->ct_mireds, ds);
        } else if (l->hue >= 0 && l->sat >= 0) {
            out_set_hs(l->ieee, l->ep, (uint8_t)l->hue, (uint8_t)l->sat, ds);
        }
    }
    if (al_pct > 0) {
        set_brightness_ex(l->ieee, l->ep, al_pct, ds, false, al);
    } else {
        set_brightness_ex(l->ieee, l->ep, l->input_pct > 0 ? l->input_pct : 100, ds, false, AL_NO_SHIFT);
    }
}

/* Switches off the lights of a room that will not take frames - every member
 * that is not one of `lamps` and not known to be dark - and lists them in
 * `out`. Through the ordinary off, so the fade and Warm while off apply and the
 * level to come back at is remembered. A light whose off did not go out (one
 * that has stopped answering) is not listed: nothing was done to it.
 *
 * Not inlined: its room and device copies would otherwise stay in
 * stream_begin_locked's frame for the rest of the start, which runs on the
 * WLED task's 4 KB stack and can go on to end the stream (stream_end_locked). */
static __attribute__((noinline)) uint8_t stream_darken(uint16_t group_id, const zb_ent_lamp_t *lamps,
                                                       size_t n, stream_dark_t *out)
{
    zbc_group_t g;
    if (!group_db_get(group_id, &g)) {
        return 0;
    }
    uint8_t k = 0;
    for (int m = 0; m < g.member_count; m++) {
        const uint8_t *ieee = g.members[m].ieee;
        const uint8_t  ep   = g.members[m].ep;
        bool streams = false;
        for (size_t i = 0; i < n && !streams; i++) {
            streams = lamps[i].ep == ep && memcmp(lamps[i].ieee, ieee, ZBC_IEEE_LEN) == 0;
        }
        zbc_device_t          dev;
        const zbc_endpoint_t *e = !streams && lighting_get_device(ieee, &dev) ? lighting_find_endpoint(&dev, ep)
                                                                               : NULL;
        if (!e) {
            continue;
        }
        const int lit = endpoint_lit(ieee, ep, e);
        if (lit == 0 || set_onoff_ex(ieee, ep, ZB_ONOFF_OFF) != ESP_OK) {
            continue;
        }
        memcpy(out[k].ieee, ieee, ZBC_IEEE_LEN);
        out[k].ep     = ep;
        out[k].was_on = (int8_t)lit;
        k++;
    }
    if (k) {
        ESP_LOGI(TAG, "Stream to group %u: %u other light(s) switched off", group_id, k);
    }
    return k;
}

/* A light the stream switched off, back on if it was lit: as any switch-on
 * would, at the brightness it had and with its curve's colour - or, in a room
 * running Adaptive Lighting (`al_pct` above 0), where the mode has the room now,
 * as for the stream's own lamps. One whose state nobody knew stays off. */
static void stream_relight(const stream_dark_t *l, int al_pct, al_shift_t al)
{
    if (l->was_on != 1) {
        return;
    }
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    if (al_pct > 0) {
        err = set_brightness_ex(l->ieee, l->ep, al_pct, settings_transition_ds(), false, al);
    }
    /* A switch-only light with no curve has no brightness to follow, as in
     * group_brightness. */
    if (err == ESP_ERR_NOT_SUPPORTED) {
        set_onoff_ex(l->ieee, l->ep, ZB_ONOFF_ON);
    }
}

typedef enum {
    STREAM_AFTER_HOLD = 0,     /* a hand: its own command follows */
    STREAM_AFTER_RESTORE,
    STREAM_AFTER_ROOM_OFF,
} stream_after_t;

/* Ends the stream, with s_stream_lock held. The lamps are told (they then hold
 * the last frame), believed lit - which they are, whatever they last reported -
 * and their deadbands forgotten, so whatever comes next is sent in full. */
static void stream_end_locked(lighting_stream_end_t why, stream_after_t after)
{
    stream_lamp_t lamps[ZB_ENT_MAX_LAMPS];
    uint8_t       n;
    uint16_t      group;
    portENTER_CRITICAL(&s_stream_mux);
    if (!s_stream.active) {
        portEXIT_CRITICAL(&s_stream_mux);
        return;
    }
    s_stream.active      = false;
    n                    = s_stream.n;
    group                = s_stream.group_id;
    memcpy(lamps, s_stream.lamp, sizeof(lamps));
    s_stream.st.active   = false;
    s_stream.st.ended_us = esp_timer_get_time();
    s_stream.st.last_end = why;
    portEXIT_CRITICAL(&s_stream_mux);

    zb_ent_end(true);
    for (int i = 0; i < n; i++) {
        note_sent(lamps[i].ieee, lamps[i].ep, true);
        curve_db_forget_sent(lamps[i].ieee, lamps[i].ep);
    }
    static const char *const WHY[] = { "", "by its source", "after its timeout", "by hand", "" };
    ESP_LOGI(TAG, "Stream to group %u ended %s", group, WHY[why <= LIGHTING_STREAM_END_FAILED ? why : 0]);

    if (after == STREAM_AFTER_ROOM_OFF) {
        lighting_group_set_onoff(group, ZB_ONOFF_OFF, settings_transition_ds());
    } else if (after == STREAM_AFTER_RESTORE) {
        /* A room running Adaptive Lighting had its ticks refused for these lamps
         * while it streamed, though the mode believes they landed. They are put
         * where the mode has the room now, here, with the restore's own fade,
         * rather than by asking the mode to send again: its tick ramps over a
         * minute, and that slow ramp is what the lamps' settle reads would then
         * wait for, leaving a lamp's level as whatever it said straight after
         * the stream (the frame's, before it moved) for that long. */
        int        al_pct = 0;
        al_shift_t al     = AL_NO_SHIFT;
        const bool al_on  = al_reason_active(group_al_reason(group, NULL)) && al_target(group, &al_pct, &al);
        for (int i = 0; i < n; i++) {
            stream_restore_lamp(&lamps[i], al_on ? al_pct : 0, al);
        }
        /* Read in place rather than copied: nothing changes the set once the
         * stream is marked ended - a hand only takes from a running one, and a
         * new stream starts under s_stream_lock, which is held here. */
        for (int i = 0; i < s_stream.n_dark; i++) {
            stream_relight(&s_stream.dark[i], al_on ? al_pct : 0, al);
        }
    }
    /* Ended by a hand, the lights the stream switched off stay dark, as its
     * lamps stay where it left them: a hand on the room has its own command for
     * them too. Ended with the room off, they are dark already. */
    zbc_state_bump();
}

static void stream_task(void *arg)
{
    const uint32_t gen = (uint32_t)(uintptr_t)arg;
    /* The colour a pixel last had, for a black one to keep. D65 until then. */
    float x[ZB_ENT_MAX_LAMPS], y[ZB_ENT_MAX_LAMPS];
    for (int i = 0; i < ZB_ENT_MAX_LAMPS; i++) {
        x[i] = 0.3127f;
        y[i] = 0.3290f;
    }
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        if (xTaskDelayUntil(&wake, pdMS_TO_TICKS(STREAM_TICK_MS)) == pdFALSE) {
            /* Behind - a frame waited on the stack lock. Carry on from now rather
             * than sending a burst to catch up. */
            wake = xTaskGetTickCount();
            portENTER_CRITICAL(&s_stream_mux);
            s_stream.st.late++;
            portEXIT_CRITICAL(&s_stream_mux);
        }
        uint8_t rgb[ZB_ENT_MAX_LAMPS][3];
        uint8_t n, master;
        bool    expired;
        portENTER_CRITICAL(&s_stream_mux);
        if (!s_stream.active || s_stream.gen != gen) {
            portEXIT_CRITICAL(&s_stream_mux);
            break;
        }
        n       = s_stream.n;
        master  = s_stream.master;
        memcpy(rgb, s_stream.rgb, sizeof(rgb));
        expired = esp_timer_get_time() - s_stream.last_rx_us >= s_stream.timeout_us;
        portEXIT_CRITICAL(&s_stream_mux);

        if (expired) {
            stream_lock();
            portENTER_CRITICAL(&s_stream_mux);
            const bool still = s_stream.active && s_stream.gen == gen;
            portEXIT_CRITICAL(&s_stream_mux);
            if (still) {
                stream_end_locked(LIGHTING_STREAM_END_TIMEOUT, STREAM_AFTER_RESTORE);
            }
            stream_unlock();
            break;
        }

        zb_ent_light_t l[ZB_ENT_MAX_LAMPS];
        for (int i = 0; i < n; i++) {
            stream_rgb_to_light(rgb[i][0], rgb[i][1], rgb[i][2], master, &l[i].bri, &x[i], &y[i]);
            l[i].x = x[i];
            l[i].y = y[i];
        }
        const esp_err_t err = zb_ent_frame(l, STREAM_TICK_MS);
        portENTER_CRITICAL(&s_stream_mux);
        if (err == ESP_OK) {
            s_stream.st.frames++;
        } else {
            s_stream.st.frames_failed++;
        }
        portEXIT_CRITICAL(&s_stream_mux);
    }
    vTaskDelete(NULL);
}

/* With s_stream_lock held. */
static esp_err_t stream_begin_locked(uint16_t group_id, int64_t now_us)
{
    zb_ent_lamp_t lamps[ZB_ENT_MAX_LAMPS];
    const size_t  n = lighting_stream_lamps(group_id, lamps, ZB_ENT_MAX_LAMPS);
    if (n == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    stream_lamp_t snap[ZB_ENT_MAX_LAMPS];
    for (size_t i = 0; i < n; i++) {
        stream_snapshot(&lamps[i], &snap[i]);
    }

    esp_err_t err = zb_ent_begin(lamps, (uint8_t)n);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_stream_mux);
        s_stream.retry_after_us = now_us + 5000000LL;
        s_stream.st.last_end    = LIGHTING_STREAM_END_FAILED;
        portEXIT_CRITICAL(&s_stream_mux);
        ESP_LOGW(TAG, "Stream to group %u could not start (%s)", group_id, esp_err_to_name(err));
        return err;
    }

    /* The room's other lights, before the stream is published: from then on the
     * transports refuse them, as they refuse its lamps. Listed in place: nothing
     * reads the set while no stream is active, and starts are serialised by
     * s_stream_lock, held here. */
    const uint8_t n_dark = settings_wled_others_off() ? stream_darken(group_id, lamps, n, s_stream.dark) : 0;

    portENTER_CRITICAL(&s_stream_mux);
    s_stream.active     = true;
    s_stream.gen++;
    s_stream.group_id   = group_id;
    s_stream.n          = (uint8_t)n;
    memcpy(s_stream.lamp, snap, sizeof(snap[0]) * n);
    s_stream.n_dark     = n_dark;
    memset(s_stream.rgb, 0, sizeof(s_stream.rgb));
    s_stream.last_rx_us = now_us;
    s_stream.st.active     = true;
    s_stream.st.group_id   = group_id;
    s_stream.st.lamps      = (uint8_t)n;
    s_stream.st.started_us = now_us;
    s_stream.st.last_end   = LIGHTING_STREAM_END_NONE;
    s_stream.st.sessions++;
    const uint32_t gen = s_stream.gen;
    portEXIT_CRITICAL(&s_stream_mux);

    if (xTaskCreate(stream_task, "stream", 4096, (void *)(uintptr_t)gen, 4, NULL) != pdPASS) {
        stream_end_locked(LIGHTING_STREAM_END_FAILED, STREAM_AFTER_RESTORE);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Stream to group %u: %u lamp(s)", group_id, (unsigned)n);
    zbc_state_bump();
    return ESP_OK;
}

esp_err_t lighting_stream_put(uint16_t group_id, const uint8_t *rgb, size_t n, uint8_t master,
                              int64_t timeout_us)
{
    const int64_t now = esp_timer_get_time();
    const int64_t t   = stream_timeout_us(timeout_us);

    for (int attempt = 0; attempt < 2; attempt++) {
        portENTER_CRITICAL(&s_stream_mux);
        if (attempt == 0 && !stream_take(&s_stream.override, &s_stream.ovr_rx_us, now, t)) {
            s_stream.st.dropped++;
            portEXIT_CRITICAL(&s_stream_mux);
            return ESP_ERR_INVALID_STATE;
        }
        if (s_stream.active) {
            if (s_stream.group_id != group_id) {
                portEXIT_CRITICAL(&s_stream_mux);
                return ESP_ERR_INVALID_STATE;
            }
            const size_t take = n < s_stream.n ? n : s_stream.n;
            memcpy(s_stream.rgb, rgb, take * 3);
            s_stream.master     = master;
            s_stream.last_rx_us = now;
            s_stream.timeout_us = t;
            s_stream.st.packets++;
            portEXIT_CRITICAL(&s_stream_mux);
            return ESP_OK;
        }
        const bool may_start = now >= s_stream.retry_after_us;
        portEXIT_CRITICAL(&s_stream_mux);
        if (!may_start || attempt > 0) {
            return ESP_ERR_INVALID_STATE;
        }

        stream_lock();
        esp_err_t err = ESP_OK;
        if (!s_stream.active) {
            err = stream_begin_locked(group_id, now);
        }
        stream_unlock();
        if (err != ESP_OK) {
            return err;
        }
        /* Round again to store this packet's pixels. */
    }
    return ESP_ERR_INVALID_STATE;
}

bool lighting_stream_stop(uint16_t group_id, bool room_off)
{
    stream_lock();
    portENTER_CRITICAL(&s_stream_mux);
    s_stream.override = false;
    const bool ours   = s_stream.active && s_stream.group_id == group_id;
    portEXIT_CRITICAL(&s_stream_mux);
    if (ours) {
        stream_end_locked(LIGHTING_STREAM_END_SOURCE, room_off ? STREAM_AFTER_ROOM_OFF : STREAM_AFTER_RESTORE);
    }
    stream_unlock();
    return ours;
}

/* A hand on the stream's room or one of its lamps. The stream ends with the lamps
 * where it left them - the hand's own command follows at once - and its source
 * is ignored until it pauses: see stream_take. */
static void stream_end_by_hand(void)
{
    stream_lock();
    stream_end_locked(LIGHTING_STREAM_END_HAND, STREAM_AFTER_HOLD);
    portENTER_CRITICAL(&s_stream_mux);
    s_stream.override  = true;
    s_stream.ovr_rx_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_stream_mux);
    stream_unlock();
}

/* A hand on one light. On a lamp of the stream it ends the stream; on a light
 * the stream switched off it takes only that light back - somebody switching on
 * a reading lamp during a film wants the reading lamp - which is then neither
 * held nor switched back on when the stream ends. */
static void stream_hand_endpoint(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    int at;
    portENTER_CRITICAL(&s_stream_mux);
    const stream_role_t role = stream_role_locked(ieee, ep, &at);
    if (role == STREAM_DARK) {
        s_stream.dark[at] = s_stream.dark[--s_stream.n_dark];
    }
    portEXIT_CRITICAL(&s_stream_mux);
    if (role == STREAM_LAMP) {
        stream_end_by_hand();
    }
}

/* A hand on a room. The stream's own room ends it. Another room is a hand on
 * each of its members, since a light can be in both: without this, its
 * brightness - group_brightness, which asks no member - would be refused for a
 * light the stream holds. */
static void stream_hand_group(uint16_t group_id)
{
    portENTER_CRITICAL(&s_stream_mux);
    const bool active = s_stream.active;
    const bool ours   = active && s_stream.group_id == group_id;
    portEXIT_CRITICAL(&s_stream_mux);
    if (ours) {
        stream_end_by_hand();
        return;
    }
    zbc_group_t g;
    if (!active || !group_db_get(group_id, &g)) {
        return;
    }
    for (int m = 0; m < g.member_count; m++) {
        stream_hand_endpoint(g.members[m].ieee, g.members[m].ep);
    }
}

void lighting_stream_status(lighting_stream_status_t *out)
{
    portENTER_CRITICAL(&s_stream_mux);
    *out            = s_stream.st;
    out->override   = s_stream.override;
    out->lights_off = s_stream.active ? s_stream.n_dark : 0;
    portEXIT_CRITICAL(&s_stream_mux);
}

#endif /* CONFIG_ZBC_WLED */

/* ------------------------------------------------ Adaptive Lighting: the day */

/* How often the day moves on, and the transition each step takes. The same, so
 * a lamp is still moving towards one step when the next extends it. */
#define AL_TICK_MS       60000
#define AL_TRANSITION_DS 600

static const char *al_reason_name(al_reason_t r)
{
    switch (r) {
    case AL_ON:          return "on";
    case AL_NO_CLOCK:    return "no_clock";
    case AL_NO_LOCATION: return "no_location";
    default:             return "off";
    }
}

static void al_tick(void)
{
    if (!al_any_running()) {
        return;
    }
    heap_watch_mark("al_tick", 0);
    zbc_group_t *groups = calloc(CONFIG_ZBC_MAX_GROUPS, sizeof(*groups));
    if (!groups) {
        return;
    }
    const size_t n = group_db_snapshot(groups, CONFIG_ZBC_MAX_GROUPS);

    for (size_t i = 0; i < n; i++) {
        const zbc_group_t *g = &groups[i];
        al_group_t         snap;
        const al_reason_t  r = group_al_reason(g->id, &snap);

        bool changed = false, have = false;
        portENTER_CRITICAL(&s_al_mux);
        al_group_t *s = al_slot_locked(g->id, false);
        if (s) {
            have      = true;
            changed   = s->reason != (uint8_t)r;
            s->reason = (uint8_t)r;
        }
        portEXIT_CRITICAL(&s_al_mux);
        if (!have) {
            continue;            /* never started since the boot */
        }
        if (changed && r != AL_OFF) {
            ESP_LOGI(TAG, "Adaptive Lighting for group %u: %s", g->id, al_reason_name(r));
            zbc_state_bump();
        }

        /* A group that went dark by some other way than being switched off here -
         * a lamp's own switch - gets nothing until it is lit again. Each group
         * follows its own day. */
        int        pct = 0;
        al_shift_t shift;
        if (!al_reason_active(r) || !al_target(g->id, &pct, &shift) || !lighting_group_any_on(g) ||
            !al_needs_send(pct, snap.last_pct, (int)shift.offset_k, snap.last_offset_k)) {
            continue;
        }
        if (group_brightness(g->id, pct, AL_TRANSITION_DS, false, false, shift) == ESP_OK) {
            al_note_sent(g->id, pct, (int)shift.offset_k);
            ESP_LOGI(TAG, "Adaptive Lighting: group %u to %d %%, %+d K", g->id, pct, (int)shift.offset_k);
        }
    }
    free(groups);
}

/* Stores which groups run the mode, when that has changed. Only ever from the
 * Adaptive Lighting task, so two starts and stops on other tasks cannot write
 * their sets in the wrong order; every start and end wakes it to come here.
 * Never before lighting_al_resume, which is why the setup portal - no task -
 * leaves the stored set alone for the boot after it. */
static void al_store_running(void)
{
    uint16_t ids[CONFIG_ZBC_MAX_GROUPS];
    size_t   n = 0;
    portENTER_CRITICAL(&s_al_mux);
    for (size_t i = 0; i < sizeof(s_al) / sizeof(s_al[0]); i++) {
        if (s_al[i].used && s_al[i].running) {
            ids[n++] = s_al[i].group_id;
        }
    }
    portEXIT_CRITICAL(&s_al_mux);
    const esp_err_t err = al_db_set_running(ids, n);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Adaptive Lighting: could not store the running groups: %s", esp_err_to_name(err));
    }
}

/* A task, not an esp_timer callback: a tick goes through the stack lock, and
 * nothing that takes it may run on the timer task (see CLAUDE.md). */
static void al_task(void *arg)
{
    (void)arg;
    uint32_t why = AL_WAKE_TICK;
    for (;;) {
        if (why & AL_WAKE_TICK) {
            al_tick();
        }
        al_store_running();
        if (xTaskNotifyWait(0, UINT32_MAX, &why, pdMS_TO_TICKS(AL_TICK_MS)) != pdTRUE) {
            why = AL_WAKE_TICK;  /* the minute is up */
        }
    }
}

void lighting_al_resume(void)
{
    uint16_t     ids[CONFIG_ZBC_MAX_GROUPS];
    const size_t n = al_db_running(ids, CONFIG_ZBC_MAX_GROUPS);
    for (size_t i = 0; i < n; i++) {
        /* A group deleted or no longer offering the mode stays ended, and leaves
         * the stored set at the first store. */
        if (al_begin(ids[i]) == ESP_OK) {
            ESP_LOGI(TAG, "Adaptive Lighting resumed for group %u: it was running before the restart", ids[i]);
        }
    }
}

esp_err_t lighting_al_start(void)
{
    if (s_al_task) {
        return ESP_OK;
    }
    return xTaskCreate(al_task, "adaptive", 6144, NULL, 3, &s_al_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void lighting_al_wake(void)
{
    al_wake(AL_WAKE_TICK);
}

bool lighting_al_day(uint16_t group_id, int64_t utc_s, zbc_sun_day_t *real, zbc_sun_day_t *used)
{
    zbc_al_profile_t prof;
    al_db_day(group_id, &prof);
    return al_day(utc_s, &prof, real, used);
}

esp_err_t lighting_group_set_adaptive_day(uint16_t group_id, const zbc_al_profile_t *day)
{
    if (!group_db_get(group_id, NULL)) {
        return ESP_ERR_NOT_FOUND;
    }
    zbc_al_profile_t was;
    al_db_day(group_id, &was);
    const esp_err_t err = al_db_set_day(group_id, day);
    if (err != ESP_OK) {
        return err;
    }
    /* A new base moves the lamps without a warmth curve while the brightness and
     * the offset stay where they were, which al_needs_send would not see: the
     * next tick sends the group again. */
    if (day->k_base != was.k_base) {
        portENTER_CRITICAL(&s_al_mux);
        al_group_t *g = al_slot_locked(group_id, false);
        if (g && g->running) {
            g->last_pct      = -1;
            g->last_offset_k = AL_NO_OFFSET;
        }
        portEXIT_CRITICAL(&s_al_mux);
    }
    al_wake(AL_WAKE_TICK);
    return ESP_OK;
}

esp_err_t lighting_group_start_adaptive(uint16_t group_id, uint16_t transition_ds)
{
    stream_hand_group(group_id);
    esp_err_t err = al_begin(group_id);
    if (err != ESP_OK) {
        return err;
    }

    int        pct = 0;
    al_shift_t shift;
    if (al_target(group_id, &pct, &shift)) {
        /* switching_on: the mode is also a way of switching the group on, so a
         * member that can only switch is switched rather than skipped. A dark
         * lamp fades in from the bottom either way - that is endpoint_is_dark's
         * decision now, not this flag's. */
        err = group_brightness(group_id, pct, transition_ds, false, true, shift);
        if (err == ESP_OK) {
            al_note_sent(group_id, pct, (int)shift.offset_k);
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            /* Nothing in the group dims: it can still be switched on. */
            err = lighting_group_set_onoff(group_id, ZB_ONOFF_ON, transition_ds);
        }
        ESP_LOGI(TAG, "Adaptive Lighting started for group %u: %d %%, %+d K", group_id, pct, (int)shift.offset_k);
    } else {
        err = lighting_group_set_onoff(group_id, ZB_ONOFF_ON, transition_ds);
        ESP_LOGI(TAG, "Adaptive Lighting started for group %u, waiting for %s", group_id,
                 al_clock_ok() ? "a location" : "the clock");
    }
    al_wake(AL_WAKE_STORE);
    return err;
}

void lighting_group_stop_adaptive(uint16_t group_id)
{
    if (al_end(group_id)) {
        ESP_LOGI(TAG, "Adaptive Lighting stopped for group %u", group_id);
    }
}

esp_err_t lighting_group_set_adaptive_expose(uint16_t group_id, uint8_t expose)
{
    if (expose >= ZBC_AL_EXPOSE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!group_db_get(group_id, NULL)) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = al_db_set(group_id, expose);
    if (err == ESP_OK && expose == ZBC_AL_EXPOSE_OFF) {
        /* A mode nobody can see any more must not go on changing the lights. */
        lighting_group_stop_adaptive(group_id);
    }
    return err;
}

void lighting_group_forget_adaptive(uint16_t group_id)
{
    /* The turn-on choice and the day too: the id is given out again. */
    al_db_forget(group_id);
    portENTER_CRITICAL(&s_al_mux);
    al_group_t *s = al_slot_locked(group_id, false);
    if (s) {
        memset(s, 0, sizeof(*s));
    }
    portEXIT_CRITICAL(&s_al_mux);
}

void lighting_note_manual_color(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep)
{
    al_note_manual_endpoint(ieee, ep, "curve changed");
}

bool lighting_group_al_status(uint16_t group_id, lighting_al_status_t *out)
{
    if (!out || !group_db_get(group_id, NULL)) {
        return false;
    }
    al_group_t        snap;
    const al_reason_t r = group_al_reason(group_id, &snap);
    memset(out, 0, sizeof(*out));
    out->expose = al_db_expose(group_id);
    out->active = snap.running;
    out->reason = al_reason_name(r);
    al_shift_t shift;
    if (al_target(group_id, &out->pct, &shift)) {
        out->offset_k = (int)shift.offset_k;
    } else {
        out->pct      = -1;
        out->offset_k = 0;
    }
    return true;
}
