/* A strip of addressable LEDs that is not on the desk, simulated, on the end of
 * what main/local_lights.c renders for one (region `strip`) and the pulses
 * main/led_strip_out.c makes of it (region `ws-encode`).
 *
 * Lifted rather than copied: see test/Makefile. The strip here is the
 * datasheets': a chain of pixels that each time a pulse by its high and its low
 * part, take the first 24 bits (32 for RGBW) they are sent and pass the rest
 * on, and show what they took once the line has been low for long enough. A
 * pulse outside the datasheet's window is counted, not guessed at. The RMT
 * driver is a loop that calls the encoder with as much room as an interrupt
 * would find.
 *
 * What it can show is whether the frame is the right bytes and the pulses are
 * the right lengths. What it cannot is an interrupt served late on the real
 * chip - see led_strip_out.h.
 *
 *   make -C test
 */
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "colorspace.h"
#include "gradient.h"
#include "light_math.h"
#include "pchip.inc"
#include "cwww.inc"

/* The RMT symbol as the chip's memory holds it (hal/rmt_types.h). */
typedef union {
    struct {
        uint16_t duration0 : 15;
        uint16_t level0 : 1;
        uint16_t duration1 : 15;
        uint16_t level1 : 1;
    };
    uint32_t val;
} rmt_symbol_word_t;

#include "ws-encode.inc"
#include "strip.inc"

static int  failures;
static long checks;

#define EXPECT(cond, ...) do {                     \
        checks++;                                  \
        if (!(cond)) {                             \
            if (failures < 20) {                   \
                printf("\n  ");                    \
                printf(__VA_ARGS__);               \
            }                                      \
            failures++;                            \
        }                                          \
    } while (0)

/* ----------------------------------------------------------- the strip */

/* Datasheet windows, in nanoseconds. */
typedef struct {
    const char *name;
    int t0h_min, t0h_max, t0l_min, t0l_max;
    int t1h_min, t1h_max, t1l_min, t1l_max;
    int reset_min;
} chip_t;

static const chip_t WS2812B = { "WS2812B", 250, 550, 700, 1000, 650, 950, 300, 600, 280000 };
static const chip_t SK6812  = { "SK6812", 150, 450, 750, 1050, 450, 750, 450, 750, 80000 };

#define SIM_PIXELS 300

typedef struct {
    const chip_t *chip;
    int           bpp, pixels;
    uint8_t       shift[SIM_PIXELS * 4];   /* bits taken since the last latch */
    long          bits;
    uint8_t       shown[SIM_PIXELS * 4];
    int           latches;
    int           bad_pulses;              /* outside the datasheet */
    int           passed_on;               /* bits past the last pixel */
    /* the pulse being timed */
    int           high_ns;
    bool          have_high;
    int64_t       total_ns;
} sim_t;

static void sim_init(sim_t *s, const chip_t *chip, int pixels, int bpp)
{
    memset(s, 0, sizeof(*s));
    s->chip   = chip;
    s->pixels = pixels;
    s->bpp    = bpp;
}

static void sim_bit(sim_t *s, int high_ns, int low_ns, bool last_before_reset)
{
    const chip_t *c = s->chip;
    const bool zero = high_ns >= c->t0h_min && high_ns <= c->t0h_max;
    const bool one  = high_ns >= c->t1h_min && high_ns <= c->t1h_max;
    bool       bit;
    /* The two windows overlap for a WS2812B; the low part settles it, as the
     * longer half of the pulse does on the chip. */
    if (zero && one) {
        bit = high_ns > low_ns;
    } else if (zero || one) {
        bit = one;
    } else {
        s->bad_pulses++;
        return;
    }
    /* The low part of the last bit runs into the pause, so it has no upper bound. */
    const int lmin = bit ? c->t1l_min : c->t0l_min, lmax = bit ? c->t1l_max : c->t0l_max;
    if (low_ns < lmin || (!last_before_reset && low_ns > lmax)) {
        s->bad_pulses++;
    }
    if (s->bits < (long)s->pixels * s->bpp * 8) {
        s->shift[s->bits / 8] = (uint8_t)((s->shift[s->bits / 8] << 1) | (bit ? 1 : 0));
    } else {
        s->passed_on++;
    }
    s->bits++;
}

/* The line at `level` for `ticks` tenths of a microsecond. */
static void sim_line(sim_t *s, int level, int ticks)
{
    const int ns = ticks * 100;
    s->total_ns += ns;
    if (level) {
        if (s->have_high) {
            s->bad_pulses++;               /* two highs with no low between */
        }
        s->high_ns   = ns;
        s->have_high = true;
        return;
    }
    const bool reset = ns >= s->chip->reset_min;
    if (s->have_high) {
        sim_bit(s, s->high_ns, ns, reset);
        s->have_high = false;
    }
    if (reset) {
        memcpy(s->shown, s->shift, sizeof(s->shown));
        memset(s->shift, 0, sizeof(s->shift));
        s->bits = 0;
        s->latches++;
    }
}

/* ---------------------------------------------------------- the driver */

/* The RMT driver's side of the simple encoder: it calls back with the room an
 * interrupt finds, here `chunk[i % n]` symbols at a time, until the encoder
 * says it is done. A symbol's two halves at one level are one stretch of line. */
static long drive(sim_t *s, const uint8_t *frame, size_t len, const int *chunk, int n)
{
    static rmt_symbol_word_t mem[256];
    size_t written = 0;
    bool   done    = false;
    long   calls   = 0;
    int    pending_level = -1, pending_ticks = 0;
    for (int i = 0; !done && calls < 200000; i++, calls++) {
        const size_t room = (size_t)chunk[i % n];
        const size_t got  = ws_encode(frame, len, written, room, mem, &done, NULL);
        EXPECT(got <= room, "the encoder wrote %zu symbols into room for %zu", got, room);
        EXPECT(got > 0 || room < WS_BYTE_SYMS, "the encoder wrote nothing into room for %zu", room);
        for (size_t k = 0; k < got; k++) {
            const int lv[2] = { mem[k].level0, mem[k].level1 };
            const int du[2] = { mem[k].duration0, mem[k].duration1 };
            for (int h = 0; h < 2; h++) {
                if (lv[h] == pending_level) {
                    pending_ticks += du[h];
                } else {
                    if (pending_level >= 0) {
                        sim_line(s, pending_level, pending_ticks);
                    }
                    pending_level = lv[h];
                    pending_ticks = du[h];
                }
            }
        }
        written += got;
    }
    if (pending_level >= 0) {
        sim_line(s, pending_level, pending_ticks);
    }
    EXPECT(done, "the encoder never finished");
    return (long)written;
}

static const int WHOLE[] = { 192 };

/* --------------------------------------------------------------- tests */

static uint32_t duty_of(int level)
{
    return zbc_level_to_duty((uint32_t)level * 1000u, STRIP_DUTY_ONE);
}

static zbc_gradient_t rgb_stops(void)
{
    /* sRGB's red, green and blue. */
    zbc_gradient_t g = { .n = 3 };
    const float xy[3][2] = { { 0.64f, 0.33f }, { 0.30f, 0.60f }, { 0.15f, 0.06f } };
    for (int i = 0; i < 3; i++) {
        g.x[i] = (uint16_t)(xy[i][0] * 65536.0f);
        g.y[i] = (uint16_t)(xy[i][1] * 65536.0f);
    }
    return g;
}

static void test_wire(void)
{
    /* Every byte value, on both chips, with the room an interrupt may find. */
    uint8_t frame[256 * 3];
    for (int i = 0; i < 256 * 3; i++) {
        frame[i] = (uint8_t)(i * 7 + (i >> 3));
    }
    static const int HALF_BLOCKS[] = { 24 }, ODD[] = { 9, 7, 8, 50, 3, 16 }, TIGHT[] = { 8 };
    const struct { const int *c; int n; const char *name; } rooms[] = {
        { WHOLE, 1, "four blocks" }, { HALF_BLOCKS, 1, "half a block" }, { ODD, 6, "odd" }, { TIGHT, 1, "one byte" },
    };
    const chip_t *chips[] = { &WS2812B, &SK6812 };
    for (int c = 0; c < 2; c++) {
        for (int r = 0; r < 4; r++) {
            sim_t s;
            sim_init(&s, chips[c], 256, 3);
            const long syms = drive(&s, frame, sizeof(frame), rooms[r].c, rooms[r].n);
            EXPECT(syms == (long)sizeof(frame) * 8 + 1, "%s, %s: %ld symbols for %zu bytes", chips[c]->name,
                   rooms[r].name, syms, sizeof(frame));
            EXPECT(s.bad_pulses == 0, "%s, %s: %d pulses outside the datasheet", chips[c]->name, rooms[r].name,
                   s.bad_pulses);
            EXPECT(s.latches == 1, "%s, %s: the strip latched %d times", chips[c]->name, rooms[r].name, s.latches);
            EXPECT(memcmp(s.shown, frame, sizeof(frame)) == 0, "%s, %s: the strip shows other bytes than were sent",
                   chips[c]->name, rooms[r].name);
            EXPECT(s.passed_on == 0, "%s, %s: %d bits ran off the end", chips[c]->name, rooms[r].name, s.passed_on);
        }
    }

    /* A frame longer than the strip: the rest goes out of its far end. */
    sim_t s;
    sim_init(&s, &WS2812B, 10, 3);
    drive(&s, frame, 36, WHOLE, 1);
    EXPECT(s.passed_on == 6 * 8 && memcmp(s.shown, frame, 30) == 0, "a strip of 10 keeps 30 bytes of 36");

    /* An empty frame is the pause alone. */
    sim_init(&s, &WS2812B, 10, 3);
    EXPECT(drive(&s, frame, 0, WHOLE, 1) == 1 && s.latches == 1 && s.bad_pulses == 0, "an empty frame is one pause");

    /* The longest RGB strip, all ones - the slowest frame - fits a fade tick. */
    static uint8_t ones[ZBC_STRIP_PIXELS_MAX * 3];
    memset(ones, 0xFF, sizeof(ones));
    sim_init(&s, &WS2812B, ZBC_STRIP_PIXELS_MAX, 3);
    drive(&s, ones, sizeof(ones), WHOLE, 1);
    EXPECT(s.bad_pulses == 0 && s.total_ns <= 10000000, "300 pixels take %lld us, more than a 10 ms tick",
           (long long)(s.total_ns / 1000));
    EXPECT((WS_T0H + WS_T0L) * 100 >= 1150 && (WS_T1H + WS_T1L) * 100 <= 1350, "a bit is about 1.25 us");
}

/* Renders, sends, and hands back the simulated strip. */
static void show(sim_t *s, const strip_colors_t *from, const strip_colors_t *to, uint32_t mix, uint32_t duty,
                 int pixels, uint8_t order, uint8_t flags)
{
    static uint8_t buf[ZBC_STRIP_PIXELS_MAX * 4];
    const size_t   len = strip_bytes((uint16_t)pixels, flags);
    memset(buf, 0xA5, sizeof(buf));
    strip_render(from, to, mix, duty, (uint16_t)pixels, order, flags, buf);
    sim_init(s, (flags & ZBC_STRIP_F_RGBW) ? &SK6812 : &WS2812B, pixels, (int)strip_bpp(flags));
    drive(s, buf, len, WHOLE, 1);
    EXPECT(s->bad_pulses == 0 && s->latches == 1, "a rendered frame reaches the strip whole");
    EXPECT(buf[len] == 0xA5, "the render wrote past its frame");
}

/* Pixel i's red, green, blue (and white) as the LEDs get them, by the order
 * the strip is wired in. */
static void pixel(const sim_t *s, int i, uint8_t order, int out[4])
{
    static const int AT[6][3] = { { 1, 0, 2 }, { 0, 1, 2 }, { 1, 2, 0 }, { 0, 2, 1 }, { 2, 0, 1 }, { 2, 1, 0 } };
    const uint8_t *p = &s->shown[i * s->bpp];
    out[0] = p[AT[order][0]];
    out[1] = p[AT[order][1]];
    out[2] = p[AT[order][2]];
    out[3] = s->bpp == 4 ? p[3] : 0;
}

static void test_gradient(void)
{
    const zbc_gradient_t g = rgb_stops();
    strip_colors_t       c;
    strip_colors_gradient(&g, &c);
    sim_t s;
    int   px[4];

    /* 31 pixels, so the 16th is the middle. */
    show(&s, NULL, &c, STRIP_MIX_ONE, STRIP_DUTY_ONE, 31, ZBC_STRIP_GRB, 0);
    pixel(&s, 0, ZBC_STRIP_GRB, px);
    EXPECT(px[0] == 255 && px[1] <= 2 && px[2] <= 2, "the first pixel is red, not %d %d %d", px[0], px[1], px[2]);
    pixel(&s, 15, ZBC_STRIP_GRB, px);
    EXPECT(px[1] == 255 && px[0] <= 2 && px[2] <= 2, "the middle pixel is green, not %d %d %d", px[0], px[1], px[2]);
    pixel(&s, 30, ZBC_STRIP_GRB, px);
    EXPECT(px[2] == 255 && px[0] <= 2 && px[1] <= 2, "the last pixel is blue, not %d %d %d", px[0], px[1], px[2]);
    /* Between two stops both of them show, and nothing of the third. */
    pixel(&s, 7, ZBC_STRIP_GRB, px);
    EXPECT(px[0] > 60 && px[1] > 60 && px[2] <= 2, "a quarter along is red and green, not %d %d %d", px[0], px[1], px[2]);
    pixel(&s, 23, ZBC_STRIP_GRB, px);
    EXPECT(px[1] > 60 && px[2] > 60 && px[0] <= 2, "three quarters is green and blue, not %d %d %d", px[0], px[1], px[2]);
    /* Along the first half red only falls and green only rises. */
    int last[4];
    pixel(&s, 0, ZBC_STRIP_GRB, last);
    for (int i = 1; i <= 15; i++) {
        pixel(&s, i, ZBC_STRIP_GRB, px);
        EXPECT(px[1] >= last[1], "green falls between pixel %d and %d on the way to green", i - 1, i);
        memcpy(last, px, sizeof(last));
    }
    for (int i = 0; i < 31; i++) {
        pixel(&s, i, ZBC_STRIP_GRB, px);
        EXPECT(px[0] == 255 || px[1] == 255 || px[2] == 255, "pixel %d at full brightness has no LED at full", i);
    }
    /* At any length, between two of the points the colours are worked out at
     * as well: every pixel has its strongest LED at full. */
    for (int n = 2; n <= ZBC_STRIP_PIXELS_MAX; n += 7) {
        show(&s, NULL, &c, STRIP_MIX_ONE, STRIP_DUTY_ONE, n, ZBC_STRIP_GRB, 0);
        for (int i = 0; i < n; i++) {
            pixel(&s, i, ZBC_STRIP_GRB, px);
            EXPECT(px[0] == 255 || px[1] == 255 || px[2] == 255, "%d pixels: pixel %d has no LED at full", n, i);
        }
    }

    /* The same from the other end. */
    show(&s, NULL, &c, STRIP_MIX_ONE, STRIP_DUTY_ONE, 31, ZBC_STRIP_GRB, ZBC_STRIP_F_REVERSED);
    pixel(&s, 0, ZBC_STRIP_GRB, px);
    EXPECT(px[2] == 255 && px[0] <= 2, "reversed, the first pixel is blue");
    pixel(&s, 30, ZBC_STRIP_GRB, px);
    EXPECT(px[0] == 255 && px[2] <= 2, "reversed, the last pixel is red");

    /* Every wiring order puts the same light on the same LED. */
    for (uint8_t o = 0; o < ZBC_STRIP_ORDER_COUNT; o++) {
        show(&s, NULL, &c, STRIP_MIX_ONE, STRIP_DUTY_ONE, 31, o, 0);
        pixel(&s, 0, o, px);
        EXPECT(px[0] == 255 && px[1] <= 2 && px[2] <= 2, "order %d: the first pixel is not red", o);
        pixel(&s, 30, o, px);
        EXPECT(px[2] == 255 && px[0] <= 2 && px[1] <= 2, "order %d: the last pixel is not blue", o);
    }
    /* GRB on the wire is green first. */
    show(&s, NULL, &c, STRIP_MIX_ONE, STRIP_DUTY_ONE, 31, ZBC_STRIP_GRB, 0);
    EXPECT(s.shown[1] == 255 && s.shown[0] <= 2, "a GRB strip's second byte is its red");

    /* Every stop of two to five colours is shown exactly where it sits. */
    for (int n = 2; n <= ZBC_GRADIENT_MAX; n++) {
        zbc_gradient_t m = { .n = (uint8_t)n };
        for (int i = 0; i < n; i++) {
            float x, y;
            zbc_hs_to_xy(i * 70, 90, &x, &y);
            m.x[i] = (uint16_t)(x * 65536.0f);
            m.y[i] = (uint16_t)(y * 65536.0f);
        }
        strip_colors_gradient(&m, &c);
        const int pixels = 12 * 5 + 1;      /* every stop on a pixel, for any count */
        show(&s, NULL, &c, STRIP_MIX_ONE, STRIP_DUTY_ONE, pixels, ZBC_STRIP_RGB, 0);
        for (int i = 0; i < n; i++) {
            strip_colors_t one;
            sim_t          t;
            int            want[4];
            strip_colors_xy(m.x[i] / 65536.0f, m.y[i] / 65536.0f, &one);
            show(&t, NULL, &one, STRIP_MIX_ONE, STRIP_DUTY_ONE, 1, ZBC_STRIP_RGB, 0);
            pixel(&t, 0, ZBC_STRIP_RGB, want);
            pixel(&s, i * (pixels - 1) / (n - 1), ZBC_STRIP_RGB, px);
            EXPECT(px[0] == want[0] && px[1] == want[1] && px[2] == want[2],
                   "%d colours: stop %d shows %d %d %d, picked %d %d %d", n, i, px[0], px[1], px[2], want[0], want[1], want[2]);
        }
    }

    /* One colour is the same on every pixel, and a strip of one shows the middle. */
    const zbc_gradient_t one = { .n = 1, .x = { g.x[1] }, .y = { g.y[1] } };
    strip_colors_gradient(&one, &c);
    show(&s, NULL, &c, STRIP_MIX_ONE, STRIP_DUTY_ONE, 60, ZBC_STRIP_GRB, 0);
    for (int i = 0; i < 60; i++) {
        pixel(&s, i, ZBC_STRIP_GRB, px);
        EXPECT(px[1] == 255 && px[0] <= 2 && px[2] <= 2, "one colour: pixel %d is not green", i);
    }
    strip_colors_gradient(&g, &c);
    show(&s, NULL, &c, STRIP_MIX_ONE, STRIP_DUTY_ONE, 1, ZBC_STRIP_GRB, 0);
    pixel(&s, 0, ZBC_STRIP_GRB, px);
    EXPECT(px[1] == 255, "a strip of one pixel shows the middle of the gradient");
    show(&s, NULL, &c, STRIP_MIX_ONE, STRIP_DUTY_ONE, 2, ZBC_STRIP_GRB, 0);
    pixel(&s, 0, ZBC_STRIP_GRB, px);
    EXPECT(px[0] == 255, "a strip of two shows the ends: the first");
    pixel(&s, 1, ZBC_STRIP_GRB, px);
    EXPECT(px[2] == 255, "a strip of two shows the ends: the last");
    show(&s, NULL, &c, STRIP_MIX_ONE, STRIP_DUTY_ONE, ZBC_STRIP_PIXELS_MAX, ZBC_STRIP_GRB, 0);
    pixel(&s, ZBC_STRIP_PIXELS_MAX - 1, ZBC_STRIP_GRB, px);
    EXPECT(px[2] == 255 && px[0] <= 2, "the longest strip ends in the last colour");
}

static void test_dimming(void)
{
    const zbc_gradient_t g = rgb_stops();
    strip_colors_t       c;
    strip_colors_gradient(&g, &c);
    sim_t s;
    int   px[4];

    /* Dark is dark, whatever the colour. */
    show(&s, NULL, &c, STRIP_MIX_ONE, 0, 31, ZBC_STRIP_GRB, 0);
    for (size_t i = 0; i < 31 * 3; i++) {
        EXPECT(s.shown[i] == 0, "off: byte %zu is %d", i, s.shown[i]);
    }

    /* From the dimmest level up, every pixel is lit and none gets dimmer. */
    static uint8_t prev[31 * 3];
    memset(prev, 0, sizeof(prev));
    for (int level = 1; level <= 254; level++) {
        show(&s, NULL, &c, STRIP_MIX_ONE, duty_of(level), 31, ZBC_STRIP_GRB, 0);
        for (int i = 0; i < 31; i++) {
            pixel(&s, i, ZBC_STRIP_GRB, px);
            EXPECT(px[0] + px[1] + px[2] > 0, "level %d: pixel %d is dark", level, i);
        }
        int sum = 0, was = 0;
        for (size_t i = 0; i < sizeof(prev); i++) {
            sum += s.shown[i];
            was += prev[i];
        }
        EXPECT(sum >= was, "level %d is dimmer than level %d", level, level - 1);
        memcpy(prev, s.shown, sizeof(prev));
    }
    pixel(&s, 0, ZBC_STRIP_GRB, px);
    EXPECT(px[0] == 255, "level 254 is full: red at %d", px[0]);
    /* Half the level is the lightness curve's 18 %, not half the counts. */
    show(&s, NULL, &c, STRIP_MIX_ONE, duty_of(127), 31, ZBC_STRIP_GRB, 0);
    pixel(&s, 0, ZBC_STRIP_GRB, px);
    EXPECT(px[0] >= 40 && px[0] <= 52, "level 127 drives red to %d, not about 46", px[0]);
    /* The dimmest level keeps each pixel's own colour: its strongest LED. */
    show(&s, NULL, &c, STRIP_MIX_ONE, duty_of(1), 31, ZBC_STRIP_GRB, 0);
    pixel(&s, 0, ZBC_STRIP_GRB, px);
    EXPECT(px[0] == 1 && px[1] == 0 && px[2] == 0, "the dimmest red pixel is %d %d %d", px[0], px[1], px[2]);
    pixel(&s, 30, ZBC_STRIP_GRB, px);
    EXPECT(px[2] == 1 && px[0] == 0 && px[1] == 0, "the dimmest blue pixel is %d %d %d", px[0], px[1], px[2]);
}

static void test_rgbw(void)
{
    sim_t          s;
    int            px[4];
    strip_colors_t c;
    const uint8_t  f = ZBC_STRIP_F_RGBW;

    strip_colors_xy(0.3127f, 0.3290f, &c);       /* D65 */
    show(&s, NULL, &c, STRIP_MIX_ONE, STRIP_DUTY_ONE, 12, ZBC_STRIP_GRB, f);
    EXPECT(s.bpp == 4, "an RGBW pixel is four bytes");
    for (int i = 0; i < 12; i++) {
        pixel(&s, i, ZBC_STRIP_GRB, px);
        EXPECT(px[3] >= 250 && px[0] <= 5 && px[1] <= 5 && px[2] <= 5, "white is the white LED, not %d %d %d + %d",
               px[0], px[1], px[2], px[3]);
    }
    strip_colors_xy(0.64f, 0.33f, &c);
    show(&s, NULL, &c, STRIP_MIX_ONE, STRIP_DUTY_ONE, 12, ZBC_STRIP_GRB, f);
    pixel(&s, 0, ZBC_STRIP_GRB, px);
    EXPECT(px[0] == 255 && px[3] <= 2, "red leaves the white LED dark: %d + %d", px[0], px[3]);
    /* A pastel: the white in it on the white LED, the rest on the colour. */
    float x, y;
    zbc_hs_to_xy(0, 30, &x, &y);
    strip_colors_xy(x, y, &c);
    show(&s, NULL, &c, STRIP_MIX_ONE, STRIP_DUTY_ONE, 12, ZBC_STRIP_GRB, f);
    pixel(&s, 0, ZBC_STRIP_GRB, px);
    EXPECT(px[3] > 80 && px[0] > 80 && px[1] <= 3 && px[2] <= 3, "a pale red is white and red: %d %d %d + %d",
           px[0], px[1], px[2], px[3]);
    EXPECT(px[0] + px[3] == 255, "which together are the red an RGB strip would get: %d + %d", px[0], px[3]);
    /* 2700 K: warm white is white and amber. */
    zbc_kelvin_to_xy(2700, &x, &y);
    strip_colors_xy(x, y, &c);
    show(&s, NULL, &c, STRIP_MIX_ONE, STRIP_DUTY_ONE, 12, ZBC_STRIP_GRB, f);
    pixel(&s, 0, ZBC_STRIP_GRB, px);
    EXPECT(px[3] > 20 && px[0] > px[1] && px[2] == 0, "2700 K is %d %d %d + %d", px[0], px[1], px[2], px[3]);
    show(&s, NULL, &c, STRIP_MIX_ONE, 0, 12, ZBC_STRIP_GRB, f);
    for (size_t i = 0; i < 48; i++) {
        EXPECT(s.shown[i] == 0, "off: RGBW byte %zu is %d", i, s.shown[i]);
    }
}

static void test_crossfade(void)
{
    sim_t          s, t;
    int            px[4], qx[4];
    strip_colors_t red, blue, grad, mid;
    const zbc_gradient_t g = rgb_stops();
    strip_colors_xy(0.64f, 0.33f, &red);
    strip_colors_xy(0.15f, 0.06f, &blue);
    strip_colors_gradient(&g, &grad);

    show(&s, &red, &blue, 0, STRIP_DUTY_ONE, 20, ZBC_STRIP_RGB, 0);
    pixel(&s, 5, ZBC_STRIP_RGB, px);
    EXPECT(px[0] == 255 && px[2] <= 2, "a cross-fade starts as where it came from");
    show(&s, &red, &blue, STRIP_MIX_ONE, STRIP_DUTY_ONE, 20, ZBC_STRIP_RGB, 0);
    pixel(&s, 5, ZBC_STRIP_RGB, px);
    EXPECT(px[2] == 255 && px[0] <= 2, "and ends as where it was going");
    show(&s, &red, &blue, 500, STRIP_DUTY_ONE, 20, ZBC_STRIP_RGB, 0);
    pixel(&s, 5, ZBC_STRIP_RGB, px);
    EXPECT(px[0] >= 253 && px[2] >= 253 && px[1] <= 2, "halfway is both, as bright as either: %d %d %d",
           px[0], px[1], px[2]);
    show(&s, &red, &blue, 250, STRIP_DUTY_ONE, 20, ZBC_STRIP_RGB, 0);
    pixel(&s, 5, ZBC_STRIP_RGB, px);
    EXPECT(px[0] == 255 && abs(px[2] - 85) <= 2, "a quarter of the way is red with a third of blue: %d %d %d",
           px[0], px[1], px[2]);
    /* No pixel goes through anything but the two it is between. */
    int last = 255;
    for (uint32_t mix = 0; mix <= STRIP_MIX_ONE; mix += 50) {
        show(&s, &red, &blue, mix, STRIP_DUTY_ONE, 20, ZBC_STRIP_RGB, 0);
        pixel(&s, 0, ZBC_STRIP_RGB, px);
        EXPECT(px[0] <= last && px[1] <= 2, "mix %u: red rose to %d, or green showed (%d)", (unsigned)mix, px[0], px[1]);
        last = px[0];
    }

    /* A cross-fade interrupted halfway carries on from exactly what it showed. */
    strip_colors_blend(&red, &grad, 500, &mid);
    show(&s, &red, &grad, 500, duty_of(200), 31, ZBC_STRIP_RGB, 0);
    show(&t, &mid, &blue, 0, duty_of(200), 31, ZBC_STRIP_RGB, 0);
    for (int i = 0; i < 31; i++) {
        pixel(&s, i, ZBC_STRIP_RGB, px);
        pixel(&t, i, ZBC_STRIP_RGB, qx);
        EXPECT(abs(px[0] - qx[0]) <= 1 && abs(px[1] - qx[1]) <= 1 && abs(px[2] - qx[2]) <= 1,
               "pixel %d jumps when a cross-fade is taken over: %d %d %d to %d %d %d", i, px[0], px[1], px[2],
               qx[0], qx[1], qx[2]);
    }
    strip_colors_blend(&red, &blue, 0, &mid);
    EXPECT(memcmp(&mid, &red, sizeof(mid)) == 0, "a blend not begun is its start");
    strip_colors_blend(&red, &blue, STRIP_MIX_ONE, &mid);
    EXPECT(memcmp(&mid, &blue, sizeof(mid)) == 0, "a blend finished is its end");
}

int main(void)
{
    test_wire();
    test_gradient();
    test_dimming();
    test_rgbw();
    test_crossfade();
    if (failures) {
        printf("\n%d of %ld checks failed\n", failures, checks);
        return 1;
    }
    printf("%ld checks passed\n", checks);
    return 0;
}
