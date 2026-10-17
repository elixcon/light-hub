#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "auth.h"
#include "auth_http.h"

uint16_t auth_http_presented_key(httpd_req_t *req, bool *from_cookie, char *canon)
{
    char   buf[64];
    size_t len = sizeof(buf);
    if (from_cookie) {
        *from_cookie = false;
    }
    if (httpd_req_get_cookie_val(req, AUTH_COOKIE, buf, &len) == ESP_OK) {
        const uint16_t id = auth_check(buf);
        if (id) {
            if (from_cookie) {
                *from_cookie = true;
            }
            if (canon) {
                auth_normalize(buf, canon);
            }
            return id;
        }
    }
    if (httpd_req_get_hdr_value_str(req, "Authorization", buf, sizeof(buf)) == ESP_OK &&
        strncasecmp(buf, "Bearer ", 7) == 0) {
        const uint16_t id = auth_check(buf + 7);
        if (id && canon) {
            auth_normalize(buf + 7, canon);
        }
        return id;
    }
    return 0;
}

void auth_http_set_cookie(httpd_req_t *req, char *buf, size_t cap, const char *canon)
{
    if (canon) {
        snprintf(buf, cap, AUTH_COOKIE "=%s; Path=/; Max-Age=315360000; HttpOnly; SameSite=Strict", canon);
    } else {
        snprintf(buf, cap, AUTH_COOKIE "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
    }
    httpd_resp_set_hdr(req, "Set-Cookie", buf);
}
