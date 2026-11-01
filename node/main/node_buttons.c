#include "node_buttons.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"

#include "auth.h"
#include "btn_gesture.h"
#include "button_db.h"
#include "light_math.h"
#include "local_lights.h"
#include "node_fallback.h"
#include "node_main.h"
#include "node_proto.h"
#include "node_zb.h"
#include "state_rev.h"

static const char *TAG = "node_btn";

/* What the sampler needs. Copied under a spinlock, since it runs on the esp_timer task
 * and must not take a mutex. */
typedef struct {
    bool             used;
    uint16_t         id;
    int8_t           gpio;
    uint8_t          key;
    bool             inverted;
    bool             is_switch;
    uint16_t         fallback;   /* a light to toggle when the hub cannot be reached; 0 none */
    zbc_btn_fsm_t    fsm;
    zbc_unlock_fsm_t unlock;
} slot_t;

typedef struct {
    uint8_t type;       /* MSG_* */
    uint8_t key;
    uint8_t event;
    bool    delivered;
} msg_t;

enum { MSG_GESTURE, MSG_RESULT, MSG_UNLOCK_OPEN, MSG_UNLOCK_HOTSPOT };

static slot_t             s_slots[NODE_MAX_KEYS];
static portMUX_TYPE       s_mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t      s_queue;
static esp_timer_handle_t s_timer;
#if CONFIG_ZBC_UNLOCK_GPIO >= 0
static zbc_unlock_fsm_t   s_unlock_pin;
static bool               s_unlock_self;
#endif

static node_fb_t          s_fb;

static uint8_t gesture_event(zbc_btn_emit_t emit, bool is_switch)
{
    switch (emit) {
    case ZBC_EMIT_SHORT:        return is_switch ? NODE_BTN_SINGLE : NODE_BTN_CLICK;
    case ZBC_EMIT_LONG:         return NODE_BTN_HOLD;
    case ZBC_EMIT_LONG_RELEASE: return NODE_BTN_RELEASE;
    /* LONG_REPEAT: the hub repeats a hold on its own clock until the release. */
    default:                    return 0;
    }
}

static void note_unlock(zbc_unlock_t u)
{
    if (u != UNLOCK_NONE) {
        const msg_t m = { .type = u == UNLOCK_OPEN ? MSG_UNLOCK_OPEN : MSG_UNLOCK_HOTSPOT };
        xQueueSend(s_queue, &m, 0);
    }
}

/* The esp_timer task: no locks but the spinlock, no blocking, nothing that takes the
 * stack lock. It decides that something happened and posts it. */
static void sample_cb(void *arg)
{
    (void)arg;
    const int64_t now = esp_timer_get_time();
    for (int i = 0; i < NODE_MAX_KEYS; i++) {
        portENTER_CRITICAL(&s_mux);
        const bool   used = s_slots[i].used;
        const int8_t pin  = s_slots[i].gpio;
        portEXIT_CRITICAL(&s_mux);
        if (!used || pin < 0) {
            continue;
        }
        const int level = gpio_get_level(pin);

        zbc_btn_emit_t emit = ZBC_EMIT_NONE;
        zbc_unlock_t   u    = UNLOCK_NONE;
        bool           edge = false, is_sw = false;
        uint8_t        key  = 0;
        portENTER_CRITICAL(&s_mux);
        if (s_slots[i].used && s_slots[i].gpio == pin) {
            slot_t       *s   = &s_slots[i];
            const uint8_t was = s->fsm.stable;
            is_sw = s->is_switch;
            key   = s->key;
            /* No double-press wait: a click goes at once, and the hub makes a double
             * out of two, for the buttons that have double-press rules. */
            emit = is_sw ? btn_switch_step(&s->fsm, level, s->inverted, now)
                         : btn_step(&s->fsm, level, s->inverted, false, now);
            u    = unlock_level(&s->unlock, level, s->inverted, now);
            edge = s->fsm.stable != was;
        }
        portEXIT_CRITICAL(&s_mux);
        if (edge) {
            zbc_state_bump();
        }
        note_unlock(u);
        const uint8_t ev = gesture_event(emit, is_sw);
        if (ev) {
            const msg_t m = { .type = MSG_GESTURE, .key = key, .event = ev };
            xQueueSend(s_queue, &m, 0);
        }
    }
#if CONFIG_ZBC_UNLOCK_GPIO >= 0
    if (s_unlock_self) {
        note_unlock(unlock_level(&s_unlock_pin, gpio_get_level(CONFIG_ZBC_UNLOCK_GPIO), true, now));
    }
#endif
}

static uint16_t fallback_of(uint8_t key)
{
    for (int i = 0; i < NODE_MAX_KEYS; i++) {
        if (s_slots[i].used && s_slots[i].key == key) {
            return s_slots[i].fallback;
        }
    }
    return 0;
}

static void run_local(uint8_t key)
{
    const uint16_t light = fallback_of(key);
    if (light) {
        const esp_err_t err = local_light_set(light, ZB_ONOFF_TOGGLE);
        ESP_LOGI(TAG, "Key %u: hub out of reach, toggled light %u (%s)", key, light, esp_err_to_name(err));
    }
}

static void buttons_task(void *arg)
{
    (void)arg;
    for (;;) {
        msg_t m;
        /* Only to run the backstop: a verdict the stack never gives. */
        const bool got = xQueueReceive(s_queue, &m, pdMS_TO_TICKS(1000)) == pdTRUE;
        const int64_t now = esp_timer_get_time();
        uint8_t key;
        while (node_fb_tick(&s_fb, now, &key)) {
            run_local(key);
        }
        if (!got) {
            continue;
        }
        switch (m.type) {
        case MSG_GESTURE: {
            const node_fb_press_t p = node_fb_press(&s_fb, m.key, m.event, fallback_of(m.key) != 0, now);
            if (p.act_now) {
                run_local(m.key);
            }
            if (node_zb_send_button(m.key, p.send_ev, p.seq) != ESP_OK && node_fb_result(&s_fb, p.seq, false, &key)) {
                /* Could not even be queued: nowhere to send it, so that was the verdict. */
                run_local(key);
            }
            break;
        }
        case MSG_RESULT:
            if (node_fb_result(&s_fb, m.key, m.delivered, &key)) {
                run_local(key);
            }
            break;
        case MSG_UNLOCK_OPEN:
            auth_failsafe_open();
            zbc_state_bump();
            ESP_LOGW(TAG, "Failsafe opened by five presses");
            break;
        case MSG_UNLOCK_HOTSPOT:
            ESP_LOGW(TAG, "Ten presses: restarting into the setup hotspot");
            node_restart_into(NODE_ASK_HOTSPOT, 0);
            break;
        default:
            break;
        }
    }
}

void node_buttons_delivered(uint8_t seq, bool delivered)
{
    if (s_queue) {
        const msg_t m = { .type = MSG_RESULT, .key = seq, .delivered = delivered };
        xQueueSend(s_queue, &m, 0);
    }
}

void node_buttons_inject(uint8_t key, uint8_t event)
{
    if (s_queue) {
        const msg_t m = { .type = MSG_GESTURE, .key = key, .event = event };
        xQueueSend(s_queue, &m, 0);
    }
}

bool node_buttons_hub_down(void)
{
    return s_fb.hub_down;
}

bool node_buttons_pin_state(uint16_t id, bool *pressed, int *level)
{
    bool found = false;
    int8_t pin = -1;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < NODE_MAX_KEYS && !found; i++) {
        if (s_slots[i].used && s_slots[i].id == id) {
            found = true;
            pin   = s_slots[i].gpio;
            if (pressed) {
                *pressed = s_slots[i].fsm.stable != 0;
            }
        }
    }
    portEXIT_CRITICAL(&s_mux);
    if (found && pin >= 0 && level) {
        *level = gpio_get_level(pin);
    }
    return found && pin >= 0;
}

/* The light a button toggles when the hub is out of reach: its first short-press rule
 * that toggles a wired light of this node. An ordinary rule, so nothing new is stored
 * and a board's other rules are simply not looked at. */
static uint16_t fallback_light(const zbc_button_t *b)
{
    zbc_button_t copy = *b;
    int n = 0;
    const zbc_btn_rule_t *r = button_db_rules(&copy, ZBC_EV_SHORT, &n);
    for (int i = 0; i < n; i++) {
        if (r[i].action == ZBC_ACT_TOGGLE && r[i].ep != 0) {
            const uint16_t id = zbc_light_id_from_ieee(r[i].ieee);
            if (id) {
                return id;
            }
        }
    }
    return 0;
}

void node_buttons_reload(void)
{
    static bool s_owned[SOC_GPIO_PIN_COUNT];
    bool wanted[SOC_GPIO_PIN_COUNT] = { 0 };

    uint16_t ids[CONFIG_ZBC_MAX_BUTTONS];
    const int n = (int)button_db_list_ids(ids, CONFIG_ZBC_MAX_BUTTONS);
    slot_t next[NODE_MAX_KEYS] = { 0 };
    int used = 0;
    for (int i = 0; i < n && used < NODE_MAX_KEYS; i++) {
        zbc_button_t *b = malloc(sizeof(*b));
        if (!b) {
            break;
        }
        if (button_db_get(ids[i], b) && b->src == ZBC_BTN_SRC_GPIO) {
            slot_t *s = &next[used++];
            s->used      = true;
            s->id        = b->id;
            s->gpio      = b->gpio;
            /* The hub's rules name a key, so it must not move when another button is
             * deleted: its id's, which is stable. button_db hands out one more than the
             * highest, so an id past the keys takes what is free below. */
            s->key       = b->id <= NODE_MAX_KEYS ? (uint8_t)(b->id - 1) : 0xFF;
            s->inverted  = b->inverted;
            s->is_switch = b->input == ZBC_INPUT_SWITCH;
            s->fallback  = fallback_light(b);
            if (b->gpio >= 0 && b->gpio < SOC_GPIO_PIN_COUNT) {
                const gpio_config_t io = {
                    .pin_bit_mask = 1ULL << b->gpio,
                    .mode         = GPIO_MODE_INPUT,
                    .pull_up_en   = b->pull == ZBC_PULL_UP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
                    .pull_down_en = b->pull == ZBC_PULL_DOWN ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
                    .intr_type    = GPIO_INTR_DISABLE,
                };
                if (gpio_config(&io) == ESP_OK) {
                    wanted[b->gpio] = true;
                    s_owned[b->gpio] = true;
                } else {
                    ESP_LOGW(TAG, "GPIO %d refused by the driver", b->gpio);
                    s->gpio = -1;
                }
            }
        }
        free(b);
    }
    for (int pin = 0; pin < SOC_GPIO_PIN_COUNT; pin++) {
        if (s_owned[pin] && !wanted[pin]) {
            gpio_reset_pin(pin);
            s_owned[pin] = false;
        }
    }

    for (int i = 0; i < used; i++) {
        if (next[i].key != 0xFF) {
            continue;
        }
        for (uint8_t k = 0; k < NODE_MAX_KEYS && next[i].key == 0xFF; k++) {
            bool taken = false;
            for (int j = 0; j < used; j++) {
                taken = taken || next[j].key == k;
            }
            if (!taken) {
                next[i].key = k;
            }
        }
    }

    /* A button whose wiring did not change keeps its state, or a switch left on would
     * read as a flip every time the editor saves. */
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < NODE_MAX_KEYS; i++) {
        for (int j = 0; j < NODE_MAX_KEYS; j++) {
            if (next[i].used && s_slots[j].used && s_slots[j].id == next[i].id &&
                s_slots[j].gpio == next[i].gpio && s_slots[j].inverted == next[i].inverted &&
                s_slots[j].is_switch == next[i].is_switch) {
                next[i].fsm    = s_slots[j].fsm;
                next[i].unlock = s_slots[j].unlock;
            }
        }
    }
    memcpy(s_slots, next, sizeof(s_slots));
    portEXIT_CRITICAL(&s_mux);

#if CONFIG_ZBC_UNLOCK_GPIO >= 0
    /* Always watched, so a bare board can be let back into. */
    const int up = CONFIG_ZBC_UNLOCK_GPIO;
    if (up < SOC_GPIO_PIN_COUNT && !wanted[up]) {
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << up,
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        s_unlock_self = gpio_config(&io) == ESP_OK;
    } else {
        s_unlock_self = false;
    }
#endif
}

esp_err_t node_buttons_start(void)
{
    s_queue = xQueueCreate(8, sizeof(msg_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(buttons_task, "buttons", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    node_buttons_reload();
    const esp_timer_create_args_t args = { .callback = sample_cb, .name = "btn_poll" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_timer, BTN_POLL_US), TAG, "start");
    return ESP_OK;
}
