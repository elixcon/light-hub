/* Host test for which endpoints of a paired Matter device become lamps, and what
 * each is told it can do - the `mctl-disc` region of main/matter_commission.cpp.
 *
 * A fault here pairs a sensor as a lamp, drops a hub's lamps, or offers a
 * colour wheel for a lamp that refuses colour commands; on hardware it shows up
 * only once somebody pairs that particular device.
 *
 *   make -C test
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* device_db.h's feature bits. That header needs ESP-IDF; these are its values,
 * and the region only ever names them. */
#define ZBC_FEAT_ONOFF      (1u << 0)
#define ZBC_FEAT_LEVEL      (1u << 1)
#define ZBC_FEAT_COLOR_TEMP (1u << 2)
#define ZBC_FEAT_COLOR_HS   (1u << 3)
#define ZBC_FEAT_COLOR_XY   (1u << 4)

#include "mctl-disc.inc"

static int failures;
static int checks;

#define EXPECT(cond, ...) do {                     \
        checks++;                                  \
        if (!(cond)) {                             \
            printf("\n  line %d: ", __LINE__);     \
            printf(__VA_ARGS__);                   \
            failures++;                            \
        }                                          \
    } while (0)

static mctl_disc_ep_t ep_of(uint16_t ep, bool onoff, bool level, bool color, uint32_t cc)
{
    mctl_disc_ep_t e;
    memset(&e, 0, sizeof(e));
    e.ep          = ep;
    e.light       = true;
    e.onoff       = onoff;
    e.level       = level;
    e.color       = color;
    e.cc_features = cc;
    return e;
}

static void test_types(void)
{
    const uint32_t lights[] = { 0x0100, 0x0101, 0x010A, 0x010B, 0x010C, 0x010D };
    for (size_t i = 0; i < sizeof(lights) / sizeof(lights[0]); i++) {
        EXPECT(mctl_disc_light_type(lights[i]), "device type 0x%04x is not a light", lights[i]);
    }
    /* Root node, aggregator, bridged node, a switch, an occupancy sensor, a
     * contact sensor, a colour dimmer switch. */
    const uint32_t others[] = { 0x0016, 0x000E, 0x0013, 0x000F, 0x0107, 0x0015, 0x0105, 0 };
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        EXPECT(!mctl_disc_light_type(others[i]), "device type 0x%04x was taken for a light", others[i]);
    }

    mctl_disc_ep_t e = ep_of(1, false, false, false, 0);
    EXPECT(!mctl_disc_is_lamp(&e), "a light type without On/Off became a lamp");
    e.onoff = true;
    EXPECT(mctl_disc_is_lamp(&e), "a light with On/Off is not a lamp");
    e.light = false;
    EXPECT(!mctl_disc_is_lamp(&e), "On/Off on a non-light endpoint became a lamp");
}

static void test_features(void)
{
    mctl_disc_ep_t e = ep_of(1, true, false, false, 0);
    EXPECT(mctl_disc_features(&e) == ZBC_FEAT_ONOFF, "on/off plug: 0x%x", mctl_disc_features(&e));
    e.level = true;
    EXPECT(mctl_disc_features(&e) == (ZBC_FEAT_ONOFF | ZBC_FEAT_LEVEL), "dimmable: 0x%x", mctl_disc_features(&e));

    /* Tunable white: CT only. */
    e = ep_of(1, true, true, true, MCTL_CC_CT);
    EXPECT(mctl_disc_features(&e) == (ZBC_FEAT_ONOFF | ZBC_FEAT_LEVEL | ZBC_FEAT_COLOR_TEMP),
           "tunable white: 0x%x", mctl_disc_features(&e));
    /* A full colour lamp, as a Hue lamp says it: HS, EHUE, CL, XY, CT = 0x1F. */
    e = ep_of(11, true, true, true, 0x1F);
    EXPECT(mctl_disc_features(&e) ==
               (ZBC_FEAT_ONOFF | ZBC_FEAT_LEVEL | ZBC_FEAT_COLOR_TEMP | ZBC_FEAT_COLOR_HS | ZBC_FEAT_COLOR_XY),
           "full colour: 0x%x", mctl_disc_features(&e));
    /* xy only, no temperature: the page must not offer a warmth slider. */
    e = ep_of(1, true, true, true, MCTL_CC_XY);
    EXPECT(mctl_disc_features(&e) == (ZBC_FEAT_ONOFF | ZBC_FEAT_LEVEL | ZBC_FEAT_COLOR_XY),
           "xy only: 0x%x", mctl_disc_features(&e));
    /* The colour bits of a FeatureMap mean nothing without the cluster. */
    e = ep_of(1, true, true, false, 0x1F);
    EXPECT(mctl_disc_features(&e) == (ZBC_FEAT_ONOFF | ZBC_FEAT_LEVEL), "no cluster: 0x%x", mctl_disc_features(&e));
    /* Colour loop and enhanced hue alone promise no command we send. */
    e = ep_of(1, true, true, true, (1u << 1) | (1u << 2));
    EXPECT(mctl_disc_features(&e) == (ZBC_FEAT_ONOFF | ZBC_FEAT_LEVEL), "EHUE/CL only: 0x%x", mctl_disc_features(&e));
}

static void test_names(void)
{
    mctl_disc_node_t n;
    memset(&n, 0, sizeof(n));
    strcpy(n.vendor, "Nanoleaf");
    strcpy(n.product, "Essentials A19");
    char name[32];

    /* A bulb of its own: its product, or the name it carries. */
    mctl_disc_ep_t e = ep_of(1, true, true, true, MCTL_CC_CT);
    mctl_disc_name(&e, &n, 1, name, sizeof(name));
    EXPECT(strcmp(name, "Essentials A19") == 0, "bulb: \"%s\"", name);
    strcpy(n.label, "Desk");
    mctl_disc_name(&e, &n, 1, name, sizeof(name));
    EXPECT(strcmp(name, "Desk") == 0, "labelled bulb: \"%s\"", name);

    /* Two lamps on one device: numbered by endpoint, so they can be told apart. */
    mctl_disc_ep_t e2 = ep_of(2, true, false, false, 0);
    mctl_disc_name(&e2, &n, 2, name, sizeof(name));
    EXPECT(strcmp(name, "Desk 2") == 0, "second outlet: \"%s\"", name);

    /* Behind a hub: what the hub calls it, never the hub's own name. */
    mctl_disc_node_t hub;
    memset(&hub, 0, sizeof(hub));
    strcpy(hub.vendor, "Signify");
    strcpy(hub.product, "Hue Bridge");
    strcpy(hub.label, "Hue Bridge 2");
    mctl_disc_ep_t b = ep_of(12, true, true, true, 0x1F);
    b.bridged = true;
    strcpy(b.label, "Kitchen ceiling");
    strcpy(b.vendor, "Signify Netherlands B.V.");
    strcpy(b.product, "Hue color lamp");
    b.vendor_id  = 0x100B;
    b.product_id = 0x0003;
    mctl_disc_name(&b, &hub, 20, name, sizeof(name));
    EXPECT(strcmp(name, "Kitchen ceiling") == 0, "hub lamp: \"%s\"", name);
    const char *vendor, *product;
    uint16_t    vid, pid;
    mctl_disc_identity(&b, &hub, &vendor, &product, &vid, &pid);
    EXPECT(strcmp(product, "Hue color lamp") == 0 && vid == 0x100B && pid == 3,
           "hub lamp identity: %s / %s %04x:%04x", vendor, product, vid, pid);

    /* A hub lamp with no label is known by its product and endpoint. */
    b.label[0] = '\0';
    mctl_disc_name(&b, &hub, 20, name, sizeof(name));
    EXPECT(strcmp(name, "Hue color lamp 12") == 0, "unlabelled hub lamp: \"%s\"", name);
    /* ...and with nothing at all, the hub's product is still better than blank. */
    b.product[0] = b.vendor[0] = '\0';
    b.vendor_id = b.product_id = 0;
    mctl_disc_name(&b, &hub, 20, name, sizeof(name));
    EXPECT(strcmp(name, "Hue Bridge 12") == 0, "bare hub lamp: \"%s\"", name);
    mctl_disc_identity(&b, &hub, &vendor, &product, &vid, &pid);
    EXPECT(strcmp(product, "Hue Bridge") == 0, "bare hub lamp identity: %s", product);

    /* A device that says nothing. */
    mctl_disc_node_t none;
    memset(&none, 0, sizeof(none));
    mctl_disc_name(&e, &none, 1, name, sizeof(name));
    EXPECT(strcmp(name, "Matter lamp") == 0, "nameless: \"%s\"", name);

    /* Cut to fit without splitting a character: "Küche" is 6 bytes. */
    mctl_disc_ep_t u = ep_of(3, true, false, false, 0);
    u.bridged = true;
    strcpy(u.label, "K\xc3\xbc" "cheK\xc3\xbc" "cheK\xc3\xbc" "che");
    char small[8];
    mctl_disc_name(&u, &hub, 1, small, sizeof(small));
    EXPECT(strcmp(small, "K\xc3\xbc" "cheK") == 0, "cut: \"%s\"", small);
    char tiny[3];
    mctl_disc_name(&u, &hub, 1, tiny, sizeof(tiny));
    EXPECT(strcmp(tiny, "K") == 0, "cut inside a character: \"%s\" (%zu bytes)", tiny, strlen(tiny));
}

int main(void)
{
    test_types();
    test_features();
    test_names();
    if (failures) {
        printf("\n%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("all %d checks passed\n", checks);
    return 0;
}
