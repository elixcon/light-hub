/* The code addresses a crash record keeps from the stack, lifted from
 * main/crash_log.c (region `crash-scan`).
 *
 * This is the backtrace of a crash nobody saw happen: the record is read days
 * later, against an ELF, by someone trying to find out why the lights went out.
 * A scan that lets data through sends them after functions that were never
 * running; one that drops a return address, or stops one word early, loses the
 * caller that would have explained it. Neither is visible in the record itself.
 *
 *   make -C test
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "crash-scan.inc"

static int s_checks;

static void ok(bool cond, const char *what)
{
    s_checks++;
    if (!cond) { printf("FAIL: %s\n", what); exit(1); }
}

/* Flash code and IRAM code, as on the C6. */
static const crash_range_t CODE[] = {
    { 0x42000020u, 0x421a0000u },
    { 0x40800000u, 0x40816000u },
};
#define N_CODE (sizeof(CODE) / sizeof(CODE[0]))

static void test_only_code_addresses_are_kept_in_stack_order(void)
{
    const uint32_t stack[] = {
        0x00000000u,   /* a zero */
        0x4200a1b2u,   /* a return address in flash */
        0x40823000u,   /* a pointer to a heap block: RAM, but not code */
        0x00000040u,   /* a length */
        0x40801a2cu,   /* a return address in IRAM */
        0x4210ff00u,   /* another in flash */
    };
    uint32_t out[8];
    size_t   n = crash_scan(stack, 6, CODE, N_CODE, out, 8);
    ok(n == 3, "three code addresses");
    ok(out[0] == 0x4200a1b2u, "nearest the stack pointer first");
    ok(out[1] == 0x40801a2cu, "IRAM code counts");
    ok(out[2] == 0x4210ff00u, "and the furthest last");
}

static void test_an_odd_value_is_data(void)
{
    /* Instructions are 2-byte aligned, so an odd value inside the code range is
     * a flag word or a byte count, never a return address. */
    const uint32_t stack[] = { 0x4200a1b3u, 0x4200a1b2u };
    uint32_t out[4];
    size_t   n = crash_scan(stack, 2, CODE, N_CODE, out, 4);
    ok(n == 1 && out[0] == 0x4200a1b2u, "the odd one is passed over");
}

static void test_the_range_is_half_open(void)
{
    const uint32_t stack[] = { 0x4200001eu, 0x42000020u, 0x4219fffeu, 0x421a0000u };
    uint32_t out[4];
    size_t   n = crash_scan(stack, 4, CODE, N_CODE, out, 4);
    ok(n == 2, "the start is in, the end is out");
    ok(out[0] == 0x42000020u && out[1] == 0x4219fffeu, "the two inside");
}

static void test_a_repeat_is_kept_once(void)
{
    /* The same return address saved twice in a row (a frame and the copy a callee
     * made of ra) is one call, not two. The same address further up the stack,
     * after another, is a second call and stays. */
    const uint32_t stack[] = { 0x4200a1b2u, 0x4200a1b2u, 0x00000007u, 0x4200a1b2u,
                               0x42000100u, 0x4200a1b2u };
    uint32_t out[8];
    size_t   n = crash_scan(stack, 6, CODE, N_CODE, out, 8);
    ok(n == 3, "a repeat collapses, a later return does not");
    ok(out[0] == 0x4200a1b2u && out[1] == 0x42000100u && out[2] == 0x4200a1b2u, "in order");
}

static void test_the_scan_stops_at_max_and_at_n(void)
{
    const uint32_t stack[] = { 0x42000100u, 0x42000200u, 0x42000300u, 0x42000400u };
    uint32_t out[4] = { 0, 0, 0, 0xdeadbeefu };
    size_t   n = crash_scan(stack, 4, CODE, N_CODE, out, 3);
    ok(n == 3, "no more than max");
    ok(out[3] == 0xdeadbeefu, "and nothing written past it");
    n = crash_scan(stack, 2, CODE, N_CODE, out, 4);
    ok(n == 2 && out[1] == 0x42000200u, "no word read past n");
    ok(crash_scan(stack, 0, CODE, N_CODE, out, 4) == 0, "an empty stack has nothing");
}

int main(void)
{
    test_only_code_addresses_are_kept_in_stack_order();
    test_an_odd_value_is_data();
    test_the_range_is_half_open();
    test_a_repeat_is_kept_once();
    test_the_scan_stops_at_max_and_at_n();
    printf("all %d checks passed\n", s_checks);
    return 0;
}
