/* Host test for how a button gesture's rules are chosen, in buttons.c.
 *
 * A gesture's rules read as cases: a rule and the rules joined to it with
 * "and". The first case whose head's condition holds runs whole. Wrong here, a
 * button does the wrong half of what it was told - on a wall, and only in the
 * state that picks the case that is wrong.
 *
 * clause.inc is extracted from main/buttons.c by the Makefile, never copied.
 *
 *   make -C test
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZBC_RULE_AND 0x01

typedef enum {
    ZBC_COND_ALWAYS = 0, ZBC_COND_OFF, ZBC_COND_ON, ZBC_COND_BELOW, ZBC_COND_ABOVE,
} zbc_btn_cond_t;

typedef enum {
    ZBC_ACT_NONE = 0, ZBC_ACT_ON, ZBC_ACT_OFF, ZBC_ACT_TOGGLE,
    ZBC_ACT_SET, ZBC_ACT_DIM_UP, ZBC_ACT_DIM_DOWN, ZBC_ACT_DIM_CYCLE, ZBC_ACT_ADAPTIVE,
} zbc_btn_action_t;

typedef enum { ZB_ONOFF_OFF = 0, ZB_ONOFF_ON = 1, ZB_ONOFF_TOGGLE = 2 } zb_onoff_cmd_t;

typedef struct {
    uint16_t group_id;
    uint8_t  cond, cond_arg, action, action_arg, flags, ep;
    uint8_t  ieee[8];
} zbc_btn_rule_t;

static bool button_db_rule_assigned(const zbc_btn_rule_t *r) { return r->ep != 0 || r->group_id != 0; }

#include "clause.inc"

/* ---- the lamps, stubbed ---- */

/* Rooms 1-7; which are lit decides every condition here. */
static bool s_lit[8];
static int  s_asked;

/* A condition: "on" and "off" against the room's state, everything else holds. */
static bool holds(const zbc_btn_rule_t *r)
{
    s_asked++;
    switch (r->cond) {
    case ZBC_COND_ON:  return s_lit[r->group_id];
    case ZBC_COND_OFF: return !s_lit[r->group_id];
    default:           return true;
    }
}

static bool lit(const zbc_btn_rule_t *r)
{
    s_asked++;
    return s_lit[r->group_id];
}

static int s_checks;
static void ok(bool cond, const char *what)
{
    s_checks++;
    if (!cond) { printf("FAIL: %s\n", what); exit(1); }
}

static zbc_btn_rule_t rule(int room, uint8_t cond, uint8_t action, bool joined)
{
    return (zbc_btn_rule_t){ .group_id = (uint16_t)room, .cond = cond, .action = action,
                             .flags = joined ? ZBC_RULE_AND : 0 };
}

static void reset(void)
{
    memset(s_lit, 0, sizeof(s_lit));
    s_asked = 0;
}

/* ---- choosing the case ---- */

/* One rule, as every button stored before "and" is: it is its own case. */
static void test_a_lone_rule_is_a_case(void)
{
    reset();
    const zbc_btn_rule_t r[] = { rule(1, ZBC_COND_ALWAYS, ZBC_ACT_TOGGLE, false) };
    int end = -1;
    ok(pick_case(r, 1, holds, &end) == 0 && end == 1, "the rule alone runs");
}

/* The first case whose head holds wins, and runs with everything joined to it -
 * and nothing past its end. */
static void test_the_first_case_that_holds_runs_whole(void)
{
    reset();
    const zbc_btn_rule_t r[] = {
        rule(1, ZBC_COND_OFF, ZBC_ACT_ON, false),
        rule(2, ZBC_COND_ALWAYS, ZBC_ACT_SET, true),
        rule(1, ZBC_COND_ALWAYS, ZBC_ACT_OFF, false),
        rule(2, ZBC_COND_ALWAYS, ZBC_ACT_OFF, true),
    };
    int end = -1;
    ok(pick_case(r, 4, holds, &end) == 0 && end == 2, "room 1 off: the first case, both of its rules");

    s_lit[1] = true;
    ok(pick_case(r, 4, holds, &end) == 2 && end == 4, "room 1 on: the second case, both of its rules");
}

/* A joined rule's own condition is never asked: the head decided for it. */
static void test_a_joined_rule_s_condition_is_not_read(void)
{
    reset();
    const zbc_btn_rule_t r[] = {
        rule(1, ZBC_COND_ALWAYS, ZBC_ACT_ON, false),
        rule(2, ZBC_COND_ON, ZBC_ACT_ON, true),       /* room 2 is off, and it runs */
    };
    int end = -1;
    ok(pick_case(r, 2, holds, &end) == 0 && end == 2, "the joined rule runs whatever its own condition says");
    ok(s_asked == 0, "and nothing was asked: an always head needs no answer");
}

/* The old if / else-if / otherwise: with no "and" anywhere nothing changes. */
static void test_otherwise_still_means_otherwise(void)
{
    reset();
    const zbc_btn_rule_t r[] = {
        rule(1, ZBC_COND_ON, ZBC_ACT_OFF, false),
        rule(1, ZBC_COND_ALWAYS, ZBC_ACT_ON, false),
    };
    int end = -1;
    ok(pick_case(r, 2, holds, &end) == 1 && end == 2, "off: the otherwise rule, alone");
    s_lit[1] = true;
    ok(pick_case(r, 2, holds, &end) == 0 && end == 1, "on: the first rule, alone");
}

/* A case with nothing to do - its targets deleted, or only "do nothing" - is
 * passed over, as a lone unassigned rule always was, rather than swallowing the
 * press. */
static void test_a_dead_case_is_passed_over(void)
{
    reset();
    const zbc_btn_rule_t r[] = {
        rule(0, ZBC_COND_ALWAYS, ZBC_ACT_TOGGLE, false),
        rule(0, ZBC_COND_ALWAYS, ZBC_ACT_ON, true),
        rule(3, ZBC_COND_ALWAYS, ZBC_ACT_NONE, false),
        rule(4, ZBC_COND_ALWAYS, ZBC_ACT_TOGGLE, false),
    };
    int end = -1;
    ok(pick_case(r, 4, holds, &end) == 3 && end == 4, "the first case with something to do runs");
}

/* An always head whose room was deleted still brings the rules joined to it. */
static void test_an_always_head_without_a_target_still_runs_its_case(void)
{
    reset();
    const zbc_btn_rule_t r[] = {
        rule(0, ZBC_COND_ALWAYS, ZBC_ACT_TOGGLE, false),
        rule(2, ZBC_COND_ALWAYS, ZBC_ACT_TOGGLE, true),
    };
    int end = -1;
    ok(pick_case(r, 2, holds, &end) == 0 && end == 2, "the case runs for the rule that still has a target");
}

/* ...but any other condition needs a target to be asked of. */
static void test_a_state_condition_needs_a_target(void)
{
    reset();
    const zbc_btn_rule_t r[] = {
        rule(0, ZBC_COND_OFF, ZBC_ACT_ON, false),
        rule(2, ZBC_COND_ALWAYS, ZBC_ACT_ON, true),
        rule(3, ZBC_COND_ALWAYS, ZBC_ACT_OFF, false),
    };
    int end = -1;
    ok(pick_case(r, 3, holds, &end) == 2 && end == 3, "a condition on nothing does not hold");
    ok(s_asked == 0, "and nothing is asked about a target that is not there");
}

/* Nothing holds: nothing runs. */
static void test_nothing_holds(void)
{
    reset();
    s_lit[1] = true;
    const zbc_btn_rule_t r[] = { rule(1, ZBC_COND_OFF, ZBC_ACT_ON, false),
                                 rule(2, ZBC_COND_ALWAYS, ZBC_ACT_ON, true) };
    int end = -1;
    ok(pick_case(r, 2, holds, &end) == -1, "no case");
    ok(pick_case(r, 0, holds, &end) == -1, "and an empty gesture has none either");
}

/* case_end is what a held button uses to find its case again on every repeat. */
static void test_a_case_ends_at_the_next_head(void)
{
    const zbc_btn_rule_t r[] = {
        rule(1, ZBC_COND_ALWAYS, ZBC_ACT_ON, false),
        rule(2, ZBC_COND_ALWAYS, ZBC_ACT_ON, true),
        rule(3, ZBC_COND_ALWAYS, ZBC_ACT_ON, true),
        rule(4, ZBC_COND_ALWAYS, ZBC_ACT_ON, false),
    };
    ok(case_end(r, 4, 0) == 3, "three rules in the first case");
    ok(case_end(r, 4, 3) == 4, "and the last runs to the end");
}

/* ---- toggles ---- */

/* One toggle is its target's own business. */
static void test_one_toggle_is_left_to_its_target(void)
{
    reset();
    s_lit[1] = true;
    const zbc_btn_rule_t r[] = { rule(1, ZBC_COND_ALWAYS, ZBC_ACT_TOGGLE, false),
                                 rule(2, ZBC_COND_ALWAYS, ZBC_ACT_ON, true) };
    ok(case_toggle(r, 2, lit) == ZB_ONOFF_TOGGLE, "a plain toggle");
    ok(s_asked == 0, "without asking anything");
}

/* Several are decided together: any lit, all off. Toggled one by one, a
 * half-lit pair would swap halves. */
static void test_toggles_are_decided_together(void)
{
    reset();
    const zbc_btn_rule_t r[] = { rule(1, ZBC_COND_ALWAYS, ZBC_ACT_TOGGLE, false),
                                 rule(2, ZBC_COND_ALWAYS, ZBC_ACT_TOGGLE, true) };
    ok(case_toggle(r, 2, lit) == ZB_ONOFF_ON, "both dark: both on");
    s_lit[2] = true;
    ok(case_toggle(r, 2, lit) == ZB_ONOFF_OFF, "one lit: both off");
    s_lit[1] = true;
    ok(case_toggle(r, 2, lit) == ZB_ONOFF_OFF, "both lit: both off");
}

/* A toggle with no target does not count, and is not asked about. */
static void test_an_unassigned_toggle_does_not_count(void)
{
    reset();
    const zbc_btn_rule_t r[] = { rule(0, ZBC_COND_ALWAYS, ZBC_ACT_TOGGLE, false),
                                 rule(2, ZBC_COND_ALWAYS, ZBC_ACT_TOGGLE, true) };
    ok(case_toggle(r, 2, lit) == ZB_ONOFF_TOGGLE, "one real toggle is still one");
}

/* Only the toggles' own targets decide: a lit room turned on beside them does
 * not make the toggles go off. */
static void test_only_the_toggles_targets_decide(void)
{
    reset();
    s_lit[3] = true;
    const zbc_btn_rule_t r[] = { rule(1, ZBC_COND_ALWAYS, ZBC_ACT_TOGGLE, false),
                                 rule(2, ZBC_COND_ALWAYS, ZBC_ACT_TOGGLE, true),
                                 rule(3, ZBC_COND_ALWAYS, ZBC_ACT_ON, true) };
    ok(case_toggle(r, 3, lit) == ZB_ONOFF_ON, "the lit room beside them is not asked");
}

int main(void)
{
    test_a_lone_rule_is_a_case();
    test_the_first_case_that_holds_runs_whole();
    test_a_joined_rule_s_condition_is_not_read();
    test_otherwise_still_means_otherwise();
    test_a_dead_case_is_passed_over();
    test_an_always_head_without_a_target_still_runs_its_case();
    test_a_state_condition_needs_a_target();
    test_nothing_holds();
    test_a_case_ends_at_the_next_head();
    test_one_toggle_is_left_to_its_target();
    test_toggles_are_decided_together();
    test_an_unassigned_toggle_does_not_count();
    test_only_the_toggles_targets_decide();
    printf("all %d checks passed\n", s_checks);
    return 0;
}
