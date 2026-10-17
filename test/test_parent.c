/* Which device an end device hangs off, lifted from main/zb_coordinator.c
 * (region `parent`).
 *
 * The answer is assembled from two tables that disagree for a while after
 * every move: our own, where the device polls, and each router's, which keeps
 * a child it has lost until its own aging clears it. Wrong one way, a remote
 * that is ours shows as a lamp's - and the warning to re-pair it is a false
 * alarm. Wrong the other way, a remote that has drifted to a lamp shows as
 * ours, and its lost presses go unexplained, which is the fault this exists
 * to catch.
 *
 *   make -C test
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "parent.inc"

static int s_checks;

static void ok(bool cond, const char *what)
{
    s_checks++;
    if (!cond) { printf("FAIL: %s\n", what); exit(1); }
}

#define HUE  0x046b
#define LAMP 0x1a2b

static void test_a_new_row_knows_nothing(void)
{
    zbc_parent_t p = { 0 };
    ok(parent_addr(&p) == ZBC_PARENT_UNKNOWN, "a zeroed row is not a child of ours");
    ok(!parent_walked(&p, false), "a walk not listing it changes nothing");
    ok(parent_addr(&p) == ZBC_PARENT_UNKNOWN, "and it is still not known");
}

static void test_our_table_makes_it_ours(void)
{
    zbc_parent_t p = { 0 };
    ok(parent_walked(&p, true), "listed by our walk: a change");
    ok(parent_addr(&p) == ZBC_PARENT_US, "ours");
    ok(!parent_walked(&p, true), "listed again: no change, nothing logged twice");
}

/* The router has not aged its old entry out yet. */
static void test_a_stale_router_entry_does_not_steal_it(void)
{
    zbc_parent_t p = { 0 };
    parent_walked(&p, true);
    ok(!parent_claimed(&p, HUE), "a router listing our child is not believed");
    ok(parent_addr(&p) == ZBC_PARENT_US, "still ours");
}

/* The drift itself: ours at one walk, gone at the next, the lamp's after it. */
static void test_drifting_to_a_lamp(void)
{
    zbc_parent_t p = { 0 };
    parent_walked(&p, true);
    ok(parent_walked(&p, false), "no longer listed: a change");
    ok(parent_addr(&p) == ZBC_PARENT_UNKNOWN, "and not known where it went");
    ok(parent_claimed(&p, HUE), "the lamp's table then lists it");
    ok(parent_addr(&p) == HUE, "the lamp's");
}

/* A router's answer is lost: the next walk must not orphan the device. */
static void test_a_router_claim_outlives_our_walks(void)
{
    zbc_parent_t p = { 0 };
    parent_claimed(&p, HUE);
    ok(!parent_walked(&p, false), "our walk not listing it says nothing about theirs");
    ok(parent_addr(&p) == HUE, "still the lamp's");
    ok(!parent_claimed(&p, HUE), "the same claim again: no change");
    ok(parent_claimed(&p, LAMP), "another lamp: a change");
    ok(parent_addr(&p) == LAMP, "that lamp's");
}

/* Re-paired next to the board: back to us, over the lamp's claim. */
static void test_coming_home(void)
{
    zbc_parent_t p = { 0 };
    parent_claimed(&p, HUE);
    ok(parent_walked(&p, true), "listed by our walk: a change");
    ok(parent_addr(&p) == ZBC_PARENT_US, "ours again");
    ok(!parent_claimed(&p, HUE), "and the lamp's stale entry is ignored");
}

static void test_leaving(void)
{
    zbc_parent_t p = { 0 };
    parent_walked(&p, true);
    ok(parent_left(&p), "a child leaving: a change");
    ok(parent_addr(&p) == ZBC_PARENT_UNKNOWN, "no parent until a table says");
    ok(parent_claimed(&p, HUE), "a router may now claim it, before our next walk");
    ok(parent_addr(&p) == HUE, "the lamp's");

    zbc_parent_t q = { 0 };
    ok(!parent_left(&q), "leaving while not known is not a change of parent");
}

static void test_nonsense_claims(void)
{
    zbc_parent_t p = { 0 };
    ok(!parent_claimed(&p, ZBC_PARENT_US), "a router claim naming us is not one");
    ok(!parent_claimed(&p, ZBC_PARENT_UNKNOWN), "nor one naming the broadcast address");
    ok(parent_addr(&p) == ZBC_PARENT_UNKNOWN, "still not known");
}

int main(void)
{
    test_a_new_row_knows_nothing();
    test_our_table_makes_it_ours();
    test_a_stale_router_entry_does_not_steal_it();
    test_drifting_to_a_lamp();
    test_a_router_claim_outlives_our_walks();
    test_coming_home();
    test_leaving();
    test_nonsense_claims();
    printf("all %d checks passed\n", s_checks);
    return 0;
}
