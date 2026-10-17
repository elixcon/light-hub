#pragma once

#include <sys/time.h>

#include "cJSON.h"
#include "esp_system.h"

/*
 * What the last crash was, kept across the restart it causes.
 *
 * There is no core dump partition (the 4 MB table is full) and the panic
 * handler's register dump goes only to the serial port, which nobody is attached
 * to on an installed box - so `reset_reason: panic` said that a crash happened
 * and nothing about where. esp_panic_handler is wrapped (main/CMakeLists.txt):
 * before IDF's own handler prints and restarts, the wrapper copies the
 * registers, the task, the fault or abort message, the heap and the code
 * addresses on the stack into RTC memory, which a restart leaves alone. The next
 * boot moves it into NVS (key `crash`), where it stays until the next crash
 * replaces it, and /api/status shows it as `last_crash`.
 *
 * The addresses mean something only against the ELF of the build that crashed
 * (`version` and `build` in the record): `riscv32-esp-elf-addr2line -pfiaC -e
 * elixcon_light_hub.elf <pc> <ra> <stack...>`.
 */

/* Early in app_main, after nvs_flash_init: takes the record the last boot left,
 * or notes a crash that left none (a watchdog reset that never reached the panic
 * handler). Never fails - a record that cannot be stored is still shown. */
void crash_log_init(void);

/* The SNTP sync callback (esp_sntp_config_t.sync_cb), so a crash can be dated. */
void crash_log_clock_synced(struct timeval *tv);

/* `last_crash` for /api/status: null when no crash was ever recorded. */
cJSON *crash_log_json(void);

/* esp_reset_reason() in words, as /api/status spells it. */
const char *crash_log_reset_reason_name(esp_reset_reason_t r);
