/* The routes for what is wired to this board, the same in both firmwares: the hub's
 * http_server.c and the node's node_http.c. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "gradient.h"
#include "esp_err.h"
#include "esp_http_server.h"

#include "button_db.h"
#include "curve_db.h"
#include "light_db.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the editor edits, added to a light's device object. */
void wired_light_to_json(cJSON *d, uint16_t light_id, const zbc_light_t *light);

/* A gradient as the API spells it: {"colors": [{"hue": 0-360, "sat": 0-100} |
 * {"kelvin": 1000-10000}, ...]}, one to five colours from a strip's start to its
 * end. Returns NULL, or why not. */
const char *gradient_from_json(const cJSON *json, zbc_gradient_t *out);
/* Adds "gradient": {"colors": [{"hue", "sat"}, ...], "state"} to an endpoint. */
void gradient_to_json(cJSON *ep, const zbc_gradient_t *g, const char *state);

/* {"points":[[x,y],...]} or null (empty). Only the shape is checked. */
bool curve_from_json(const cJSON *obj, zbc_curve_t *out);

/* Why a button cannot read this pin, or NULL when it can. */
const char *button_pin_problem(int pin);

/* A pin button's wiring, as both firmwares' routes write and read it: gpio, inverted,
 * pull and input. The second says what is wrong with a body, or NULL. Whether another
 * button or a light has the pin is the caller's to ask. */
void        button_wiring_to_json(cJSON *o, const zbc_button_t *b);
const char *button_wiring_from_json(const cJSON *json, zbc_button_t *b);

/* /api/status's thermal and wired members. */
cJSON *thermal_json(void);
cJSON *wired_json(void);

/* POST /api/lights, PATCH /api/lights/{id}. */
esp_err_t light_dispatch(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
