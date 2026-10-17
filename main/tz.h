/* A time zone as a POSIX TZ rule, for the one thing on the coordinator that
 * needs the local clock: Adaptive Lighting's sunrise and sunset limits ("the day
 * starts no later than 06:30"). Everything else works in UTC.
 *
 * A leaf like sun.c - the C library and nothing else - so test/test_tz.c
 * compiles it whole. Our own evaluator rather than setenv("TZ") and localtime_r:
 * no process-wide state shared between tasks, the same answer on the host as on
 * the board, and the settings POST can refuse a rule before storing it.
 *
 * The page writes the rule from the browser's own zone (web/src/lib/tz.ts), so
 * only what it writes has to be read: `std offset [dst [offset],start,end]` with
 * both rules in the `Mm.w.d[/time]` form, names alphabetic or in <...>. The
 * Julian-day rule forms are refused, and so is a DST name without rules (POSIX
 * leaves those to the implementation).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest rule stored, not counting the terminator. */
#define ZBC_TZ_MAX_LEN 63

bool    zbc_tz_valid(const char *posix);

/* Seconds east of UTC in force at `utc_s`: 3600 for CET, 7200 for CEST, -18000
 * for EST. 0 for NULL, an empty string or a rule that does not parse. */
int32_t zbc_tz_offset(const char *posix, int64_t utc_s);

#ifdef __cplusplus
}
#endif
