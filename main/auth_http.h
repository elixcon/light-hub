/* The request side of access keys: finding the key a request carries, and
 * handing a browser its cookie.
 *
 * Shared by the coordinator's web server and the updater's, which both have to
 * accept the same cookie - a browser signed in to one is signed in to the other,
 * because they answer on the same address. Which requests need a key at all is
 * each server's own decision.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUTH_COOKIE "zbc_key"

/* The key a request carries - the cookie first, then an Authorization: Bearer
 * header - and its canonical form for a cookie when `canon` is given
 * (ZBC_AUTH_KEY_CANON_LEN). 0 when it carries none, or none that is ours. A
 * stale cookie does not hide a good header: a browser whose key was revoked can
 * still be handed a new one. */
uint16_t auth_http_presented_key(httpd_req_t *req, bool *from_cookie, char *canon);

/* `buf` must outlive the response: httpd keeps the pointer, not a copy. NULL
 * canon clears the cookie. No Secure flag - this is plain HTTP on a LAN, and a
 * Secure cookie would simply never be sent back. */
void auth_http_set_cookie(httpd_req_t *req, char *buf, size_t cap, const char *canon);

#ifdef __cplusplus
}
#endif
