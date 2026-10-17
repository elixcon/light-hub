/* Converting a device that runs another firmware - ESPHome, typically - to
 * this project's flash layout, from the updater that firmware was handed as an
 * ordinary OTA upload. See convert.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/* Reads the partition table on the flash and the one this updater carries.
 * Call before anything decides what to boot: on another firmware's layout, "start
 * the coordinator" would start that firmware instead. */
void convert_init(void);

/* The flash does not hold this project's partition table. */
bool convert_needed(void);

/* A conversion is running, or has written something: uploads must wait. */
bool convert_busy(void);

/* Starts the conversion in its own task. An empty ssid leaves the converted
 * device to open its setup hotspot. ESP_ERR_INVALID_STATE when it cannot start:
 * not needed, blocked, or already running. */
esp_err_t convert_start(const char *ssid, const char *password);

/* The device, the plan and the progress, as one JSON object. */
void convert_status_json(char *buf, size_t cap);
