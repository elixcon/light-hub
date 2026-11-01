#include "node_debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_private/esp_clk.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"

#include "light_db.h"
#include "local_lights.h"
#include "node_buttons.h"
#include "node_main.h"
#include "node_zb.h"
#include "radio_coex.h"
#include "wifi_manager.h"

static char s_namespaces[160];

void node_debug_note_namespaces(void)
{
    /* Before anything is erased, so the first boot shows what it found. */
    char seen[8][16];
    int  n = 0;
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find("nvs", NULL, NVS_TYPE_ANY, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        bool known = false;
        for (int i = 0; i < n; i++) {
            known = known || strcmp(seen[i], info.namespace_name) == 0;
        }
        if (!known && n < 8) {
            strlcpy(seen[n++], info.namespace_name, sizeof(seen[0]));
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    for (int i = 0; i < n; i++) {
        strlcat(s_namespaces, seen[i], sizeof(s_namespaces));
        strlcat(s_namespaces, " ", sizeof(s_namespaces));
    }
}

static esp_err_t node_get(httpd_req_t *req)
{
    char *buf = malloc(4096);
    if (!buf) {
        return httpd_resp_send_500(req);
    }
    node_zb_stats_t z;
    node_zb_stats(&z);
    wifi_mgr_status_t w;
    wifi_manager_get_status(&w);
    radio_coex_stats_t m = { 0 };
    radio_coex_get_stats(&m);
    local_lights_fade_stats_t fade;
    local_lights_fade_stats(&fade);
    esp_ota_img_states_t ota = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(esp_ota_get_running_partition(), &ota);

    int len = snprintf(buf, 4096,
        "{\"mode\":%d,\"window_left_s\":%d,\"hub_down\":%d,\"uptime_ms\":%lld,\"mac\":{\"tx_done\":%lu,\"tx_no_ack\":%lu,\"tx_bad_ack\":%lu,\"tx_coex\":%lu,\"tx_cca_busy\":%lu,\"rx\":%lu,\"rx_coex\":%lu,\"rx_ack_coex\":%lu,\"tx_ack_coex\":%lu},\"uptime_s\":%lld,\"parent\":{\"short\":%u,\"ieee\":\"%02x%02x%02x%02x%02x%02x%02x%02x\",\"lqi\":%u,\"rssi\":%d,\"type\":%u,\"neighbours\":%u},"
        "\"fade\":{\"ticks\":%lu,\"late\":%lu,\"max_gap_ms\":%lu},\"cpu_mhz\":%d,\"heap\":%u,\"heap_min\":%u,\"project\":\"%s\",\"ota_state\":%d,"
        "\"nvs_found\":\"%s\",\"wifi\":{\"mode\":%d,\"connected\":%d,\"ip\":\"%s\",\"rssi\":%d,\"channel\":%d},"
        "\"zb\":{\"started\":%d,\"joined\":%d,\"rx_on\":%d,\"short\":%u,\"pan\":%u,\"channel\":%d,\"steers\":%lu,"
        "\"frames\":%lu,\"handled\":%lu,\"dups\":%lu,\"refused\":%lu,\"syncs\":%lu},\"lights\":[",
        (int)node_mode(), node_window_left_s(), node_buttons_hub_down(),
        esp_timer_get_time() / 1000,
        (unsigned long)m.tx_done, (unsigned long)m.tx_no_ack, (unsigned long)m.tx_bad_ack, (unsigned long)m.tx_coex,
        (unsigned long)m.tx_cca_busy, (unsigned long)m.rx, (unsigned long)m.rx_coex, (unsigned long)m.rx_ack_coex,
        (unsigned long)m.tx_ack_coex,
        esp_timer_get_time() / 1000000,
        z.parent_short, z.parent_ieee[7], z.parent_ieee[6], z.parent_ieee[5], z.parent_ieee[4], z.parent_ieee[3],
        z.parent_ieee[2], z.parent_ieee[1], z.parent_ieee[0], z.parent_lqi, z.parent_rssi, z.parent_type, z.neighbours,
        (unsigned long)fade.ticks, (unsigned long)fade.late, (unsigned long)fade.max_gap_ms,
        esp_clk_cpu_freq() / 1000000,
        (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
        esp_app_get_description()->project_name, (int)ota, s_namespaces,
        (int)w.mode, w.connected, w.ip, w.rssi, w.channel,
        z.started, z.joined, z.rx_on, z.short_addr, z.pan_id, z.channel, (unsigned long)z.steer_attempts,
        (unsigned long)z.frames, (unsigned long)z.handled, (unsigned long)z.duplicates,
        (unsigned long)z.refused, (unsigned long)z.attr_syncs);

    zbc_light_t lights[CONFIG_ZBC_MAX_LIGHTS];
    const int nl = (int)light_db_snapshot(lights, CONFIG_ZBC_MAX_LIGHTS);
    for (int i = 0; i < nl; i++) {
        zbc_local_state_t st = { .on = -1, .level = -1, .mireds = -1 };
        local_light_get(lights[i].id, &st);
        len += snprintf(buf + len, 4096 - len, "%s{\"id\":%u,\"name\":\"%s\",\"kind\":%u,\"hz\":%u,\"on\":%d,\"level\":%d,\"mireds\":%ld}",
                        i ? "," : "", lights[i].id, lights[i].name, lights[i].kind, lights[i].pwm_hz, st.on, st.level, (long)st.mireds);
    }
    len += snprintf(buf + len, 4096 - len, "],\"signals\":[");
    node_zb_signal_t sig[12];
    const int ns = node_zb_signals(sig, 12);
    for (int i = 0; i < ns; i++) {
        len += snprintf(buf + len, 4096 - len, "%s[%lu,\"%s\",%d]", i ? "," : "", (unsigned long)sig[i].at_ms, sig[i].name, sig[i].status);
    }
    len += snprintf(buf + len, 4096 - len, "],\"frames\":[");
    node_zb_frame_t fr[16];
    const int nf = node_zb_frames(fr, 16);
    for (int i = 0; i < nf; i++) {
        len += snprintf(buf + len, 4096 - len, "%s[%lu,%u,%u,%u,%u,%u,%u]", i ? "," : "", (unsigned long)fr[i].at_ms,
                        fr[i].src, fr[i].dst_ep, fr[i].cluster, fr[i].cmd, fr[i].fc, fr[i].handled);
    }
    len += snprintf(buf + len, 4096 - len, "]}");
    httpd_resp_set_type(req, "application/json");
    const esp_err_t err = httpd_resp_send(req, buf, len);
    free(buf);
    return err;
}

/* GET /node/pins?mask=<hex>&ms=<n>: every change of the masked pins, as sampled by
 * this loop, for checking PWM without a scope. */
static esp_err_t pins_get(httpd_req_t *req)
{
    char q[48] = "", v[12];
    uint32_t mask = 0, ms = 1;
    httpd_req_get_url_query_str(req, q, sizeof(q));
    if (httpd_query_key_value(q, "mask", v, sizeof(v)) == ESP_OK) mask = strtoul(v, NULL, 16);
    if (httpd_query_key_value(q, "ms", v, sizeof(v)) == ESP_OK) ms = strtoul(v, NULL, 10);
    if (ms < 1) ms = 1;
    if (ms > 5) ms = 5;
    for (int pin = 0; pin < 24; pin++) {
        if (mask & (1u << pin)) gpio_input_enable(pin);
    }
    enum { MAX_EDGES = 400 };
    typedef struct { uint32_t cyc, val; } edge_t;
    edge_t *edges = malloc(MAX_EDGES * sizeof(edge_t));
    if (!edges) return httpd_resp_send_500(req);

    const uint32_t span = ms * (esp_clk_cpu_freq() / 1000);
    int n = 0;
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&mux);
    const uint32_t start = esp_cpu_get_cycle_count();
    uint32_t last = REG_READ(GPIO_IN_REG) & mask;
    edges[n++] = (edge_t){ 0, last };
    while (esp_cpu_get_cycle_count() - start < span && n < MAX_EDGES) {
        const uint32_t now = REG_READ(GPIO_IN_REG) & mask;
        if (now != last) {
            edges[n++] = (edge_t){ esp_cpu_get_cycle_count() - start, now };
            last = now;
        }
    }
    portEXIT_CRITICAL(&mux);

    char *buf = malloc(MAX_EDGES * 24 + 64);
    if (!buf) { free(edges); return httpd_resp_send_500(req); }
    int len = snprintf(buf, 64, "{\"cpu_hz\":%d,\"edges\":[", esp_clk_cpu_freq());
    for (int i = 0; i < n; i++) {
        len += sprintf(buf + len, "%s[%lu,%lu]", i ? "," : "", (unsigned long)edges[i].cyc, (unsigned long)edges[i].val);
    }
    len += sprintf(buf + len, "]}");
    httpd_resp_set_type(req, "application/json");
    const esp_err_t err = httpd_resp_send(req, buf, len);
    free(buf);
    free(edges);
    return err;
}

/* GET /node/blob?kb=<n>: zeros, to time Wi-Fi at this clock. */
static esp_err_t blob_get(httpd_req_t *req)
{
    char q[16] = "", v[8];
    int kb = 256;
    httpd_req_get_url_query_str(req, q, sizeof(q));
    if (httpd_query_key_value(q, "kb", v, sizeof(v)) == ESP_OK) kb = atoi(v);
    static char chunk[1024];
    httpd_resp_set_type(req, "application/octet-stream");
    for (int i = 0; i < kb; i++) {
        if (httpd_resp_send_chunk(req, chunk, sizeof(chunk)) != ESP_OK) return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static void restart_later(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

/* While on trial this is a rollback to the updater. */
static esp_err_t restart_post(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "restarting\n");
    xTaskCreate(restart_later, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* POST /node/level?id=&level=&ds=: drives a light straight, so PWM can be checked
 * with or without a network. */
static esp_err_t level_post(httpd_req_t *req)
{
    char q[48] = "", v[8];
    int id = 1, level = 127, ds = 0;
    httpd_req_get_url_query_str(req, q, sizeof(q));
    if (httpd_query_key_value(q, "id", v, sizeof(v)) == ESP_OK) id = atoi(v);
    if (httpd_query_key_value(q, "level", v, sizeof(v)) == ESP_OK) level = atoi(v);
    if (httpd_query_key_value(q, "ds", v, sizeof(v)) == ESP_OK) ds = atoi(v);
    const esp_err_t err = local_light_set_level((uint16_t)id, (uint8_t)level, (uint16_t)ds);
    return httpd_resp_sendstr(req, esp_err_to_name(err));
}

/* POST /node/press?key=&ev=: a gesture as though a pin had made it. */
static esp_err_t press_post(httpd_req_t *req)
{
    char q[32] = "", v[8];
    int key = 0, ev = 1;
    httpd_req_get_url_query_str(req, q, sizeof(q));
    if (httpd_query_key_value(q, "key", v, sizeof(v)) == ESP_OK) key = atoi(v);
    if (httpd_query_key_value(q, "ev", v, sizeof(v)) == ESP_OK) ev = atoi(v);
    node_buttons_inject((uint8_t)key, (uint8_t)ev);
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t node_debug_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    httpd_handle_t srv;
    esp_err_t err = httpd_start(&srv, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    static const httpd_uri_t routes[] = {
        { .uri = "/node",         .method = HTTP_GET,  .handler = node_get },
        { .uri = "/node/pins",    .method = HTTP_GET,  .handler = pins_get },
        { .uri = "/node/blob",    .method = HTTP_GET,  .handler = blob_get },
        { .uri = "/node/level",   .method = HTTP_POST, .handler = level_post },
        { .uri = "/node/press",   .method = HTTP_POST, .handler = press_post },
        { .uri = "/node/restart", .method = HTTP_POST, .handler = restart_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(srv, &routes[i]);
    }
    return ESP_OK;
}
