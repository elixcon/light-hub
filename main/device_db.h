/* In-RAM registry of paired Zigbee devices, persisted to NVS.
 *
 * Written from the Zigbee stack task, read from the HTTP task, so every entry
 * point takes the internal mutex and hands out copies rather than pointers.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZBC_IEEE_LEN     8
#define ZBC_NAME_LEN     32
#define ZBC_STRING_LEN   33  /* ZCL character strings here are capped at 32 chars */

/* The device has not reported or answered a read for this attribute yet. */
#define ZBC_UNKNOWN (-1)

/* The short address of a row restored from a configuration backup made on
 * another network (config_backup.c): the device is not on this one until it
 * joins again, and the address it had there may belong to another device here.
 * 0xFFFF is the broadcast address, never a device's own. No lookup by short
 * address finds such a row, device_db_short_by_ieee() answers false for it, and
 * a caller that copies a whole row checks zbc_short_known() before sending to
 * it. The device's next announce (device_db_upsert) gives it its address back. */
#define ZBC_SHORT_NONE 0xFFFF

static inline bool zbc_short_known(uint16_t short_addr)
{
    return short_addr != ZBC_SHORT_NONE;
}

/* What an endpoint can do, from its input cluster list and, for colour, from the
 * ColorCapabilities attribute. */
#define ZBC_FEAT_ONOFF      (1u << 0)
#define ZBC_FEAT_LEVEL      (1u << 1)  /* dimmable */
#define ZBC_FEAT_COLOR_TEMP (1u << 2)  /* tunable white */
#define ZBC_FEAT_COLOR_HS   (1u << 3)  /* colour, hue/saturation */
#define ZBC_FEAT_COLOR_XY   (1u << 4)  /* colour, CIE xy only */
#define ZBC_FEAT_ANY_COLOR  (ZBC_FEAT_COLOR_HS | ZBC_FEAT_COLOR_XY)
/* This endpoint sends us button presses - a wireless remote, or a wall switch
 * with no lamp behind it. A spare bit of the existing features word on purpose:
 * a new field in zbc_endpoint_t changes sizeof(zbc_device_t), the load-time size
 * check then rejects the stored table, and the names the user gave their lights
 * are gone. Bits 9..15 are still free. */
#define ZBC_FEAT_SWITCH     (1u << 5)
/* A Hue dimmer whose buttons have been heard on Philips' own cluster (0xFC00),
 * on its button endpoint: from then on that is where its presses are read, and
 * its endpoint 1 commands - the same presses in a poorer language - are not.
 * Persisted, because the binding that makes it true lives on the dimmer and
 * survives our restarts; cleared when the dimmer announces itself, which is
 * what a re-pair (and a lost binding) looks like from here. */
#define ZBC_FEAT_HUE_BUTTONS (1u << 6)
/* A motion sensor: Occupancy Sensing is an input cluster, or the endpoint was
 * heard reporting it (a Xiaomi sensor often never answers the simple descriptor
 * request). Never a lamp and never a remote. */
#define ZBC_FEAT_MOTION      (1u << 7)
/* The sensor has been heard reporting "unoccupied" itself, so its own report
 * starts the no-motion time; one that never does (Xiaomi) gets the time counted
 * from its last motion. Learned from the first such report and persisted:
 * interrogation leaves it alone, since no cluster list says it. */
#define ZBC_FEAT_MOTION_CLEARS (1u << 8)

/* Attributes we track per endpoint, addressed by this enum so one setter serves
 * every ZCL attribute we care about. */
typedef enum {
    ZBC_ATTR_ONOFF = 0,
    ZBC_ATTR_LEVEL,       /* 0-254 */
    ZBC_ATTR_CT,          /* mireds */
    ZBC_ATTR_CT_MIN,      /* physical limit, mireds (coolest) */
    ZBC_ATTR_CT_MAX,      /* physical limit, mireds (warmest) */
    ZBC_ATTR_HUE,         /* 0-254 */
    ZBC_ATTR_SATURATION,  /* 0-254 */
    ZBC_ATTR_COLOR_MODE,  /* 0 = hue/sat, 1 = xy, 2 = colour temperature */
} zbc_attr_t;

typedef struct {
    uint8_t  ep;            /* endpoint number */
    uint16_t device_id;     /* HA device id from the simple descriptor */
    uint16_t features;      /* ZBC_FEAT_* */
    int8_t   on_off;        /* 1 on, 0 off, ZBC_UNKNOWN */
    int16_t  level;         /* 0-254, ZBC_UNKNOWN */
    int32_t  ct_mireds;     /* current colour temperature, ZBC_UNKNOWN */
    int32_t  ct_min_mireds; /* coolest the lamp can go, ZBC_UNKNOWN */
    int32_t  ct_max_mireds; /* warmest the lamp can go, ZBC_UNKNOWN */
    int16_t  hue;           /* 0-254, ZBC_UNKNOWN */
    int16_t  saturation;    /* 0-254, ZBC_UNKNOWN */
    int8_t   color_mode;    /* ZBC_UNKNOWN until reported */
} zbc_endpoint_t;

typedef struct {
    bool     in_use;
    uint8_t  ieee[ZBC_IEEE_LEN];              /* primary key: never changes */
    uint16_t short_addr;                      /* changes on rejoin */
    char     name[ZBC_NAME_LEN];              /* user-assigned, falls back to model */
    char     manufacturer[ZBC_STRING_LEN];
    char     model[ZBC_STRING_LEN];
    uint8_t  ep_count;
    zbc_endpoint_t eps[CONFIG_ZBC_MAX_ENDPOINTS_PER_DEVICE];
    int64_t  last_seen_us;                    /* esp_timer_get_time() of last contact */
    bool     online;
} zbc_device_t;

esp_err_t device_db_init(void);

/* Create the device if unknown, otherwise refresh its short address; always
 * marks it online and updates last_seen. Set *created when a row was added. */
esp_err_t device_db_upsert(const uint8_t ieee[ZBC_IEEE_LEN], uint16_t short_addr, bool *created);

esp_err_t device_db_set_identity(const uint8_t ieee[ZBC_IEEE_LEN], const char *manufacturer, const char *model);

/* Link quality (0-255) from the last neighbour scan that saw this device.
 * Runtime only, and deliberately not part of zbc_device_t: the registry blob is
 * written raw, so a new field there would discard every stored device and the
 * names with them. device_db_lqa() returns ZBC_UNKNOWN until a scan reports one. */
esp_err_t device_db_set_lqa(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t lqa);
int       device_db_lqa(const uint8_t ieee[ZBC_IEEE_LEN]);
esp_err_t device_db_add_endpoint(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint16_t device_id, uint16_t features);
/* Adds capability bits discovered after the endpoint was created, e.g. from the
 * ColorCapabilities attribute. */
esp_err_t device_db_add_features(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint16_t features);
/* Takes bits away again. Only interrogation uses this: the simple descriptor is
 * the authority on what an endpoint is, so a device recorded as a lamp must be
 * able to become a remote, and device_db_add_features can only add. */
esp_err_t device_db_clear_features(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep, uint16_t features);
/* Forgets a recorded endpoint the simple descriptor says is neither a light nor
 * a remote (a Hue dimmer's battery endpoint is not a second remote). Refused
 * (ESP_ERR_INVALID_STATE) for an endpoint recorded as a light: groups and curves
 * name those, and a lamp does not stop being one. ESP_OK when there was nothing
 * to forget. */
esp_err_t device_db_remove_endpoint(const uint8_t ieee[ZBC_IEEE_LEN], uint8_t ep);
esp_err_t device_db_rename(const uint8_t ieee[ZBC_IEEE_LEN], const char *name);
esp_err_t device_db_remove(const uint8_t ieee[ZBC_IEEE_LEN]);

/* ZCL responses carry the short address, so state updates come in by short addr. */
esp_err_t device_db_set_attr_by_short(uint16_t short_addr, uint8_t ep, zbc_attr_t attr, int32_t value);
esp_err_t device_db_touch_by_short(uint16_t short_addr);
bool      device_db_short_by_ieee(const uint8_t ieee[ZBC_IEEE_LEN], uint16_t *out_short);
bool      device_db_ieee_by_short(uint16_t short_addr, uint8_t out_ieee[ZBC_IEEE_LEN]);

/* The feature bits of one endpoint, by short address, or 0 when there is no such
 * endpoint. A cheap lookup on purpose: it is asked on every attribute of every
 * report and read response, from the Zigbee task, and device_db_get copies the
 * whole device row - endpoints, names and all - onto the caller's stack. */
uint16_t  device_db_features_by_short(uint16_t short_addr, uint8_t ep);

/* The Basic cluster's ModelIdentifier, by short address, into `out` (at least
 * ZBC_STRING_LEN). False, with `out` empty, when there is no such device. Cheap
 * for the same reason: it is asked on every remote press, from the Zigbee task. */
bool      device_db_model_by_short(uint16_t short_addr, char out[ZBC_STRING_LEN]);

/* When the device was last heard from (esp_timer_get_time()), by short address;
 * 0 when never since boot, or when there is no such device. Cheap, for the
 * same reason as the two above: it is asked before every send. */
int64_t   device_db_last_seen_by_short(uint16_t short_addr);

/* How many endpoints the registry holds for a device, by short address; -1 when
 * there is no such device. 0 is a device whose interrogation never finished. */
int       device_db_ep_count_by_short(uint16_t short_addr);

esp_err_t device_db_set_online(const uint8_t ieee[ZBC_IEEE_LEN], bool online);
bool      device_db_get(const uint8_t ieee[ZBC_IEEE_LEN], zbc_device_t *out);

/* Copies up to max devices into buf; returns how many were written. */
size_t    device_db_snapshot(zbc_device_t *buf, size_t max);
/* The IEEE addresses of up to max devices, in the order snapshot gives them -
 * so a caller can copy one row at a time rather than the whole table. */
size_t    device_db_list_ieee(uint8_t out[][ZBC_IEEE_LEN], size_t max);
size_t    device_db_count(void);
/* Whether this device is reachable as far as anything here can tell: online,
 * and heard from within max_age_us. One definition, so a caller working from a
 * copied row and the count below cannot come to disagree about what "there" is. */
bool      device_db_is_fresh(const zbc_device_t *dev, int64_t max_age_us);
/* How many devices are fresh by that definition - the number the web page's
 * "offline" badge is derived from, which can change with nothing else moving;
 * see state_rev.h. */
size_t    device_db_count_fresh(int64_t max_age_us);

/* Runs cb for each known device from inside the lock; used to re-read state at
 * boot. cb must not call back into device_db. */
void      device_db_for_each(void (*cb)(const zbc_device_t *dev, void *ctx), void *ctx);

esp_err_t device_db_save(void);
esp_err_t device_db_erase(void);

/* Whether a stored registry blob is one the boot load takes rather than
 * discards: its version, and a size that fits it. The one statement of that,
 * shared with the configuration restore, which refuses a file whose table the
 * load would throw away. */
bool      device_db_blob_ok(const void *blob, size_t len);
/* Sets every row of such a blob to ZBC_SHORT_NONE: a registry restored onto
 * another network keeps its names, and none of its addresses. */
void      device_db_blob_forget_addresses(void *blob, size_t len);

/* "0x00124b0001abcdef" <-> bytes, most significant byte first. */
void      zbc_ieee_to_str(const uint8_t ieee[ZBC_IEEE_LEN], char out[19]);
bool      zbc_str_to_ieee(const char *str, uint8_t out[ZBC_IEEE_LEN]);

#ifdef __cplusplus
}
#endif
