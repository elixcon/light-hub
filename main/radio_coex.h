/* What the 802.15.4 radio loses to Wi-Fi, beside what it sends and hears - and
 * the one thing done about it: our sends bid for the antenna above listening.
 *
 * Wi-Fi and 802.15.4 take turns at one antenna, and Wi-Fi wins at the default
 * priorities. The radio counts in hardware each time Wi-Fi took the antenna in
 * the middle of something: receiving (a frame on its way to us, or plain
 * listening - the counter does not tell them apart), waiting for the ack to a
 * frame we sent, or acking a frame we received. Nothing in ESP-IDF reads those
 * counters unless CONFIG_IEEE802154_TXRX_STATISTIC is on, which also clears
 * them, so this reads them itself: 16 bits wide and stopping at the top, so
 * taken and cleared into 32-bit totals every few seconds.
 *
 * Beside them, the driver's own verdict on every frame we sent and a count of
 * the frames it received (linker --wrap on its callbacks into the stack, as
 * radio_ed.c does). An ack Wi-Fi cut off reaches the stack only as "invalid
 * ack", among genuinely garbled ones - rx_ack_coex is what tells them apart.
 *
 * Totals since boot: sample twice and divide, like the rest of zigbee.radio.
 *
 * The send priority. The driver bids at one level, `txrx` (low), both while
 * sending and while listening; a coordinator listens all the time, so raising
 * that level starves Wi-Fi (median request ~95 ms -> 1.4 s). But what Wi-Fi
 * takes is our sends: tx_coex was 36-39 % of them around commands arriving over
 * the web, 14 % around remote presses, and acks and listening lost nothing
 * measurable. So the send phase alone - from the start of a frame, through its
 * CCA, to its ack or its failure, a few milliseconds - bids middle, which is
 * what the driver already gives our acks. Measured 2026-09-30 against low in
 * the same boot: tx_coex 3-6 %, sends needed down ~40 %, APS retries (cmd_slow)
 * 1 of 36 commands against 4-10, Wi-Fi request times unchanged.
 *
 * The one other raise: an energy scan bids high for as long as it runs. It sets
 * the listening phase too, and an associated Wi-Fi refuses it the antenna at
 * listening and at middle alike (radio_ed.h). Scans are the stack's alone -
 * forming, and a channel survey somebody asked for, 77-138 ms a channel - and
 * cost Wi-Fi a second at a time: a ping across a survey lost 5-9 % and peaked
 * at 1.4 s, against nothing lost idle.
 *
 * A leaf: the 802.15.4 driver and HAL, and esp_timer. */
#pragma once

#include <stdint.h>

typedef struct {
    /* Every frame we sent, by the driver's verdict. A MAC retry is a frame of
     * its own; the stack sends one after any failure below. */
    uint32_t tx_done;      /* sent, and acked where an ack was asked for */
    uint32_t tx_no_ack;    /* no ack in time */
    uint32_t tx_bad_ack;   /* an ack cut short or garbled - Wi-Fi among the causes */
    uint32_t tx_coex;      /* cut off by Wi-Fi while sending */
    uint32_t tx_cca_busy;  /* the channel was busy, so not sent */
    uint32_t tx_other;     /* aborted, or a security error */
    uint32_t rx;           /* frames received and handed to the stack */
    /* Hardware: Wi-Fi took the antenna while the radio was... */
    uint32_t rx_coex;      /* receiving, or listening */
    uint32_t rx_ack_coex;  /* receiving the ack to a frame we sent */
    uint32_t tx_ack_coex;  /* sending the ack to a frame it received - the sender repeats it */
} radio_coex_stats_t;

/* Once the stack has the radio up; the counters are read only in between, as
 * the radio's registers are not clocked before and after. */
void radio_coex_start(void);
void radio_coex_stop(void);

void radio_coex_get_stats(radio_coex_stats_t *out);
