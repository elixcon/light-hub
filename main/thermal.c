/* See thermal.h. */
#include "thermal.h"

#include <inttypes.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "soc/soc_caps.h"

#include "local_lights.h"
#include "ntc.h"
#include "settings.h"
#include "state_rev.h"

static const char *TAG = "thermal";

#define VREF_MV        3300             /* the divider's supply: the board's 3V3 */
#define PERIOD_MS      1000
#define SAMPLE_GAP_MS  2
#define RAW_MAX        ((1 << SOC_ADC_RTC_MAX_BITWIDTH) - 1)

/* Owned by the sampling task once it runs; by thermal_start() before that. */
static adc_oneshot_unit_handle_t s_unit;
static adc_cali_handle_t         s_cali;
static adc_channel_t             s_chan;
static zbc_thermal_cfg_t         s_cfg;
static zbc_thermal_t             s_st;

static TaskHandle_t      s_task;
static volatile bool     s_reload;
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
static thermal_status_t  s_pub = { .mv = -1 };
static uint32_t          s_trips_before;   /* trips of earlier configurations, since boot */

bool thermal_pin_is_adc(int gpio)
{
    adc_unit_t    unit;
    adc_channel_t chan;
    return gpio >= 0 && adc_oneshot_io_to_channel(gpio, &unit, &chan) == ESP_OK && unit == ADC_UNIT_1;
}

static void teardown(void)
{
    if (s_cali) {
        adc_cali_delete_scheme_curve_fitting(s_cali);
        s_cali = NULL;
    }
    if (s_unit) {
        adc_oneshot_del_unit(s_unit);
        s_unit = NULL;
    }
}

/* False when the pin cannot be read at all, which the caller counts as a fault. */
static bool setup(void)
{
    teardown();
    settings_thermal(&s_cfg);
    s_trips_before += s_st.trips;
    zbc_thermal_init(&s_st);
    if (s_cfg.gpio < 0) {
        return true;
    }
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(s_cfg.gpio, &unit, &s_chan) != ESP_OK || unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "GPIO %d is not an ADC1 pin", s_cfg.gpio);
        return false;
    }
    const adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    const adc_oneshot_chan_cfg_t      ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_oneshot_new_unit(&ucfg, &s_unit) != ESP_OK ||
        adc_oneshot_config_channel(s_unit, s_chan, &ccfg) != ESP_OK) {
        ESP_LOGE(TAG, "Could not set up the ADC on GPIO %d", s_cfg.gpio);
        teardown();
        return false;
    }
    const adc_cali_curve_fitting_config_t cal = {
        .unit_id = ADC_UNIT_1, .chan = s_chan, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) {
        /* No eFuse calibration: the raw scale is close enough to be useful, and a
         * sensor that reads a few degrees off is better than none. */
        ESP_LOGW(TAG, "No ADC calibration on this chip; reading uncalibrated");
        s_cali = NULL;
    }
    ESP_LOGI(TAG, "Overheat sensor on GPIO %d: B %u, %" PRIu32 " Ohm at 25 °C, %" PRIu32 " Ohm to %s, shutoff %u °C",
             s_cfg.gpio, s_cfg.b, s_cfg.r0_ohm, s_cfg.rs_ohm, s_cfg.ntc_to_3v3 ? "GND" : "3V3", s_cfg.trip_c);
    return true;
}

/* One judged reading: five samples, the median. A raw value pinned at either
 * end of the scale is an open or shorted NTC, whatever the calibration says. */
static void sample(bool adc_ok)
{
    int32_t mv    = -1;
    bool    valid = false;
    int16_t deci  = 0;
    if (adc_ok && s_unit) {
        int32_t raw[5];
        bool    read_ok = true;
        for (int i = 0; i < 5 && read_ok; i++) {
            int r = 0;
            read_ok = adc_oneshot_read(s_unit, s_chan, &r) == ESP_OK;
            raw[i]  = r;
            if (i < 4) {
                vTaskDelay(pdMS_TO_TICKS(SAMPLE_GAP_MS));
            }
        }
        if (read_ok) {
            const int32_t med = zbc_median5(raw);
            int           v   = (int)((int64_t)med * VREF_MV / RAW_MAX);
            if (s_cali) {
                adc_cali_raw_to_voltage(s_cali, med, &v);
            }
            mv = v;
            const zbc_ntc_cfg_t ncfg = {
                .b = s_cfg.b, .r0_ohm = s_cfg.r0_ohm, .rs_ohm = s_cfg.rs_ohm, .ntc_to_3v3 = s_cfg.ntc_to_3v3,
            };
            valid = med > 0 && med < RAW_MAX && zbc_ntc_celsius((uint32_t)v, VREF_MV, &ncfg, &deci);
        }
    }

    const uint8_t was     = s_st.state;
    const bool    changed = zbc_thermal_step(&s_st, valid, deci, (int16_t)(s_cfg.trip_c * 10));

    portENTER_CRITICAL(&s_mux);
    s_pub = (thermal_status_t){
        .configured = true, .state = s_st.state, .have = s_st.have, .deci_c = s_st.deci_c,
        .max_deci_c = s_st.max_deci_c, .trip_c = s_cfg.trip_c, .trips = s_trips_before + s_st.trips, .mv = mv,
    };
    portEXIT_CRITICAL(&s_mux);

    if (changed) {
        const bool held = s_st.state != ZBC_THERMAL_OK;
        if (s_st.state == ZBC_THERMAL_FAULT) {
            ESP_LOGE(TAG, "Overheat sensor reads as broken (%" PRIi32 " mV): wired lights held off", mv);
        } else if (s_st.state == ZBC_THERMAL_HOT) {
            ESP_LOGE(TAG, "Board at %d.%d °C, shutoff %u °C: wired lights held off",
                     s_st.deci_c / 10, abs(s_st.deci_c % 10), s_cfg.trip_c);
        } else {
            ESP_LOGW(TAG, "Board back to %d.%d °C (was %s): wired lights may be switched on again",
                     s_st.deci_c / 10, abs(s_st.deci_c % 10), was == ZBC_THERMAL_FAULT ? "a sensor fault" : "too hot");
        }
        local_lights_hold(held);
        zbc_state_bump();
    }
}

/* A new configuration: judged afresh, and with no sensor nothing is held. */
static bool configure(void)
{
    const bool adc_ok = setup();
    if (s_cfg.gpio < 0) {
        portENTER_CRITICAL(&s_mux);
        s_pub = (thermal_status_t){ .configured = false, .trip_c = s_cfg.trip_c, .trips = s_trips_before, .mv = -1 };
        portEXIT_CRITICAL(&s_mux);
        local_lights_hold(false);
        zbc_state_bump();
        return adc_ok;
    }
    /* The first reading decides at once (ntc.h): a board that is fine is let
     * go, one that is not is held from this moment. */
    sample(adc_ok);
    if (s_st.state == ZBC_THERMAL_OK) {
        local_lights_hold(false);
    }
    zbc_state_bump();
    return adc_ok;
}

static void thermal_task(void *arg)
{
    bool adc_ok = (bool)(intptr_t)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PERIOD_MS));
        if (s_reload) {
            s_reload = false;
            adc_ok   = configure();
            continue;
        }
        if (s_cfg.gpio >= 0) {
            sample(adc_ok);
        }
    }
}

esp_err_t thermal_start(void)
{
    if (s_task) {
        return ESP_OK;
    }
    zbc_thermal_init(&s_st);
    const bool adc_ok = configure();
    if (xTaskCreate(thermal_task, "thermal", 3072, (void *)(intptr_t)adc_ok, 2, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "Could not start the overheat sampler");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void thermal_reload(void)
{
    s_reload = true;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void thermal_get(thermal_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_pub;
    portEXIT_CRITICAL(&s_mux);
}
