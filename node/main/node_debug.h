/* Temporary diagnostics for the first test images: pwm-test has no cable, so this
 * is the only window into it. Goes when node_http.c arrives. */
#pragma once

#include "esp_err.h"

esp_err_t node_debug_start(void);
void      node_debug_note_namespaces(void);
