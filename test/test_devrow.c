/* Host test for the device registry's stored records in device_db.c: a record
 * per device, the table a backup carries, and the table the builds before the
 * records stored, which every box updated to them has in flash once.
 *
 * Wrong here, a box forgets its devices' names at an update, or a restore puts
 * them back with another device's endpoints - once, with nobody watching, and
 * with nothing left to retry from.
 *
 * dev-types.inc and devrow.inc are extracted from main/device_db.h and
 * main/device_db.c by the Makefile, never copied.
 *
 *   make -C test
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Small, so that full is easy to reach. */
#define CONFIG_ZBC_MAX_DEVICES              4
#define CONFIG_ZBC_MAX_ENDPOINTS_PER_DEVICE 4

#include "dev-types.inc"
#include "devrow.inc"

static int s_checks;
static void ok(bool cond, const char *what)
{
    s_checks++;
    if (!cond) { printf("FAIL: %s\n", what); exit(1); }
}

static zbc_device_t device(uint8_t id, int eps)
{
    zbc_device_t d;
    memset(&d, 0, sizeof(d));
    d.in_use = true;
    for (int i = 0; i < ZBC_IEEE_LEN; i++) {
        d.ieee[i] = (uint8_t)(0x10 * i + id);
    }
    d.short_addr = (uint16_t)(0x1000 + id);
    snprintf(d.name, sizeof(d.name), "Lamp %u", id);
    snprintf(d.manufacturer, sizeof(d.manufacturer), "Maker %u", id);
    snprintf(d.model, sizeof(d.model), "Model %u", id);
    for (int e = 0; e < eps; e++) {
        zbc_endpoint_t *p = &d.eps[d.ep_count++];
        *p = (zbc_endpoint_t){ .ep = (uint8_t)(10 + e), .device_id = (uint16_t)(0x0100 + e),
                               .features = (uint16_t)(0x8000 | e), .on_off = 1, .level = 200,
                               .ct_mireds = 300, .ct_min_mireds = 153 + e, .ct_max_mireds = 500 - e,
                               .hue = 7, .saturation = 8, .color_mode = 2 };
    }
    d.last_seen_us = 123456;
    d.online       = true;
    return d;
}

/* What survives a restart is the same; what does not is unknown or cleared. */
static void same_stored(const zbc_device_t *a, const zbc_device_t *b, const char *what)
{
    char m[160];
    snprintf(m, sizeof(m), "%s: identity", what);
    ok(b->in_use && memcmp(a->ieee, b->ieee, ZBC_IEEE_LEN) == 0 && a->short_addr == b->short_addr &&
       strcmp(a->name, b->name) == 0 && strcmp(a->manufacturer, b->manufacturer) == 0 &&
       strcmp(a->model, b->model) == 0, m);
    snprintf(m, sizeof(m), "%s: nothing seen, offline", what);
    ok(!b->online && b->last_seen_us == 0, m);
    snprintf(m, sizeof(m), "%s: endpoint count", what);
    ok(a->ep_count == b->ep_count, m);
    for (int e = 0; e < a->ep_count; e++) {
        const zbc_endpoint_t *x = &a->eps[e], *y = &b->eps[e];
        snprintf(m, sizeof(m), "%s: endpoint %d stored fields", what, e);
        ok(x->ep == y->ep && x->device_id == y->device_id && x->features == y->features &&
           x->ct_min_mireds == y->ct_min_mireds && x->ct_max_mireds == y->ct_max_mireds, m);
        snprintf(m, sizeof(m), "%s: endpoint %d live state unknown", what, e);
        ok(y->on_off == ZBC_UNKNOWN && y->level == ZBC_UNKNOWN && y->ct_mireds == ZBC_UNKNOWN &&
           y->hue == ZBC_UNKNOWN && y->saturation == ZBC_UNKNOWN && y->color_mode == ZBC_UNKNOWN, m);
    }
}

static void test_keys(void)
{
    char a[13], b[13];
    const uint8_t ieee[8] = { 0x00, 0x17, 0x88, 0x01, 0x02, 0x6e, 0xf2, 0xa8 };
    dr_key(ieee, a);
    dr_key(ieee, b);
    ok(strcmp(a, b) == 0, "a key is the same every time");
    ok(strlen(a) == 12 && a[0] == 'd', "a key is \"d\" and eleven characters, inside NVS's 15");
    for (int i = 1; i < 12; i++) {
        const char c = a[i];
        ok((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_',
           "a key uses only letters, digits, - and _");
    }
    /* Every bit counts: one flipped anywhere gives another key. */
    for (int byte = 0; byte < 8; byte++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t other[8];
            memcpy(other, ieee, 8);
            other[byte] ^= (uint8_t)(1u << bit);
            dr_key(other, b);
            ok(strcmp(a, b) != 0, "addresses one bit apart have different keys");
        }
    }
}

static void test_record(void)
{
    uint8_t      row[DR_MAX];
    zbc_device_t d = device(3, 2), back;
    uint8_t      slot = 0;
    const size_t len  = dr_encode(&d, 2, row, sizeof(row));
    ok(len > 0 && len < 100, "a two-endpoint lamp is well under 100 bytes");
    ok(dr_decode(row, len, &back, &slot), "a record reads back");
    ok(slot == 2, "it keeps its slot");
    same_stored(&d, &back, "round trip");

    /* The longest strings this build keeps, and every bit of the numbers. */
    memset(d.name, 'n', sizeof(d.name) - 1);
    d.name[sizeof(d.name) - 1] = '\0';
    memset(d.manufacturer, 'm', sizeof(d.manufacturer) - 1);
    d.manufacturer[sizeof(d.manufacturer) - 1] = '\0';
    memset(d.model, 'o', sizeof(d.model) - 1);
    d.model[sizeof(d.model) - 1] = '\0';
    d.short_addr = 0xFFFE;
    d.ep_count   = 4;
    for (int e = 0; e < 4; e++) {
        d.eps[e] = (zbc_endpoint_t){ .ep = 0xF0, .device_id = 0xFFFF, .features = 0xFFFF,
                                     .ct_min_mireds = ZBC_UNKNOWN, .ct_max_mireds = 0x7FFFFFFF };
    }
    const size_t full = dr_encode(&d, 3, row, sizeof(row));
    ok(full == DR_MAX, "a device with everything at its longest is DR_MAX");
    ok(dr_decode(row, full, &back, &slot), "and reads back");
    same_stored(&d, &back, "longest");
    ok(dr_encode(&d, 3, row, full - 1) == 0, "a record that does not fit is not written");

    /* No endpoints, empty strings. */
    zbc_device_t bare = device(1, 0);
    bare.manufacturer[0] = bare.model[0] = '\0';
    const size_t blen = dr_encode(&bare, 0, row, sizeof(row));
    ok(blen == DR_HEAD + 3 + strlen(bare.name), "a bare device is the head, three lengths and its name");
    ok(dr_decode(row, blen, &back, &slot) && back.ep_count == 0 && back.model[0] == '\0', "and reads back");
}

static void test_record_refused(void)
{
    uint8_t      row[DR_MAX + 64];
    zbc_device_t d = device(5, 2), back;
    uint8_t      slot;
    const size_t len = dr_encode(&d, 1, row, sizeof(row));

    ok(!dr_decode(row, len - 1, &back, &slot), "a record one byte short is refused");
    row[len] = 0;
    ok(!dr_decode(row, len + 1, &back, &slot), "a record with a byte more than its counts is refused");
    row[0] = DR_VERSION + 1;
    ok(!dr_decode(row, len, &back, &slot), "a record of another version is refused");
    row[0] = DR_VERSION;
    row[2] = 3;
    ok(!dr_decode(row, len, &back, &slot), "an endpoint count its bytes do not have is refused");
    row[2] = 2;
    row[DR_HEAD] = 200;
    ok(!dr_decode(row, len, &back, &slot), "a name running past the record is refused");
    ok(!dr_decode(row, 5, &back, &slot), "a stub is refused");
}

/* A newer build may keep more: longer strings are cut, more endpoints left out. */
static void test_record_from_bigger_build(void)
{
    uint8_t row[512];
    size_t  at = 0;
    row[at++] = DR_VERSION;
    row[at++] = 9;                         /* past this build's table */
    row[at++] = 6;                         /* endpoints */
    for (int i = 0; i < 8; i++) {
        row[at++] = (uint8_t)(0xA0 + i);
    }
    row[at++] = 0x34;
    row[at++] = 0x12;
    row[at++] = 40;                        /* a name longer than 31 */
    memset(row + at, 'x', 40);
    at += 40;
    row[at++] = 0;
    row[at++] = 1;
    row[at++] = 'M';
    for (int e = 0; e < 6; e++) {
        row[at++] = (uint8_t)(e + 1);
        dr_put16(row + at, 0x0102);
        dr_put16(row + at + 2, (uint16_t)e);
        dr_put32(row + at + 4, 150);
        dr_put32(row + at + 8, 450);
        at += 12;
    }
    zbc_device_t d;
    uint8_t      slot;
    ok(dr_decode(row, at, &d, &slot), "a record with more than this build keeps reads");
    ok(slot == 9, "with the slot it names");
    ok(d.short_addr == 0x1234 && d.ieee[0] == 0xA0 && d.ieee[7] == 0xA7, "its address");
    ok(strlen(d.name) == ZBC_NAME_LEN - 1 && d.manufacturer[0] == '\0' && strcmp(d.model, "M") == 0,
       "a name too long cut to what this build keeps");
    ok(d.ep_count == CONFIG_ZBC_MAX_ENDPOINTS_PER_DEVICE && d.eps[3].ep == 4 && d.eps[3].features == 3,
       "the first endpoints, in order, as many as this build keeps");
}

static size_t v2_table(uint8_t *out, const dr_v2_device_t *rows, int n)
{
    dr_put32(out, DR_TABLE_V2);
    dr_put16(out + 4, (uint16_t)n);
    dr_put16(out + 6, 0);
    memcpy(out + DR_TABLE_HEAD, rows, (size_t)n * sizeof(rows[0]));
    return DR_TABLE_HEAD + (size_t)n * sizeof(rows[0]);
}

static dr_v2_device_t v2_row(uint8_t id, int eps)
{
    const zbc_device_t d = device(id, eps);
    dr_v2_device_t     r;
    memset(&r, 0, sizeof(r));
    r.in_use = true;
    memcpy(r.ieee, d.ieee, 8);
    r.short_addr = d.short_addr;
    memcpy(r.name, d.name, sizeof(r.name));
    memcpy(r.manufacturer, d.manufacturer, sizeof(r.manufacturer));
    memcpy(r.model, d.model, sizeof(r.model));
    r.ep_count = (uint8_t)eps;
    for (int e = 0; e < eps; e++) {
        const zbc_endpoint_t *s = &d.eps[e];
        r.eps[e] = (dr_v2_endpoint_t){ s->ep, s->device_id, s->features, ZBC_UNKNOWN, ZBC_UNKNOWN, ZBC_UNKNOWN,
                                       s->ct_min_mireds, s->ct_max_mireds, ZBC_UNKNOWN, ZBC_UNKNOWN, ZBC_UNKNOWN };
    }
    return r;
}

static void test_old_table(void)
{
    dr_v2_device_t rows[6] = { v2_row(1, 1), v2_row(2, 4), v2_row(3, 0), v2_row(4, 2), v2_row(5, 1), v2_row(6, 1) };
    uint8_t        blob[DR_TABLE_HEAD + 6 * sizeof(dr_v2_device_t)];
    zbc_device_t   out[CONFIG_ZBC_MAX_DEVICES];

    size_t len = v2_table(blob, rows, 3);
    ok(dr_table_parse(blob, len, NULL, 0) == 0, "an old table is one this build reads");
    memset(out, 0, sizeof(out));
    ok(dr_table_parse(blob, len, out, CONFIG_ZBC_MAX_DEVICES) == 3, "its three devices are placed");
    for (int i = 0; i < 3; i++) {
        const zbc_device_t d = device((uint8_t)(i + 1), rows[i].ep_count);
        same_stored(&d, &out[i], "old table, in its order");
    }
    ok(!out[3].in_use, "and nothing else");

    ok(dr_table_parse(blob, len - 1, NULL, 0) < 0, "an old table a byte short is refused");
    blob[0] = 1;
    ok(dr_table_parse(blob, len, NULL, 0) < 0, "a table of an unknown version is refused");

    /* Longer than this build's table: the first ones, as the builds before. */
    len = v2_table(blob, rows, 6);
    memset(out, 0, sizeof(out));
    ok(dr_table_parse(blob, len, out, CONFIG_ZBC_MAX_DEVICES) == CONFIG_ZBC_MAX_DEVICES,
       "an old table longer than this build's fills it");
    ok(out[3].ieee[0] == 4, "with the first devices");

    /* A device twice is placed once. */
    rows[1] = rows[0];
    len = v2_table(blob, rows, 3);
    memset(out, 0, sizeof(out));
    ok(dr_table_parse(blob, len, out, CONFIG_ZBC_MAX_DEVICES) == 2, "a device in a table twice is placed once");

    /* Forgetting the addresses. */
    rows[1] = v2_row(2, 4);
    len = v2_table(blob, rows, 3);
    dr_table_forget_addresses(blob, len);
    memset(out, 0, sizeof(out));
    ok(dr_table_parse(blob, len, out, CONFIG_ZBC_MAX_DEVICES) == 3, "an old table without its addresses reads");
    for (int i = 0; i < 3; i++) {
        ok(out[i].short_addr == ZBC_SHORT_NONE && strcmp(out[i].name, rows[i].name) == 0,
           "every address forgotten, nothing else");
    }
}

static void test_packed_table(void)
{
    zbc_device_t devs[CONFIG_ZBC_MAX_DEVICES], out[CONFIG_ZBC_MAX_DEVICES];
    memset(devs, 0, sizeof(devs));
    devs[0] = device(1, 1);
    devs[2] = device(3, 4);
    devs[3] = device(4, 0);
    uint8_t      blob[DR_TABLE_MAX(CONFIG_ZBC_MAX_DEVICES) + 8];
    const size_t len = dr_table_pack(devs, CONFIG_ZBC_MAX_DEVICES, blob, sizeof(blob));
    ok(len > DR_TABLE_HEAD && dr_get16(blob + 4) == 3, "three devices packed");
    ok(dr_table_parse(blob, len, NULL, 0) == 0, "a packed table is one this build reads");

    memset(out, 0, sizeof(out));
    ok(dr_table_parse(blob, len, out, CONFIG_ZBC_MAX_DEVICES) == 3, "and its devices are placed");
    same_stored(&devs[0], &out[0], "slot 0");
    same_stored(&devs[2], &out[2], "slot 2");
    same_stored(&devs[3], &out[3], "slot 3");
    ok(!out[1].in_use, "the empty slot stays empty");

    /* A slot already taken: the first free one. */
    memset(out, 0, sizeof(out));
    out[2] = device(9, 1);
    ok(dr_table_parse(blob, len, out, CONFIG_ZBC_MAX_DEVICES) == 3, "a table into a table with a slot taken");
    ok(out[1].in_use && out[1].ieee[0] == 3, "the device whose slot was taken goes to the first free one");

    ok(dr_table_parse(blob, len - 1, NULL, 0) < 0, "a packed table a byte short is refused");
    uint8_t longer[sizeof(blob) + 1];
    memcpy(longer, blob, len);
    longer[len] = 0;
    ok(dr_table_parse(longer, len + 1, NULL, 0) < 0, "a packed table with a byte more is refused");
    ok(dr_table_parse(blob, 4, NULL, 0) < 0 && dr_table_parse(NULL, 0, NULL, 0) < 0, "a stub is refused");
    ok(dr_table_pack(devs, CONFIG_ZBC_MAX_DEVICES, blob, len - 1) == 0, "a table that does not fit is not packed");

    /* The bound a backup line is sized by holds a full table at its longest. */
    zbc_device_t big[CONFIG_ZBC_MAX_DEVICES];
    for (int i = 0; i < CONFIG_ZBC_MAX_DEVICES; i++) {
        big[i] = device((uint8_t)i, CONFIG_ZBC_MAX_ENDPOINTS_PER_DEVICE);
        memset(big[i].name, 'n', sizeof(big[i].name) - 1);
        memset(big[i].manufacturer, 'm', sizeof(big[i].manufacturer) - 1);
        memset(big[i].model, 'o', sizeof(big[i].model) - 1);
    }
    ok(dr_table_pack(big, CONFIG_ZBC_MAX_DEVICES, blob, DR_TABLE_MAX(CONFIG_ZBC_MAX_DEVICES)) ==
           DR_TABLE_MAX(CONFIG_ZBC_MAX_DEVICES), "a full table at its longest is DR_TABLE_MAX");

    /* Forgetting the addresses. */
    const size_t len2 = dr_table_pack(devs, CONFIG_ZBC_MAX_DEVICES, blob, sizeof(blob));
    dr_table_forget_addresses(blob, len2);
    memset(out, 0, sizeof(out));
    ok(dr_table_parse(blob, len2, out, CONFIG_ZBC_MAX_DEVICES) == 3, "a packed table without its addresses reads");
    ok(out[0].short_addr == ZBC_SHORT_NONE && out[2].short_addr == ZBC_SHORT_NONE &&
       out[3].short_addr == ZBC_SHORT_NONE, "every address forgotten");
    ok(strcmp(out[2].name, devs[2].name) == 0 && out[2].ep_count == 4 && out[2].eps[3].ep == 13,
       "nothing else");
}

int main(void)
{
    test_keys();
    test_record();
    test_record_refused();
    test_record_from_bigger_build();
    test_old_table();
    test_packed_table();
    printf("all %d checks passed\n", s_checks);
    return 0;
}
