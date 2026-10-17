/* The two ways pixels reach the WLED interface - DDP and WLED's own UDP
 * protocols - lifted from main/wled.c (region `wled-parse`).
 *
 * Packets are built here byte by byte from the protocols' descriptions, not
 * captured: a mistake here sends a TV's picture to the wrong lamp, or nowhere,
 * and the clients that send them give no sign that anything is wrong. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wled-parse.inc"

static int failures, checks;

static void check(bool ok, const char *what)
{
    checks++;
    if (!ok) {
        printf("\n  %s", what);
        failures++;
    }
}

static void expect_px(const uint8_t *px, int i, uint8_t r, uint8_t g, uint8_t b, const char *what)
{
    checks++;
    if (px[i * 3] != r || px[i * 3 + 1] != g || px[i * 3 + 2] != b) {
        printf("\n  %s: pixel %d is (%u,%u,%u), expected (%u,%u,%u)", what, i, px[i * 3], px[i * 3 + 1],
               px[i * 3 + 2], r, g, b);
        failures++;
    }
}

int main(void)
{
    wled_packet_t pk;
    uint8_t       px[10 * 3];

    /* DDP, version 1 with push, RGB 8 bits, default output, offset 0, 6 bytes. */
    {
        const uint8_t p[] = { 0x41, 0x05, 0x0B, 0x01, 0, 0, 0, 0, 0, 6, 10, 20, 30, 40, 50, 60 };
        check(ddp_parse(p, sizeof(p), &pk), "DDP frame is taken");
        check(pk.start == 0 && pk.count == 2 && pk.stride == 3 && !pk.rgbw && !pk.indexed,
              "DDP frame: two RGB pixels from 0");
        check(pk.timeout_ms == 0, "DDP gives no timeout");
        memset(px, 0, sizeof(px));
        wled_apply(&pk, px, 10);
        expect_px(px, 0, 10, 20, 30, "DDP");
        expect_px(px, 1, 40, 50, 60, "DDP");
        expect_px(px, 2, 0, 0, 0, "DDP leaves the rest");
    }
    /* With a timecode the header is 14 bytes. */
    {
        const uint8_t p[] = { 0x51, 0x01, 0x0B, 0x01, 0, 0, 0, 0, 0, 3, 0xAA, 0xBB, 0xCC, 0xDD, 1, 2, 3 };
        check(ddp_parse(p, sizeof(p), &pk), "DDP with a timecode is taken");
        memset(px, 0, sizeof(px));
        wled_apply(&pk, px, 10);
        expect_px(px, 0, 1, 2, 3, "DDP after a timecode");
    }
    /* An offset of 6 bytes is pixel 2. */
    {
        const uint8_t p[] = { 0x41, 0x01, 0x0B, 0x01, 0, 0, 0, 6, 0, 3, 7, 8, 9 };
        check(ddp_parse(p, sizeof(p), &pk) && pk.start == 2 && pk.count == 1, "DDP offset 6 is pixel 2");
        memset(px, 0, sizeof(px));
        wled_apply(&pk, px, 10);
        expect_px(px, 2, 7, 8, 9, "DDP at an offset");
        expect_px(px, 0, 0, 0, 0, "DDP at an offset leaves pixel 0");
    }
    /* RGBW (data type 0x1B): the white adds to the colour, saturating. */
    {
        const uint8_t p[] = { 0x41, 0x01, 0x1B, 0x01, 0, 0, 0, 0, 0, 8, 10, 20, 30, 5, 250, 0, 0, 10 };
        check(ddp_parse(p, sizeof(p), &pk) && pk.rgbw && pk.stride == 4 && pk.count == 2, "DDP RGBW");
        memset(px, 0, sizeof(px));
        wled_apply(&pk, px, 10);
        expect_px(px, 0, 15, 25, 35, "DDP RGBW adds white");
        expect_px(px, 1, 255, 10, 10, "DDP RGBW saturates");
    }
    /* The length field limits what is read; a short packet reads what is there. */
    {
        const uint8_t p[] = { 0x41, 0x01, 0x0B, 0x01, 0, 0, 0, 0, 0, 3, 1, 2, 3, 4, 5, 6 };
        check(ddp_parse(p, sizeof(p), &pk) && pk.count == 1, "DDP reads only its length");
        const uint8_t q[] = { 0x41, 0x01, 0x0B, 0x01, 0, 0, 0, 0, 0, 30, 1, 2, 3, 4, 5, 6 };
        check(ddp_parse(q, sizeof(q), &pk) && pk.count == 2, "DDP reads only what arrived");
    }
    /* Not pixels. */
    {
        const uint8_t query[]  = { 0x43, 0x01, 0x0B, 0x01, 0, 0, 0, 0, 0, 3, 1, 2, 3 };
        const uint8_t reply[]  = { 0x45, 0x01, 0x0B, 0x01, 0, 0, 0, 0, 0, 3, 1, 2, 3 };
        const uint8_t v0[]     = { 0x01, 0x01, 0x0B, 0x01, 0, 0, 0, 0, 0, 3, 1, 2, 3 };
        const uint8_t status[] = { 0x41, 0x01, 0x0B, 251, 0, 0, 0, 0, 0, 3, 1, 2, 3 };
        const uint8_t all[]    = { 0x41, 0x01, 0x0B, 255, 0, 0, 0, 0, 0, 3, 1, 2, 3 };
        const uint8_t odd[]    = { 0x41, 0x01, 0x0B, 0x01, 0, 0, 0, 4, 0, 3, 1, 2, 3 };
        const uint8_t empty[]  = { 0x41, 0x01, 0x0B, 0x01, 0, 0, 0, 0, 0, 0 };
        const uint8_t shorty[] = { 0x41, 0x01, 0x0B, 0x01, 0, 0, 0, 0, 0 };
        check(!ddp_parse(query, sizeof(query), &pk), "a DDP query is not pixels");
        check(!ddp_parse(reply, sizeof(reply), &pk), "a DDP reply is not pixels");
        check(!ddp_parse(v0, sizeof(v0), &pk), "another DDP version is not");
        check(!ddp_parse(status, sizeof(status), &pk), "the status destination is not");
        check(ddp_parse(all, sizeof(all), &pk), "destination 255 (all) is");
        check(!ddp_parse(odd, sizeof(odd), &pk), "an offset between pixels is not");
        check(!ddp_parse(empty, sizeof(empty), &pk), "no data is not");
        check(!ddp_parse(shorty, sizeof(shorty), &pk), "a short header is not");
    }

    /* WARLS: (index, r, g, b) tuples, timeout 2 s; an index past the strip is
     * passed over, the rest still taken. */
    {
        const uint8_t p[] = { 1, 2, 0, 10, 20, 30, 12, 9, 9, 9, 5, 1, 2, 3 };
        check(wled_udp_parse(p, sizeof(p), &pk) && pk.indexed && pk.count == 3, "WARLS");
        check(pk.timeout_ms == 2000, "WARLS timeout 2 s");
        memset(px, 0, sizeof(px));
        wled_apply(&pk, px, 10);
        expect_px(px, 0, 10, 20, 30, "WARLS index 0");
        expect_px(px, 5, 1, 2, 3, "WARLS index 5 after one past the strip");
    }
    /* DRGB, timeout 255 = none. */
    {
        const uint8_t p[] = { 2, 255, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
        check(wled_udp_parse(p, sizeof(p), &pk) && pk.count == 3 && pk.start == 0, "DRGB");
        check(pk.timeout_ms == WLED_NO_TIMEOUT, "255 is no timeout");
        memset(px, 0, sizeof(px));
        wled_apply(&pk, px, 10);
        expect_px(px, 2, 7, 8, 9, "DRGB third pixel");
    }
    /* DRGBW. */
    {
        const uint8_t p[] = { 3, 1, 10, 20, 30, 40 };
        check(wled_udp_parse(p, sizeof(p), &pk) && pk.rgbw && pk.count == 1, "DRGBW");
        check(pk.timeout_ms == 1000, "DRGBW timeout 1 s");
        memset(px, 0, sizeof(px));
        wled_apply(&pk, px, 10);
        expect_px(px, 0, 50, 60, 70, "DRGBW adds white");
    }
    /* DNRGB: a big-endian start index. */
    {
        const uint8_t p[] = { 4, 5, 0x00, 0x02, 1, 2, 3, 4, 5, 6 };
        check(wled_udp_parse(p, sizeof(p), &pk) && pk.start == 2 && pk.count == 2, "DNRGB from 2");
        memset(px, 0, sizeof(px));
        wled_apply(&pk, px, 10);
        expect_px(px, 2, 1, 2, 3, "DNRGB pixel 2");
        expect_px(px, 3, 4, 5, 6, "DNRGB pixel 3");
        const uint8_t far[] = { 4, 5, 0x01, 0x00, 1, 2, 3 };
        check(wled_udp_parse(far, sizeof(far), &pk) && pk.start == 256, "DNRGB start is big-endian");
        memset(px, 0, sizeof(px));
        wled_apply(&pk, px, 10);
        uint8_t none[30] = { 0 };
        check(memcmp(px, none, sizeof(px)) == 0, "pixels past the strip change nothing");
    }
    /* Not pixels: WLED's own sync, protocols past 4, and short packets. */
    {
        const uint8_t sync[] = { 0, 2, 1, 2, 3 };
        const uint8_t five[] = { 5, 2, 1, 2, 3 };
        const uint8_t one[]  = { 2 };
        const uint8_t dn[]   = { 4, 2, 0 };
        const uint8_t bare[] = { 2, 2 };
        check(!wled_udp_parse(sync, sizeof(sync), &pk), "WLED sync is not pixels");
        check(!wled_udp_parse(five, sizeof(five), &pk), "protocol 5 is not");
        check(!wled_udp_parse(one, sizeof(one), &pk), "a lone byte is not");
        check(!wled_udp_parse(dn, sizeof(dn), &pk), "a DNRGB without its start is not");
        check(!wled_udp_parse(bare, sizeof(bare), &pk), "a header with no pixel is not");
    }

    /* A strip longer than the room: only the room's pixels are kept. */
    {
        uint8_t p[2 + 12 * 3];
        p[0] = 2;
        p[1] = 2;
        for (int i = 0; i < 12 * 3; i++) {
            p[2 + i] = (uint8_t)(i + 1);
        }
        check(wled_udp_parse(p, sizeof(p), &pk) && pk.count == 12, "twelve pixels arrive");
        uint8_t small[4 * 3 + 3];
        memset(small, 0xEE, sizeof(small));
        wled_apply(&pk, small, 4);
        check(small[4 * 3] == 0xEE, "nothing is written past the room's pixels");
        expect_px(small, 3, 10, 11, 12, "the room's last pixel");
    }

    if (failures) {
        printf("\n%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("all %d checks passed\n", checks);
    return 0;
}
