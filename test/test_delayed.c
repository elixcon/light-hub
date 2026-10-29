/* Host test for the table of waiting actions in buttons.c.
 *
 * "And off after ten minutes" is a rule copied into this table when its event
 * runs, taken out when it is due, and dropped when the same button's next event
 * comes first. Wrong here, a room goes dark while somebody still moves in it,
 * or a cancelled "off" turns up anyway.
 *
 * delayed.inc is extracted from main/buttons.c by the Makefile, never copied.
 *
 *   make -C test
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The rule, from button_db.h. */
#define CONFIG_ZBC_BUTTON_RULES 12
#include "bdb-types.inc"

#include "delayed.inc"

static int s_checks;
static void ok(bool cond, const char *what)
{
    s_checks++;
    if (!cond) { printf("FAIL: %s\n", what); exit(1); }
}

static zbc_btn_rule_t room(int g)
{
    return (zbc_btn_rule_t){ .group_id = (uint16_t)g };
}

/* Due ones come out in order of when they are due, and not before. */
static void test_they_come_out_when_due(void)
{
    zbc_delayed_table_t t;
    memset(&t, 0, sizeof(t));
    zbc_btn_rule_t a = room(1), b = room(2), out;
    uint16_t id;
    ok(delayed_next_due(&t) == 0, "nothing waits");
    ok(delayed_add(&t, 7, &a, 500), "one in");
    ok(delayed_add(&t, 8, &b, 200), "two in");
    ok(delayed_next_due(&t) == 200, "the next is the earlier");
    ok(!delayed_take_due(&t, 199, &id, &out), "not yet");
    ok(delayed_take_due(&t, 200, &id, &out) && id == 8 && out.group_id == 2, "the earlier first");
    ok(!delayed_take_due(&t, 499, &id, &out), "the later one not yet");
    ok(delayed_take_due(&t, 1000, &id, &out) && id == 7 && out.group_id == 1, "then the later");
    ok(delayed_next_due(&t) == 0, "and the table is empty");
}

/* A case's rules with the same delay run in the order they are written, even
 * when an earlier slot was freed in between and the later rule landed in it. */
static void test_a_tie_keeps_the_order_they_were_scheduled_in(void)
{
    zbc_delayed_table_t t;
    memset(&t, 0, sizeof(t));
    zbc_btn_rule_t r1 = room(1), r2 = room(2), r3 = room(3), out;
    uint16_t id;
    delayed_add(&t, 1, &r1, 50);
    delayed_add(&t, 2, &r2, 100);        /* slot 1 */
    delayed_cancel(&t, 1);               /* slot 0 is free again */
    delayed_add(&t, 3, &r3, 100);        /* slot 0, scheduled after r2 */
    ok(delayed_take_due(&t, 100, &id, &out) && out.group_id == 2, "the one scheduled first runs first");
    ok(delayed_take_due(&t, 100, &id, &out) && out.group_id == 3, "then the other");
}

/* Cancelling is by button: another button's waiting actions are left. */
static void test_cancel_is_per_button(void)
{
    zbc_delayed_table_t t;
    memset(&t, 0, sizeof(t));
    zbc_btn_rule_t r = room(1), out;
    uint16_t id;
    int64_t  first = 0;
    delayed_add(&t, 4, &r, 300);
    delayed_add(&t, 4, &r, 100);
    delayed_add(&t, 5, &r, 200);
    ok(delayed_of(&t, 4, &first) == 2 && first == 100, "button 4 has two, the first at 100");
    ok(delayed_cancel(&t, 4) == 2, "both dropped");
    ok(delayed_of(&t, 4, &first) == 0, "none left");
    ok(delayed_take_due(&t, 1000, &id, &out) && id == 5, "button 5's is still there");
    ok(delayed_cancel(&t, 0) == 0, "id 0 is never a button");
}

/* A full table refuses one more and keeps what it has. */
static void test_a_full_table_refuses(void)
{
    zbc_delayed_table_t t;
    memset(&t, 0, sizeof(t));
    zbc_btn_rule_t r = room(1);
    for (int i = 0; i < ZBC_DELAYED_MAX; i++) {
        ok(delayed_add(&t, (uint16_t)(1 + i % 3), &r, 100 + i), "fits");
    }
    ok(!delayed_add(&t, 9, &r, 1), "one more is refused");
    ok(delayed_next_due(&t) == 100, "and nothing was pushed out");
    delayed_cancel(&t, 2);
    ok(delayed_add(&t, 9, &r, 1), "room again once a button's are dropped");
}

int main(void)
{
    test_they_come_out_when_due();
    test_a_tie_keeps_the_order_they_were_scheduled_in();
    test_cancel_is_per_button();
    test_a_full_table_refuses();
    printf("all %d checks passed\n", s_checks);
    return 0;
}
