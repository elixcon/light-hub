/* Configuration backup and restore: the settings and tables this box keeps in
 * NVS, as a JSON Lines file somebody can download and give back.
 *
 * Configuration only. Never the Zigbee network - the stack's own "zigbee"
 * namespace is opaque, and an old copy of it rolls the network frame counter
 * back, so every lamp would drop the coordinator's frames as replays - never
 * the Matter fabrics (nvs_matter), and never runtime state. What a file holds
 * is CB_ENTRIES in config_backup.c and nothing else. After a reset, lamps and
 * remotes are paired again and find their names, rooms, curves and buttons
 * waiting, because every table is keyed by IEEE address.
 *
 * A restore is staged in a namespace of its own and applied at the next boot,
 * before any table loads (config_backup_apply, from app_main). The tables are
 * read once and cannot be read again at runtime, and some of them write
 * themselves on their own - the registry on an announce, the Adaptive Lighting
 * running set every tick, a wired light's state after a change - so a key
 * restored under them would be overwritten from RAM before the restart.
 *
 * Built only with CONFIG_ZBC_CONFIG_BACKUP (on by default); without it
 * config_backup_apply() is an empty inline and the rest is not declared.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_ZBC_CONFIG_BACKUP

/* A backup is the settings, the tables and two secrets: the Wi-Fi network (its
 * SSID and password) and the access keys (their SHA-256 hashes). Only the
 * secrets can be left out of a download or kept through a restore, named
 * "wifi" and "auth".
 *
 * "wifi,auth" -> the mask the two calls below take; NULL or "" is none. False
 * for any other name, so a mistyped "omit" is refused rather than read as
 * "leave nothing out" - which would put the Wi-Fi password in a file meant to
 * have none. */
bool config_backup_parse_secrets(const char *list, uint8_t *out);

/* Takes the next piece of the file; anything but ESP_OK ends the export. */
typedef esp_err_t (*cb_sink_fn)(void *ctx, const char *text, size_t len);
/* Up to len bytes of the file into buf: how many, 0 at its end, < 0 when the
 * connection failed. */
typedef int (*cb_read_fn)(void *ctx, char *buf, size_t len);

/* Writes the whole file through `sink`, one line at a time, leaving out the
 * secrets in `omit`. No more than one table in memory at a time (the
 * registry, ~8 KB at 32 devices). An error ends the file before its last line,
 * and a file without its last line is one a restore refuses. */
esp_err_t config_backup_export(uint8_t omit, cb_sink_fn sink, void *ctx);

typedef struct {
    /* The devices keep their short addresses: the file was made on the network
     * that is up now. Otherwise every row is ZBC_SHORT_NONE until it joins. */
    bool    addresses_kept;
    bool    wifi;              /* the next boot replaces the Wi-Fi network */
    bool    auth;              /* and the access keys */
    int     entries;           /* written to the stage */
    int     skipped;           /* keys this build does not know */
    char    error[112];        /* why, when the answer is not ESP_OK */
} cb_result_t;

/* Reads a file and stages it for the next boot, keeping the secrets in `keep`
 * and any the file says were left out when it was made. `size_hint` is
 * the file's length, for the room check before anything is written.
 * ESP_ERR_INVALID_ARG for a file that is not one this build restores in full,
 * ESP_ERR_NVS_NOT_ENOUGH_SPACE when it does not fit, ESP_ERR_TIMEOUT when the
 * upload broke off - each with nothing staged and out->error saying why.
 * Nothing changes until the next boot, so the caller restarts on ESP_OK. */
esp_err_t config_backup_stage(cb_read_fn read, void *ctx, size_t size_hint, uint8_t keep, cb_result_t *out);

/* At boot, after NVS and before any table loads: carries out a staged restore,
 * if there is one, and clears the stage. Idempotent until its last step, so a
 * power cut part way through repeats it at the next boot. */
void config_backup_apply(void);

#else

static inline void config_backup_apply(void) {}

#endif

#ifdef __cplusplus
}
#endif
