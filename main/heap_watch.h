/* Where the heap goes at its lowest. Built only with CONFIG_ZBC_HEAP_WATCH;
 * without it every call below is an empty inline and `heap` in /api/status
 * keeps only free, min_free and largest.
 *
 * `heap.min_free` in /api/status is one number for the whole boot: it says how
 * low the heap went, never what took it there. This keeps the answer:
 *
 *   - Each HTTP request is bracketed with the heap's local-minimum monitor, so
 *     every route has the deepest it took the heap below where it started. The
 *     server answers one request at a time, so the brackets never overlap -
 *     but anything another task allocates meanwhile counts against the request.
 *   - A new boot-wide low reached between requests is recorded with its time
 *     and the newest marks. A sampler looks once a second, so the time is good
 *     to the second: a low used to be noticed only at the next request, and on
 *     a box nobody looks at that is hours later, with the marks long gone.
 *   - So is one reached inside a request (`by` "in_request"), with the route
 *     and the free heap when it began. The route's own row keeps only the low
 *     at its deepest drop, so a low reached in a request that began with the
 *     heap already down - something else holding it - was recorded nowhere,
 *     and `min_free` named a number no table explained.
 *   - Other tasks leave marks (a device announcing, a Zigbee work job, the end
 *     of a firmware transfer, an Adaptive Lighting tick, a Matter command or
 *     device event, a controller's subscription asked for, established or
 *     ended, a Matter secure session set up, a Wi-Fi or IP event), timed, so a
 *     low between requests can be matched to what was happening. A mark repeated within a second is
 *     counted in its row rather than given another.
 *   - With CONFIG_ZBC_HEAP_WATCH_TASKS, every block is charged to the task that
 *     allocated it until it is freed, whichever task frees it: `tasks` has what
 *     each task holds, its peak and what it held ten minutes after boot (a
 *     leak is the one that keeps growing), and each low keeps the three tasks
 *     whose holding rose most in the second before it (for a low inside a
 *     request, since the request began), and each mark what the
 *     task leaving it held, so a step between two of one task's marks is what
 *     happened between them. The heap component's
 *     own per-task bookkeeping is replaced (linker --wrap, main/CMakeLists.txt):
 *     it keeps a list entry per block, ~24 bytes each, and walks it on every
 *     free. Only the 4-byte owner it puts in every block is kept. An IDF upgrade
 *     must recheck that heap_caps_base.c still calls the three functions and
 *     what `size` it passes - the prototypes are checked, the meaning is not.
 *
 * begin/end are for the HTTP server task only. heap_watch_mark is callable from
 * any task, and from a spinlock. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* `routes`, `lows_between_requests` and `marks` as members of /api/status's
 * `heap` object, a row at a time rather than as one tree: with the watch built
 * in the three tables are the largest thing in that answer, and cJSON holds
 * five to six bytes of heap per byte of JSON - the opposite of what a heap
 * measurement should cost. `table` says a table begins (the caller writes the
 * member and its brackets); `row` is handed one row as its own object and takes
 * it over, freeing it. The marks are copied before any callback runs, so a
 * mark left by another task mid-answer cannot shift the ring under the caller;
 * see heap_watch_stream_json for the routes and the lows. */
typedef struct {
    void *ctx;
    void (*table)(void *ctx, const char *name);
    void (*row)(void *ctx, cJSON *item);
} heap_watch_sink_t;

#if CONFIG_ZBC_HEAP_WATCH

/* The sampler and the Wi-Fi/IP marks. After the default event loop exists. */
esp_err_t heap_watch_start(void);

void heap_watch_request_begin(void);
void heap_watch_request_end(httpd_req_t *req);

/* The boot-wide minimum free heap. heap_caps_get_minimum_free_size reads the
 * local minimum while a request is being measured, which inside a request
 * handler is nearly always; this does not. */
uint32_t heap_watch_min_free(void);

/* The length of the JSON body the current request is sending. HTTP task. */
void heap_watch_note_body(size_t len);

/* `what` must be a string literal; `arg` is any number worth keeping with it. */
void heap_watch_mark(const char *what, uint32_t arg);

void heap_watch_stream_json(const heap_watch_sink_t *sink);

#else /* !CONFIG_ZBC_HEAP_WATCH: the calls stay in place and compile to nothing */

static inline esp_err_t heap_watch_start(void) { return ESP_OK; }
static inline void heap_watch_request_begin(void) {}
static inline void heap_watch_request_end(httpd_req_t *req) { (void)req; }
static inline void heap_watch_note_body(size_t len) { (void)len; }
static inline uint32_t heap_watch_min_free(void)
{
    return (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
}
static inline void heap_watch_mark(const char *what, uint32_t arg) { (void)what; (void)arg; }
/* Still takes the sink, so the caller needs no #if around naming its callbacks. */
static inline void heap_watch_stream_json(const heap_watch_sink_t *sink) { (void)sink; }

#endif

#ifdef __cplusplus
}
#endif
