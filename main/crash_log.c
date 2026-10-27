#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_private/freertos_debug.h"
#include "esp_private/panic_internal.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "riscv/rvruntime-frames.h"
#include "soc/soc.h"
#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD
#include "esp_private/hw_stack_guard.h"
#endif

#include "crash_log.h"

static const char *TAG = "crash_log";

#define CRASH_NVS_NAMESPACE "zbc"
#define KEY_CRASH           "crash"

/* The layout of crash_rec_t: change it with the struct. A record from a build
 * with another layout then fails the check and is dropped, never misread - but
 * for a stored version 1, which load() carries over. */
#define CRASH_MAGIC        0x32485243u   /* "CRH2" */
#define CRASH_BLOB_VERSION 2
#define CRASH_STACK_WORDS  16
/* How far up the stack to look for return addresses. Enough for several of
 * CHIP's or the Zigbee stack's frames; the scan stops sooner at the end of the
 * task's stack or with CRASH_STACK_WORDS found. */
#define CRASH_SCAN_BYTES   4096
/* Before this the clock is the 1970 a power-on starts from, not a time. */
#define CLOCK_VALID_S      1735689600    /* 2025-01-01 */

/* What the panic handler leaves for the next boot. Fixed-size fields only, so the
 * layout does not depend on the build's configuration. */
typedef struct {
    uint32_t magic;
    uint32_t pc, ra, sp, cause, tval;    /* mepc, ra, sp, mcause, mtval */
    int64_t  uptime_us;
    int64_t  at_us;                      /* wall clock, 0 when the boot had none */
    uint32_t free, min_free;             /* internal heap at the crash, and its low */
    uint8_t  n_stack;
    uint8_t  reserved[3];
    uint32_t stack[CRASH_STACK_WORDS];   /* code addresses on the stack, nearest first */
    char     task[16];
    char     reason[32];
    char     message[96];                /* an abort's; empty for a fault */
    char     version[32];
    char     build[12];                  /* the ELF SHA prefix /api/status calls `build` */
    /* Since version 2, and after everything version 1 had: see load(). */
    uint32_t stack_lo, stack_hi;         /* the crashed task's stack; 0 outside a task */
    /* A stack protection fault: where the hardware guard saw sp leave the bounds
     * it held it to, and those bounds. pc and sp are where its interrupt was
     * taken, which can be a few instructions on, with sp back in bounds. 0 for
     * any other crash. */
    uint32_t guard_pc, guard_lo, guard_hi;
    uint32_t crc;                        /* over everything above */
} crash_rec_t;

/* What NVS keeps: the last record, and what the boot after it said. */
typedef struct {
    uint8_t     version;
    uint8_t     reset_reason;   /* esp_reset_reason_t of the boot that followed */
    uint8_t     captured;       /* 0: the reset reason said crash, the handler left nothing */
    uint8_t     reserved;
    uint32_t    count;          /* crashes recorded since the key was first written */
    crash_rec_t rec;
} crash_blob_t;

/* A version 1 blob: this one up to stack_lo, then its crc and padding. Kept
 * across the update rather than dropped, since the crash it holds may be the one
 * the update is meant to look into. */
#define CRASH_BLOB_V1_SIZE 320
_Static_assert(offsetof(crash_blob_t, rec) + offsetof(crash_rec_t, stack_lo) + 8 == CRASH_BLOB_V1_SIZE,
               "the fields a version 1 record has moved");

/* In RTC memory, which a panic's restart leaves as it was; at power-on it holds
 * whatever the RAM came up with, which the magic and the CRC turn away. */
static RTC_NOINIT_ATTR crash_rec_t s_rtc;

static crash_blob_t s_blob;
static bool         s_have;        /* s_blob holds a crash */
static bool         s_this_boot;   /* and it is the one this boot restarted from */

/* Wall clock minus uptime, in microseconds, for dating a crash from inside the
 * panic handler, where the time functions' locks are off limits. 0: no clock. */
static volatile int64_t s_clock_base_us;

/* esp_reset_reason() in words. "panic" is a crash, "task_wdt" / "int_wdt" a
 * hang, "brownout" the power supply, "software" a restart this firmware asked
 * for (rename, OTA handover, hotspot, the Restart button), "power_on" the plug,
 * "usb" the serial port being opened. */
const char *crash_log_reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power_on";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    case ESP_RST_USB:       return "usb";
    case ESP_RST_JTAG:      return "jtag";
    case ESP_RST_EFUSE:     return "efuse";
    case ESP_RST_PWR_GLITCH: return "power_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default:                return "unknown";
    }
}

/* >>> host-test region: crash-scan <<< */
/* A stretch of code addresses, [lo, hi). */
typedef struct {
    uint32_t lo, hi;
} crash_range_t;

/* The code addresses among `n` words of a stack, nearest the stack pointer
 * first. RISC-V code here is built without frame pointers, so there is no chain
 * to walk; what a stack holds that points into code is mostly saved return
 * addresses, which is a backtrace with some noise in it (a function pointer in a
 * local, a return address left over from a call that already came back).
 * Instructions are 2-byte aligned (compressed), so an odd value is data. A value
 * found again right after itself is kept once. */
static size_t crash_scan(const uint32_t *words, size_t n, const crash_range_t *code, size_t n_code,
                         uint32_t *out, size_t max)
{
    size_t got = 0;
    for (size_t i = 0; i < n && got < max; i++) {
        const uint32_t v = words[i];
        if (v & 1u) {
            continue;
        }
        bool in = false;
        for (size_t c = 0; c < n_code && !in; c++) {
            in = v >= code[c].lo && v < code[c].hi;
        }
        if (in && (got == 0 || out[got - 1] != v)) {
            out[got++] = v;
        }
    }
    return got;
}
/* >>> end host-test region: crash-scan <<< */

static uint32_t rec_crc(const crash_rec_t *r)
{
    return esp_rom_crc32_le(0, (const uint8_t *)r, offsetof(crash_rec_t, crc));
}

/* A string the panic may have pointed at, if it is somewhere a read cannot fault:
 * an abort's message is on the stack or in flash, a fault's reason in flash. */
static void copy_str(char *dst, size_t cap, const char *src)
{
    dst[0] = '\0';
    if (!src || !(esp_ptr_in_dram(src) || esp_ptr_in_drom(src))) {
        return;
    }
    size_t i = 0;
    while (i + 1 < cap && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* Flash code and IRAM code, from the linker script. */
extern int _text_start, _text_end, _iram_text_start, _iram_text_end;

/* The words from sp up to the end of the task's stack, or CRASH_SCAN_BYTES. A
 * fault in an interrupt has sp on the interrupt stack, not the task's, and is
 * bounded by the scan length and the end of RAM alone. */
static void scan_stack(crash_rec_t *r)
{
    uint32_t sp = r->sp & ~3u;
    if (!esp_ptr_in_dram((const void *)sp)) {
        return;
    }
    uint32_t end = sp + CRASH_SCAN_BYTES;
    if (end > SOC_DRAM_HIGH) {
        end = SOC_DRAM_HIGH;
    }
    if (sp >= r->stack_lo && sp < r->stack_hi && r->stack_hi < end) {
        end = r->stack_hi;
    }
    const crash_range_t code[] = {
        { (uint32_t)&_text_start, (uint32_t)&_text_end },
        { (uint32_t)&_iram_text_start, (uint32_t)&_iram_text_end },
    };
    r->n_stack = (uint8_t)crash_scan((const uint32_t *)sp, (end - sp) / 4, code,
                                     sizeof(code) / sizeof(code[0]), r->stack, CRASH_STACK_WORDS);
}

/* Inside the panic, before IDF's handler prints anything: the cache is back on
 * (panicHandler re-enables it), other code may hold any lock, and the RTC
 * watchdog is running. So nothing here locks or allocates - the heap counters,
 * the timer and the task snapshot are plain reads. */
static void capture(const panic_info_t *info)
{
    crash_rec_t *r = &s_rtc;
    memset(r, 0, sizeof(*r));

    const RvExcFrame *f = info->frame;
    if (f) {
        r->pc    = f->mepc;
        r->ra    = f->ra;
        r->sp    = f->sp;
        r->cause = f->mcause;
        r->tval  = f->mtval;
    }
    r->uptime_us = esp_timer_get_time();
    const int64_t base = s_clock_base_us;
    r->at_us     = base ? base + r->uptime_us : 0;
    r->free      = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    r->min_free  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);

    /* An abort is an illegal instruction on purpose (panic_abort), and IDF's
     * handler overrides the reason only after this runs. */
    if (g_panic_abort) {
        copy_str(r->reason, sizeof(r->reason), "abort");
        copy_str(r->message, sizeof(r->message), g_panic_abort_details);
    } else {
        copy_str(r->reason, sizeof(r->reason), info->reason);
    }

    TaskHandle_t task = NULL;
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        task = xTaskGetCurrentTaskHandle();
        copy_str(r->task, sizeof(r->task), task ? pcTaskGetName(task) : NULL);
    }
    /* pxEndOfStack is where the task's sp started (the top, aligned down), so
     * everything pushed since lies below it. */
    TaskSnapshot_t snap;
    if (task && vTaskGetSnapshot(task, &snap) == pdTRUE) {
        r->stack_lo = (uint32_t)pxTaskGetStackStart(task);
        r->stack_hi = (uint32_t)snap.pxEndOfStack;
    }
#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD
    /* Its interrupt comes straight here (ASTDBG_ISR is the panic handler), so the
     * bounds are still the ones it fired against: the task's, or the interrupt
     * stack's. */
    if (f && f->mcause == ETS_ASSIST_DEBUG_INUM) {
        const uint32_t core = esp_hw_stack_guard_get_fired_cpu();
        if (core != ESP_HW_STACK_GUARD_NOT_FIRED) {
            r->guard_pc = esp_hw_stack_guard_get_pc(core);
            esp_hw_stack_guard_get_bounds(core, &r->guard_lo, &r->guard_hi);
        }
    }
#endif
    const esp_app_desc_t *app = esp_app_get_description();
    copy_str(r->version, sizeof(r->version), app->version);
    esp_app_get_elf_sha256(r->build, sizeof(r->build));

    scan_stack(r);

    r->magic = CRASH_MAGIC;
    r->crc   = rec_crc(r);
}

/* esp_panic_handler, wrapped (main/CMakeLists.txt). panic_handler() calls it from
 * another file, which is what makes --wrap reach it; an IDF upgrade must recheck
 * that it still does. */
void __real_esp_panic_handler(panic_info_t *info);

void __wrap_esp_panic_handler(panic_info_t *info)
{
    /* A fault inside the capture, or inside IDF's handler, comes back here: the
     * first panic is the one worth keeping. */
    static bool s_taken;
    if (!s_taken) {
        s_taken = true;
        capture(info);
    }
    __real_esp_panic_handler(info);
}

void crash_log_clock_synced(struct timeval *tv)
{
    if (tv && tv->tv_sec >= CLOCK_VALID_S) {
        s_clock_base_us = (int64_t)tv->tv_sec * 1000000 + tv->tv_usec - esp_timer_get_time();
    }
}

/* A restart nobody asked for. ESP_RST_WDT is the RTC watchdog, which is also what
 * ends a panic handler that hangs. */
static bool is_crash(esp_reset_reason_t r)
{
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
           r == ESP_RST_WDT || r == ESP_RST_CPU_LOCKUP;
}

/* The strings are printed with %s and handed to cJSON, so a record read back from
 * flash ends each one within its field whatever it holds. */
static void terminate(crash_rec_t *r)
{
    r->task[sizeof(r->task) - 1]       = '\0';
    r->reason[sizeof(r->reason) - 1]   = '\0';
    r->message[sizeof(r->message) - 1] = '\0';
    r->version[sizeof(r->version) - 1] = '\0';
    r->build[sizeof(r->build) - 1]     = '\0';
}

static void load(void)
{
    nvs_handle_t nvs;
    if (nvs_open(CRASH_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    size_t     len = sizeof(s_blob);
    const bool got = nvs_get_blob(nvs, KEY_CRASH, &s_blob, &len) == ESP_OK;
    nvs_close(nvs);
    if (got && len == CRASH_BLOB_V1_SIZE && s_blob.version == 1) {
        /* Its crc and padding landed on the fields it does not have. */
        const size_t from = offsetof(crash_blob_t, rec) + offsetof(crash_rec_t, stack_lo);
        memset((uint8_t *)&s_blob + from, 0, sizeof(s_blob) - from);
        s_blob.version = CRASH_BLOB_VERSION;
        len            = sizeof(s_blob);
    }
    s_have = got && len == sizeof(s_blob) && s_blob.version == CRASH_BLOB_VERSION;
    if (s_have) {
        terminate(&s_blob.rec);
    }
}

static void store(void)
{
    nvs_handle_t nvs;
    esp_err_t    err = nvs_open(CRASH_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, KEY_CRASH, &s_blob, sizeof(s_blob));
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "crash record not stored: %s", esp_err_to_name(err));
    }
}

void crash_log_init(void)
{
    /* A restart keeps the RTC timer and the time set on it, so a clock that was
     * right before a crash is right again now, before SNTP has answered. */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    crash_log_clock_synced(&tv);

    load();
    const esp_reset_reason_t why     = esp_reset_reason();
    const bool               written = s_rtc.magic == CRASH_MAGIC && s_rtc.crc == rec_crc(&s_rtc);
    if (written || is_crash(why)) {
        const uint32_t count = s_have ? s_blob.count : 0;
        memset(&s_blob, 0, sizeof(s_blob));
        s_blob.version      = CRASH_BLOB_VERSION;
        s_blob.reset_reason = (uint8_t)why;
        s_blob.captured     = written;
        s_blob.count        = count + 1;
        if (written) {
            s_blob.rec = s_rtc;
        }
        /* A crash from a boot without a clock, dated by this one: the restart
         * follows a panic within a second or two. */
        if (s_blob.rec.at_us == 0 && s_clock_base_us) {
            s_blob.rec.at_us = s_clock_base_us + esp_timer_get_time();
        }
        s_have      = true;
        s_this_boot = true;
        store();
        if (written) {
            ESP_LOGW(TAG, "the last boot crashed: %s%s%s in %s at pc 0x%08" PRIx32 " after %" PRId64 " s (%s)",
                     s_rtc.reason, s_rtc.message[0] ? " - " : "", s_rtc.message,
                     s_rtc.task[0] ? s_rtc.task : "?", s_rtc.pc, s_rtc.uptime_us / 1000000,
                     s_rtc.version);
        } else {
            ESP_LOGW(TAG, "the last boot ended in %s, with no crash record",
                     crash_log_reset_reason_name(why));
        }
    }
    s_rtc.magic = 0;
}

static cJSON *hex(uint32_t v)
{
    char s[11];
    snprintf(s, sizeof(s), "0x%08" PRIx32, v);
    return cJSON_CreateString(s);
}

static cJSON *str_or_null(const char *s)
{
    return s[0] ? cJSON_CreateString(s) : cJSON_CreateNull();
}

/* [lo, hi] as two hex strings; null when not known (0, 0). */
static cJSON *bounds(uint32_t lo, uint32_t hi)
{
    if (!lo && !hi) {
        return cJSON_CreateNull();
    }
    cJSON *a = cJSON_CreateArray();
    cJSON_AddItemToArray(a, hex(lo));
    cJSON_AddItemToArray(a, hex(hi));
    return a;
}

cJSON *crash_log_json(void)
{
    if (!s_have) {
        return cJSON_CreateNull();
    }
    const crash_rec_t *r = &s_blob.rec;
    cJSON             *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "count", s_blob.count);
    cJSON_AddBoolToObject(o, "this_boot", s_this_boot);
    cJSON_AddStringToObject(o, "reset_reason",
                            crash_log_reset_reason_name((esp_reset_reason_t)s_blob.reset_reason));
    cJSON_AddItemToObject(o, "at", r->at_us > 0 ? cJSON_CreateNumber((double)(r->at_us / 1000000))
                                                : cJSON_CreateNull());
    cJSON_AddBoolToObject(o, "captured", s_blob.captured);
    if (!s_blob.captured) {
        return o;
    }
    cJSON_AddNumberToObject(o, "uptime_s", (double)(r->uptime_us / 1000000));
    cJSON_AddItemToObject(o, "version", str_or_null(r->version));
    cJSON_AddItemToObject(o, "build", str_or_null(r->build));
    cJSON_AddItemToObject(o, "reason", str_or_null(r->reason));
    cJSON_AddItemToObject(o, "message", str_or_null(r->message));
    cJSON_AddItemToObject(o, "task", str_or_null(r->task));
    cJSON_AddItemToObject(o, "pc", hex(r->pc));
    cJSON_AddItemToObject(o, "ra", hex(r->ra));
    cJSON_AddItemToObject(o, "sp", hex(r->sp));
    cJSON_AddItemToObject(o, "stack_bounds", bounds(r->stack_lo, r->stack_hi));
    cJSON_AddNumberToObject(o, "mcause", r->cause);
    cJSON_AddItemToObject(o, "mtval", hex(r->tval));
    if (r->guard_pc) {
        cJSON *g = cJSON_AddObjectToObject(o, "guard");
        cJSON_AddItemToObject(g, "pc", hex(r->guard_pc));
        cJSON_AddItemToObject(g, "bounds", bounds(r->guard_lo, r->guard_hi));
        /* An overflow leaves sp past them. Back inside, the guard went off on
         * an sp that never left (esp-idf#14456: right after a load into sp) or
         * on another stack - guard.pc says which. */
        cJSON_AddBoolToObject(g, "sp_in_bounds", r->sp >= r->guard_lo && r->sp <= r->guard_hi);
    } else {
        cJSON_AddNullToObject(o, "guard");
    }
    cJSON_AddNumberToObject(o, "free", r->free);
    cJSON_AddNumberToObject(o, "min_free", r->min_free);
    cJSON *stack = cJSON_AddArrayToObject(o, "stack");
    for (size_t i = 0; i < r->n_stack && i < CRASH_STACK_WORDS; i++) {
        cJSON_AddItemToArray(stack, hex(r->stack[i]));
    }
    return o;
}
