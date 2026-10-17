/* Who may use the web UI and the REST API.
 *
 * A key is 20 characters of Crockford base32 - 100 random bits, shown as
 * K7M2Q-9XH4T-R3WZP-6NBVA - short enough to write down, and unguessable without
 * any rate limiting. The same key works as the browser's cookie, as an
 * `Authorization: Bearer` header, and typed into the lock screen; `kind` only
 * records how it came to exist, for the list in the UI.
 *
 * Only the SHA-256 of a key is stored, so the key itself is seen exactly once:
 * in the response that issued it.
 *
 * Nothing here decides whether a request is allowed - that is http_server.c,
 * which also knows about the setup portal. This module holds the table and the
 * failsafe window, and is a leaf in the sense settings.c is: it depends on NVS
 * and mbedtls, never on another module of this project.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZBC_AUTH_KEY_CHARS     20
#define ZBC_AUTH_KEY_CANON_LEN (ZBC_AUTH_KEY_CHARS + 1)       /* no dashes, NUL */
#define ZBC_AUTH_KEY_SHOWN_LEN (ZBC_AUTH_KEY_CHARS + 3 + 1)   /* four groups of five */
#define ZBC_AUTH_NAME_LEN      32

typedef enum {
    ZBC_KEY_BROWSER = 0,   /* issued to a browser as its cookie */
    ZBC_KEY_API,           /* created in Settings -> Access for a script */
} zbc_key_kind_t;

typedef struct {
    uint16_t id;
    uint8_t  kind;               /* zbc_key_kind_t */
    char     name[ZBC_AUTH_NAME_LEN];
    int64_t  last_used_us;       /* esp_timer time, 0 = not since this boot */
} zbc_auth_key_t;

esp_err_t auth_init(void);

/* Turns anything a person might type into the 20-character canonical form:
 * case, spaces and dashes are ignored, O reads as 0 and I or L as 1. `out` must
 * hold ZBC_AUTH_KEY_CANON_LEN. False when it cannot be a key at all. */
bool auth_normalize(const char *in, char *out);

/* The id of the stored key `presented` is, or 0. Accepts any form
 * auth_normalize does. */
uint16_t auth_check(const char *presented);

/* Notes that a key was just used. RAM only: a flash write per request would
 * wear the flash out, and there is no wall clock to put in it anyway. */
void auth_touch(uint16_t id);

size_t auth_key_count(void);

/* Makes a new key. `shown_out` (ZBC_AUTH_KEY_SHOWN_LEN) receives it with
 * dashes - the one and only time it exists outside this call. An empty name
 * gets a default. ESP_ERR_NO_MEM when the table is full. */
esp_err_t auth_create(zbc_key_kind_t kind, const char *name, char *shown_out, uint16_t *id_out);

/* Canonical form (no dashes) of a key for a cookie, from what auth_create gave. */
void auth_canonical(const char *shown, char *canon_out);

bool   auth_get(uint16_t id, zbc_auth_key_t *out);
size_t auth_snapshot(zbc_auth_key_t *buf, size_t max);
esp_err_t auth_revoke(uint16_t id);
esp_err_t auth_erase(void);

/* Whether a stored blob of this table is one the boot load takes rather than
 * discards. The one statement of that, shared with the configuration restore
 * (config_backup.c), which refuses a file whose table the load would throw away. */
bool auth_blob_ok(const void *blob, size_t len);

/* The press-5-times window. open() takes no mutex and never blocks, so the
 * esp_timer sampler in buttons.c can call it directly. */
void auth_failsafe_open(void);
void auth_failsafe_close(void);
int  auth_failsafe_left_s(void);

#ifdef __cplusplus
}
#endif
