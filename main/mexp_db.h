/* Which Zigbee device endpoints the Matter bridge shows on their own, persisted.
 *
 * Every room is a Matter light whatever this says (matter_bridge.h); this is the
 * opt-in, off by default, for one device's endpoint besides: a lamp, a remote's
 * endpoint (each of its buttons becomes a switch) or a motion sensor. What an
 * endpoint becomes is decided by the bridge from the device, never stored here,
 * so a lamp that turns out to have colour is shown with it.
 *
 * A table of its own rather than a field of zbc_device_t: that record is written
 * raw and has no spare byte, and its feature bits are rewritten by interrogation.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "device_db.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t ieee[ZBC_IEEE_LEN];
    uint8_t ep;
} mexp_entry_t;

esp_err_t mexp_db_init(void);
bool      mexp_db_has(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep);
/* ESP_ERR_NO_MEM when the endpoint is not yet in the table and the table is full.
 * Setting what is already set writes nothing. On any error the table is as it
 * was: a change that could not be stored is not kept until the restart either. */
esp_err_t mexp_db_set(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, bool expose);
/* Copies up to max entries; returns how many. */
size_t    mexp_db_list(mexp_entry_t *out, size_t max);
/* The device is deleted, or one of its endpoints is: the next device to join with
 * that address, or the light a Light Node gives that endpoint next, starts off. */
void      mexp_db_forget_device(const uint8_t ieee[ZBC_IEEE_LEN]);
void      mexp_db_forget_endpoint(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep);
esp_err_t mexp_db_erase(void);

/* Whether a stored blob is one the boot load takes, shared with the configuration
 * restore (config_backup.c). */
bool      mexp_db_blob_ok(const void *blob, size_t len);

#ifdef __cplusplus
}
#endif
