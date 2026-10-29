/* Host test for the button table in button_db.c: every button's rules in one
 * pool behind the buttons, stored as the table is.
 *
 * A button's rules are found by adding up the counts of the buttons before it,
 * so growing one button's list moves every rule behind it, and the save moves
 * the whole pool for the write and back. Wrong here, a button runs another
 * button's rules or none - after an update, with nobody watching, on every
 * button behind the one that went wrong.
 *
 * bdb-types.inc and bdb-table.inc are extracted from main/button_db.h and
 * main/button_db.c by the Makefile, never copied.
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
#define CONFIG_ZBC_MAX_BUTTONS  4
#define CONFIG_ZBC_BUTTON_RULES 12
#define CONFIG_ZBC_BUTTON_POOL  20

#include "bdb-types.inc"
#include "bdb-table.inc"

static int s_checks;
static void ok(bool cond, const char *what)
{
    s_checks++;
    if (!cond) { printf("FAIL: %s\n", what); exit(1); }
}

/* A rule that says whose it is and where it stood: room id * 100 + k. */
static zbc_btn_rule_t mark(int id, int k)
{
    return (zbc_btn_rule_t){ .group_id = (uint16_t)(id * 100 + k), .action = ZBC_ACT_ON,
                             .delay_s = (uint16_t)k };
}

static zbc_button_t button(int id, int c0, int c1, int c2, int c3)
{
    zbc_button_t b;
    memset(&b, 0, sizeof(b));
    b.id = (uint16_t)id;
    snprintf(b.name, sizeof(b.name), "button %d", id);
    b.gpio = -1;
    const int c[4] = { c0, c1, c2, c3 };
    int at = 0;
    for (int e = 0; e < 4; e++) {
        b.rule_count[e] = (uint8_t)c[e];
        for (int r = 0; r < c[e]; r++, at++) {
            b.rules[at] = mark(id, at);
        }
    }
    return b;
}

static void reset(void)
{
    memset(&s_table, 0, sizeof(s_table));
}

/* Button `id` is in the table as `want` is: name, counts and every rule. */
static bool same(uint16_t id, const zbc_button_t *want)
{
    const int i = find_index(id);
    if (i < 0) {
        return false;
    }
    zbc_button_t got;
    copy_out(i, &got);
    const int n = button_db_rule_total(want);
    return got.id == want->id && strcmp(got.name, want->name) == 0 && got.src == want->src &&
           got.gpio == want->gpio && got.inverted == want->inverted && got.pull == want->pull &&
           memcmp(got.ieee, want->ieee, sizeof(got.ieee)) == 0 && got.zb_ep == want->zb_ep &&
           got.zb_key == want->zb_key && got.input == want->input && got.clear_s == want->clear_s &&
           memcmp(got.rule_count, want->rule_count, sizeof(got.rule_count)) == 0 &&
           memcmp(got.rules, want->rules, (size_t)n * sizeof(got.rules[0])) == 0;
}

/* The pool holds exactly the rules the counts say, nothing between them. */
static bool pool_adds_up(void)
{
    return rules_at(s_table.hdr.count) == s_table.hdr.rules;
}

static uint8_t s_out[16384];
static size_t  s_out_len;
static int capture(void *ctx, const void *blob, size_t len)
{
    (void)ctx;
    memcpy(s_out, blob, len);
    s_out_len = len;
    return 0;
}

/* A version-6 blob of these buttons, as the build before the pool wrote it. */
static size_t v6_blob(uint8_t *out, const zbc_button_t *b, int n)
{
    button_blob_header_t hdr = { .version = 6, .count = (uint16_t)n };
    memcpy(out, &hdr, sizeof(hdr));
    for (int i = 0; i < n; i++) {
        zbc_button_v6_t r;
        memset(&r, 0, sizeof(r));
        r.in_use = true;
        COPY_V2_FIELDS(&r, b[i]);
        memcpy(r.rule_count, b[i].rule_count, sizeof(r.rule_count));
        memcpy(r.rules, b[i].rules, sizeof(r.rules));
        r.input   = b[i].input;
        r.clear_s = b[i].clear_s;
        memcpy(out + sizeof(hdr) + (size_t)i * sizeof(r), &r, sizeof(r));
    }
    return sizeof(hdr) + (size_t)n * sizeof(zbc_button_v6_t);
}

/* A version-7 blob of these buttons, as a build with more room might write. */
static size_t v7_blob(uint8_t *out, const zbc_button_t *b, int n)
{
    uint8_t *heads = out + sizeof(button_blob_header_t);
    uint8_t *rules = heads + (size_t)n * sizeof(bdb_head_t);
    int      total = 0;
    for (int i = 0; i < n; i++) {
        bdb_head_t h;
        head_from(&h, &b[i]);
        memcpy(heads + (size_t)i * sizeof(h), &h, sizeof(h));
        const int t = button_db_rule_total(&b[i]);
        memcpy(rules + (size_t)total * sizeof(zbc_btn_rule_t), b[i].rules, (size_t)t * sizeof(zbc_btn_rule_t));
        total += t;
    }
    button_blob_header_t hdr = { .version = BUTTON_BLOB_VERSION, .count = (uint16_t)n, .rules = (uint16_t)total };
    memcpy(out, &hdr, sizeof(hdr));
    return (size_t)(rules - out) + (size_t)total * sizeof(zbc_btn_rule_t);
}

/* Every button of a version-6 table keeps every rule, in its gesture, with its
 * delay, and a sensor its no-motion time. */
static void test_a_v6_table_converts_whole(void)
{
    zbc_button_t b[3] = { button(1, 2, 1, 0, 0), button(2, 0, 0, 0, 0), button(5, 1, 1, 0, 0) };
    b[2].src     = ZBC_BTN_SRC_SENSOR;
    b[2].clear_s = 120;
    uint8_t blob[2048];
    const size_t len = v6_blob(blob, b, 3);
    ok(button_db_blob_ok(blob, len), "a version-6 blob is taken");
    ok(!button_db_blob_ok(blob, len - 1), "but not one a byte short");

    reset();
    zbc_button_t scratch;
    ok(table_load(blob, &scratch) == 0, "nothing dropped");
    ok(s_table.hdr.count == 3 && s_table.hdr.rules == 5, "three buttons, five rules");
    ok(same(1, &b[0]) && same(2, &b[1]) && same(5, &b[2]), "each as it was");
    ok(pool_adds_up(), "and the pool adds up");
}

/* A full button of twelve converts too: the pool takes as many as one
 * button could hold before it. */
static void test_a_full_v6_button(void)
{
    zbc_button_t b[2] = { button(1, 3, 3, 3, 3), button(2, 1, 0, 0, 0) };
    uint8_t blob[2048];
    const size_t len = v6_blob(blob, b, 2);
    reset();
    zbc_button_t scratch;
    ok(button_db_blob_ok(blob, len) && table_load(blob, &scratch) == 0, "both fit");
    ok(same(1, &b[0]) && same(2, &b[1]), "twelve rules and one, in place");
}

/* A version-6 record whose counts run past twelve is damaged: it keeps its
 * name and source and loses its rules, and the next one is not affected. */
static void test_a_v6_record_with_too_many(void)
{
    zbc_button_t b[2] = { button(1, 3, 3, 3, 3), button(2, 1, 0, 0, 0) };
    b[0].rule_count[0] = 4;            /* 13 in all */
    uint8_t blob[2048];
    v6_blob(blob, b, 2);
    reset();
    zbc_button_t scratch;
    table_load(blob, &scratch);
    zbc_button_t got;
    copy_out(find_index(1), &got);
    ok(strcmp(got.name, "button 1") == 0 && button_db_rule_total(&got) == 0, "name kept, rules gone");
    ok(same(2, &b[1]), "the next one intact");
}

/* A version-5 rule had no delay: it gets none, and keeps the rest. */
static void test_a_v5_table(void)
{
    button_blob_header_t hdr = { .version = 5, .count = 1 };
    zbc_button_v5_t r;
    memset(&r, 0, sizeof(r));
    r.in_use = true;
    r.id = 7;
    strcpy(r.name, "old");
    r.gpio = 4;
    r.rule_count[0] = 1;
    r.rule_count[1] = 1;
    r.rules[0] = (zbc_btn_rule_v5_t){ .group_id = 3, .action = ZBC_ACT_TOGGLE };
    r.rules[1] = (zbc_btn_rule_v5_t){ .action = ZBC_ACT_DIM_CYCLE, .action_arg = 10, .flags = 0, .ep = 11,
                                      .ieee = { 1, 2, 3, 4, 5, 6, 7, 8 } };
    uint8_t blob[512];
    memcpy(blob, &hdr, sizeof(hdr));
    memcpy(blob + sizeof(hdr), &r, sizeof(r));
    ok(button_db_blob_ok(blob, sizeof(hdr) + sizeof(r)), "a version-5 blob is taken");

    reset();
    zbc_button_t got;
    table_load(blob, &got);
    copy_out(find_index(7), &got);
    ok(got.gpio == 4 && got.rule_count[0] == 1 && got.rule_count[1] == 1, "wiring and counts");
    ok(got.rules[0].group_id == 3 && got.rules[0].action == ZBC_ACT_TOGGLE && got.rules[0].delay_s == 0,
       "the room rule, at once");
    ok(got.rules[1].ep == 11 && got.rules[1].ieee[7] == 8 && got.rules[1].action_arg == 10,
       "the light rule");
}

/* What the save writes is the table less what is not in use, and loads back
 * as it was; the table itself is as it was after the write. */
static void test_the_save_round_trips(void)
{
    zbc_button_t b[3] = { button(1, 2, 1, 0, 0), button(2, 0, 0, 0, 0), button(3, 1, 0, 1, 0) };
    reset();
    for (int i = 0; i < 3; i++) {
        append_locked(&b[i]);
    }
    ok(table_write(capture, NULL) == 0, "written");
    ok(s_out_len == sizeof(button_blob_header_t) + 3 * sizeof(bdb_head_t) + 5 * sizeof(zbc_btn_rule_t),
       "header, three heads and five rules, nothing else");
    ok(button_db_blob_ok(s_out, s_out_len), "and its own load takes it");
    ok(same(1, &b[0]) && same(2, &b[1]) && same(3, &b[2]), "the table unchanged by the write");

    reset();
    zbc_button_t scratch;
    ok(table_load(s_out, &scratch) == 0, "loads whole");
    ok(same(1, &b[0]) && same(2, &b[1]) && same(3, &b[2]), "as it was");

    /* And with every head in use there is no gap to close. */
    zbc_button_t d = button(4, 0, 0, 0, 2);
    append_locked(&d);
    table_write(capture, NULL);
    reset();
    table_load(s_out, &scratch);
    ok(s_table.hdr.count == 4 && same(3, &b[2]) && same(4, &d), "a full table too");

    /* And an empty one. */
    reset();
    table_write(capture, NULL);
    ok(s_out_len == sizeof(button_blob_header_t) && button_db_blob_ok(s_out, s_out_len), "an empty table");
}

/* Changing one button's rules moves the ones behind it, never into them. */
static void test_set_moves_the_rest(void)
{
    zbc_button_t b[3] = { button(1, 2, 1, 0, 0), button(2, 0, 0, 0, 0), button(3, 1, 0, 1, 0) };
    reset();
    for (int i = 0; i < 3; i++) {
        append_locked(&b[i]);
    }
    zbc_button_t grown = button(2, 1, 2, 0, 0);
    ok(table_set(find_index(2), &grown), "the middle one grows");
    ok(same(1, &b[0]) && same(2, &grown) && same(3, &b[2]), "the others as they were");
    ok(s_table.hdr.rules == 8 && pool_adds_up(), "eight rules");

    zbc_button_t shrunk = button(1, 1, 0, 0, 0);
    ok(table_set(find_index(1), &shrunk), "the first shrinks");
    ok(same(1, &shrunk) && same(2, &grown) && same(3, &b[2]), "the others moved down whole");
    ok(s_table.hdr.rules == 6 && pool_adds_up(), "six rules");

    /* The id is the table's: a record that says otherwise does not move it. */
    zbc_button_t other = button(9, 0, 1, 0, 0);
    table_set(find_index(3), &other);
    ok(find_index(3) == 2 && find_index(9) < 0, "the id stays");
}

/* A full pool refuses the rule that does not fit, and changes nothing. */
static void test_a_full_pool(void)
{
    zbc_button_t b[3] = { button(1, 3, 3, 3, 3), button(2, 3, 2, 0, 0), button(3, 1, 0, 0, 0) };
    reset();
    for (int i = 0; i < 3; i++) {
        ok(append_locked(&b[i]), "fits");
    }
    ok(s_table.hdr.rules == 18, "eighteen of twenty");
    zbc_button_t big = button(2, 3, 3, 1, 0);   /* 7: two more than its 5, and 20 is the most */
    ok(table_set(find_index(2), &big), "up to the last rule");
    ok(s_table.hdr.rules == 20, "the pool full");
    zbc_button_t more = button(3, 2, 0, 0, 0);
    ok(!table_set(find_index(3), &more), "one more is refused");
    ok(same(1, &b[0]) && same(2, &big) && same(3, &b[2]) && s_table.hdr.rules == 20, "and nothing moved");
    zbc_button_t d = button(4, 0, 0, 0, 0);
    ok(append_locked(&d), "a button with no rules still fits");
    zbc_button_t fewer = button(2, 1, 0, 0, 0);
    ok(table_set(find_index(2), &fewer) && table_set(find_index(3), &more), "room again once one has fewer");
    ok(same(1, &b[0]) && same(2, &fewer) && same(3, &more) && same(4, &d) && pool_adds_up(), "each its own");
}

/* Removing a button takes its rules with it and leaves the others theirs. */
static void test_remove(void)
{
    zbc_button_t b[3] = { button(1, 2, 1, 0, 0), button(2, 1, 1, 1, 0), button(3, 1, 0, 1, 0) };
    reset();
    for (int i = 0; i < 3; i++) {
        append_locked(&b[i]);
    }
    table_remove(find_index(2));
    ok(s_table.hdr.count == 2 && find_index(2) < 0, "gone");
    ok(same(1, &b[0]) && same(3, &b[2]) && s_table.hdr.rules == 5 && pool_adds_up(), "the others whole");
    table_remove(find_index(3));
    table_remove(find_index(1));
    ok(s_table.hdr.count == 0 && s_table.hdr.rules == 0, "and the last ones");
}

/* A table from a build with more room keeps the buttons that fit here, in
 * order, and each keeps its own rules. */
static void test_a_bigger_table(void)
{
    zbc_button_t b[6] = { button(1, 1, 0, 0, 0), button(2, 3, 3, 3, 3), button(3, 3, 3, 0, 0),
                          button(4, 0, 0, 0, 0), button(5, 1, 0, 0, 0), button(6, 1, 0, 0, 0) };
    uint8_t blob[4096];
    const size_t len = v7_blob(blob, b, 6);
    ok(button_db_blob_ok(blob, len), "taken");
    reset();
    zbc_button_t scratch;
    /* 1 (1), 2 (12), 3 would make 19 and fits, 4 (0) fits; 5 and 6: no head left. */
    ok(table_load(blob, &scratch) == 2, "two dropped");
    ok(same(1, &b[0]) && same(2, &b[1]) && same(3, &b[2]) && same(4, &b[3]), "the first four whole");

    /* Out of rules rather than buttons: the one that does not fit is left out
     * and a smaller one behind it still comes in. */
    zbc_button_t c[3] = { button(1, 3, 3, 3, 3), button(2, 3, 3, 3, 0), button(3, 2, 0, 0, 0) };
    v7_blob(blob, c, 3);
    reset();
    ok(table_load(blob, &scratch) == 1, "one dropped");
    ok(same(1, &c[0]) && find_index(2) < 0 && same(3, &c[2]), "the one that fits after it is kept");
}

/* A button with more rules than this build allows keeps its name and source;
 * the buttons behind it still find their own rules. */
static void test_a_button_with_more_rules_than_here(void)
{
    uint8_t blob[4096];
    /* Built by hand: a button of 14 is more than zbc_button_t holds here. */
    bdb_head_t h[2];
    memset(h, 0, sizeof(h));
    h[0].id = 1;
    strcpy(h[0].name, "big");
    h[0].rule_count[0] = 14;
    h[1].id = 2;
    strcpy(h[1].name, "small");
    h[1].rule_count[1] = 1;
    button_blob_header_t hdr = { .version = BUTTON_BLOB_VERSION, .count = 2, .rules = 15 };
    uint8_t *p = blob;
    memcpy(p, &hdr, sizeof(hdr));
    p += sizeof(hdr);
    memcpy(p, h, sizeof(h));
    p += sizeof(h);
    for (int k = 0; k < 15; k++) {
        zbc_btn_rule_t r = mark(k < 14 ? 1 : 2, k);
        memcpy(p, &r, sizeof(r));
        p += sizeof(r);
    }
    ok(button_db_blob_ok(blob, (size_t)(p - blob)), "taken");
    reset();
    zbc_button_t got;
    table_load(blob, &got);
    copy_out(find_index(1), &got);
    ok(strcmp(got.name, "big") == 0 && button_db_rule_total(&got) == 0, "the big one keeps its name");
    copy_out(find_index(2), &got);
    ok(got.rule_count[1] == 1 && got.rules[0].group_id == 2 * 100 + 14, "the next one its own rule");
}

/* Counts that do not add up to the rules stored would hand every button the
 * wrong ones: such a blob is refused, as is one of the wrong length. */
static void test_counts_must_add_up(void)
{
    zbc_button_t b[2] = { button(1, 2, 0, 0, 0), button(2, 1, 0, 0, 0) };
    uint8_t blob[2048];
    const size_t len = v7_blob(blob, b, 2);
    ok(button_db_blob_ok(blob, len), "as written");
    ok(!button_db_blob_ok(blob, len + 18) && !button_db_blob_ok(blob, len - 18), "a rule more or less");
    button_blob_header_t hdr;
    memcpy(&hdr, blob, sizeof(hdr));
    hdr.rules = 2;
    memcpy(blob, &hdr, sizeof(hdr));
    ok(!button_db_blob_ok(blob, len - 18), "counts of three and two rules stored");
    hdr.version = 8;
    memcpy(blob, &hdr, sizeof(hdr));
    ok(!button_db_blob_ok(blob, len), "a version from the future");
}

/* The restore's line has to fit the largest table any build before the pool
 * could write, as well as this one's. */
static void test_blob_max(void)
{
    ok(button_db_blob_max() >= sizeof(button_blob_header_t) + 16 * sizeof(zbc_button_v6_t), "a full version 6");
    ok(button_db_blob_max() >= sizeof(button_blob_header_t) + sizeof(s_table.head) + sizeof(s_table.rule),
       "a full table here");
}

int main(void)
{
    test_a_v6_table_converts_whole();
    test_a_full_v6_button();
    test_a_v6_record_with_too_many();
    test_a_v5_table();
    test_the_save_round_trips();
    test_set_moves_the_rest();
    test_a_full_pool();
    test_remove();
    test_a_bigger_table();
    test_a_button_with_more_rules_than_here();
    test_counts_must_add_up();
    test_blob_max();
    printf("all %d checks passed\n", s_checks);
    return 0;
}
