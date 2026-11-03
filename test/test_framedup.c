/* Host test for main/frame_dup.h, which keeps a repeated frame from running twice. A
 * leaf, so it is included whole.
 *
 *   make -C test
 */
#include <stdio.h>

#include "../main/frame_dup.h"

static int failures;
static int checks;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAILED line %d: %s\n", __LINE__, #x); } } while (0)

#define WINDOW 1000
#define N 3

int main(void)
{
    zbc_frame_seen_t t[N] = { 0 };
    const zbc_frame_seen_t f = { .addr = 1, .cluster = 6, .ep = 2, .cmd = 3, .tsn = 4, .fc = 5 };
#define SEEN(frame, now) zbc_frame_repeated(t, N, (frame), (now), WINDOW)

    /* The first time is news, and the next within the window is not. */
    CHECK(!SEEN(f, 100));
    CHECK(SEEN(f, 100 + WINDOW - 1));
    /* The window runs from the first sighting, and past it the frame is news again. */
    CHECK(!SEEN(f, 100 + WINDOW));

    /* Any one field different is another frame. */
    zbc_frame_seen_t g = f; g.addr = 9;     CHECK(!SEEN(g, 1200));
    g = f; g.cluster = 9;                   CHECK(!SEEN(g, 1201));
    g = f; g.ep = 9;                        CHECK(!SEEN(g, 1202));
    g = f; g.cmd = 9;                       CHECK(!SEEN(g, 1203));
    g = f; g.tsn = 9;                       CHECK(!SEEN(g, 1204));
    g = f; g.fc = 9;                        CHECK(!SEEN(g, 1205));

    /* A full table gives up its oldest. */
    zbc_frame_seen_t e[N] = { 0 };
    for (int i = 0; i < N; i++) {
        zbc_frame_seen_t x = f; x.tsn = (uint8_t)i;
        CHECK(!zbc_frame_repeated(e, N, x, 10 + i, WINDOW));
    }
    zbc_frame_seen_t x = f; x.tsn = 200;
    CHECK(!zbc_frame_repeated(e, N, x, 20, WINDOW));
    x.tsn = 0;   /* evicted */
    CHECK(!zbc_frame_repeated(e, N, x, 21, WINDOW));
    x.tsn = 2;   /* still there */
    CHECK(zbc_frame_repeated(e, N, x, 22, WINDOW));

    if (failures) {
        printf("%d of %d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("all %d checks passed\n", checks);
    return 0;
}
