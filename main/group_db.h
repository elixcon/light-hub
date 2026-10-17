/* Logical groups of lights, persisted to NVS.
 *
 * A group is purely a coordinator-side idea: a name and a set of (IEEE, endpoint)
 * members that the HTTP layer fans a command out to. Nothing is stored on the
 * lamps, so a lamp that is factory reset and rejoins is still a member.
 *
 * Same shape as device_db: written and read from more than one task, so every
 * entry point takes the internal mutex and hands out copies rather than pointers.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "device_db.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZBC_GROUP_NAME_LEN 32

typedef struct {
    uint8_t ieee[ZBC_IEEE_LEN];
    uint8_t ep;
} zbc_group_member_t;

typedef struct {
    bool     in_use;
    uint16_t id;                  /* primary key, assigned here, never reused while alive */
    char     name[ZBC_GROUP_NAME_LEN];
    uint8_t  member_count;
    zbc_group_member_t members[CONFIG_ZBC_MAX_GROUP_MEMBERS];
    /* Last brightness this group was commanded to, in percent. The lamps in a
     * group drift apart by a percent or two, so their average would never settle
     * on the value the slider asked for; the commanded value is what the slider
     * is showing. Runtime only, ZBC_UNKNOWN after a reboot. */
    int16_t  last_pct;
} zbc_group_t;

esp_err_t group_db_init(void);

esp_err_t group_db_create(const char *name, uint16_t *out_id);
esp_err_t group_db_rename(uint16_t id, const char *name);
esp_err_t group_db_remove(uint16_t id);

esp_err_t group_db_add_member(uint16_t id, const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep);
esp_err_t group_db_remove_member(uint16_t id, const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep);

bool      group_db_get(uint16_t id, zbc_group_t *out);
/* Copies up to max groups into buf; returns how many were written. */
size_t    group_db_snapshot(zbc_group_t *buf, size_t max);

/* Remembers what the group was last told to do, for the UI to render.
 * ZBC_UNKNOWN forgets it, which is what ending a sweep does: the commanded value
 * is then the level the last command was still travelling towards, not the one
 * the user stopped on. */
void      group_db_note_level(uint16_t id, int pct);

/* Drops a device from every group it is in; called when it is unpaired. */
void      group_db_forget_device(const uint8_t ieee[ZBC_IEEE_LEN]);

esp_err_t group_db_erase(void);

/* Whether a stored blob of this table is one the boot load takes rather than
 * discards. The one statement of that, shared with the configuration restore
 * (config_backup.c), which refuses a file whose table the load would throw away. */
bool      group_db_blob_ok(const void *blob, size_t len);

#ifdef __cplusplus
}
#endif
