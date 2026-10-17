/* The lost-frame arithmetic behind `frames_missed` in /api/devices and
 * /api/status, lifted from main/zb_coordinator.c (region `linkgap`).
 *
 * Worth a test of its own because the number it produces is the one that says
 * whether a remote's press was lost on air or never sent - and a wrap-around
 * fault here would report two hundred lost frames every time a device's
 * sequence number passed 255, which is every 256 presses. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "linkgap.inc"

static int failures, checks;

static void expect(uint8_t last, uint8_t tsn, unsigned want)
{
    checks++;
    unsigned got = link_gap(last, tsn);
    if (got != want) {
        printf("\n  link_gap(%u, %u) = %u, expected %u", last, tsn, got, want);
        failures++;
    }
}

int main(void)
{
    /* The next number is no loss; a step of two is one frame gone. */
    expect(10, 11, 0);
    expect(10, 12, 1);
    expect(10, 15, 4);
    /* The counter wraps at 256 and that is not a loss either. */
    expect(255, 0, 0);
    expect(255, 1, 1);
    expect(254, 3, 4);
    /* The same number again is a duplicate, not a loss. */
    expect(7, 7, 0);
    /* Up to LINK_GAP_MAX frames may be missed in one step; past that the
     * device has started over, or is talking to somebody else too. */
    expect(0, LINK_GAP_MAX + 1, LINK_GAP_MAX);
    expect(0, LINK_GAP_MAX + 2, 0);
    expect(0, 200, 0);
    expect(200, 100, 0);

    /* A long run with frames dropped at random adds up to exactly the number
     * dropped, provided no run of drops exceeds the cap. */
    srand(1);
    uint8_t  last = 0;
    unsigned dropped = 0, counted = 0, run = 0;
    for (int i = 1; i < 20000; i++) {
        const uint8_t tsn = (uint8_t)i;
        if (rand() % 10 == 0 && run < LINK_GAP_MAX) {
            dropped++;
            run++;
            continue;
        }
        run = 0;
        counted += link_gap(last, tsn);
        last = tsn;
    }
    checks++;
    if (counted != dropped) {
        printf("\n  counted %u lost frames over a run that dropped %u", counted, dropped);
        failures++;
    }

    if (failures) {
        printf("\n%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("all %d checks passed\n", checks);
    return 0;
}
