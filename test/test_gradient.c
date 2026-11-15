/* Host test for main/gradient.c: a gradient, where a pixel falls on it, and
 * Philips' format for one on cluster 0xFC03.
 *
 * gradient.c is a leaf, so it is compiled whole. The frames are the ones
 * Bifrost (doc/hue-zigbee-format.md) and zigbee2mqtt (lib/philips.ts) document
 * from real lamps - the only word there is here on what a Hue gradient strip
 * takes, since none is on the desk. A byte out of place shows on the lamp as
 * nothing at all: it rejects a frame it cannot parse to the end.
 *
 *   make -C test
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "colorspace.h"
#include "gradient.h"

static int s_checks;
static void ok(bool cond, const char *what)
{
    s_checks++;
    if (!cond) { printf("FAIL: %s\n", what); exit(1); }
}

static bool nearf(float a, float b, float tol)
{
    return fabsf(a - b) <= tol;
}

static size_t unhex(const char *hex, uint8_t *out)
{
    size_t n = 0;
    for (; hex[0] && hex[1]; hex += 2) {
        unsigned v;
        sscanf(hex, "%2x", &v);
        out[n++] = (uint8_t)v;
    }
    return n;
}

static zbc_gradient_t three(void)
{
    /* red, green, blue as sRGB's primaries */
    zbc_gradient_t g = { .n = 3 };
    const float xy[3][2] = { { 0.64f, 0.33f }, { 0.30f, 0.60f }, { 0.15f, 0.06f } };
    for (int i = 0; i < 3; i++) {
        g.x[i] = (uint16_t)(xy[i][0] * 65536.0f);
        g.y[i] = (uint16_t)(xy[i][1] * 65536.0f);
    }
    return g;
}

static void test_blend(void)
{
    const zbc_gradient_t g = three();
    float x, y;
    zbc_gradient_at(&g, 0.0f, &x, &y);
    ok(nearf(x, 0.64f, 1e-4f) && nearf(y, 0.33f, 1e-4f), "the start is the first colour");
    zbc_gradient_at(&g, 0.5f, &x, &y);
    ok(nearf(x, 0.30f, 1e-4f) && nearf(y, 0.60f, 1e-4f), "the middle of three is the second colour");
    zbc_gradient_at(&g, 1.0f, &x, &y);
    ok(nearf(x, 0.15f, 1e-4f) && nearf(y, 0.06f, 1e-4f), "the end is the last colour");
    zbc_gradient_at(&g, 0.25f, &x, &y);
    ok(nearf(x, 0.47f, 1e-4f) && nearf(y, 0.465f, 1e-4f), "a quarter along is halfway between the first two");
    zbc_gradient_at(&g, 0.75f, &x, &y);
    ok(nearf(x, 0.225f, 1e-4f) && nearf(y, 0.33f, 1e-4f), "three quarters is halfway between the last two");
    zbc_gradient_at(&g, -1.0f, &x, &y);
    ok(nearf(x, 0.64f, 1e-4f), "before the start is the start");
    zbc_gradient_at(&g, 7.0f, &x, &y);
    ok(nearf(x, 0.15f, 1e-4f), "past the end is the end");

    zbc_gradient_t one = { .n = 1, .x = { 20000 }, .y = { 21000 } };
    for (float p = 0.0f; p <= 1.0f; p += 0.25f) {
        zbc_gradient_at(&one, p, &x, &y);
        ok(nearf(x, 20000 / 65536.0f, 1e-6f) && nearf(y, 21000 / 65536.0f, 1e-6f), "one colour is the whole strip");
    }

    /* Never outside the two colours it is between, anywhere, for any count. */
    for (int n = 2; n <= ZBC_GRADIENT_MAX; n++) {
        zbc_gradient_t m = { .n = (uint8_t)n };
        for (int i = 0; i < n; i++) {
            m.x[i] = (uint16_t)(8000 + 9000 * i);
            m.y[i] = (uint16_t)(50000 - 7000 * i);
        }
        float lx = 1.0f;
        for (int k = 0; k <= 200; k++) {
            zbc_gradient_at(&m, k / 200.0f, &x, &y);
            ok(x >= 8000 / 65536.0f - 1e-5f && x <= m.x[n - 1] / 65536.0f + 1e-5f, "a blend stays between its ends");
            ok(k == 0 || x >= lx - 1e-6f, "a blend of ascending colours ascends");
            lx = x;
        }
        for (int i = 0; i < n; i++) {
            zbc_gradient_at(&m, (float)i / (float)(n - 1), &x, &y);
            ok(nearf(x, m.x[i] / 65536.0f, 1e-5f) && nearf(y, m.y[i] / 65536.0f, 1e-5f), "each colour is shown where it sits");
        }
    }

    ok(zbc_gradient_pixel_pos(0, 30) == 0.0f && zbc_gradient_pixel_pos(29, 30) == 1.0f, "the first and last pixels are the ends");
    ok(zbc_gradient_pixel_pos(0, 1) == 0.5f, "a strip of one pixel shows the middle");

    zbc_gradient_t r = three();
    zbc_gradient_reverse(&r);
    ok(r.x[0] == g.x[2] && r.y[0] == g.y[2] && r.x[1] == g.x[1] && r.x[2] == g.x[0], "reversed is the other way round");

    zbc_gradient_t none = { 0 }, many = { .n = ZBC_GRADIENT_MAX + 1 };
    ok(!zbc_gradient_valid(&none) && !zbc_gradient_valid(&many) && !zbc_gradient_valid(NULL), "none and too many are not gradients");
    ok(zbc_gradient_near(&g, &g, 0), "a gradient is itself");
    zbc_gradient_t off = g;
    off.y[1] += 40;
    ok(zbc_gradient_near(&g, &off, 40) && !zbc_gradient_near(&g, &off, 39), "near is within the tolerance at every colour");
    off.n = 2;
    ok(!zbc_gradient_near(&g, &off, 65535), "another count is another gradient");
}

static void test_pack(void)
{
    uint8_t b[3];
    float   x, y;
    zbc_hue_pack_xy(ZBC_HUE_GAMUT_X, ZBC_HUE_GAMUT_Y, b);
    ok(b[0] == 0xFF && b[1] == 0xFF && b[2] == 0xFF, "the gamut's corner is all ones");
    zbc_hue_pack_xy(0.0f, 0.0f, b);
    ok(b[0] == 0 && b[1] == 0 && b[2] == 0, "the origin is zero");
    const uint8_t in[3] = { 0x94, 0x03, 0x1f };    /* Bifrost: X=0x394, Y=0x1F0 */
    zbc_hue_unpack_xy(in, &x, &y);
    ok(nearf(x, 0x394 * ZBC_HUE_GAMUT_X / 4095.0f, 1e-6f) && nearf(y, 0x1F0 * ZBC_HUE_GAMUT_Y / 4095.0f, 1e-6f),
       "three bytes unpack as Bifrost reads them");
    /* Every twelve-bit pair survives being read into 65536ths and sent again. */
    for (unsigned qx = 0; qx < 4096; qx += 7) {
        for (unsigned qy = 0; qy < 4096; qy += 113) {
            const uint8_t  p[3] = { (uint8_t)qx, (uint8_t)((qx >> 8) | ((qy & 0xF) << 4)), (uint8_t)(qy >> 4) };
            uint8_t        state[ZBC_HUE_STATE_LEN_MAX] = { 0x00, 0x01, 0x07, 0x10, 0x00, 0x00, 0x00, p[0], p[1], p[2] };
            zbc_hue_state_t st;
            ok(zbc_hue_state_decode(state, 10, &st), "a one-colour state parses");
            uint8_t out[ZBC_HUE_GRADIENT_LEN_MAX];
            ok(zbc_hue_gradient_encode(&st.gradient, 0, out, sizeof(out)) == 14, "and encodes");
            ok(memcmp(&out[9], p, 3) == 0, "a colour read from a lamp is sent back as the same bits");
        }
    }
}

static void test_frames(void)
{
    uint8_t         buf[64];
    zbc_hue_state_t st;

    /* Bifrost's worked example: on, brightness 192, fade 4, five colours, scale 5.0. */
    size_t n = unhex("530101c00400135000000094031fda1955b98347f00468c426792800", buf);
    ok(n == 28 && zbc_hue_state_decode(buf, n, &st), "Bifrost's example parses");
    ok(st.flags == 0x0153 && st.on && st.brightness == 192 && st.fade == 4, "its on, brightness and fade");
    ok(st.colors == 5 && st.gradient.n == 5 && st.style == ZBC_HUE_STYLE_LINEAR, "its five colours, linear");
    ok(st.scale == 0x28 && st.offset == 0, "its scale and offset");
    ok(st.gradient.x[0] == (uint16_t)(0x394 * ZBC_HUE_GAMUT_X / 4095.0f * 65536.0f + 0.5f), "its first x");
    ok(st.gradient.y[4] == (uint16_t)(0x792 * ZBC_HUE_GAMUT_Y / 4095.0f * 65536.0f + 0.5f), "its last y");

    /* The command for those five colours is that frame without on and brightness. */
    uint8_t out[ZBC_HUE_GRADIENT_LEN_MAX], want[32];
    const size_t len = zbc_hue_gradient_encode(&st.gradient, 4, out, sizeof(out));
    const size_t wn  = unhex("50010400135000000094031fda1955b98347f00468c426792800", want);
    ok(len == wn && memcmp(out, want, wn) == 0, "the command for Bifrost's colours is Bifrost's bytes");

    /* zigbee2mqtt's example of a reported state: on, 100, an xy, five colours. */
    n = unhex("4b010164fb74346b1350000000f3297fda7d55da7d55f3297fda7d552800", buf);
    ok(n == 30 && zbc_hue_state_decode(buf, n, &st), "zigbee2mqtt's example parses");
    ok(st.flags == 0x014B && st.on && st.brightness == 0x64 && st.x == 0x74fb && st.y == 0x6b34, "its on, brightness and xy");
    ok(st.colors == 5 && st.scale == 0x28, "its five colours");
    ok(st.gradient.x[0] == st.gradient.x[3] && st.gradient.x[1] == st.gradient.x[2] && st.gradient.x[1] == st.gradient.x[4],
       "its colours repeat as they do in the frame");

    /* The other states it lists: xy alone, colour temperature, brightness only. */
    n = unhex("0b00016400401040", buf);
    ok(zbc_hue_state_decode(buf, n, &st) && !(st.flags & ZBC_HUE_F_COLORS) && st.x == 0x4000, "a plain colour has no gradient");
    n = unhex("0f000164f40100401040", buf);
    ok(zbc_hue_state_decode(buf, n, &st) && st.mireds == 500, "a colour temperature");
    n = unhex("03000164", buf);
    ok(zbc_hue_state_decode(buf, n, &st) && st.brightness == 100, "brightness alone");

    /* What a lamp rejects. */
    n = unhex("530101c00400135000000094031fda1955b98347f00468c4267928", buf);
    ok(!zbc_hue_state_decode(buf, n, &st), "a frame cut short");
    n = unhex("530101c00400135000000094031fda1955b98347f00468c42679280000", buf);
    ok(!zbc_hue_state_decode(buf, n, &st), "a frame with a byte left over");
    n = unhex("5003040013500000", buf);
    ok(!zbc_hue_state_decode(buf, n, &st), "a flag nobody knows");
    n = unhex("0001" "22" "a0000000" "000000000000000000000000000000000000000000000000000000000000", buf);
    ok(!zbc_hue_state_decode(buf, n, &st), "ten colours");
    n = unhex("0001" "13" "51000000" "000000000000000000000000000000", buf);
    ok(!zbc_hue_state_decode(buf, n, &st), "a count with its low nibble set");
    n = unhex("0001" "10" "50000000" "000000000000000000000000", buf);
    ok(!zbc_hue_state_decode(buf, n, &st), "a size that disagrees with the count");
    ok(!zbc_hue_state_decode(buf, 1, &st) && !zbc_hue_state_decode(NULL, 4, &st), "nothing at all");

    /* Nine colours, the lamp's own limit, are read at five points. */
    uint8_t nine[2 + 5 + 27] = { 0x00, 0x01, 4 + 27, 0x90, 0, 0, 0 };
    for (int i = 0; i < 9; i++) {
        zbc_hue_pack_xy(0.1f + 0.05f * i, 0.2f, &nine[7 + 3 * i]);
    }
    ok(zbc_hue_state_decode(nine, sizeof(nine), &st) && st.colors == 9 && st.gradient.n == 5, "nine colours are kept as five");
    ok(nearf(st.gradient.x[0] / 65536.0f, 0.1f, 4e-4f) && nearf(st.gradient.x[2] / 65536.0f, 0.3f, 4e-4f) &&
       nearf(st.gradient.x[4] / 65536.0f, 0.5f, 4e-4f), "the ends and the middle of the nine");

    /* Every count the editor offers: encoded, and read back the same. */
    for (int c = 1; c <= ZBC_GRADIENT_MAX; c++) {
        zbc_gradient_t g = { .n = (uint8_t)c };
        for (int i = 0; i < c; i++) {
            g.x[i] = (uint16_t)(9000 + 7000 * i);
            g.y[i] = (uint16_t)(30000 - 4000 * i);
        }
        const size_t l = zbc_hue_gradient_encode(&g, 7, out, sizeof(out));
        ok(l == (size_t)(11 + 3 * c), "a command's length");
        ok(out[0] == 0x50 && out[1] == 0x01 && out[2] == 7 && out[3] == 0, "its flags and fade");
        ok(out[4] == 4 + 3 * c && out[5] == (c << 4) && out[l - 2] == (c << 3) && out[l - 1] == 0, "its size, count, scale and offset");
        ok(zbc_hue_state_decode(out, l, &st) && st.fade == 7, "a lamp parses it");
        ok(zbc_gradient_near(&g, &st.gradient, 16), "to the colours that were sent, within twelve bits");
        ok(zbc_hue_gradient_encode(&g, 7, out, l - 1) == 0, "a buffer too small is refused");
    }
    zbc_gradient_t none = { 0 };
    ok(zbc_hue_gradient_encode(&none, 4, out, sizeof(out)) == 0, "no gradient is no command");

    /* A state as a Light Node builds it. */
    zbc_hue_state_t mine = { .flags = ZBC_HUE_F_ON | ZBC_HUE_F_BRIGHTNESS | ZBC_HUE_F_COLORS, .on = true, .brightness = 77,
                             .gradient = three() };
    uint8_t sb[ZBC_HUE_STATE_LEN_MAX];
    size_t  sl = zbc_hue_state_encode(&mine, sb, sizeof(sb));
    ok(sl == 2 + 2 + 5 + 9 + 2 && zbc_hue_state_decode(sb, sl, &st), "a state with a gradient encodes and parses");
    ok(st.on && st.brightness == 77 && (st.flags & ZBC_HUE_F_COLORS) && zbc_gradient_near(&mine.gradient, &st.gradient, 16),
       "and says what was put in");
    mine.flags = ZBC_HUE_F_ON | ZBC_HUE_F_BRIGHTNESS | ZBC_HUE_F_XY;
    mine.x = 12345, mine.y = 23456;
    sl = zbc_hue_state_encode(&mine, sb, sizeof(sb));
    ok(sl == 8 && zbc_hue_state_decode(sb, sl, &st) && !(st.flags & ZBC_HUE_F_COLORS) && st.x == 12345 && st.y == 23456,
       "a state with a plain colour has no gradient");
    ok(zbc_hue_state_encode(&mine, sb, 7) == 0, "a state buffer too small is refused");
}

static void test_rgb(void)
{
    float rgb[3];
    ok(zbc_xy_to_rgb(0.64f, 0.33f, rgb) && rgb[0] == 1.0f && rgb[1] < 0.01f && rgb[2] < 0.01f, "sRGB's red is red");
    ok(zbc_xy_to_rgb(0.30f, 0.60f, rgb) && rgb[1] == 1.0f && rgb[0] < 0.01f && rgb[2] < 0.01f, "its green is green");
    ok(zbc_xy_to_rgb(0.15f, 0.06f, rgb) && rgb[2] == 1.0f && rgb[0] < 0.01f && rgb[1] < 0.01f, "its blue is blue");
    ok(zbc_xy_to_rgb(0.3127f, 0.3290f, rgb) && rgb[0] > 0.98f && rgb[1] > 0.98f && rgb[2] > 0.98f, "D65 is white");
    ok(!zbc_xy_to_rgb(0.3f, 0.0f, rgb) && rgb[0] == 0.0f, "no y is no colour");
    float x, y;
    zbc_kelvin_to_xy(2700, &x, &y);
    ok(zbc_xy_to_rgb(x, y, rgb) && rgb[0] == 1.0f && rgb[1] < rgb[0] && rgb[2] < rgb[1], "2700 K is red over green over blue");
    /* The hue/saturation path is this one plus the gamma step: it still round-trips. */
    for (int h = 0; h < 360; h += 30) {
        float hh, ss;
        zbc_hs_to_xy(h, 80, &x, &y);
        zbc_xy_to_hs(x, y, &hh, &ss);
        float dh = fabsf(hh - (float)h);
        if (dh > 180.0f) { dh = 360.0f - dh; }
        ok(dh <= 1.0f && fabsf(ss - 80.0f) <= 1.0f, "hue and saturation round-trip through xy");
    }
}

int main(void)
{
    test_blend();
    test_pack();
    test_frames();
    test_rgb();
    printf("ok (%d checks)\n", s_checks);
    return 0;
}
