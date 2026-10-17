/* See radio_coex.h.
 *
 * The callback counters are single 32-bit words written only by the radio ISR,
 * each write atomic on this core, and read without a lock, as in radio_ed.c.
 * The hardware totals are folded from two tasks (the timer's and a status
 * request's), so they take a spinlock - held for a few register accesses. */
#include "radio_coex.h"
#include "radio_ed.h"

#include "esp_attr.h"
#include "esp_ieee802154.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "hal/ieee802154_ll.h"
/* The driver's own, from its private include directory (main/CMakeLists.txt):
 * the phase names and the prototype __wrap_ieee802154_set_txrx_pti must match. */
#include "esp_ieee802154_util.h"

#if CONFIG_IEEE802154_TXRX_STATISTIC
#error "CONFIG_IEEE802154_TXRX_STATISTIC clears the counters radio_coex.c reads"
#endif

static const char *TAG = "radio_coex";

/* The counters are 16 bits and stop at 0xFFFF rather than going round: both
 * boards read rx_coex 65533 hours after boot, the top less what stood in the
 * register at start. So each fold takes what has been counted and clears it,
 * as the driver's own statistics do (esp_ieee802154_debug.c). Listening broken
 * off by Wi-Fi comes with each of its wake-ups, ~10 a second; five seconds is a
 * thousandfold margin from the top. */
#define FOLD_US 5000000

enum { HW_RX, HW_RX_ACK, HW_TX_ACK, HW_COUNT };

static portMUX_TYPE        s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool                s_on;
static uint32_t            s_total[HW_COUNT];
static esp_timer_handle_t  s_timer;

static volatile uint32_t s_tx_done;
static volatile uint32_t s_tx_no_ack;
static volatile uint32_t s_tx_bad_ack;
static volatile uint32_t s_tx_coex;
static volatile uint32_t s_tx_cca_busy;
static volatile uint32_t s_tx_other;
static volatile uint32_t s_rx;

/* Each counter is cleared right after it is read, so a count that lands in
 * between is lost - a window of two register accesses. The driver clears one
 * bit at a time and nothing says whether a bit left set holds its counter at
 * zero, so the register is left at 0. */
static void take_hw(uint32_t out[HW_COUNT])
{
    out[HW_RX] = ieee802154_ll_get_rx_abort_coex_cnt() & 0xFFFFU;
    ieee802154_ll_clear_debug_cnt(IEEE802154_RX_ABORT_COEX_CNT_CLEAR);
    out[HW_RX_ACK] = ieee802154_ll_get_rx_ack_abort_coex_cnt() & 0xFFFFU;
    ieee802154_ll_clear_debug_cnt(IEEE802154_RX_ACK_ABORT_COEX_CNT_CLEAR);
    out[HW_TX_ACK] = ieee802154_ll_get_tx_ack_abort_coex_cnt() & 0xFFFFU;
    ieee802154_ll_clear_debug_cnt(IEEE802154_TX_ACK_ABORT_COEX_CNT_CLEAR);
    ieee802154_ll_clear_debug_cnt(0);
}

static void fold_locked(void)
{
    if (!s_on) {
        return;
    }
    uint32_t now[HW_COUNT];
    take_hw(now);
    for (int i = 0; i < HW_COUNT; i++) {
        s_total[i] += now[i];
    }
}

/* esp_timer task: a spinlock and a few register accesses, nothing blocking. */
static void fold_cb(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_mux);
    fold_locked();
    portEXIT_CRITICAL(&s_mux);
}

void radio_coex_start(void)
{
    /* What the counters hold from before is not ours to count. */
    uint32_t stale[HW_COUNT];
    portENTER_CRITICAL(&s_mux);
    take_hw(stale);
    s_on = true;
    portEXIT_CRITICAL(&s_mux);

    if (!s_timer) {
        const esp_timer_create_args_t args = { .callback = fold_cb, .name = "radio_coex" };
        if (esp_timer_create(&args, &s_timer) != ESP_OK) {
            ESP_LOGE(TAG, "Could not create the coexistence counter timer");
            return;
        }
    }
    if (esp_timer_start_periodic(s_timer, FOLD_US) != ESP_OK) {
        ESP_LOGE(TAG, "Could not start the coexistence counter timer");
    }
}

void radio_coex_stop(void)
{
    if (s_timer) {
        esp_timer_stop(s_timer);
    }
    portENTER_CRITICAL(&s_mux);
    fold_locked();
    s_on = false;
    portEXIT_CRITICAL(&s_mux);
}

void radio_coex_get_stats(radio_coex_stats_t *out)
{
    portENTER_CRITICAL(&s_mux);
    fold_locked();
    out->rx_coex     = s_total[HW_RX];
    out->rx_ack_coex = s_total[HW_RX_ACK];
    out->tx_ack_coex = s_total[HW_TX_ACK];
    portEXIT_CRITICAL(&s_mux);

    out->tx_done     = s_tx_done;
    out->tx_no_ack   = s_tx_no_ack;
    out->tx_bad_ack  = s_tx_bad_ack;
    out->tx_coex     = s_tx_coex;
    out->tx_cca_busy = s_tx_cca_busy;
    out->tx_other    = s_tx_other;
    out->rx          = s_rx;
}

void __real_esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *frame_info);
void __real_esp_ieee802154_transmit_done(const uint8_t *frame, const uint8_t *ack,
                                         esp_ieee802154_frame_info_t *ack_frame_info);
void __real_esp_ieee802154_transmit_failed(const uint8_t *frame, esp_ieee802154_tx_error_t error);

/* All three called by the driver from the radio ISR. */
IRAM_ATTR void __wrap_esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *frame_info)
{
    s_rx++;
    __real_esp_ieee802154_receive_done(frame, frame_info);
}

IRAM_ATTR void __wrap_esp_ieee802154_transmit_done(const uint8_t *frame, const uint8_t *ack,
                                                   esp_ieee802154_frame_info_t *ack_frame_info)
{
    s_tx_done++;
    __real_esp_ieee802154_transmit_done(frame, ack, ack_frame_info);
}

IRAM_ATTR void __wrap_esp_ieee802154_transmit_failed(const uint8_t *frame, esp_ieee802154_tx_error_t error)
{
    switch (error) {
    case ESP_IEEE802154_TX_ERR_NO_ACK:      s_tx_no_ack++;   break;
    case ESP_IEEE802154_TX_ERR_INVALID_ACK: s_tx_bad_ack++;  break;
    case ESP_IEEE802154_TX_ERR_COEXIST:     s_tx_coex++;     break;
    case ESP_IEEE802154_TX_ERR_CCA_BUSY:    s_tx_cca_busy++; break;
    default:                                s_tx_other++;    break;
    }
    __real_esp_ieee802154_transmit_failed(frame, error);
}

void __real_ieee802154_set_txrx_pti(ieee802154_txrx_scene_t txrx_scene);

/* The send priority - see radio_coex.h. Called by the driver whenever it
 * changes phase, from the radio ISR as often as not. Only the send phase is
 * raised; every other phase is the driver's own, and the driver sets the
 * listening phase again once a send has finished, so nothing here puts it
 * back. A timed send (IEEE802154_SCENE_TX_AT) is left alone: Zigbee makes
 * none. */
IRAM_ATTR void __wrap_ieee802154_set_txrx_pti(ieee802154_txrx_scene_t txrx_scene)
{
    /* An energy scan sets the listening phase too, and at the listening level an
     * associated Wi-Fi refuses it every time - see radio_ed.h. A scan is the
     * stack's alone (forming, or a channel survey somebody asked for), and runs
     * 138 ms a channel; its listening is raised for that long and no longer. */
    if (txrx_scene == IEEE802154_SCENE_RX && radio_ed_live()) {
        esp_coex_ieee802154_txrx_pti_set(IEEE802154_HIGH);
        return;
    }
    if (txrx_scene == IEEE802154_SCENE_TX) {
        esp_coex_ieee802154_txrx_pti_set(IEEE802154_MIDDLE);
        return;
    }
    __real_ieee802154_set_txrx_pti(txrx_scene);
}
