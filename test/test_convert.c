/* The conversion plan from updater/main/convert.c.
 *
 * Lifted rather than copied: see test/Makefile. The plan decides what is erased
 * and written on a device that has no serial console, so the two faults worth
 * catching here are ones nobody would see until a device stopped starting:
 * writing over the running updater, and erasing the old otadata - which is what a
 * power cut before the table switch boots from - too early. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "convert-plan.inc"

static int failures;
static int checks;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        checks++;                                         \
        if (!(cond)) {                                    \
            printf("\n  %s: ", #cond);                    \
            printf(__VA_ARGS__);                          \
            failures++;                                   \
        }                                                 \
    } while (0)

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void make_table(uint8_t *bin, const cv_part_t *rows, int n)
{
    memset(bin, 0xFF, CV_TABLE_LEN);
    size_t off = 0;
    for (int i = 0; i < n; i++, off += 32) {
        uint8_t *e = bin + off;
        e[0]       = 0xAA;
        e[1]       = 0x50;
        e[2]       = rows[i].type;
        e[3]       = rows[i].subtype;
        put32(e + 4, rows[i].addr);
        put32(e + 8, rows[i].size);
        memset(e + 12, 0, 16);
        memcpy(e + 12, rows[i].label, strlen(rows[i].label));
        put32(e + 28, 0);
    }
    bin[off]     = 0xEB;   /* an MD5 entry, as gen_esp32part writes */
    bin[off + 1] = 0xEB;
    memset(bin + off + 16, 0x5A, 16);
}

#define MB (1024u * 1024u)
#define UPDATER_LEN 0xC6000u   /* about what the updater builds to */

/* partitions.csv */
static const cv_part_t OURS[] = {
    { "nvs", 1, 0x02, 0x9000, 0x9000 },       { "phy_init", 1, 0x01, 0x12000, 0x1000 },
    { "otadata", 1, 0x00, 0x13000, 0x2000 },  { "nvs_matter", 1, 0x02, 0x15000, 0xA000 },
    { "zb_fct", 1, 0x81, 0x1F000, 0x400 },    { "updater", 0, 0x00, 0x20000, 0x100000 },
    { "app", 0, 0x10, 0x120000, 0x260000 },   { "zb_ota", 0x40, 0x00, 0x380000, 0x80000 },
};
/* partitions.csv before zb_ota was cut from the end of app */
static const cv_part_t OURS_BEFORE_ZB_OTA[] = {
    { "nvs", 1, 0x02, 0x9000, 0x9000 },       { "phy_init", 1, 0x01, 0x12000, 0x1000 },
    { "otadata", 1, 0x00, 0x13000, 0x2000 },  { "nvs_matter", 1, 0x02, 0x15000, 0xA000 },
    { "zb_fct", 1, 0x81, 0x1F000, 0x400 },    { "updater", 0, 0x00, 0x20000, 0x100000 },
    { "app", 0, 0x10, 0x120000, 0x2E0000 },
};
#define ZB_OTA_START 0x380000u
#define ZB_OTA_END   0x400000u
/* ESPHome, ESP-IDF framework, 4 MB */
static const cv_part_t ESPHOME_IDF[] = {
    { "otadata", 1, 0x00, 0x9000, 0x2000 },   { "phy_init", 1, 0x01, 0xB000, 0x1000 },
    { "app0", 0, 0x10, 0x10000, 0x1C0000 },   { "app1", 0, 0x11, 0x1D0000, 0x1C0000 },
    { "nvs", 1, 0x02, 0x390000, 0x6D000 },
};
/* ESPHome, Arduino framework, 4 MB */
static const cv_part_t ESPHOME_ARDUINO[] = {
    { "nvs", 1, 0x02, 0x9000, 0x5000 },       { "otadata", 1, 0x00, 0xE000, 0x2000 },
    { "app0", 0, 0x10, 0x10000, 0x1C0000 },   { "app1", 0, 0x11, 0x1D0000, 0x1C0000 },
    { "eeprom", 1, 0x99, 0x390000, 0x1000 },  { "spiffs", 1, 0x82, 0x391000, 0xF000 },
};
/* A Shelly Gen4's stock table (Shelly 1 Mini Gen4); it sits at 0x10000, inside
 * our `nvs`. */
static const cv_part_t SHELLY_GEN4[] = {
    { "otadata", 1, 0x00, 0x11000, 0x2000 },  { "nvs", 1, 0x02, 0x14000, 0xC000 },
    { "app_0", 0, 0x10, 0x20000, 0x300000 },  { "fs_0", 1, 0x82, 0x320000, 0xE0000 },
    { "app_1", 0, 0x11, 0x400000, 0x300000 }, { "fs_1", 1, 0x82, 0x700000, 0xE0000 },
    { "scratch", 1, 0x80, 0x7E0000, 0x8000 }, { "zb_storage", 1, 0x81, 0x7E8000, 0x8000 },
    { "shelly", 1, 0x88, 0x7F0000, 0x10000 },
};
#define SHELLY_TABLE_AT 0x10000u

#define N(a) ((int)(sizeof(a) / sizeof(a[0])))

static cv_part_t newt[CV_MAX_PARTS];
static int       n_new;

static cv_range_t slot(const cv_part_t *rows, int n, const char *label)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(rows[i].label, label) == 0) {
            return (cv_range_t){ rows[i].addr, rows[i].addr + rows[i].size };
        }
    }
    return (cv_range_t){ 0, 0 };
}

static bool contains(const cv_range_t *list, int n, uint32_t start, uint32_t end)
{
    for (int i = 0; i < n; i++) {
        if (list[i].start == start && list[i].end == end) {
            return true;
        }
    }
    return false;
}

/* What must hold of every plan that lets the conversion run, whatever the tables. */
static void invariants(const char *name, const cv_plan_t *p, const cv_part_t *old, int n_old, cv_range_t running)
{
    if (p->verdict != CV_GO) {
        return;
    }
    CHECK(!cv_overlap(p->factory_write, running), "%s: the updater copy lands on the running updater\n", name);
    CHECK(!cv_overlap((cv_range_t){ 0, CV_TABLE_OFFSET + CV_SECTOR }, running), "%s: running in the table\n", name);
    for (int i = 0; i < p->n_before; i++) {
        CHECK(!cv_overlap(p->erase_before[i], running), "%s: erases the running updater\n", name);
        for (int j = 0; j < n_old; j++) {
            if (old[j].type == CV_TYPE_DATA && old[j].subtype == CV_SUBTYPE_OTADATA) {
                CHECK(!cv_overlap(p->erase_before[i], (cv_range_t){ old[j].addr, old[j].addr + old[j].size }),
                      "%s: erases the old otadata before the table switch\n", name);
            }
        }
        CHECK(!cv_overlap(p->erase_before[i], p->factory_write), "%s: erases its own copy\n", name);
    }
    for (int i = 0; i < p->n_after; i++) {
        CHECK(!cv_overlap(p->erase_after[i], running), "%s: erases the running updater after the switch\n", name);
    }
    int data = 0;
    for (int i = 0; i < n_new; i++) {
        data += newt[i].type == CV_TYPE_DATA;
    }
    CHECK(p->n_before + p->n_after == data, "%s: %d+%d erases for %d data partitions\n", name, p->n_before,
          p->n_after, data);
    /* zb_ota lies in ESPHome's second app slot. Erasing it would be erasing the
     * running updater there, and it needs no erasing: its content is CRC-gated. */
    const cv_range_t zb_ota = { ZB_OTA_START, ZB_OTA_END };
    for (int i = 0; i < p->n_before; i++) {
        CHECK(!cv_overlap(p->erase_before[i], zb_ota), "%s: erases zb_ota before the switch\n", name);
    }
    for (int i = 0; i < p->n_after; i++) {
        CHECK(!cv_overlap(p->erase_after[i], zb_ota), "%s: erases zb_ota after the switch\n", name);
    }
}

/* A copy of a table with one row changed. */
static int with_row(cv_part_t *out, const cv_part_t *rows, int n, const char *label, cv_part_t row)
{
    for (int i = 0; i < n; i++) {
        out[i] = strcmp(rows[i].label, label) == 0 ? row : rows[i];
    }
    return n;
}

static void relayout(void)
{
    cv_part_t t[CV_MAX_PARTS];
    int       n;
    const int nb = N(OURS_BEFORE_ZB_OTA);
    const int nn = N(OURS);

    CHECK(cv_relayout_check(OURS, nn, OURS_BEFORE_ZB_OTA, nb, 1901760) == CV_RELAYOUT_GO,
          "the older table with today's coordinator is rewritten\n");
    CHECK(cv_relayout_check(OURS, nn, OURS_BEFORE_ZB_OTA, nb, 0) == CV_RELAYOUT_GO,
          "the older table with no coordinator is rewritten\n");
    CHECK(cv_relayout_check(OURS, nn, OURS_BEFORE_ZB_OTA, nb, 0x260000) == CV_RELAYOUT_GO,
          "a coordinator exactly the new size still fits\n");
    CHECK(cv_relayout_check(OURS, nn, OURS_BEFORE_ZB_OTA, nb, 0x260001) == CV_RELAYOUT_KEEP,
          "a coordinator longer than the new app keeps the old table\n");

    CHECK(cv_relayout_check(OURS, nn, OURS, nn, 0) == CV_RELAYOUT_NOT_OURS, "the same table is not a relayout\n");
    CHECK(cv_relayout_check(OURS_BEFORE_ZB_OTA, nb, OURS, nn, 0) == CV_RELAYOUT_NOT_OURS,
          "growing app back over zb_ota is not a relayout\n");
    CHECK(cv_relayout_check(OURS, nn, ESPHOME_IDF, N(ESPHOME_IDF), 0) == CV_RELAYOUT_NOT_OURS, "ESPHome IDF\n");
    CHECK(cv_relayout_check(OURS, nn, ESPHOME_ARDUINO, N(ESPHOME_ARDUINO), 0) == CV_RELAYOUT_NOT_OURS,
          "ESPHome Arduino\n");
    CHECK(cv_relayout_check(OURS, -1, OURS_BEFORE_ZB_OTA, nb, 0) == CV_RELAYOUT_NOT_OURS, "no embedded table\n");
    CHECK(cv_relayout_check(OURS, nn, OURS_BEFORE_ZB_OTA, -1, 0) == CV_RELAYOUT_NOT_OURS, "no flash table\n");

    /* Every change to a kept row makes it someone else's table. */
    n = with_row(t, OURS_BEFORE_ZB_OTA, nb, "nvs", (cv_part_t){ "nvs", 1, 0x02, 0x9000, 0x6000 });
    CHECK(cv_relayout_check(OURS, nn, t, n, 0) == CV_RELAYOUT_NOT_OURS, "nvs of another size\n");
    n = with_row(t, OURS_BEFORE_ZB_OTA, nb, "otadata", (cv_part_t){ "otadata", 1, 0x00, 0xD000, 0x2000 });
    CHECK(cv_relayout_check(OURS, nn, t, n, 0) == CV_RELAYOUT_NOT_OURS, "otadata elsewhere\n");
    n = with_row(t, OURS_BEFORE_ZB_OTA, nb, "zb_fct", (cv_part_t){ "zb_fct", 1, 0x82, 0x1F000, 0x400 });
    CHECK(cv_relayout_check(OURS, nn, t, n, 0) == CV_RELAYOUT_NOT_OURS, "zb_fct of another subtype\n");
    n = with_row(t, OURS_BEFORE_ZB_OTA, nb, "updater", (cv_part_t){ "updater", 0, 0x00, 0x20000, 0xC0000 });
    CHECK(cv_relayout_check(OURS, nn, t, n, 0) == CV_RELAYOUT_NOT_OURS, "a smaller updater\n");
    n = with_row(t, OURS_BEFORE_ZB_OTA, nb, "phy_init", (cv_part_t){ "phy", 1, 0x01, 0x12000, 0x1000 });
    CHECK(cv_relayout_check(OURS, nn, t, n, 0) == CV_RELAYOUT_NOT_OURS, "a renamed row\n");
    n = with_row(t, OURS_BEFORE_ZB_OTA, nb, "app", (cv_part_t){ "app", 0, 0x10, 0x110000, 0x2F0000 });
    CHECK(cv_relayout_check(OURS, nn, t, n, 0) == CV_RELAYOUT_NOT_OURS, "app at another offset\n");
    n = with_row(t, OURS_BEFORE_ZB_OTA, nb, "app", (cv_part_t){ "ota_0", 0, 0x10, 0x120000, 0x2E0000 });
    CHECK(cv_relayout_check(OURS, nn, t, n, 0) == CV_RELAYOUT_NOT_OURS, "app under another label\n");
    n = with_row(t, OURS_BEFORE_ZB_OTA, nb, "app", (cv_part_t){ "app", 0, 0x11, 0x120000, 0x2E0000 });
    CHECK(cv_relayout_check(OURS, nn, t, n, 0) == CV_RELAYOUT_NOT_OURS, "app as ota_1\n");

    /* A row the old table did not have must sit in what app gave up. */
    n = with_row(t, OURS, nn, "zb_ota", (cv_part_t){ "zb_ota", 0x40, 0x00, 0x378000, 0x80000 });
    CHECK(cv_relayout_check(t, n, OURS_BEFORE_ZB_OTA, nb, 0) == CV_RELAYOUT_NOT_OURS, "a new row over the new app\n");
    n = with_row(t, OURS, nn, "zb_ota", (cv_part_t){ "zb_ota", 0x40, 0x00, 0x380000, 0x81000 });
    CHECK(cv_relayout_check(t, n, OURS_BEFORE_ZB_OTA, nb, 0) == CV_RELAYOUT_NOT_OURS, "a new row past the old app\n");
    n = with_row(t, OURS, nn, "zb_ota", (cv_part_t){ "zb_ota", 0x40, 0x00, 0x1F400, 0xC00 });
    CHECK(cv_relayout_check(t, n, OURS_BEFORE_ZB_OTA, nb, 0) == CV_RELAYOUT_NOT_OURS, "a new row in a gap elsewhere\n");

    /* Two old rows under one label cannot both be kept. */
    cv_part_t dup[CV_MAX_PARTS];
    memcpy(dup, OURS_BEFORE_ZB_OTA, sizeof(OURS_BEFORE_ZB_OTA));
    dup[nb] = OURS_BEFORE_ZB_OTA[0];
    CHECK(cv_relayout_check(OURS, nn, dup, nb + 1, 0) == CV_RELAYOUT_NOT_OURS, "a duplicated old row\n");
}

/* A Shelly Gen4 package: its installer writes our table where our bootloader
 * will look for it and puts the updater in the app slot the unit is not
 * running from - which is exactly where our factory partition begins. */
static void shelly(void)
{
    uint8_t   bin[CV_TABLE_LEN];
    make_table(bin, OURS, N(OURS));
    n_new = cv_parse_table(bin, sizeof(bin), newt, CV_MAX_PARTS);

    const cv_range_t app_0 = { 0x20000, 0x320000 };
    const cv_range_t app_1 = { 0x400000, 0x700000 };

    /* Installed into app_0, which is where `updater` begins: nothing to copy. */
    cv_plan_t p = cv_make_plan(newt, n_new, SHELLY_GEN4, N(SHELLY_GEN4), app_0, app_1, UPDATER_LEN, 8 * MB, false);
    CHECK(p.verdict == CV_GO, "Shelly: in place is a go, not a move (%d: %s)\n", p.verdict, p.why ? p.why : "");
    CHECK(p.factory_write.start == p.factory_write.end, "Shelly: nothing to copy\n");
    invariants("Shelly", &p, SHELLY_GEN4, N(SHELLY_GEN4), app_0);
    /* The old otadata is at 0x11000, inside our nvs and phy_init. */
    CHECK(contains(p.erase_after, p.n_after, 0x9000, 0x12000), "Shelly: nvs waits for the table switch\n");
    CHECK(contains(p.erase_after, p.n_after, 0x12000, 0x13000), "Shelly: phy_init waits too\n");
    CHECK(contains(p.erase_before, p.n_before, 0x13000, 0x15000), "Shelly: otadata goes first\n");
    CHECK(p.n_after == 2 && p.n_before == 3, "Shelly: %d before, %d after\n", p.n_before, p.n_after);

    /* An image too long for the factory row is refused wherever it runs. */
    p = cv_make_plan(newt, n_new, SHELLY_GEN4, N(SHELLY_GEN4), app_0, app_1, 0x100001, 8 * MB, false);
    CHECK(p.verdict == CV_BLOCKED, "Shelly: an updater past the factory row is blocked\n");

    /* Running one byte short of covering it is a copy, not an "already there". */
    const cv_range_t clipped = { 0x20000, 0x20000 + UPDATER_LEN - 1 };
    p = cv_make_plan(newt, n_new, SHELLY_GEN4, N(SHELLY_GEN4), clipped, app_1, UPDATER_LEN, 8 * MB, false);
    CHECK(p.factory_write.start != p.factory_write.end, "Shelly: a partly-covered image is still copied\n");
    CHECK(p.verdict == CV_MOVE, "Shelly: and then it has to move first\n");

    /* Installed into app_1 instead: an ordinary go, with the copy. */
    p = cv_make_plan(newt, n_new, SHELLY_GEN4, N(SHELLY_GEN4), app_1, app_0, UPDATER_LEN, 8 * MB, false);
    CHECK(p.verdict == CV_GO, "Shelly: from app_1 it is a go\n");
    CHECK(contains(&p.factory_write, 1, 0x20000, 0x20000 + UPDATER_LEN), "Shelly: and copies to 0x20000\n");
    invariants("Shelly app_1", &p, SHELLY_GEN4, N(SHELLY_GEN4), app_1);

    /* What the package rests on: the app slot it can name and our factory row
     * are the same address. If `updater` ever moves, the package stops working
     * and this is the only thing that would say so. */
    const cv_part_t *stock_app = NULL;
    const cv_part_t *factory   = NULL;
    for (int i = 0; i < N(SHELLY_GEN4); i++) {
        if (SHELLY_GEN4[i].type == 0 && SHELLY_GEN4[i].subtype == 0x10) {
            stock_app = &SHELLY_GEN4[i];
        }
    }
    for (int i = 0; i < n_new; i++) {
        if (newt[i].type == 0 && newt[i].subtype == 0x00) {
            factory = &newt[i];
        }
    }
    CHECK(stock_app && factory && stock_app->addr == factory->addr, "Shelly: app_0 is where `updater` is\n");
    CHECK(factory && UPDATER_LEN <= factory->size, "Shelly: the updater fits it\n");
}

int main(void)
{
    uint8_t   bin[CV_TABLE_LEN];
    cv_part_t old[CV_MAX_PARTS];

    /* Parsing: what gen_esp32part writes comes back as it went in. */
    make_table(bin, OURS, N(OURS));
    n_new = cv_parse_table(bin, sizeof(bin), newt, CV_MAX_PARTS);
    CHECK(n_new == N(OURS), "parsed %d entries\n", n_new);
    for (int i = 0; i < N(OURS) && i < n_new; i++) {
        CHECK(strcmp(newt[i].label, OURS[i].label) == 0 && newt[i].addr == OURS[i].addr &&
                  newt[i].size == OURS[i].size && newt[i].type == OURS[i].type && newt[i].subtype == OURS[i].subtype,
              "entry %d\n", i);
    }
    uint8_t junk[CV_TABLE_LEN];
    memset(junk, 0xFF, sizeof(junk));
    CHECK(cv_parse_table(junk, sizeof(junk), old, CV_MAX_PARTS) == -1, "an erased sector is not a table\n");
    memset(junk, 0x00, sizeof(junk));
    CHECK(cv_parse_table(junk, sizeof(junk), old, CV_MAX_PARTS) == -1, "zeros are not a table\n");
    make_table(junk, OURS, N(OURS));
    CHECK(cv_parse_table(junk, 5 * 32, old, CV_MAX_PARTS) == -1, "a table with no end is not a table\n");

    /* ESPHome on ESP-IDF, the updater in app1: go. */
    cv_range_t run = slot(ESPHOME_IDF, N(ESPHOME_IDF), "app1");
    cv_plan_t  p   = cv_make_plan(newt, n_new, ESPHOME_IDF, N(ESPHOME_IDF), run,
                                  slot(ESPHOME_IDF, N(ESPHOME_IDF), "app0"), UPDATER_LEN, 8 * MB, false);
    CHECK(p.verdict == CV_GO, "IDF/app1: verdict %d (%s)\n", p.verdict, p.why ? p.why : "");
    CHECK(p.factory_write.start == 0x20000 && p.factory_write.end == 0x20000 + UPDATER_LEN,
          "IDF/app1: factory write 0x%x-0x%x\n", p.factory_write.start, p.factory_write.end);
    CHECK(contains(p.erase_after, p.n_after, 0x9000, 0x12000), "IDF/app1: new nvs covers the old otadata, so it waits\n");
    CHECK(contains(p.erase_before, p.n_before, 0x13000, 0x15000), "IDF/app1: new otadata erased before\n");
    CHECK(contains(p.erase_before, p.n_before, 0x1F000, 0x20000), "IDF/app1: zb_fct rounded to a sector\n");
    invariants("IDF/app1", &p, ESPHOME_IDF, N(ESPHOME_IDF), run);

    /* ...the updater in app0, which the updater copy lands on: move to app1. */
    run = slot(ESPHOME_IDF, N(ESPHOME_IDF), "app0");
    p   = cv_make_plan(newt, n_new, ESPHOME_IDF, N(ESPHOME_IDF), run, slot(ESPHOME_IDF, N(ESPHOME_IDF), "app1"),
                       UPDATER_LEN, 8 * MB, false);
    CHECK(p.verdict == CV_MOVE, "IDF/app0: verdict %d\n", p.verdict);

    /* ESPHome on Arduino, the updater in app1: go, and its nvs overlaps its otadata region too. */
    run = slot(ESPHOME_ARDUINO, N(ESPHOME_ARDUINO), "app1");
    p   = cv_make_plan(newt, n_new, ESPHOME_ARDUINO, N(ESPHOME_ARDUINO), run,
                       slot(ESPHOME_ARDUINO, N(ESPHOME_ARDUINO), "app0"), UPDATER_LEN, 4 * MB, false);
    CHECK(p.verdict == CV_GO, "Arduino/app1: verdict %d (%s)\n", p.verdict, p.why ? p.why : "");
    CHECK(contains(p.erase_after, p.n_after, 0x9000, 0x12000), "Arduino/app1: new nvs covers otadata at 0xE000\n");
    invariants("Arduino/app1", &p, ESPHOME_ARDUINO, N(ESPHOME_ARDUINO), run);

    /* Blocked: one big factory app and nowhere to move to. */
    static const cv_part_t SINGLE[] = { { "nvs", 1, 0x02, 0x9000, 0x6000 }, { "factory", 0, 0x00, 0x10000, 0x3F0000 } };
    p = cv_make_plan(newt, n_new, SINGLE, N(SINGLE), slot(SINGLE, N(SINGLE), "factory"), (cv_range_t){ 0, 0 },
                     UPDATER_LEN, 4 * MB, false);
    CHECK(p.verdict == CV_BLOCKED, "single factory: verdict %d\n", p.verdict);

    /* Blocked: the other slot is clear of the writes but too small for the updater. */
    static const cv_part_t SMALL[] = { { "otadata", 1, 0x00, 0x9000, 0x2000 },
                                       { "app0", 0, 0x10, 0x10000, 0x100000 },
                                       { "app1", 0, 0x11, 0x110000, 0x80000 } };
    p = cv_make_plan(newt, n_new, SMALL, N(SMALL), slot(SMALL, N(SMALL), "app0"), slot(SMALL, N(SMALL), "app1"),
                     UPDATER_LEN, 4 * MB, false);
    CHECK(p.verdict == CV_BLOCKED, "small other slot: verdict %d\n", p.verdict);

    /* Blocked for reasons that have nothing to do with where it runs. */
    run = slot(ESPHOME_IDF, N(ESPHOME_IDF), "app1");
    cv_range_t other = slot(ESPHOME_IDF, N(ESPHOME_IDF), "app0");
    p = cv_make_plan(newt, n_new, ESPHOME_IDF, N(ESPHOME_IDF), run, other, UPDATER_LEN, 8 * MB, true);
    CHECK(p.verdict == CV_BLOCKED, "flash encryption\n");
    p = cv_make_plan(newt, n_new, ESPHOME_IDF, N(ESPHOME_IDF), run, other, UPDATER_LEN, 2 * MB, false);
    CHECK(p.verdict == CV_BLOCKED, "2 MB flash\n");
    p = cv_make_plan(newt, n_new, ESPHOME_IDF, N(ESPHOME_IDF), run, other, 0x101000, 8 * MB, false);
    CHECK(p.verdict == CV_BLOCKED, "an updater bigger than its partition\n");
    p = cv_make_plan(newt, n_new, ESPHOME_IDF, -1, run, other, UPDATER_LEN, 8 * MB, false);
    CHECK(p.verdict == CV_BLOCKED, "no table on the flash\n");
    p = cv_make_plan(newt, -1, ESPHOME_IDF, N(ESPHOME_IDF), run, other, UPDATER_LEN, 8 * MB, false);
    CHECK(p.verdict == CV_BLOCKED, "no embedded table\n");
    p = cv_make_plan(newt, n_new, ESPHOME_IDF, N(ESPHOME_IDF), (cv_range_t){ 0, 0 }, other, UPDATER_LEN, 8 * MB, false);
    CHECK(p.verdict == CV_BLOCKED, "unknown running partition\n");

    /* The invariants over every slot the updater could have landed in. */
    static const struct { const char *name; const cv_part_t *rows; int n; } LAYOUTS[] = {
        { "IDF", ESPHOME_IDF, N(ESPHOME_IDF) }, { "Arduino", ESPHOME_ARDUINO, N(ESPHOME_ARDUINO) },
        { "small", SMALL, N(SMALL) },
    };
    for (size_t l = 0; l < sizeof(LAYOUTS) / sizeof(LAYOUTS[0]); l++) {
        for (int i = 0; i < LAYOUTS[l].n; i++) {
            if (LAYOUTS[l].rows[i].type != CV_TYPE_APP) {
                continue;
            }
            run = (cv_range_t){ LAYOUTS[l].rows[i].addr, LAYOUTS[l].rows[i].addr + LAYOUTS[l].rows[i].size };
            for (int j = 0; j < LAYOUTS[l].n; j++) {
                if (LAYOUTS[l].rows[j].type != CV_TYPE_APP || j == i) {
                    continue;
                }
                other = (cv_range_t){ LAYOUTS[l].rows[j].addr, LAYOUTS[l].rows[j].addr + LAYOUTS[l].rows[j].size };
                p = cv_make_plan(newt, n_new, LAYOUTS[l].rows, LAYOUTS[l].n, run, other, UPDATER_LEN, 4 * MB, false);
                invariants(LAYOUTS[l].name, &p, LAYOUTS[l].rows, LAYOUTS[l].n, run);
                if (p.verdict == CV_MOVE) {
                    /* After the move the updater runs from `other`; that plan must be a go. */
                    cv_plan_t after = cv_make_plan(newt, n_new, LAYOUTS[l].rows, LAYOUTS[l].n, other, run,
                                                   UPDATER_LEN, 4 * MB, false);
                    CHECK(after.verdict == CV_GO, "%s: moving did not make room\n", LAYOUTS[l].name);
                    invariants(LAYOUTS[l].name, &after, LAYOUTS[l].rows, LAYOUTS[l].n, other);
                }
            }
        }
    }

    relayout();
    shelly();

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("all %d checks passed\n", checks);
    return 0;
}
