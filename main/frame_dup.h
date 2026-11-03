/* Whether a ZCL frame is one just seen again: a retry the stack lets through, or a
 * device that sends one frame twice. A repeated Toggle would undo itself.
 *
 * A leaf, so test_framedup drives it with cc. Each caller keeps its own table, and
 * says which endpoint counts. Not thread-safe: one task to a table. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int64_t  at_us;
    uint16_t addr;
    uint16_t cluster;
    uint8_t  ep;
    uint8_t  cmd;
    uint8_t  tsn;
    uint8_t  fc;
    bool     used;
} zbc_frame_seen_t;

/* True when `f` is in the table from under window_us ago. Otherwise it goes in, in
 * place of the oldest. A tsn repeats only after 256 frames, hence the window. */
static inline bool zbc_frame_repeated(zbc_frame_seen_t *table, int n, zbc_frame_seen_t f, int64_t now_us,
                                      int64_t window_us)
{
    int oldest = 0;
    for (int i = 0; i < n; i++) {
        const zbc_frame_seen_t *t = &table[i];
        if (t->used && t->addr == f.addr && t->ep == f.ep && t->cluster == f.cluster && t->cmd == f.cmd &&
            t->tsn == f.tsn && t->fc == f.fc && now_us - t->at_us < window_us) {
            return true;
        }
        if (t->at_us < table[oldest].at_us) {
            oldest = i;
        }
    }
    f.at_us       = now_us;
    f.used        = true;
    table[oldest] = f;
    return false;
}
