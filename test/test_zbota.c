/* The Zigbee OTA file header and Query Next Image parse, main/zb_ota_file.c.
 *
 * A leaf, compiled whole. What the header check lets through is served byte for
 * byte to a lamp, and what it refuses never reaches flash - so the two faults
 * worth catching are a file served under the wrong identity (a lamp flashed
 * with another model's firmware only by the lamp's own check, if it has one)
 * and a real image refused for no reason, which is only ever seen as "the
 * update button does nothing". */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "zb_ota_file.h"

#define ZBC_IEEE_LEN 8
#include "zota-wake.inc"

static int failures;
static int checks;

#define CHECK(cond, ...)                               \
    do {                                               \
        checks++;                                      \
        if (!(cond)) {                                 \
            printf("\n  %s: ", #cond);                 \
            printf(__VA_ARGS__);                       \
            failures++;                                \
        }                                              \
    } while (0)

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

typedef struct {
    uint16_t fc;
    uint16_t hdr_length;
    uint16_t manuf;
    uint16_t type;
    uint32_t version;
    uint32_t total;
    const char *str;
} spec_t;

static size_t make_header(uint8_t *h, const spec_t *s)
{
    memset(h, 0, ZBC_OTA_HDR_MAX);
    put32(h, 0x0BEEF11E);
    put16(h + 4, 0x0100);
    put16(h + 6, s->hdr_length);
    put16(h + 8, s->fc);
    put16(h + 10, s->manuf);
    put16(h + 12, s->type);
    put32(h + 14, s->version);
    put16(h + 18, 0x0002);
    if (s->str) {
        memcpy(h + 20, s->str, strlen(s->str) < 32 ? strlen(s->str) : 32);
    }
    put32(h + 52, s->total);
    size_t at = 56;
    if (s->fc & 1) {
        h[at++] = 0x02;
    }
    if (s->fc & 2) {
        memset(h + at, 0xAB, 8);
        at += 8;
    }
    if (s->fc & 4) {
        put16(h + at, 0x0003);
        put16(h + at + 2, 0x0007);
        at += 4;
    }
    return at;
}

static void header(void)
{
    uint8_t       h[ZBC_OTA_HDR_MAX];
    zbc_ota_hdr_t out;

    /* The first 64 bytes of the Hue file in the Zigbee2MQTT index,
     * 100B-010C-01001A02-ConfLight-Lamps_0012.zigbee, as downloaded. */
    static const uint8_t HUE[] = {
        0x1e, 0xf1, 0xee, 0x0b, 0x00, 0x01, 0x38, 0x00, 0x00, 0x00, 0x0b, 0x10, 0x0c, 0x01, 0x02, 0x1a,
        0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xbc, 0x14, 0x04, 0x00, 0x00, 0xf0, 0x40, 0x03, 0x00, 0x00, 0x2a, 0x00,
    };
    const char *why = zbc_ota_header_check(HUE, sizeof(HUE), 267452, &out);
    CHECK(why == NULL, "a real Hue file is refused: %s\n", why ? why : "");
    CHECK(out.manuf_code == 4107 && out.image_type == 268 && out.file_version == 16783874,
          "Hue identity: 0x%04x 0x%04x 0x%08x\n", out.manuf_code, out.image_type, out.file_version);
    CHECK(out.total == 267452 && out.hdr_length == 56 && out.hdr_string[0] == '\0', "Hue total %u\n", out.total);
    CHECK(zbc_ota_header_check(HUE, sizeof(HUE), 0, &out) == NULL, "a length not yet known is fine\n");
    CHECK(zbc_ota_header_check(HUE, 56, 267452, &out) == NULL, "56 bytes are enough without options\n");
    CHECK(zbc_ota_header_check(HUE, 55, 267452, &out) != NULL, "55 bytes are not\n");
    CHECK(zbc_ota_header_check(HUE, sizeof(HUE), 267453, &out) != NULL, "one byte longer than it says\n");
    CHECK(zbc_ota_header_check(HUE, sizeof(HUE), 267451, &out) != NULL, "one byte shorter than it says\n");

    /* Every optional field. */
    spec_t s = { .fc = 7, .hdr_length = 69, .manuf = 0x117C, .type = 0x2803, .version = 0x23053631,
                 .total = 227500, .str = "EBL tradfri_wrgb" };
    size_t n = make_header(h, &s);
    CHECK(n == ZBC_OTA_HDR_MAX, "all options come to %zu bytes\n", n);
    why = zbc_ota_header_check(h, n, 227500, &out);
    CHECK(why == NULL, "all options refused: %s\n", why ? why : "");
    CHECK(out.hw_range && out.hw_min == 3 && out.hw_max == 7, "hardware range %u..%u\n", out.hw_min, out.hw_max);
    CHECK(strcmp(out.hdr_string, "EBL tradfri_wrgb") == 0, "header string '%s'\n", out.hdr_string);
    CHECK(zbc_ota_header_check(h, n - 1, 227500, &out) != NULL, "options announced but not all there\n");

    /* A header string of exactly 32 bytes has no terminator in the file. */
    s.str = "0123456789ABCDEF0123456789ABCDEF";
    n     = make_header(h, &s);
    CHECK(zbc_ota_header_check(h, n, 227500, &out) == NULL && strlen(out.hdr_string) == 32,
          "a 32-byte string: '%s'\n", out.hdr_string);
    h[25] = 0x07;
    h[26] = 0xC3;
    CHECK(zbc_ota_header_check(h, n, 227500, &out) == NULL && out.hdr_string[5] == '.' && out.hdr_string[6] == '.',
          "unprintable bytes in the string are replaced: '%s'\n", out.hdr_string);

    /* Refusals. */
    const spec_t ok = { .fc = 0, .hdr_length = 56, .manuf = 0x1189, .type = 0x00A0, .version = 0x03197310,
                        .total = 314466, .str = "A60S_RGBW" };
    s = ok;
    n = make_header(h, &s);
    CHECK(zbc_ota_header_check(h, n, 314466, &out) == NULL, "the base case\n");

    h[0] = 0x1F;
    CHECK(zbc_ota_header_check(h, n, 314466, &out) != NULL, "wrong magic\n");
    /* IKEA's signed files start with "NGIS"; the browser trims that off. */
    memcpy(h, "NGIS", 4);
    CHECK(zbc_ota_header_check(h, n, 314466, &out) != NULL, "a signing wrapper\n");

    n = make_header(h, &s);
    put16(h + 4, 0x0200);
    CHECK(zbc_ota_header_check(h, n, 314466, &out) != NULL, "header version 2\n");

    s            = ok;
    s.hdr_length = 55;
    n            = make_header(h, &s);
    CHECK(zbc_ota_header_check(h, n, 314466, &out) != NULL, "a header shorter than 56\n");
    s            = ok;
    s.fc         = 4;
    s.hdr_length = 56;
    n            = make_header(h, &s);
    CHECK(zbc_ota_header_check(h, n, 314466, &out) != NULL, "hardware versions announced, length not grown\n");
    s            = ok;
    s.hdr_length = 60;
    s.total      = 59;
    n            = make_header(h, &s);
    CHECK(zbc_ota_header_check(h, n, 0, &out) != NULL, "a total shorter than the header\n");

    s       = ok;
    s.manuf = 0xFFFF;
    n       = make_header(h, &s);
    CHECK(zbc_ota_header_check(h, n, 314466, &out) != NULL, "wildcard manufacturer\n");
    s      = ok;
    s.type = 0xFFC0;
    n      = make_header(h, &s);
    CHECK(zbc_ota_header_check(h, n, 314466, &out) != NULL, "security credential image type\n");
    s.type = 0xFFBF;
    n      = make_header(h, &s);
    CHECK(zbc_ota_header_check(h, n, 314466, &out) == NULL, "the last firmware image type\n");

    CHECK(zbc_ota_header_check(NULL, 69, 0, &out) != NULL, "no buffer\n");
}

static void qni(void)
{
    zbc_ota_qni_t q;
    /* fc 0, Hue 0x100B, type 0x010C, version 0x01001A02 */
    static const uint8_t PLAIN[] = { 0x00, 0x0B, 0x10, 0x0C, 0x01, 0x02, 0x1A, 0x00, 0x01 };
    CHECK(zbc_ota_qni_parse(PLAIN, sizeof(PLAIN), &q), "plain query\n");
    CHECK(q.manuf_code == 0x100B && q.image_type == 0x010C && q.file_version == 0x01001A02 && !q.hw_present,
          "plain query fields 0x%04x 0x%04x 0x%08x\n", q.manuf_code, q.image_type, q.file_version);

    static const uint8_t HW[] = { 0x01, 0x89, 0x11, 0xA0, 0x00, 0x10, 0x73, 0x19, 0x03, 0x05, 0x00 };
    CHECK(zbc_ota_qni_parse(HW, sizeof(HW), &q), "query with hardware version\n");
    CHECK(q.manuf_code == 0x1189 && q.image_type == 0x00A0 && q.file_version == 0x03197310 && q.hw_present &&
              q.hw_version == 5,
          "hardware query fields\n");

    CHECK(!zbc_ota_qni_parse(PLAIN, 8, &q), "8 bytes\n");
    CHECK(!zbc_ota_qni_parse(HW, 9, &q), "hardware version announced but missing\n");
    CHECK(!zbc_ota_qni_parse(HW, 10, &q), "half a hardware version\n");
    uint8_t odd[sizeof(PLAIN)];
    memcpy(odd, PLAIN, sizeof(odd));
    odd[0] = 0x02;
    CHECK(!zbc_ota_qni_parse(odd, sizeof(odd), &q), "an undefined field control bit\n");
    CHECK(!zbc_ota_qni_parse(NULL, 9, &q), "no payload\n");
}

/* The notify held for a sleepy device (zb_ota.c). What goes wrong unseen:
 * notifies on every frame of a turning wheel, which is airtime during exactly
 * the presses the radio counters watch, and a remote that never gets one
 * because the row was lost - "the update button does nothing" again. */
static void wake(void)
{
    const int64_t S = 1000000LL;
    static const uint8_t A[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    static const uint8_t B[8] = { 2, 2, 2, 2, 2, 2, 2, 2 };
    zota_wake_t rows[ZOTA_WAKE_ROWS];
    uint8_t     got[8];
    memset(rows, 0, sizeof(rows));

    CHECK(!zota_wake_heard(rows, ZOTA_WAKE_ROWS, A, 0), "not armed\n");
    zota_wake_arm(rows, ZOTA_WAKE_ROWS, A, 100 * S);
    CHECK(!zota_wake_heard(rows, ZOTA_WAKE_ROWS, A, 105 * S), "right after the direct notify\n");
    CHECK(!zota_wake_take(rows, ZOTA_WAKE_ROWS, got), "nothing due yet\n");
    CHECK(!zota_wake_heard(rows, ZOTA_WAKE_ROWS, B, 111 * S), "another device\n");
    CHECK(zota_wake_heard(rows, ZOTA_WAKE_ROWS, A, 110 * S), "heard 10 s after the notify\n");
    CHECK(!zota_wake_heard(rows, ZOTA_WAKE_ROWS, A, 110 * S + 1), "the next frame of the same press\n");
    CHECK(!zota_wake_heard(rows, ZOTA_WAKE_ROWS, A, 125 * S), "still due and not yet sent: not posted twice\n");
    CHECK(zota_wake_take(rows, ZOTA_WAKE_ROWS, got) && memcmp(got, A, 8) == 0, "taken\n");
    CHECK(!zota_wake_take(rows, ZOTA_WAKE_ROWS, got), "taken once\n");
    CHECK(!zota_wake_heard(rows, ZOTA_WAKE_ROWS, A, 113 * S), "3 s after the last one\n");
    CHECK(zota_wake_heard(rows, ZOTA_WAKE_ROWS, A, 121 * S), "a later press\n");
    zota_wake_take(rows, ZOTA_WAKE_ROWS, got);

    /* Ten minutes after arming it gives up, and the row is free. */
    CHECK(zota_wake_heard(rows, ZOTA_WAKE_ROWS, A, 100 * S + ZOTA_WAKE_ARM_US - 1), "just before expiry\n");
    zota_wake_take(rows, ZOTA_WAKE_ROWS, got);
    CHECK(!zota_wake_heard(rows, ZOTA_WAKE_ROWS, A, 100 * S + ZOTA_WAKE_ARM_US + 20 * S), "expired\n");
    CHECK(!rows[0].used, "an expired row is freed\n");

    /* A query disarms it. */
    zota_wake_arm(rows, ZOTA_WAKE_ROWS, A, 1000 * S);
    zota_wake_clear(rows, ZOTA_WAKE_ROWS, A);
    CHECK(!zota_wake_heard(rows, ZOTA_WAKE_ROWS, A, 1020 * S), "cleared by a query\n");

    /* Re-arming the same device keeps one row and restarts both clocks. */
    memset(rows, 0, sizeof(rows));
    zota_wake_arm(rows, ZOTA_WAKE_ROWS, A, 0);
    zota_wake_arm(rows, ZOTA_WAKE_ROWS, A, 500 * S);
    CHECK(!rows[1].used, "one row per device\n");
    CHECK(zota_wake_heard(rows, ZOTA_WAKE_ROWS, A, 500 * S + ZOTA_WAKE_ARM_US - S), "re-armed clock\n");

    /* Full: the row nearest expiry makes way. */
    memset(rows, 0, sizeof(rows));
    for (int i = 0; i < ZOTA_WAKE_ROWS; i++) {
        uint8_t id[8] = { 0 };
        id[7] = (uint8_t)(10 + i);
        zota_wake_arm(rows, ZOTA_WAKE_ROWS, id, (i == 2 ? 1 : 10 + i) * S);
    }
    zota_wake_arm(rows, ZOTA_WAKE_ROWS, B, 30 * S);
    CHECK(rows[2].used && memcmp(rows[2].ieee, B, 8) == 0, "the oldest row replaced\n");
    uint8_t first[8] = { 0 };
    first[7] = 10;
    CHECK(zota_wake_heard(rows, ZOTA_WAKE_ROWS, first, 60 * S), "the others kept\n");
}

int main(void)
{
    header();
    qni();
    wake();
    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("all %d checks passed\n", checks);
    return 0;
}
