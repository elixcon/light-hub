/* See radio_ed.h.
 *
 * No locks. The done callback runs in the radio ISR under the driver's spinlock,
 * and the watch restarts a scan through the driver, which takes that spinlock
 * too; a lock of ours around either would nest with it in both orders. The
 * shared state is single 32-bit words instead, each write atomic on this core,
 * and the one word both sides may clear is cleared by compare-and-swap. */
#include "radio_ed.h"

#include "esp_attr.h"
#include "esp_bit_defs.h"
#include "esp_ieee802154.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hal/ieee802154_ll.h"

static const char *TAG = "radio_ed";

/* A scan asks for (1 << duration) + 1 of 15.36 ms beacon intervals - 138 ms at
 * the stack's setting. A second is long past any scan it makes, and short next
 * to the indefinite wait it replaces. */
#define ED_OVERDUE_MS   1000
#define ED_WATCH_US     250000
/* How long one channel's scan may take, restarts and all, before the stack is
 * told it is done with nothing measured. Everything the coordinator would hear
 * on its own channel is lost meanwhile, so this is what bounds a survey's cost. */
#define ED_GIVE_UP_MS   1500
/* The reading reported for a scan given up on: below anything a radio measures. */
#define ED_NO_READING   INT8_MIN

/* The two ways the radio ends a scan early. The driver enables neither event,
 * so either one ends the scan in the hardware with no interrupt at all - no done,
 * no failure - and the stack waits for ever. They are enabled while a scan runs
 * and only then: the driver asserts on one arriving in any other state. */
#define ED_ABORTS (BIT(IEEE802154_RX_ABORT_BY_ED_ABORT - 1) | BIT(IEEE802154_RX_ABORT_BY_ED_COEX_REJECT - 1))

static volatile uint32_t s_started_ms;    /* this attempt; 0 when no scan is outstanding */
static volatile uint32_t s_asked_ms;      /* when the stack asked for it */
static volatile uint32_t s_duration;
/* The scan is running in the radio, as opposed to outstanding: false from the
 * moment it was refused until the watch starts it again. radio_coex.c raises
 * the radio's bid for the antenna while it is true. */
static volatile bool     s_live;
static volatile bool     s_refused;       /* start again at the next tick */
static radio_ed_stats_t  s_stats;

/* Never 0, which means "none outstanding". Wraps after 49 days, harmlessly. */
static IRAM_ATTR uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000) | 1;
}

/* Clears the outstanding scan if it is still `expected`; true for the caller
 * that did. The done callback and the watch both end a scan, and only one of
 * them may tell the stack. */
static IRAM_ATTR bool take_scan(uint32_t expected)
{
    return __atomic_compare_exchange_n(&s_started_ms, &expected, 0, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static IRAM_ATTR void scan_ended(void)
{
    s_started_ms = 0;
    s_live       = false;
    ieee802154_ll_disable_rx_abort_events(ED_ABORTS);
}

esp_err_t __real_esp_ieee802154_energy_detect(uint32_t duration);
void      __real_esp_ieee802154_energy_detect_done(int8_t power);
esp_err_t __real_esp_ieee802154_receive(void);
esp_err_t __real_esp_ieee802154_transmit(const uint8_t *frame, bool cca);
esp_err_t __real_esp_ieee802154_sleep(void);
void      __real_esp_ieee802154_ed_failed(uint16_t error);

static IRAM_ATTR esp_err_t start_scan(uint32_t duration)
{
    s_refused    = false;
    s_live       = true;    /* before the driver sets its antenna bid for it */
    s_started_ms = now_ms();
    ieee802154_ll_enable_rx_abort_events(ED_ABORTS);
    return __real_esp_ieee802154_energy_detect(duration);
}

/* Called by the stack, from its task or from the radio ISR's done path. */
IRAM_ATTR esp_err_t __wrap_esp_ieee802154_energy_detect(uint32_t duration)
{
    s_stats.requested++;
    s_duration = duration;
    s_asked_ms = now_ms();
    return start_scan(duration);
}

/* Called by the driver from the radio ISR. A scan the watch has already given
 * up on has been answered; its late result is not passed on. */
IRAM_ATTR void __wrap_esp_ieee802154_energy_detect_done(int8_t power)
{
    const uint32_t started = s_started_ms;
    if (started == 0 || !take_scan(started)) {
        return;
    }
    s_stats.completed++;
    scan_ended();
    __real_esp_ieee802154_energy_detect_done(power);
}

/* Called by the driver from the radio ISR when the scan was refused the antenna,
 * or lost it mid-scan. The stack does not implement this, so the scan would
 * simply never finish; the watch starts it again at its next tick rather than
 * waiting for it to be overdue. */
IRAM_ATTR void __wrap_esp_ieee802154_ed_failed(uint16_t error)
{
    s_stats.refused++;
    if (ieee802154_ll_get_rx_abort_reason() == IEEE802154_RX_ABORT_BY_ED_COEX_REJECT) {
        s_stats.refused_coex++;
    }
    s_live    = false;
    s_refused = true;
    ieee802154_ll_disable_rx_abort_events(ED_ABORTS);
    __real_esp_ieee802154_ed_failed(error);
}

IRAM_ATTR bool radio_ed_live(void)
{
    return s_live;
}

/* Anything else the stack asks of the radio ends a scan in the driver (it stops
 * the current operation, silently), so the stack has moved on and the scan is
 * no longer outstanding. Without these the watch would start a scan nobody is
 * waiting for. */
IRAM_ATTR esp_err_t __wrap_esp_ieee802154_receive(void)
{
    scan_ended();
    return __real_esp_ieee802154_receive();
}

/* Counted too: beside the driver's verdicts in radio_coex.c, whether the stack
 * hands the driver frames at all tells a stack that has stopped sending from a
 * driver that has stopped finishing. Single core; a count off by one is fine. */
static volatile uint32_t s_tx_requests, s_tx_request_errors;

IRAM_ATTR esp_err_t __wrap_esp_ieee802154_transmit(const uint8_t *frame, bool cca)
{
    scan_ended();
    s_tx_requests++;
    const esp_err_t err = __real_esp_ieee802154_transmit(frame, cca);
    if (err != ESP_OK) {
        s_tx_request_errors++;
    }
    return err;
}

IRAM_ATTR esp_err_t __wrap_esp_ieee802154_sleep(void)
{
    scan_ended();
    return __real_esp_ieee802154_sleep();
}

/* Runs in the esp_timer task: no stack lock, nothing blocking. Telling the stack
 * a scan is done from here is safe: its callback only sets an event and writes
 * an eventfd, which its own task reads. */
static void watch_cb(void *arg)
{
    (void)arg;
    const uint32_t started = s_started_ms;
    if (started == 0) {
        return;
    }
    const uint32_t now = now_ms();
    if (now - s_asked_ms > ED_GIVE_UP_MS) {
        if (take_scan(started)) {
            s_stats.given_up++;
            scan_ended();
            __real_esp_ieee802154_energy_detect_done(ED_NO_READING);
        }
        return;
    }
    const bool refused = s_refused;
    if (!refused && now - started <= ED_OVERDUE_MS) {
        return;
    }
    s_stats.restarted++;
    (void)start_scan(s_duration);
    /* A refusal is Wi-Fi's ordinary business and comes often; a scan that went
     * silent is the fault this watch exists for. */
    if (!refused) {
        ESP_LOGW(TAG, "Energy scan never finished; started it again (%" PRIu32 " so far)", s_stats.restarted);
    }
}

void radio_ed_start(void)
{
    const esp_timer_create_args_t args = { .callback = watch_cb, .name = "radio_ed" };
    esp_timer_handle_t timer;
    if (esp_timer_create(&args, &timer) != ESP_OK || esp_timer_start_periodic(timer, ED_WATCH_US) != ESP_OK) {
        ESP_LOGE(TAG, "Could not start the energy scan watch");
    }
}

void radio_ed_get_stats(radio_ed_stats_t *out)
{
    *out                   = s_stats;
    out->tx_requests       = s_tx_requests;
    out->tx_request_errors = s_tx_request_errors;
}
