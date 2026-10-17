/* Energy scans the 802.15.4 driver never finishes.
 *
 * Forming a network starts with an energy scan, and so does a channel survey
 * (zb_channel_survey_start). On this chip, with Wi-Fi running, the driver
 * starts one and then reports nothing at all, and esp-zigbee-lib waits for it
 * indefinitely: formation stays EZB_BDB_STATUS_IN_PROGRESS, a new start is
 * refused as busy, ezb_bdb_cancel_formation() is not supported - and on a
 * running network the radio stays in the scan, deaf and mute on its own channel
 * until a restart (2026-10-05: `rx` and `tx_done` frozen).
 *
 * Why (2026-10-05): the coexistence arbiter refuses the scan the antenna
 * (IEEE802154_RX_ABORT_BY_ED_COEX_REJECT) whenever Wi-Fi is associated, and the
 * driver enables neither that abort event nor ED_ABORT, so the scan ends in the
 * hardware without an interrupt. At the listening priority every scan of a
 * survey was refused, and at middle still every one (328 of 328); at high none
 * (128 of 128). radio_coex.c bids high while a scan runs, which radio_ed_live()
 * says.
 *
 * So the scan the stack asked for is watched between the two (linker --wrap,
 * main/CMakeLists.txt): the two abort events are enabled while it runs, so a
 * refusal arrives as esp_ieee802154_ed_failed() (which the stack does not
 * implement) and is started again at the next tick; one outstanding for well
 * past its duration is started again too; and one that has still not finished
 * 1.5 s after the stack asked is reported done with INT8_MIN, nothing measured,
 * so the stack moves on whatever the radio does. A scan the stack abandons (by
 * asking the radio for anything else) is not watched.
 *
 * A leaf: esp_ieee802154, its HAL and esp_timer only. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t requested;   /* scans the stack started */
    uint32_t completed;   /* scans the driver reported done */
    uint32_t restarted;   /* scans started again by the watch */
    uint32_t refused;     /* scans refused the antenna, or that lost it */
    uint32_t refused_coex;/* ...of them refused by the coexistence arbiter */
    uint32_t given_up;    /* scans reported to the stack as done with no reading */
    uint32_t tx_requests; /* frames the stack handed the driver to send */
    uint32_t tx_request_errors; /* ...that the driver would not start */
} radio_ed_stats_t;

/* Starts the watch. Call once, before the Zigbee stack starts. */
void radio_ed_start(void);

void radio_ed_get_stats(radio_ed_stats_t *out);

/* True while a scan is running in the radio. From the radio ISR as well:
 * radio_coex.c asks it when the driver sets its bid for the antenna. */
bool radio_ed_live(void);
