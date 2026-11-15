#include "led_strip_out.h"

#if CONFIG_ZBC_LED_STRIP

#include <stdlib.h>

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"

static const char *TAG = "led_strip";

/* >>> host-test region: ws-encode <<< */
/* A bit is a high time and a low time, and which is the longer says which bit
 * it is. In tenths of a microsecond, the channel's clock. Chosen inside both
 * datasheets at once:
 *
 *             WS2812B           SK6812            here
 *   0 high    0.40 +-0.15       0.30 +-0.15       0.3
 *   0 low     0.85 +-0.15       0.90 +-0.15       0.9
 *   1 high    0.80 +-0.15       0.60 +-0.15       0.7
 *   1 low     0.45 +-0.15       0.60 +-0.15       0.6
 *
 * A frame ends with the line low for longer than any strip waits before it
 * shows what it was sent: 50 us for the first WS2812B, 80 for an SK6812, 280
 * for later WS2812B. */
#define WS_CLOCK_HZ    10000000u
#define WS_T0H         3
#define WS_T0L         9
#define WS_T1H         7
#define WS_T1L         6
#define WS_RESET       3000              /* 300 us */
#define WS_BYTE_SYMS   8

/* The RMT driver's simple-encoder callback: `data` is the frame, and each call
 * writes the symbols of the next whole byte, or - after the last - the pause,
 * and says it is done. Called from the driver's interrupt as the channel's
 * memory empties, with however much room there is; fewer than a byte's worth
 * and it waits for more. */
static size_t ws_encode(const void *data, size_t data_size, size_t symbols_written, size_t symbols_free,
                        rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    (void)arg;
    if (symbols_free < WS_BYTE_SYMS) {
        return 0;
    }
    const size_t at = symbols_written / WS_BYTE_SYMS;
    if (at >= data_size) {
        symbols[0] = (rmt_symbol_word_t){ .duration0 = WS_RESET / 2, .level0 = 0,
                                          .duration1 = WS_RESET / 2, .level1 = 0 };
        *done = true;
        return 1;
    }
    const uint8_t byte = ((const uint8_t *)data)[at];
    for (int bit = 0; bit < 8; bit++) {
        const bool one = (byte >> (7 - bit)) & 1;
        symbols[bit] = (rmt_symbol_word_t){ .duration0 = one ? WS_T1H : WS_T0H, .level0 = 1,
                                            .duration1 = one ? WS_T1L : WS_T0L, .level1 = 0 };
    }
    return WS_BYTE_SYMS;
}
/* >>> end host-test region: ws-encode <<< */

struct led_strip_out {
    rmt_channel_handle_t chan;
    rmt_encoder_handle_t enc;
};

esp_err_t led_strip_out_new(int gpio, bool invert, int symbols, led_strip_out_t **out)
{
    led_strip_out_t *s = calloc(1, sizeof(*s));
    if (!s) {
        return ESP_ERR_NO_MEM;
    }
    const rmt_tx_channel_config_t cc = {
        .gpio_num          = gpio,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = WS_CLOCK_HZ,
        .mem_block_symbols = (size_t)symbols,
        .trans_queue_depth = 1,
        /* The highest a C handler may have: a refill served late is a frame cut short. */
        .intr_priority     = 3,
        .flags             = { .invert_out = invert },
    };
    esp_err_t err = rmt_new_tx_channel(&cc, &s->chan);
    if (err == ESP_OK) {
        const rmt_simple_encoder_config_t ec = { .callback = ws_encode, .min_chunk_size = WS_BYTE_SYMS };
        err = rmt_new_simple_encoder(&ec, &s->enc);
    }
    if (err == ESP_OK) {
        err = rmt_enable(s->chan);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GPIO %d: no RMT channel for a strip (%s)", gpio, esp_err_to_name(err));
        if (s->enc) {
            rmt_del_encoder(s->enc);
        }
        if (s->chan) {
            rmt_del_channel(s->chan);
        }
        free(s);
        return err;
    }
    *out = s;
    return ESP_OK;
}

void led_strip_out_del(led_strip_out_t *s)
{
    if (!s) {
        return;
    }
    rmt_tx_wait_all_done(s->chan, 50);
    rmt_disable(s->chan);
    rmt_del_encoder(s->enc);
    rmt_del_channel(s->chan);
    free(s);
}

bool led_strip_out_busy(led_strip_out_t *s)
{
    return rmt_tx_wait_all_done(s->chan, 0) != ESP_OK;
}

esp_err_t led_strip_out_send(led_strip_out_t *s, const uint8_t *data, size_t len)
{
    if (led_strip_out_busy(s)) {
        return ESP_ERR_INVALID_STATE;
    }
    const rmt_transmit_config_t tc = { .loop_count = 0, .flags = { .queue_nonblocking = 1 } };
    return rmt_transmit(s->chan, s->enc, data, len, &tc);
}

#endif
