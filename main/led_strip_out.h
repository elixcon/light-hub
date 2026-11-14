/* The wire of an addressable LED strip (WS2812B, SK6812 and their like): bytes
 * out of one pin as the pulses those LEDs count, by the RMT peripheral.
 *
 * Only the wire. What the bytes are - which colour each pixel has, in which
 * order its LEDs come - is local_lights.c's (region `strip`), and nothing here
 * knows a light from a pixel.
 *
 * The ESP32-C6 has two RMT transmit channels, so two strips. Its RMT has no
 * DMA: an interrupt refills the channel's memory as it empties, half of it at
 * a time, and a strip's bit is 1.2 us - so a channel with one 48-symbol block
 * wants its interrupt served within 30 us, with Wi-Fi and the 802.15.4 radio
 * wanting theirs. A strip on its own is therefore given all four blocks
 * (120 us), and of two the second gets three. An interrupt served late leaves
 * part of a frame unsent; the strip then shows wrong pixels until the next
 * frame, which is why local_lights.c sends a lit strip its frame again every
 * second.
 *
 * Built only with CONFIG_ZBC_LED_STRIP; without it nothing is ever made.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How many strips the chip drives, and the symbols of channel memory each is
 * given: `index` 0 or 1 among `count` strips. */
#define LED_STRIP_OUT_MAX 2
static inline int led_strip_out_symbols(int index, int count)
{
    return count <= 1 ? 192 : (index == 0 ? 48 : 144);
}

typedef struct led_strip_out led_strip_out_t;

#if CONFIG_ZBC_LED_STRIP

/* A strip's data line on `gpio`, idle low - or idle high and inverted with
 * `invert`, for a level shifter that inverts. ESP_ERR_NOT_FOUND when both
 * channels are taken. */
esp_err_t led_strip_out_new(int gpio, bool invert, int symbols, led_strip_out_t **out);
/* Waits for a frame in flight, then lets the pin and the channel go. */
void      led_strip_out_del(led_strip_out_t *s);
/* Whether a frame is still going out. */
bool      led_strip_out_busy(led_strip_out_t *s);
/* Sends `len` bytes, most significant bit first, and the pause that latches
 * them. Returns at once; `data` must stay as it is until led_strip_out_busy()
 * is false again. ESP_ERR_INVALID_STATE while busy. */
esp_err_t led_strip_out_send(led_strip_out_t *s, const uint8_t *data, size_t len);

#else

static inline esp_err_t led_strip_out_new(int gpio, bool invert, int symbols, led_strip_out_t **out)
{
    (void)gpio; (void)invert; (void)symbols; (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline void      led_strip_out_del(led_strip_out_t *s) { (void)s; }
static inline bool      led_strip_out_busy(led_strip_out_t *s) { (void)s; return false; }
static inline esp_err_t led_strip_out_send(led_strip_out_t *s, const uint8_t *data, size_t len)
{
    (void)s; (void)data; (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif

#ifdef __cplusplus
}
#endif
