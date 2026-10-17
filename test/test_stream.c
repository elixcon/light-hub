/* What a stream pixel becomes, and when a stream's packets are taken, lifted
 * from main/lighting.c (region `stream`).
 *
 * The colour vectors are the wide gamut's own corners and white point: what
 * Bifrost's XY::from_rgb gives for the same input, and Bifrost's frames are
 * known to look right on Hue lamps. A fault in the take/override decision shows
 * on a sofa as "the lights fought the remote" or "the TV never got them back",
 * with nothing in a log to say which. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stream.inc"

static int failures, checks;

static void check(bool ok, const char *what)
{
    checks++;
    if (!ok) {
        printf("\n  %s", what);
        failures++;
    }
}

static void expect_light(uint8_t r, uint8_t g, uint8_t b, uint8_t master, unsigned want_bri, float want_x,
                         float want_y, const char *what)
{
    uint16_t bri = 0;
    float    x = -1, y = -1;
    stream_rgb_to_light(r, g, b, master, &bri, &x, &y);
    checks++;
    if (bri != want_bri || fabsf(x - want_x) > 0.0005f || fabsf(y - want_y) > 0.0005f) {
        printf("\n  %s: (%u,%u,%u)@%u -> bri %u x %.4f y %.4f, expected %u %.4f %.4f", what, r, g, b, master,
               bri, x, y, want_bri, want_x, want_y);
        failures++;
    }
}

int main(void)
{
    /* The gamut's corners: full brightness each, whatever their luminance -
     * a full blue is a full blue. */
    expect_light(255, 0, 0, 255, 2047, 0.7347f, 0.2653f, "red");
    expect_light(0, 255, 0, 255, 2047, 0.1152f, 0.8264f, "green");
    expect_light(0, 0, 255, 255, 2047, 0.1566f, 0.0177f, "blue");
    /* White is the gamut's white point, D50 for these primaries. */
    expect_light(255, 255, 255, 255, 2047, 0.3457f, 0.3585f, "white");
    /* Brightness is the largest channel: half grey is half, the colour white. */
    expect_light(128, 128, 128, 255, (128u * 255 * 2047 + 255 * 255 / 2) / (255 * 255), 0.3457f, 0.3585f,
                 "half grey");
    /* ...and so is a dim red, whose colour does not move. */
    expect_light(64, 0, 0, 255, (64u * 255 * 2047 + 255 * 255 / 2) / (255 * 255), 0.7347f, 0.2653f, "dim red");
    /* WLED's `bri` scales it. */
    expect_light(255, 0, 0, 128, (255u * 128 * 2047 + 255 * 255 / 2) / (255 * 255), 0.7347f, 0.2653f,
                 "red at master 128");

    /* Black: dark, and the colour left where it was. */
    {
        uint16_t bri = 99;
        float    x = 0.5f, y = 0.4f;
        stream_rgb_to_light(0, 0, 0, 255, &bri, &x, &y);
        check(bri == 0, "black is sent dark");
        check(x == 0.5f && y == 0.4f, "black keeps the colour it had");
        bri = 99;
        stream_rgb_to_light(255, 255, 255, 0, &bri, &x, &y);
        check(bri == 0, "a master brightness of 0 is dark");
        stream_rgb_to_light(1, 0, 0, 255, &bri, &x, &y);
        check(bri == 8, "the dimmest red is not rounded away");
    }

    /* Timeouts: the source's own, WLED's default when it gave none, never past
     * the cap. */
    check(stream_timeout_us(0) == STREAM_DEFAULT_US, "no timeout given is the default");
    check(stream_timeout_us(-1) == STREAM_DEFAULT_US, "a negative timeout is the default");
    check(stream_timeout_us(1000000) == 1000000, "a timeout of 1 s stands");
    check(stream_timeout_us(INT64_MAX) == STREAM_MAX_SILENCE_US, "no timeout at all is capped");

    /* No hand: every packet is taken. */
    {
        bool    ovr  = false;
        int64_t last = 0;
        check(stream_take(&ovr, &last, 1000, 2500000), "a packet with no override is taken");
        check(stream_take(&ovr, &last, 2000, 2500000), "and the next");
        check(last == 2000, "the last packet is remembered");
    }

    /* A hand at 0 s: the source keeps sending every 40 ms and is ignored, pauses
     * for 2 s (under its 2.5 s timeout) and is still ignored, pauses for 3 s and
     * the next packet is a new stream. */
    {
        bool    ovr  = true;
        int64_t last = 0;
        bool    any  = false;
        for (int64_t t = 40000; t <= 5000000; t += 40000) {
            any |= stream_take(&ovr, &last, t, 2500000);
        }
        check(!any, "a source that keeps playing after a hand is ignored");
        check(ovr, "the override holds while it plays");
        check(!stream_take(&ovr, &last, 7000000, 2500000), "a 2 s pause is not the end of it");
        check(stream_take(&ovr, &last, 10000000, 2500000), "a 3 s pause is");
        check(!ovr, "and lifts the override");
        check(stream_take(&ovr, &last, 10040000, 2500000), "the stream after it is taken");
    }
    /* The pause is measured from the last packet, not from the hand. */
    {
        bool    ovr  = true;
        int64_t last = 0;
        check(!stream_take(&ovr, &last, 2000000, 2500000), "a packet 2 s after the hand is dropped");
        check(!stream_take(&ovr, &last, 4000000, 2500000), "2 s after that one too");
        check(stream_take(&ovr, &last, 6600000, 2500000), "2.6 s after it is a new stream");
    }

    /* Hue: Signify and Philips, nothing else. */
    check(stream_is_hue("Signify Netherlands B.V."), "Signify is a Hue");
    check(stream_is_hue("Philips"), "Philips is a Hue");
    check(!stream_is_hue("OSRAM"), "OSRAM is not");
    check(!stream_is_hue("_TZ3000_ksw8qtmt"), "a Tuya lamp is not");
    check(!stream_is_hue(""), "no manufacturer is not");
    check(!stream_is_hue("Signif"), "half a name is not");

    if (failures) {
        printf("\n%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("all %d checks passed\n", checks);
    return 0;
}
