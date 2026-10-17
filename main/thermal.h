/* Overheat protection: samples the NTC set in settings_thermal() and holds the
 * wired lights off (local_lights_hold) while the board is too hot or the
 * sensor reads as broken. The arithmetic and the decision are ntc.c's, and
 * host-tested; this is the ADC and the timing.
 *
 * Once a second, five samples, the median. Not an esp_timer: adc_oneshot_read
 * takes a lock, and nothing that blocks may run in the shared timer task.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Before local_lights_start(): with a sensor set, the first reading is taken
 * here, so a board that starts too hot - or with a dead sensor - never switches
 * a light on. Never fails the boot; a sensor that cannot be read is a fault. */
esp_err_t thermal_start(void);

/* After settings_set_thermal(): the sampler picks the new configuration up and
 * starts its judgement afresh. Returns at once. */
void      thermal_reload(void);

/* ADC1 on this chip: GPIO 0-6. */
bool      thermal_pin_is_adc(int gpio);

typedef struct {
    bool     configured;     /* a pin is set */
    uint8_t  state;          /* zbc_thermal_state_t, when configured */
    bool     have;           /* a good reading has been seen since (re)configuring */
    int16_t  deci_c;         /* last good reading */
    int16_t  max_deci_c;     /* highest good reading since (re)configuring */
    uint8_t  trip_c;
    uint32_t trips;          /* since boot */
    int32_t  mv;             /* last pin voltage, -1 when the ADC could not be read */
} thermal_status_t;

void      thermal_get(thermal_status_t *out);

#ifdef __cplusplus
}
#endif
