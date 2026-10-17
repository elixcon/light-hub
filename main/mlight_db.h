/* Matter lamps paired to this box, persisted to NVS.
 *
 * One row per light endpoint of a Matter device: a Wi-Fi bulb is one row, a hub
 * such as a Hue Bridge one row per lamp behind it, all with the same node id. A
 * lamp is known everywhere else by a made-up IEEE address on endpoint 1 (see
 * light_math.h), so groups, curves, button rules and the Matter bridge take it
 * exactly as they take a Zigbee lamp or a wired light; lighting.c alone knows
 * which of the three it is talking to.
 *
 * Not in device_db, for the reason wired lights are not: a u64 node id does not
 * fit there, and a new field bumps DB_BLOB_VERSION and discards the names the
 * user gave every Zigbee lamp. Not in light_db either - its record has eight
 * reserved bytes and a node id plus an endpoint is ten.
 *
 * Only what pairing and a rescan learn is kept here. Whether a lamp is on, and
 * how bright, is matter_ctl's, in RAM, from the lamp's own subscription - the
 * same rule device_db follows for its runtime fields.
 *
 * Same shape as light_db: fixed static table, own recursive mutex, hands out
 * copies, own NVS blob with its own version.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "light_math.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZBC_MLIGHT_NAME_LEN 32

/* flags */
#define ZBC_MLIGHT_BRIDGED  (1u << 0)   /* behind a hub: one of its bridged endpoints */
#define ZBC_MLIGHT_VERIFIED (1u << 1)   /* its device attestation was checked - never, yet */

typedef struct {
    bool     in_use;
    uint16_t id;                        /* primary key, assigned here; 1..0xffff */
    uint64_t node_id;                   /* its operational node id on our fabric */
    uint16_t remote_ep;                 /* the endpoint on that node */
    char     name[ZBC_MLIGHT_NAME_LEN];
    char     vendor[ZBC_MLIGHT_NAME_LEN];   /* VendorName, "" when it gave none */
    char     product[ZBC_MLIGHT_NAME_LEN];  /* ProductName */
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t features;                  /* ZBC_FEAT_* */
    uint16_t ct_min_mireds;             /* ColorTempPhysicalMinMireds, 0 unknown */
    uint16_t ct_max_mireds;
    uint8_t  flags;                     /* ZBC_MLIGHT_* */
    /* The index our fabric has in the lamp's own fabric table, from its answer
     * to AddNOC: what RemoveFabric names when the lamp is removed. The same on
     * every row of one node. */
    uint8_t  lamp_fabric;
    /* Zero. A field added in here later changes nothing stored, where one added
     * after it would bump MLIGHT_BLOB_VERSION. */
    uint8_t  reserved[8];
} zbc_mlight_t;

esp_err_t mlight_db_init(void);

/* Adds a row, or - when the node already has one for that endpoint, as after a
 * rescan or a second pairing of the same device - refreshes what the device
 * says about itself and keeps the id and the name the user gave it. */
esp_err_t mlight_db_put(const zbc_mlight_t *in, uint16_t *out_id);
esp_err_t mlight_db_rename(uint16_t id, const char *name);
esp_err_t mlight_db_remove(uint16_t id);

bool      mlight_db_get(uint16_t id, zbc_mlight_t *out);
/* 0 when the node has no row for that endpoint. */
uint16_t  mlight_db_find(uint64_t node_id, uint16_t remote_ep);
/* How many rows a node still has. */
size_t    mlight_db_node_rows(uint64_t node_id);
size_t    mlight_db_snapshot(zbc_mlight_t *buf, size_t max);
/* The ids alone, in snapshot order - so a caller can copy one row at a time. */
size_t    mlight_db_list_ids(uint16_t *out, size_t max);
size_t    mlight_db_count(void);

esp_err_t mlight_db_erase(void);

/* Whether a stored blob of this table is one the boot load takes rather than
 * discards. The one statement of that, shared with the configuration restore
 * (config_backup.c), which refuses a file whose table the load would throw away. */
bool      mlight_db_blob_ok(const void *blob, size_t len);

#ifdef __cplusplus
}
#endif
