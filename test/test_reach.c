/* Whether a device is still there to be talked to, lifted from
 * main/zb_coordinator.c (region `reach`).
 *
 * The two ways this goes wrong are both silent on hardware. Too eager, and a
 * lamp on a poor link that loses a frame now and then stops taking commands for
 * half a minute - "the light ignored me". Too lax, and an unplugged lamp keeps
 * the radio busy with retries and route searches, and the remotes lose presses
 * while it does - "the remote is flaky". Neither leaves a line in any log that
 * points at this.
 *
 *   make -C test
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "reach.inc"

static int s_checks;

static void ok(bool cond, const char *what)
{
    s_checks++;
    if (!cond) { printf("FAIL: %s\n", what); exit(1); }
}

#define S (1000LL * 1000)

/* A device heard at `heard` and failing from `t` on, one frame a second. */
static void fail_n(zbc_reach_t *r, int n, int64_t *t, int64_t heard)
{
    for (int i = 0; i < n; i++) {
        *t += 1 * S;
        reach_fail(r, *t, heard);
    }
}

static void test_it_takes_a_run_of_failures(void)
{
    zbc_reach_t r = { 0 };
    int64_t t = 100 * S;
    fail_n(&r, ZBC_UNREACH_FAILS - 1, &t, 50 * S);
    ok(!reach_gone(&r, 50 * S), "one short of the run is still there");
    ok(reach_may_command(&r, t, 50 * S), "and still takes every command");
    fail_n(&r, 1, &t, 50 * S);
    ok(reach_gone(&r, 50 * S), "the whole run, with nothing heard, is gone");
}

/* The poor-link lamp: it loses frames, but it answers in between. */
static void test_hearing_it_breaks_the_run(void)
{
    zbc_reach_t r = { 0 };
    int64_t t = 100 * S, heard = 50 * S;
    for (int round = 0; round < 10; round++) {
        fail_n(&r, ZBC_UNREACH_FAILS - 1, &t, heard);
        heard = t + S / 2;                  /* a report, a read answer */
    }
    fail_n(&r, 1, &t, heard);
    ok(!reach_gone(&r, heard), "failures with answers between them never add up");
}

static void test_a_delivered_frame_breaks_the_run(void)
{
    zbc_reach_t r = { 0 };
    int64_t t = 100 * S;
    fail_n(&r, ZBC_UNREACH_FAILS - 1, &t, 0);
    reach_ok(&r);
    fail_n(&r, ZBC_UNREACH_FAILS - 1, &t, 0);
    ok(!reach_gone(&r, 0), "a delivered frame in the middle starts the count again");
}

/* Gone, and then heard - plugged back in, it announces. */
static void test_hearing_it_brings_it_back(void)
{
    zbc_reach_t r = { 0 };
    int64_t t = 100 * S;
    fail_n(&r, ZBC_UNREACH_FAILS, &t, 0);
    ok(reach_gone(&r, 0), "gone");
    ok(!reach_gone(&r, t + 1), "heard after the last failure, it is back at once");
    ok(reach_may_command(&r, t + 2, t + 1), "and takes the next command without waiting");
}

static void test_a_gone_device_is_probed_once_per_interval(void)
{
    zbc_reach_t r = { 0 };
    int64_t t = 100 * S;
    fail_n(&r, ZBC_UNREACH_FAILS, &t, 0);
    ok(reach_may_command(&r, t, 0), "the first command after going is the probe");
    ok(!reach_may_command(&r, t + S, 0), "the next one inside the interval is not sent");
    ok(!reach_may_command(&r, t + ZBC_UNREACH_PROBE_US - 1, 0), "nor the last one inside it");
    ok(reach_may_command(&r, t + ZBC_UNREACH_PROBE_US, 0), "the interval over, another probe");
    ok(!reach_may_command(&r, t + ZBC_UNREACH_PROBE_US + S, 0), "and the interval starts again");
}

/* The probe's confirmation says it is back. */
static void test_a_delivered_probe_brings_it_back(void)
{
    zbc_reach_t r = { 0 };
    int64_t t = 100 * S;
    fail_n(&r, ZBC_UNREACH_FAILS, &t, 0);
    ok(reach_may_command(&r, t, 0), "probe");
    reach_ok(&r);
    ok(!reach_gone(&r, 0), "delivered: back");
    ok(reach_may_command(&r, t + S, 0) && reach_may_command(&r, t + 2 * S, 0),
       "and every command goes again");
}

/* A failed probe keeps it gone; it does not need another whole run. */
static void test_a_failed_probe_keeps_it_gone(void)
{
    zbc_reach_t r = { 0 };
    int64_t t = 100 * S;
    fail_n(&r, ZBC_UNREACH_FAILS, &t, 0);
    ok(reach_may_command(&r, t, 0), "probe");
    fail_n(&r, 1, &t, 0);
    ok(reach_gone(&r, 0), "the probe undelivered, still gone");
}

/* A settle pass reads three attributes as three frames sent together, so one bad
 * moment fails all three within milliseconds: one event, not a run. */
static void test_a_burst_of_failures_counts_once(void)
{
    zbc_reach_t r = { 0 };
    const int64_t t = 100 * S;
    reach_fail(&r, t, 0);
    reach_fail(&r, t + 3000, 0);
    reach_fail(&r, t + 9000, 0);
    ok(!reach_gone(&r, 0), "three failures in one burst are not a device that is gone");
    reach_fail(&r, t + ZBC_UNREACH_BURST_US, 0);
    reach_fail(&r, t + 2 * ZBC_UNREACH_BURST_US, 0);
    ok(reach_gone(&r, 0), "three that are apart in time are");
}

/* The probe is a request, and a request can be several commands issued
 * together: the parking dim and then the colour and the off, a level and its
 * colour. The first alone would leave a lamp that is back half done. */
static void test_the_probe_carries_its_whole_request(void)
{
    zbc_reach_t r = { 0 };
    int64_t t = 100 * S;
    fail_n(&r, ZBC_UNREACH_FAILS, &t, 0);
    ok(reach_may_command(&r, t, 0), "the probe");
    ok(reach_may_command(&r, t + 2000, 0), "the rest of its request goes with it");
    ok(!reach_may_command(&r, t + ZBC_UNREACH_INTENT_US, 0), "a later request does not");
}

/* A device never heard since boot (heard 0) that has never failed is fine. */
static void test_a_fresh_row_is_there(void)
{
    zbc_reach_t r = { 0 };
    ok(!reach_gone(&r, 0), "no failures, not gone, even never heard");
    ok(reach_may_command(&r, 5 * S, 0), "and it is commanded");
}

int main(void)
{
    test_it_takes_a_run_of_failures();
    test_hearing_it_breaks_the_run();
    test_a_delivered_frame_breaks_the_run();
    test_hearing_it_brings_it_back();
    test_a_gone_device_is_probed_once_per_interval();
    test_a_delivered_probe_brings_it_back();
    test_a_failed_probe_keeps_it_gone();
    test_a_fresh_row_is_there();
    test_a_burst_of_failures_counts_once();
    test_the_probe_carries_its_whole_request();
    printf("all %d checks passed\n", s_checks);
    return 0;
}
