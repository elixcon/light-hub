/* See heap_watch.h. */
#include "heap_watch.h"

#if CONFIG_ZBC_HEAP_WATCH

#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_netif_types.h"
#include "esp_timer.h"
#include "esp_wifi_types.h"

#if CONFIG_ZBC_HEAP_WATCH_TASKS
#include "esp_attr.h"
#include "freertos/task.h"
/* The heap component's own, from its private include directories
 * (main/CMakeLists.txt): heap_t, and the prototypes the __wrap_ functions below
 * must match. MULTI_HEAP_FREERTOS is what the heap component compiles its own
 * files with (its CMakeLists.txt); without it heap_t has no lock type. */
#define MULTI_HEAP_FREERTOS
#include "heap_private.h"
#include "esp_heap_task_info_internal.h"
#endif

#define CAPS         MALLOC_CAP_INTERNAL
#define ROUTE_ROWS   16
#define ROUTE_LEN    40
#define LOW_ROWS     8
#define LOW_MARKS    5           /* the newest marks kept with a low */
#define LOW_STEP     1024        /* a low makes a row only this far under the last row */
#define MARK_ROWS    16
#define MARK_FOLD_MS 1000        /* a mark repeated within this of its last is counted, not added */
#define SAMPLE_US    1000000
#define TASK_ROWS    32
#define LOW_TASKS    3           /* kept with a low: the tasks whose holding rose most */
#define BASE_AT_MS   (10 * 60 * 1000)   /* the boot has settled: what each task holds then */

typedef struct {
    char     route[ROUTE_LEN];   /* "GET /api/state", query cut off */
    uint32_t count;
    uint32_t deepest;            /* bytes below the free heap at its start */
    uint32_t shallowest;         /* the least of those: nearest the route's own cost */
    uint64_t depth_sum;
    uint32_t body_max;           /* the largest JSON body it sent */
    uint32_t low;                /* the free heap at that deepest point */
    int64_t  deepest_us;
} route_t;

/* Times in milliseconds since boot, in 32 bits: these are kept by the dozen,
 * and a difference is right for 49 days. */
typedef struct {
    const char *what;
    uint32_t    arg;
    uint32_t    first_ms;
    uint32_t    last_ms;
    uint16_t    n;               /* times it came, folded into this row */
#if CONFIG_ZBC_HEAP_WATCH_TASKS
    uint16_t    task_blocks;     /* what the marking task held at the newest, so a */
    uint32_t    task_held;       /* step between two of its marks is what came between */
#endif
} mark_t;

#if CONFIG_ZBC_HEAP_WATCH_TASKS
/* What a task holds: blocks it allocated that nobody has freed yet, whichever
 * task frees them. The window is the time since the sampler's last tick, which
 * is the time a low it notices can have happened in. */
typedef struct {
    TaskHandle_t handle;         /* NULL: before the scheduler started */
    char         name[configMAX_TASK_NAME_LEN];
    bool         used;
    bool         alive;          /* cleared when its TCB is freed */
    uint32_t     held;
    uint32_t     blocks;
    uint32_t     peak;           /* the most it has held since boot */
    uint32_t     base;           /* held at BASE_AT_MS; 0 for a task started after it */
    uint32_t     win_start;      /* held at the sampler's last tick */
    uint32_t     win_peak;       /* the most held since then */
    uint32_t     req_start;      /* held when the request being measured began */
    uint32_t     req_peak;       /* the most held since then */
} task_row_t;

typedef struct {
    char     name[configMAX_TASK_NAME_LEN];
    uint32_t rise;               /* the most it held in the window, less what it held at its start */
    uint32_t held;               /* when the low was noticed */
} low_task_t;
#endif

/* Who noticed a low: the sampler, a request at its start (it happened since the
 * sampler last looked), or a request at its end (it happened while the request
 * ran, which the sampler cannot see: it keeps out while a request is measured). */
typedef enum { LOW_BY_SAMPLER, LOW_BY_REQUEST, LOW_IN_REQUEST } low_by_t;

typedef struct {
    uint32_t min_free;
    uint32_t free_then;
    uint32_t at_ms;              /* when it was noticed */
    uint8_t  by;                 /* low_by_t */
    char     route[ROUTE_LEN];   /* LOW_IN_REQUEST: the request it happened in */
    uint32_t free_before;        /* LOW_IN_REQUEST: the free heap when that request began */
    mark_t   marks[LOW_MARKS];   /* the newest when it was noticed, newest first */
#if CONFIG_ZBC_HEAP_WATCH_TASKS
    low_task_t tasks[LOW_TASKS]; /* the most risen first; rise 0 is no row */
#endif
} low_t;

/* Everything below is under s_mux, except where a field says it belongs to the
 * HTTP task. The sampler runs on the esp_timer task and only reads two counters
 * and copies a few rows here: nothing that blocks. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static route_t  s_routes[ROUTE_ROWS];   /* written by the HTTP task alone */
static bool     s_open;         /* a request is being measured: the sampler keeps out */
static uint32_t s_free_before;   /* HTTP task */
static uint32_t s_body_len;      /* HTTP task */
static uint32_t s_boot_min = UINT32_MAX;
static uint32_t s_row_min  = UINT32_MAX;   /* min_free of the newest low row */
static low_t    s_lows[LOW_ROWS];
static size_t   s_low_next;
static mark_t   s_marks[MARK_ROWS];
static size_t   s_mark_next;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

#if CONFIG_ZBC_HEAP_WATCH_TASKS
/* Written from inside every allocation and free, on any task and with the cache
 * possibly off, so the code is in IRAM, the table in DRAM, and the lock a
 * spinlock held for a scan of TASK_ROWS rows. Never taken around anything that
 * allocates. Taken inside s_mux, never the other way round. */
static portMUX_TYPE s_task_mux = portMUX_INITIALIZER_UNLOCKED;
static task_row_t   s_tasks[TASK_ROWS];
static task_row_t   s_task_other = { .name = "(other)", .used = true, .alive = true };
static bool         s_base_taken;

/* The row of the task holding h's blocks. Allocating, a task's own live row,
 * made at its first block. Freeing, the live row, else the row of a task that
 * has ended (a reused TCB address would have both). With the table full, or a
 * block whose task never got a row, the (other) row. */
static IRAM_ATTR task_row_t *task_row_locked(TaskHandle_t h, bool allocating)
{
    task_row_t *spare = NULL, *ended = NULL;
    for (size_t i = 0; i < TASK_ROWS; i++) {
        task_row_t *t = &s_tasks[i];
        if (!t->used || (!t->alive && !t->blocks)) {
            if (!spare) {
                spare = t;
            }
        } else if (t->handle == h) {
            if (t->alive) {
                return t;
            }
            ended = t;
        }
    }
    if (!allocating) {
        return ended ? ended : &s_task_other;
    }
    if (!spare) {
        return &s_task_other;
    }
    *spare = (task_row_t){ .handle = h, .used = true, .alive = true };
    /* pcTaskGetName is in IRAM (FREERTOS_PLACE_FUNCTIONS_INTO_FLASH is off), as
     * IDF's own bookkeeping relies on. */
    const char *name = h ? pcTaskGetName(h) : "(boot)";
    for (size_t k = 0; k + 1 < sizeof(spare->name) && name[k]; k++) {
        spare->name[k] = name[k];
    }
    return spare;
}

static IRAM_ATTR void task_add(TaskHandle_t h, size_t size)
{
    portENTER_CRITICAL_SAFE(&s_task_mux);
    task_row_t *t = task_row_locked(h, true);
    t->held += (uint32_t)size;
    t->blocks++;
    if (t->held > t->peak) {
        t->peak = t->held;
    }
    if (t->held > t->win_peak) {
        t->win_peak = t->held;
    }
    if (t->held > t->req_peak) {
        t->req_peak = t->held;
    }
    portEXIT_CRITICAL_SAFE(&s_task_mux);
}

/* `ptr` is the block's own address: a task's TCB is a block whose address is
 * its handle, and freeing it is how a task is seen to end. */
static IRAM_ATTR void task_sub(TaskHandle_t h, size_t size, const void *ptr)
{
    portENTER_CRITICAL_SAFE(&s_task_mux);
    task_row_t *t = task_row_locked(h, false);
    t->held   = t->held > size ? t->held - (uint32_t)size : 0;
    t->blocks = t->blocks ? t->blocks - 1 : 0;
    if (ptr) {
        for (size_t i = 0; i < TASK_ROWS; i++) {
            if (s_tasks[i].used && s_tasks[i].alive && s_tasks[i].handle == (TaskHandle_t)ptr) {
                s_tasks[i].alive = false;
                break;
            }
        }
    }
    portEXIT_CRITICAL_SAFE(&s_task_mux);
}

/* Internal RAM only, as everything else here. `size` is the block's full size,
 * the owner word included: what IDF passes on allocating, and what the free
 * below asks for of the same block. */
static IRAM_ATTR bool counted(const heap_t *heap)
{
    return (get_all_caps(heap) & CAPS) != 0;
}

IRAM_ATTR void __wrap_heap_caps_update_per_task_info_alloc(heap_t *heap, void *ptr, size_t size, uint32_t caps)
{
    (void)ptr;
    (void)caps;
    if (counted(heap)) {
        task_add(xTaskGetCurrentTaskHandle(), size);
    }
}

/* Before the block is freed, so its owner word and size are still there. */
IRAM_ATTR void __wrap_heap_caps_update_per_task_info_free(heap_t *heap, void *ptr)
{
    if (counted(heap)) {
        TaskHandle_t *owner = (TaskHandle_t *)ptr - 1;
        task_sub(*owner, multi_heap_get_full_block_size(heap->heap, owner), ptr);
    }
}

/* A block grown or shrunk where it is: the old size off its owner, the new size
 * on the task resizing it, which is now its owner. */
IRAM_ATTR void __wrap_heap_caps_update_per_task_info_realloc(heap_t *heap, void *old_ptr, void *new_ptr,
                                                             size_t old_size, TaskHandle_t old_task,
                                                             size_t new_size, uint32_t caps)
{
    (void)old_ptr;
    (void)new_ptr;
    (void)caps;
    if (counted(heap)) {
        task_sub(old_task, old_size, NULL);
        task_add(xTaskGetCurrentTaskHandle(), new_size);
    }
}

_Static_assert(__builtin_types_compatible_p(__typeof__(__wrap_heap_caps_update_per_task_info_alloc),
                                            __typeof__(heap_caps_update_per_task_info_alloc)),
               "the heap component's alloc bookkeeping changed");
_Static_assert(__builtin_types_compatible_p(__typeof__(__wrap_heap_caps_update_per_task_info_free),
                                            __typeof__(heap_caps_update_per_task_info_free)),
               "the heap component's free bookkeeping changed");
_Static_assert(__builtin_types_compatible_p(__typeof__(__wrap_heap_caps_update_per_task_info_realloc),
                                            __typeof__(heap_caps_update_per_task_info_realloc)),
               "the heap component's realloc bookkeeping changed");

/* The tasks that rose most, the most first: in the sampler's window, or with
 * `request` since the request being measured began - a low inside a request can
 * be anywhere in it, and the sampler's window starts again every second. */
static void tasks_top_locked(low_task_t out[LOW_TASKS], bool request)
{
    memset(out, 0, sizeof(low_task_t) * LOW_TASKS);
    for (size_t i = 0; i <= TASK_ROWS; i++) {
        const task_row_t *t     = i < TASK_ROWS ? &s_tasks[i] : &s_task_other;
        const uint32_t    start = request ? t->req_start : t->win_start;
        const uint32_t    peak  = request ? t->req_peak : t->win_peak;
        if (!t->used || peak <= start) {
            continue;
        }
        const uint32_t rise = peak - start;
        size_t         k    = LOW_TASKS;
        while (k > 0 && out[k - 1].rise < rise) {
            if (k < LOW_TASKS) {
                out[k] = out[k - 1];
            }
            k--;
        }
        if (k < LOW_TASKS) {
            memcpy(out[k].name, t->name, sizeof(out[k].name));
            out[k].rise = rise;
            out[k].held = t->held;
        }
    }
}

/* Every tick: a new window, and once, what each task holds when the boot has
 * settled. */
static void tasks_tick_locked(void)
{
    const bool take_base = !s_base_taken && now_ms() >= BASE_AT_MS;
    for (size_t i = 0; i <= TASK_ROWS; i++) {
        task_row_t *t = i < TASK_ROWS ? &s_tasks[i] : &s_task_other;
        t->win_start = t->held;
        t->win_peak  = t->held;
        if (take_base) {
            t->base = t->held;
        }
    }
    if (take_base) {
        s_base_taken = true;
    }
}

/* A request is being measured from now: its own window. */
static void tasks_request_begin_locked(void)
{
    for (size_t i = 0; i <= TASK_ROWS; i++) {
        task_row_t *t = i < TASK_ROWS ? &s_tasks[i] : &s_task_other;
        t->req_start  = t->held;
        t->req_peak   = t->held;
    }
}
#endif

/* A new boot-wide low. It makes a row only LOW_STEP under the last one, so a
 * minimum creeping down a few bytes at a time does not wash the table out; the
 * row keeps the newest marks, since by the time anybody reads it the ring has
 * long moved on. The first reading only sets where lows are counted from.
 * `route` and `free_before` are for LOW_IN_REQUEST. */
static void note_low_locked(uint32_t boot_min, uint32_t free_now, low_by_t by, const char *route,
                            uint32_t free_before)
{
    if (boot_min >= s_boot_min) {
        return;
    }
    const bool first = s_boot_min == UINT32_MAX;
    s_boot_min       = boot_min;
    if (first || (uint64_t)boot_min + LOW_STEP > s_row_min) {
        return;
    }
    low_t *l       = &s_lows[s_low_next];
    s_low_next     = (s_low_next + 1) % LOW_ROWS;
    s_row_min      = boot_min;
    l->min_free    = boot_min;
    l->free_then   = free_now;
    l->at_ms       = now_ms();
    l->by          = (uint8_t)by;
    l->free_before = free_before;
    strlcpy(l->route, route ? route : "", sizeof(l->route));
    for (size_t k = 0; k < LOW_MARKS; k++) {
        l->marks[k] = s_marks[(s_mark_next + MARK_ROWS - 1 - k) % MARK_ROWS];
    }
#if CONFIG_ZBC_HEAP_WATCH_TASKS
    portENTER_CRITICAL_SAFE(&s_task_mux);
    tasks_top_locked(l->tasks, by == LOW_IN_REQUEST);
    portEXIT_CRITICAL_SAFE(&s_task_mux);
#endif
}

/* Once a second, so a low between requests is dated to the second rather than
 * to whenever somebody next asked for a page - on a box nobody looks at, hours.
 * Not while a request is measured: its monitor makes the minimum read local. */
static void sample_cb(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_mux);
    if (!s_open) {
        note_low_locked((uint32_t)heap_caps_get_minimum_free_size(CAPS), (uint32_t)heap_caps_get_free_size(CAPS),
                        LOW_BY_SAMPLER, NULL, 0);
    }
#if CONFIG_ZBC_HEAP_WATCH_TASKS
    /* While a request is measured too: a low its start notices happened since
     * this tick, and the window has to say what rose in that time. */
    portENTER_CRITICAL(&s_task_mux);
    tasks_tick_locked();
    portEXIT_CRITICAL(&s_task_mux);
#endif
    portEXIT_CRITICAL(&s_mux);
}

/* Wi-Fi and IP events as marks: a reconnect, a scan, a new lease. Registered
 * here rather than in wifi_manager.c, which the updater shares. */
static void net_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    heap_watch_mark(base == WIFI_EVENT ? "wifi" : "ip", (uint32_t)id);
}

esp_err_t heap_watch_start(void)
{
    static esp_timer_handle_t timer;
    if (timer) {
        return ESP_OK;
    }
    const esp_timer_create_args_t args = { .callback = sample_cb, .name = "heap_watch" };
    esp_err_t                     err  = esp_timer_create(&args, &timer);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(timer, SAMPLE_US);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, net_event_cb, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, net_event_cb, NULL);
    }
    return err;
}

void heap_watch_mark(const char *what, uint32_t arg)
{
    const uint32_t now = now_ms();
#if CONFIG_ZBC_HEAP_WATCH_TASKS
    portENTER_CRITICAL_SAFE(&s_task_mux);
    const task_row_t *t      = task_row_locked(xTaskGetCurrentTaskHandle(), false);
    const uint32_t    held   = t->held;
    const uint16_t    blocks = t->blocks > UINT16_MAX ? UINT16_MAX : (uint16_t)t->blocks;
    portEXIT_CRITICAL_SAFE(&s_task_mux);
#endif
    portENTER_CRITICAL_SAFE(&s_mux);
    mark_t *last = &s_marks[(s_mark_next + MARK_ROWS - 1) % MARK_ROWS];
    if (last->what && strcmp(last->what, what) == 0 && last->arg == arg && now - last->last_ms <= MARK_FOLD_MS &&
        last->n < UINT16_MAX) {
        /* A run of the same mark is one row: six work jobs in a second would
         * otherwise push everything else out of the ring. */
        last->n++;
        last->last_ms = now;
    } else {
        s_marks[s_mark_next] = (mark_t){ .what = what, .arg = arg, .first_ms = now, .last_ms = now, .n = 1 };
        last                 = &s_marks[s_mark_next];
        s_mark_next          = (s_mark_next + 1) % MARK_ROWS;
    }
#if CONFIG_ZBC_HEAP_WATCH_TASKS
    last->task_held   = held;
    last->task_blocks = blocks;
#endif
    portEXIT_CRITICAL_SAFE(&s_mux);
}

void heap_watch_request_begin(void)
{
    /* Not monitoring here, so this is the boot-wide minimum, including whatever
     * happened since the sampler last looked. s_open goes up before the monitor
     * starts, so the sampler never reads the local minimum as a boot-wide one. */
    portENTER_CRITICAL(&s_mux);
    s_free_before = (uint32_t)heap_caps_get_free_size(CAPS);
    note_low_locked((uint32_t)heap_caps_get_minimum_free_size(CAPS), s_free_before, LOW_BY_REQUEST, NULL, 0);
    s_open = true;
#if CONFIG_ZBC_HEAP_WATCH_TASKS
    portENTER_CRITICAL(&s_task_mux);
    tasks_request_begin_locked();
    portEXIT_CRITICAL(&s_task_mux);
#endif
    portEXIT_CRITICAL(&s_mux);
    s_body_len = 0;
    if (heap_caps_monitor_local_minimum_free_size_start() != ESP_OK) {
        portENTER_CRITICAL(&s_mux);
        s_open = false;
        portEXIT_CRITICAL(&s_mux);
    }
}

void heap_watch_note_body(size_t len)
{
    s_body_len = (uint32_t)len;
}

uint32_t heap_watch_min_free(void)
{
    const uint32_t now_min = (uint32_t)heap_caps_get_minimum_free_size(CAPS);
    return (s_open && s_boot_min < now_min) ? s_boot_min : now_min;
}

void heap_watch_request_end(httpd_req_t *req)
{
    if (!s_open) {
        return;
    }
    const uint32_t local = (uint32_t)heap_caps_get_minimum_free_size(CAPS);
    heap_caps_monitor_local_minimum_free_size_stop();

    /* Cut to fit on purpose: the start of a path is what tells routes apart. */
    char route[ROUTE_LEN];
    strlcpy(route, http_method_str((enum http_method)req->method), sizeof(route));
    strlcat(route, " ", sizeof(route));
    strlcat(route, req->uri, sizeof(route));
    route[strcspn(route, "?")] = '\0';

    /* A boot-wide low inside this request gets a row of its own. The route's
     * `low` below keeps only the one at its deepest drop, and a low reached
     * while the heap was already down when the request began is not that one:
     * such a low used to be recorded nowhere. */
    portENTER_CRITICAL(&s_mux);
    note_low_locked((uint32_t)heap_caps_get_minimum_free_size(CAPS), (uint32_t)heap_caps_get_free_size(CAPS),
                    LOW_IN_REQUEST, route, s_free_before);
    s_open = false;
    portEXIT_CRITICAL(&s_mux);

    const uint32_t depth = s_free_before > local ? s_free_before - local : 0;
    route_t       *r     = NULL;
    route_t       *spare = NULL;
    for (size_t i = 0; i < ROUTE_ROWS; i++) {
        if (s_routes[i].count && strcmp(s_routes[i].route, route) == 0) {
            r = &s_routes[i];
            break;
        }
        /* A free row, or when full the shallowest makes way. */
        if (!s_routes[i].count) {
            if (!spare || spare->count) {
                spare = &s_routes[i];
            }
        } else if (!spare || (spare->count && s_routes[i].deepest < spare->deepest)) {
            spare = &s_routes[i];
        }
    }
    portENTER_CRITICAL(&s_mux);
    if (!r) {
        r = spare;
        memset(r, 0, sizeof(*r));
        strlcpy(r->route, route, sizeof(r->route));
        r->shallowest = UINT32_MAX;
    }
    r->count++;
    r->depth_sum += depth;
    if (depth < r->shallowest) {
        r->shallowest = depth;
    }
    if (s_body_len > r->body_max) {
        r->body_max = s_body_len;
    }
    if (depth >= r->deepest) {
        r->deepest    = depth;
        r->low        = local;
        r->deepest_us = esp_timer_get_time();
    }
    portEXIT_CRITICAL(&s_mux);
}

/* A mark's age from ref_ms: in seconds for the marks table, in milliseconds
 * before the low for the marks kept with one. */
static cJSON *mark_json(const mark_t *m, uint32_t ref_ms, bool seconds)
{
    const uint32_t scale = seconds ? 1000 : 1;
    cJSON         *o     = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "what", m->what);
    cJSON_AddNumberToObject(o, "arg", m->arg);
    cJSON_AddNumberToObject(o, "n", m->n);
    cJSON_AddNumberToObject(o, seconds ? "s_ago" : "ms_before", (double)((ref_ms - m->last_ms) / scale));
    if (m->n > 1) {
        cJSON_AddNumberToObject(o, seconds ? "first_s_ago" : "first_ms_before",
                                (double)((ref_ms - m->first_ms) / scale));
    }
#if CONFIG_ZBC_HEAP_WATCH_TASKS
    cJSON_AddNumberToObject(o, "task_held", m->task_held);
    cJSON_AddNumberToObject(o, "task_blocks", m->task_blocks);
#endif
    return o;
}

void heap_watch_stream_json(const heap_watch_sink_t *sink)
{
    /* The marks are copied whole, so a mark left by another task mid-answer
     * cannot shift the ring under the caller. The routes are read in place: only
     * heap_watch_request_end writes them, on this task, after this answer. The
     * lows are copied a row at a time: whole, with their marks, they would double
     * this function's stack, and a row the sampler adds meanwhile only takes the
     * oldest's place. */
    mark_t marks[MARK_ROWS];
    portENTER_CRITICAL(&s_mux);
    memcpy(marks, s_marks, sizeof(marks));
    const size_t mark_next = s_mark_next;
    const size_t low_next  = s_low_next;
    portEXIT_CRITICAL(&s_mux);
    const int64_t  now    = esp_timer_get_time();
    const uint32_t now_m  = now_ms();

    /* One row at a time, so the sink can write each one out and free it: see
     * heap_watch_sink_t. An empty table is still named, as an empty array. */
    sink->table(sink->ctx, "routes");
    for (size_t i = 0; i < ROUTE_ROWS; i++) {
        const route_t *r = &s_routes[i];
        if (!r->count) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "route", r->route);
        cJSON_AddNumberToObject(o, "count", r->count);
        cJSON_AddNumberToObject(o, "deepest", r->deepest);
        cJSON_AddNumberToObject(o, "shallowest", r->shallowest);
        cJSON_AddNumberToObject(o, "mean", (double)(r->depth_sum / r->count));
        cJSON_AddNumberToObject(o, "body_max", r->body_max);
        cJSON_AddNumberToObject(o, "low", r->low);
        cJSON_AddNumberToObject(o, "s_ago", (double)((now - r->deepest_us) / 1000000));
        sink->row(sink->ctx, o);
    }
    sink->table(sink->ctx, "lows_between_requests");
    for (size_t k = 0; k < LOW_ROWS; k++) {
        low_t l;
        portENTER_CRITICAL(&s_mux);
        l = s_lows[(low_next + LOW_ROWS - 1 - k) % LOW_ROWS];
        portEXIT_CRITICAL(&s_mux);
        if (!l.at_ms) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "min_free", l.min_free);
        cJSON_AddNumberToObject(o, "free_then", l.free_then);
        cJSON_AddNumberToObject(o, "noticed_s_ago", (double)((now_m - l.at_ms) / 1000));
        static const char *const BY[] = { "sampler", "request", "in_request" };
        cJSON_AddStringToObject(o, "by", BY[l.by <= LOW_IN_REQUEST ? l.by : 0]);
        if (l.by == LOW_IN_REQUEST) {
            cJSON_AddStringToObject(o, "route", l.route);
            cJSON_AddNumberToObject(o, "free_before", l.free_before);
        }
        cJSON *lm = cJSON_AddArrayToObject(o, "marks");
        for (size_t j = 0; j < LOW_MARKS; j++) {
            if (l.marks[j].what) {
                cJSON_AddItemToArray(lm, mark_json(&l.marks[j], l.at_ms, false));
            }
        }
#if CONFIG_ZBC_HEAP_WATCH_TASKS
        cJSON *lt = cJSON_AddArrayToObject(o, "tasks");
        for (size_t j = 0; j < LOW_TASKS && l.tasks[j].rise; j++) {
            cJSON *t = cJSON_CreateObject();
            cJSON_AddStringToObject(t, "task", l.tasks[j].name);
            cJSON_AddNumberToObject(t, "rise", l.tasks[j].rise);
            cJSON_AddNumberToObject(t, "held", l.tasks[j].held);
            cJSON_AddItemToArray(lt, t);
        }
#endif
        sink->row(sink->ctx, o);
    }
    sink->table(sink->ctx, "marks");
    for (size_t k = 0; k < MARK_ROWS; k++) {
        const mark_t *m = &marks[(mark_next + MARK_ROWS - 1 - k) % MARK_ROWS];
        if (m->what) {
            sink->row(sink->ctx, mark_json(m, now_m, true));
        }
    }
#if CONFIG_ZBC_HEAP_WATCH_TASKS
    /* A row at a time, as the lows: copied whole the table would be 1.5 KB of
     * this task's stack. Nothing is built under the lock - building allocates,
     * and every allocation takes it. A task that has ended is listed while it
     * still holds blocks: what it left behind. */
    sink->table(sink->ctx, "tasks");
    portENTER_CRITICAL(&s_task_mux);
    const bool base_taken = s_base_taken;
    portEXIT_CRITICAL(&s_task_mux);
    for (size_t i = 0; i <= TASK_ROWS; i++) {
        task_row_t t;
        portENTER_CRITICAL(&s_task_mux);
        t = i < TASK_ROWS ? s_tasks[i] : s_task_other;
        portEXIT_CRITICAL(&s_task_mux);
        if (!t.used || !t.peak || (!t.alive && !t.blocks)) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "task", t.name);
        cJSON_AddNumberToObject(o, "held", t.held);
        cJSON_AddNumberToObject(o, "blocks", t.blocks);
        cJSON_AddNumberToObject(o, "peak", t.peak);
        if (base_taken) {
            cJSON_AddNumberToObject(o, "held_at_10min", t.base);
        } else {
            cJSON_AddNullToObject(o, "held_at_10min");
        }
        cJSON_AddBoolToObject(o, "alive", t.alive);
        sink->row(sink->ctx, o);
    }
#endif
}

#endif /* CONFIG_ZBC_HEAP_WATCH */
