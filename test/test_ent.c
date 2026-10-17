/* Philips' entertainment frame (cluster 0xFC01), lifted from
 * main/zb_coordinator.c (region `ent-frame`).
 *
 * The vectors are Bifrost's own (crates/hue/src/zigbee/{entertainment,stream}.rs),
 * the implementation that is known to drive real Hue lamps. A fault here shows
 * on a lamp as "it does nothing" or "it flickers in the wrong colour", with
 * nothing on the coordinator's side to say why: the frames go out unanswered by
 * design. */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "ent-frame.inc"

static int failures, checks;

static void expect_bytes(const char *what, const uint8_t *got, const uint8_t *want, size_t n)
{
    checks++;
    if (memcmp(got, want, n) != 0) {
        printf("\n  %s:", what);
        printf("\n    got ");
        for (size_t i = 0; i < n; i++) {
            printf(" %02x", got[i]);
        }
        printf("\n    want");
        for (size_t i = 0; i < n; i++) {
            printf(" %02x", want[i]);
        }
        failures++;
    }
}

static void expect_u(const char *what, unsigned got, unsigned want)
{
    checks++;
    if (got != want) {
        printf("\n  %s: got 0x%x, expected 0x%x", what, got, want);
        failures++;
    }
}

int main(void)
{
    uint8_t rec[ZBC_ENT_REC_LEN];
    const uint8_t raw[3] = { 0xAA, 0xBB, 0xCC };

    /* Bifrost light_record_device: addr 0x1122, brightness 0x7FF, a whole lamp. */
    ent_pack_record(0x1122, 0x7FF, ZBC_ENT_MODE_DEVICE, raw, rec);
    expect_bytes("record, device", rec, (const uint8_t[]){ 0x22, 0x11, 0xeb, 0xff, 0xaa, 0xbb, 0xcc }, 7);

    /* light_record_segment: the same with mode 0. */
    ent_pack_record(0x1122, 0x7FF, 0x00, raw, rec);
    expect_bytes("record, segment", rec, (const uint8_t[]){ 0x22, 0x11, 0xe0, 0xff, 0xaa, 0xbb, 0xcc }, 7);

    /* hue_ent_frame: brightness word 0x0123 is brightness 9, mode 3. */
    ent_pack_record(0x7788, 0x123 >> 5, 0x123 & 0x1F, (const uint8_t[]){ 0xCC, 0xDD, 0xEE }, rec);
    expect_bytes("record, frame vector", rec, (const uint8_t[]){ 0x88, 0x77, 0x23, 0x01, 0xcc, 0xdd, 0xee }, 7);

    /* Brightness is 11 bits: more is held at full. 2048 unclamped shifts out of
     * the word altogether and arrives as brightness 0. */
    ent_pack_record(0x0001, 0x800, ZBC_ENT_MODE_DEVICE, raw, rec);
    expect_bytes("record, brightness clamped", rec, (const uint8_t[]){ 0x01, 0x00, 0xeb, 0xff, 0xaa, 0xbb, 0xcc }, 7);

    /* The mode is 5 bits, all of them kept, and nothing above them spills into
     * the brightness. */
    ent_pack_record(0x0001, 0, 0x1F, raw, rec);
    expect_bytes("record, mode 5 bits", rec, (const uint8_t[]){ 0x01, 0x00, 0x1f, 0x00, 0xaa, 0xbb, 0xcc }, 7);
    ent_pack_record(0x0001, 0, 0xFF, raw, rec);
    expect_bytes("record, mode masked", rec, (const uint8_t[]){ 0x01, 0x00, 0x1f, 0x00, 0xaa, 0xbb, 0xcc }, 7);

    /* The header: counter and smoothing, both little-endian. */
    uint8_t hdr[ZBC_ENT_HDR_LEN];
    expect_u("header length", (unsigned)ent_pack_header(0x11223344, 0xAABB, hdr), ZBC_ENT_HDR_LEN);
    expect_bytes("header", hdr, (const uint8_t[]){ 0x44, 0x33, 0x22, 0x11, 0xbb, 0xaa }, 6);

    /* ezs_reset: the sync is 00 01 and the counter. */
    uint8_t sync[6];
    expect_u("sync length", (unsigned)ent_pack_sync(0x1122, sync), 6);
    expect_bytes("sync", sync, (const uint8_t[]){ 0x00, 0x01, 0x22, 0x11, 0x00, 0x00 }, 6);

    /* Smoothing: Bifrost's duration_zero / duration_half / duration_max, and
     * one frame interval at 20 fps. Past the end is held at the end. */
    expect_u("smoothing 0", ent_smoothing(0), 0);
    expect_u("smoothing half", ent_smoothing(ZBC_ENT_SMOOTH_MAX_US / 2), 0x8000);
    expect_u("smoothing max - 1 us", ent_smoothing(ZBC_ENT_SMOOTH_MAX_US - 1), 0xFFFF);
    expect_u("smoothing past max", ent_smoothing(ZBC_ENT_SMOOTH_MAX_US * 2), 0xFFFF);
    expect_u("smoothing 50 ms", ent_smoothing(50000), 0x0500);

    /* xy, 12 bits each over the wide gamut, x low byte first. */
    uint8_t xy[3];
    ent_quant_xy(0.0f, 0.0f, xy);
    expect_bytes("xy origin", xy, (const uint8_t[]){ 0x00, 0x00, 0x00 }, 3);
    ent_quant_xy(ZBC_ENT_GAMUT_X, ZBC_ENT_GAMUT_Y, xy);
    expect_bytes("xy corner", xy, (const uint8_t[]){ 0xff, 0xff, 0xff }, 3);
    ent_quant_xy(1.0f, 1.0f, xy);
    expect_bytes("xy past the corner", xy, (const uint8_t[]){ 0xff, 0xff, 0xff }, 3);
    ent_quant_xy(-0.1f, -0.1f, xy);
    expect_bytes("xy below zero", xy, (const uint8_t[]){ 0x00, 0x00, 0x00 }, 3);
    /* x alone and y alone, so a swapped nibble cannot hide. x 0x123, y 0x456 */
    ent_quant_xy(0x123 * ZBC_ENT_GAMUT_X / 4095.0f + 1e-4f, 0.0f, xy);
    expect_bytes("x only", xy, (const uint8_t[]){ 0x23, 0x01, 0x00 }, 3);
    ent_quant_xy(0.0f, 0x456 * ZBC_ENT_GAMUT_Y / 4095.0f + 1e-4f, xy);
    expect_bytes("y only", xy, (const uint8_t[]){ 0x00, 0x60, 0x45 }, 3);
    /* D65, by Bifrost's formula: x 1742 (0x6ce), y 1630 (0x65e). */
    ent_quant_xy(0.3127f, 0.3290f, xy);
    expect_bytes("D65", xy, (const uint8_t[]){ 0xce, 0xe6, 0x65 }, 3);

    /* Ten lamps fit one secured unicast: the 5-byte manufacturer ZCL header
     * plus the frame stays within the ~82 bytes Philips' design assumes. */
    expect_u("ten lamps fit", 5 + ZBC_ENT_HDR_LEN + ZBC_ENT_LAMPS_MAX * ZBC_ENT_REC_LEN <= 82, 1);

    if (failures) {
        printf("\n%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("%d checks passed\n", checks);
    return 0;
}
