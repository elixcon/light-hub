/* The routes for what is wired to this board, the same in both firmwares: the hub's
 * http_server.c and the node's node_http.c. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

#include "curve_db.h"
#include "light_db.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the editor edits, added to a light's device object. */
void wired_light_to_json(cJSON *d, uint16_t light_id, const zbc_light_t *light);

/* {"points":[[x,y],...]} or null (empty). Only the shape is checked. */
bool curve_from_json(const cJSON *obj, zbc_curve_t *out);

/* Why a button cannot read this pin, or NULL when it can. */
const char *button_pin_problem(int pin);

/* /api/status's thermal and wired members. */
cJSON *thermal_json(void);
cJSON *wired_json(void);

/* POST /api/lights, PATCH /api/lights/{id}. */
esp_err_t light_dispatch(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
