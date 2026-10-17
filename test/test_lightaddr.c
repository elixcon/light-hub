/* A wired light's made-up address and its pin level, from main/light_db.c.
 *
 * Lifted rather than copied: see test/Makefile. The address is what groups,
 * curves and the device routes name a wired light by, so a fault here is a
 * wired light that is mistaken for a Zigbee lamp - or a Zigbee lamp that is
 * mistaken for a pin, and silently never receives a command again. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "light_math.h"
#include "lightaddr.inc"

static int failures;
static int checks;

#define EXPECT(cond, ...) do {                     \
        checks++;                                  \
        if (!(cond)) {                             \
            printf("\n  ");                        \
            printf(__VA_ARGS__);                   \
            failures++;                            \
        }                                          \
    } while (0)

static void not_a_light(const char *what, const uint8_t ieee[8])
{
    EXPECT(zbc_light_id_from_ieee(ieee) == 0, "%s was taken for wired light %u", what,
           (unsigned)zbc_light_id_from_ieee(ieee));
    EXPECT(zbc_mlight_id_from_ieee(ieee) == 0, "%s was taken for Matter lamp %u", what,
           (unsigned)zbc_mlight_id_from_ieee(ieee));
}

int main(void)
{
    uint8_t a[8];

    /* Every id the table can hand out comes back as itself, and never as the
     * other kind: a Matter lamp taken for a wired light would drive a pin, and a
     * wired light taken for a Matter lamp would never switch again. */
    for (uint32_t id = 1; id <= 0xffff; id++) {
        zbc_light_ieee((uint16_t)id, a);
        EXPECT(zbc_light_id_from_ieee(a) == id, "id %u came back as %u", id,
               (unsigned)zbc_light_id_from_ieee(a));
        EXPECT(zbc_mlight_id_from_ieee(a) == 0, "wired light %u was taken for Matter lamp %u", id,
               (unsigned)zbc_mlight_id_from_ieee(a));
        zbc_mlight_ieee((uint16_t)id, a);
        EXPECT(zbc_mlight_id_from_ieee(a) == id, "Matter id %u came back as %u", id,
               (unsigned)zbc_mlight_id_from_ieee(a));
        EXPECT(zbc_light_id_from_ieee(a) == 0, "Matter lamp %u was taken for wired light %u", id,
               (unsigned)zbc_light_id_from_ieee(a));
    }

    /* Locally administered, unicast: the two bits that keep it out of the range
     * a manufacturer can assign. */
    zbc_light_ieee(1, a);
    EXPECT(a[0] & 0x02, "the prefix is not locally administered (0x%02x)", a[0]);
    EXPECT(!(a[0] & 0x01), "the prefix is a group address (0x%02x)", a[0]);
    zbc_mlight_ieee(1, a);
    EXPECT(a[0] & 0x02, "the Matter prefix is not locally administered (0x%02x)", a[0]);
    EXPECT(!(a[0] & 0x01), "the Matter prefix is a group address (0x%02x)", a[0]);

    /* Real device addresses, and the coordinator's own. */
    not_a_light("the coordinator", (const uint8_t[8]){ 0x40, 0x4c, 0xca, 0xff, 0xfe, 0x49, 0x4d, 0x78 });
    not_a_light("an LST002", (const uint8_t[8]){ 0x00, 0x17, 0x88, 0x01, 0x0b, 0xd6, 0xf2, 0xa1 });
    not_a_light("a ZBMINI", (const uint8_t[8]){ 0x00, 0x15, 0x8d, 0x00, 0x02, 0xa1, 0xb2, 0xc3 });
    not_a_light("all zeros", (const uint8_t[8]){ 0 });

    /* One byte out anywhere in the prefix is neither kind. 0x20 rather than a
     * low bit: flipping bit 0 of byte 4 turns one prefix into the other. */
    for (int i = 0; i < 6; i++) {
        char what[40];
        zbc_light_ieee(7, a);
        a[i] ^= 0x20;
        snprintf(what, sizeof(what), "prefix byte %d changed", i);
        not_a_light(what, a);
        zbc_mlight_ieee(7, a);
        a[i] ^= 0x20;
        snprintf(what, sizeof(what), "Matter prefix byte %d changed", i);
        not_a_light(what, a);
    }

    /* Power-on: only ON, and LAST with a stored "on", come up lit. */
    EXPECT(!zbc_light_boot_on(ZBC_LIGHT_BOOT_OFF, 1), "OFF came up on");
    EXPECT(!zbc_light_boot_on(ZBC_LIGHT_BOOT_OFF, -1), "OFF with nothing stored came up on");
    EXPECT(zbc_light_boot_on(ZBC_LIGHT_BOOT_ON, 0), "ON came up off");
    EXPECT(zbc_light_boot_on(ZBC_LIGHT_BOOT_ON, -1), "ON with nothing stored came up off");
    EXPECT(zbc_light_boot_on(ZBC_LIGHT_BOOT_LAST, 1), "LAST after on came up off");
    EXPECT(!zbc_light_boot_on(ZBC_LIGHT_BOOT_LAST, 0), "LAST after off came up on");
    EXPECT(!zbc_light_boot_on(ZBC_LIGHT_BOOT_LAST, -1), "LAST with nothing stored came up on");
    EXPECT(!zbc_light_boot_on(ZBC_LIGHT_BOOT_COUNT, 1), "an unknown setting came up on");

    /* The pin level, both polarities. */
    EXPECT(zbc_light_pin_level(true, false) == 1, "on, active high is not HIGH");
    EXPECT(zbc_light_pin_level(false, false) == 0, "off, active high is not LOW");
    EXPECT(zbc_light_pin_level(true, true) == 0, "on, active low is not LOW");
    EXPECT(zbc_light_pin_level(false, true) == 1, "off, active low is not HIGH");

    if (failures) {
        printf("\n%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("all %d checks passed\n", checks);
    return 0;
}
