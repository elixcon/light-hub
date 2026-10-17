/* Lights wired to pins of this board, persisted to NVS.
 *
 * A wired light is known everywhere else by a made-up IEEE address and
 * endpoint 1 (see light_math.h), so groups, curves, button rules and the Matter
 * bridge take it exactly as they take a Zigbee lamp. It is not a row in
 * device_db: a new field there bumps DB_BLOB_VERSION and discards the names the
 * user gave their lights, and everything that walks that table - the neighbour
 * scan, interrogation, the settle reads - assumes a radio at the other end.
 *
 * Same shape as button_db: fixed static table, own recursive mutex, hands out
 * copies, own NVS blob with its own version.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "light_math.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZBC_LIGHT_NAME_LEN 32

typedef enum {
    ZBC_LIGHT_ONOFF = 0,              /* one pin, driven high or low */
    ZBC_LIGHT_CWWW,                   /* two PWM pins, cold and warm white */
    ZBC_LIGHT_DIM,                    /* one PWM pin, brightness only: a single-colour strip */
    ZBC_LIGHT_CNW,                    /* three PWM pins, cold, neutral and warm white */
    ZBC_LIGHT_KIND_COUNT,
} zbc_light_kind_t;

/* The PWM channels a light of this kind takes: three or two for tunable white,
 * one for a dimmable light, none for one that is only switched. */
static inline uint8_t zbc_light_pwm_channels(uint8_t kind)
{
    return kind == ZBC_LIGHT_CNW ? 3 : kind == ZBC_LIGHT_CWWW ? 2 : kind == ZBC_LIGHT_DIM ? 1 : 0;
}

/* Tunable white, with two channels or three: a colour temperature to take. */
static inline bool zbc_light_tunable(uint8_t kind)
{
    return kind == ZBC_LIGHT_CWWW || kind == ZBC_LIGHT_CNW;
}

#define ZBC_LIGHT_DEFAULT_HZ     4882     /* the fastest at 14 bits (light_math.h) */
#define ZBC_LIGHT_DEFAULT_COLD_K 6500
#define ZBC_LIGHT_DEFAULT_WARM_K 2700
#define ZBC_LIGHT_DEFAULT_NEUTRAL_K 4000
#define ZBC_LIGHT_KELVIN_MIN     1000
#define ZBC_LIGHT_KELVIN_MAX     10000

typedef struct {
    bool     in_use;
    uint16_t id;                      /* primary key, assigned here; 1..0xffff */
    char     name[ZBC_LIGHT_NAME_LEN];
    uint8_t  kind;                    /* zbc_light_kind_t */
    int8_t   gpio;                    /* -1 = not wired up yet; the cold white of a CWWW light */
    bool     active_low;              /* the pin goes LOW to light the LEDs */
    uint8_t  boot;                    /* zbc_light_boot_t */
    /* Tunable white (`pwm_hz` dimmable too). Kept, and ignored, while the light
     * is another kind, so switching the type back and forth does not lose them. */
    int8_t   gpio_warm;               /* -1 = not wired up yet */
    uint8_t  mix;                     /* zbc_light_mix_t */
    uint16_t pwm_hz;
    uint16_t cold_k;
    uint16_t warm_k;
    /* Three-channel tunable white: the neutral white between the two. */
    int8_t   gpio_neutral;            /* -1 = not wired up yet */
    uint16_t neutral_k;               /* warm_k < neutral_k < cold_k */
    /* ZBC_MIX_CUSTOM: one curve per channel, ZBC_CH_COLD/NEUTRAL/WARM (a
     * two-channel light ignores the neutral one). Kept whatever the mix, so
     * going back to a formula and then to the curves loses nothing. */
    zbc_mix_curve_t curve[ZBC_PWM_MAX_LIGHT_CH];
    /* Zero. A field added in here later changes nothing stored, where one added
     * after it would bump LIGHT_BLOB_VERSION. */
    uint8_t  reserved[8];
} zbc_light_t;

/* What a light was last doing, for its power-on setting. `level` means something
 * only for a light that dims, `mireds` only for a tunable white one; 0 is "not
 * known". */
typedef struct {
    uint8_t  on;
    uint8_t  level;
    uint16_t mireds;
} zbc_light_state_t;

esp_err_t light_db_init(void);

esp_err_t light_db_create(const char *name, uint16_t *out_id);
/* Whole-record update; id and in_use are kept. */
esp_err_t light_db_set(uint16_t id, const zbc_light_t *in);
esp_err_t light_db_rename(uint16_t id, const char *name);
esp_err_t light_db_remove(uint16_t id);

bool      light_db_get(uint16_t id, zbc_light_t *out);
size_t    light_db_snapshot(zbc_light_t *buf, size_t max);

/* True when another light already drives this pin - either pin of a tunable
 * white light. Pass id 0 to ask about every light. The button table is asked
 * separately - see http_server.c. */
bool      light_db_pin_taken(int gpio, uint16_t except_id);

/* Kept under a key of its own per light rather than in the blob, so switching a
 * light does not rewrite the whole table. Saving writes only when the state
 * differs from what is stored. */
bool      light_db_load_state(uint16_t id, zbc_light_state_t *out);
void      light_db_save_state(uint16_t id, const zbc_light_state_t *st);

esp_err_t light_db_erase(void);

/* Whether a stored blob of this table is one the boot load takes rather than
 * discards. The one statement of that, shared with the configuration restore
 * (config_backup.c), which refuses a file whose table the load would throw away. */
bool      light_db_blob_ok(const void *blob, size_t len);

#ifdef __cplusplus
}
#endif
