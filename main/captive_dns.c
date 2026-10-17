#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"

#include "captive_dns.h"

static const char *TAG = "captive_dns";

#define DNS_PORT        53
#define DNS_MAX_LEN     512
#define DNS_QR_RESPONSE 0x80
#define DNS_QTYPE_A     1
#define DNS_QCLASS_IN   1
/* Short TTL: the portal address stops being valid as soon as we reboot into STA. */
#define DNS_ANSWER_TTL  60

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint8_t  flags1;
    uint8_t  flags2;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

static TaskHandle_t s_task;
static int          s_sock = -1;

/* Builds a response that points every A query at the SoftAP address. Returns the
 * response length, or 0 if the query is not one we should answer. */
static int build_response(const uint8_t *req, int req_len, uint8_t *rsp, int rsp_cap, uint32_t ap_ip)
{
    if (req_len < (int)sizeof(dns_header_t) || req_len > DNS_MAX_LEN) {
        return 0;
    }
    dns_header_t hdr;
    memcpy(&hdr, req, sizeof(hdr));
    if ((hdr.flags1 & DNS_QR_RESPONSE) || ntohs(hdr.qd_count) != 1) {
        return 0;  /* only plain single-question queries */
    }

    /* Walk the QNAME labels to find where the question ends. */
    int pos = sizeof(dns_header_t);
    while (pos < req_len && req[pos] != 0) {
        int label_len = req[pos];
        if (label_len > 63) {
            return 0;  /* compression pointer: not expected in a query */
        }
        pos += label_len + 1;
    }
    pos += 1 + 4;  /* terminating zero + QTYPE + QCLASS */
    if (pos > req_len) {
        return 0;
    }

    uint16_t qtype = (req[pos - 4] << 8) | req[pos - 3];
    if (qtype != DNS_QTYPE_A) {
        return 0;
    }

    int answer_len = 2 + 2 + 2 + 4 + 2 + 4;  /* name ptr, type, class, ttl, rdlen, rdata */
    if (pos + answer_len > rsp_cap) {
        return 0;
    }

    memcpy(rsp, req, pos);
    dns_header_t *out = (dns_header_t *)rsp;
    out->flags1   = DNS_QR_RESPONSE | 0x01;  /* response, recursion desired echoed */
    out->flags2   = 0x80;                    /* recursion available, no error */
    out->an_count = htons(1);
    out->ns_count = 0;
    out->ar_count = 0;

    uint8_t *a = rsp + pos;
    *a++ = 0xC0;  /* pointer to the question's name */
    *a++ = 0x0C;
    *a++ = 0; *a++ = DNS_QTYPE_A;
    *a++ = 0; *a++ = DNS_QCLASS_IN;
    uint32_t ttl = htonl(DNS_ANSWER_TTL);
    memcpy(a, &ttl, 4); a += 4;
    *a++ = 0; *a++ = 4;
    memcpy(a, &ap_ip, 4); a += 4;

    return pos + answer_len;
}

static void captive_dns_task(void *arg)
{
    (void)arg;
    uint8_t req[DNS_MAX_LEN];
    uint8_t rsp[DNS_MAX_LEN];

    struct sockaddr_in bind_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_sock < 0 || bind(s_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "Cannot bind UDP/53, captive portal disabled (errno %d)", errno);
        if (s_sock >= 0) {
            close(s_sock);
            s_sock = -1;
        }
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Captive DNS responder listening");

    while (s_sock >= 0) {
        struct sockaddr_in from;
        socklen_t          from_len = sizeof(from);
        int len = recvfrom(s_sock, req, sizeof(req), 0, (struct sockaddr *)&from, &from_len);
        if (len < 0) {
            break;
        }

        esp_netif_ip_info_t ip_info = {0};
        esp_netif_t        *ap      = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (!ap || esp_netif_get_ip_info(ap, &ip_info) != ESP_OK) {
            continue;
        }

        int rsp_len = build_response(req, len, rsp, sizeof(rsp), ip_info.ip.addr);
        if (rsp_len > 0) {
            sendto(s_sock, rsp, rsp_len, 0, (struct sockaddr *)&from, from_len);
        }
    }

    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t captive_dns_start(void)
{
    if (s_task) {
        return ESP_OK;
    }
    if (xTaskCreate(captive_dns_task, "captive_dns", 3072, NULL, 4, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void captive_dns_stop(void)
{
    int sock = s_sock;
    s_sock   = -1;
    if (sock >= 0) {
        shutdown(sock, 0);
        close(sock);
    }
}
