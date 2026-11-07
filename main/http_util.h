/* What a JSON route needs, whichever firmware it is in: answering, a response
 * written in chunks as it is built, the request body, the path, and who gets in.
 * Shared by the hub's http_server.c and the node's node_http.c. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

#include "auth.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A whole button - its wiring and every gesture's rules - is the largest body
 * anything posts here: about 1.8 KB with a full pool of rules that each name a
 * light. */
#define MAX_BODY_LEN     4096

#define JSON_STREAM_BUF 2048

esp_err_t send_json(httpd_req_t *req, cJSON *root, const char *status);
esp_err_t send_error(httpd_req_t *req, const char *status, const char *message);
esp_err_t send_ok(httpd_req_t *req, const char *status);
void      add_kelvin(cJSON *obj, const char *name, int32_t mireds);

/* The stream writes the response head itself, so the content type and any one
 * more header are set here, after json_stream_begin, not through httpd: what
 * httpd_resp_set_hdr holds is never sent. Both strings must last until the
 * first chunk goes out. */
typedef struct {
    httpd_req_t *req;
    char        *block;        /* the allocation: room for the head, buf, room for a chunk's end */
    char        *buf;          /* JSON_STREAM_BUF of text, inside block */
    size_t       len;
    size_t       total;
    bool         failed;
    bool         head_sent;
    const char  *type;         /* "application/json" unless set */
    const char  *hdr_name;     /* one more header, or NULL */
    const char  *hdr_value;
} json_stream_t;

bool      json_stream_begin(json_stream_t *js, httpd_req_t *req);
void      json_stream_flush(json_stream_t *js);
void      json_stream_raw(json_stream_t *js, const char *text);
bool      json_stream_item(json_stream_t *js, const char *sep, cJSON *item);
bool      json_stream_open(json_stream_t *js, const char *sep, cJSON *item);
void      json_stream_value(json_stream_t *js, cJSON *item);
void      json_stream_element(json_stream_t *js, bool *first, cJSON *item);
void      json_stream_key(json_stream_t *js, bool *first, const char *name);
void      json_stream_member(json_stream_t *js, bool *first, const char *name, cJSON *value);
esp_err_t json_stream_fail(json_stream_t *js);
esp_err_t json_stream_end(json_stream_t *js);

/* Reads the request body and parses it as JSON. Returns NULL on any problem,
 * having already answered the request. */
cJSON *read_json_body(httpd_req_t *req);
/* Copies path segment `index` of the URI (0 = first) into out. */
bool   uri_segment(const char *uri, int index, char *out, size_t out_size);
/* The "name" of a body, when it is a string with something in it. */
const char *name_from_json(const cJSON *json);
/* Where a string is in a table of names, or -1. */
int name_index(const char *const *names, int n, const cJSON *item);

typedef enum {
    ACCESS_LOCKED = 0,
    ACCESS_SETUP,      /* the provisioning AP: whoever is on it is standing at the board */
    ACCESS_OPEN,       /* no keys yet */
    ACCESS_FAILSAFE,   /* a button was pressed five times */
} access_mode_t;

extern const char *const ACCESS_NAMES[];
access_mode_t access_mode(void);

typedef esp_err_t (*route_fn_t)(httpd_req_t *req);

/* Route handlers: the real handler rides in user_ctx. See http_util.c. */
esp_err_t guarded(httpd_req_t *req);
esp_err_t watched(httpd_req_t *req);

void   reboot_task(void *arg);

/* The access state, as /api/auth and /api/state carry it. */
cJSON *auth_json(httpd_req_t *req);
/* The Wi-Fi status, as /api/status carries it. */
cJSON *wifi_json(void);
/* This firmware and the updater beside it. */
cJSON *ota_json(void);

/* The last members of /api/status, the same in both: uptime, why this boot happened, the
 * last crash, and which image this is. */
void   stream_firmware_status(json_stream_t *js, bool *first);
/* Whether `?rev=` is what the page saw last time, so nothing has changed since. */
bool   rev_unchanged(httpd_req_t *req, const char *rev);
/* The scope of POST /api/factory_reset. False when there is to be no reset: the request
 * has been answered, and the handler returns *ret. */
bool   reset_scope_from_body(httpd_req_t *req, bool *zigbee_only, esp_err_t *ret);

/* Routes the hub and the node serve alike. */
esp_err_t index_get(httpd_req_t *req);
esp_err_t auth_state_get(httpd_req_t *req);
esp_err_t auth_session_post(httpd_req_t *req);
esp_err_t send_session(httpd_req_t *req, const zbc_auth_key_t *k, const char *shown);
esp_err_t wifi_scan_get(httpd_req_t *req);
esp_err_t wifi_post(httpd_req_t *req);
esp_err_t wifi_test_post(httpd_req_t *req);
esp_err_t wifi_test_get(httpd_req_t *req);
esp_err_t wifi_resume_post(httpd_req_t *req);
esp_err_t updater_dispatch(httpd_req_t *req);
esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err);

#ifdef __cplusplus
}
#endif
