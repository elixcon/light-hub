/* The WLED interface - see wled.h.
 *
 * What the three clients this is written for ask of a WLED, read from their
 * source (2026-10-09):
 *
 * - Hyperion.ng: GET /json/ and /json/info (`ver` at least 0.11.0 for DDP), PUT
 *   /json/state {"live":true,"on":true} to start and {"live":false,"tt":0,
 *   "on":false} - or the state it saved - to stop; DDP to UDP 4048.
 * - HyperHDR: GET /json (info.udpport, info.leds.count, ver), PUT /json/state
 *   {"on":..,"live":..} or its saved state; DRGB, timeout 255, to `udpport`.
 * - LedFx: GET /json/info (leds.count, leds.rgbw, vid, name), /json/state
 *   (segments) and /json/cfg when `vid` >= 2110060; finds devices by mDNS
 *   _wled._tcp; DDP to 4048, or DNRGB to 21324.
 *
 * Not offered: raw RGB on 19446 (Hyperion's non-DDP choice), E1.31, Art-Net,
 * WLED's websocket and effects. The UDP side is one task that exists from the
 * first time the interface is switched on: it holds a 4 KB stack, so a box
 * that never uses it pays nothing. */
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "mdns.h"

#include "group_db.h"
#include "lighting.h"
#include "settings.h"
#include "wifi_manager.h"
#include "wled.h"

#if CONFIG_ZBC_WLED

static const char *TAG = "wled";

#define WLED_DDP_PORT 4048
#define WLED_UDP_PORT 21324
/* What a client is told about this "WLED": a version new enough for DDP and for
 * everything the three clients test, and a build id to match. */
#define WLED_VER      "0.14.4"
#define WLED_VID      2405180

/* >>> host-test region: wled-parse <<< */

/* One packet's pixels, as either protocol carries them. */
typedef struct {
    uint16_t       start;      /* the first pixel */
    uint16_t       count;      /* pixels in the packet */
    uint8_t        stride;     /* bytes per pixel: 3 RGB, 4 RGBW or a WARLS tuple */
    bool           indexed;    /* WARLS: each pixel is (index, r, g, b) */
    bool           rgbw;
    const uint8_t *data;
    int32_t        timeout_ms; /* the source's: 0 none given, WLED_NO_TIMEOUT for 255 */
} wled_packet_t;

#define WLED_NO_TIMEOUT INT32_MAX

/* DDP (www.3waylabs.com/ddp): a 10-byte header, 14 with a timecode - flags with
 * the version in the top two bits, a sequence number, the data type, the
 * destination, a big-endian byte offset and a big-endian length. Queries,
 * replies and anything for another destination than the default output (1) or
 * everyone (255) are not pixels. The data type's middle three bits are 3 for
 * RGBW; anything else is taken as RGB, which is what WLED does too. */
static bool ddp_parse(const uint8_t *p, size_t len, wled_packet_t *out)
{
    if (len < 10 || (p[0] & 0xC0) != 0x40 || (p[0] & 0x06) != 0) {
        return false;
    }
    const size_t hdr = (p[0] & 0x10) ? 14 : 10;
    if (len < hdr || (p[3] != 1 && p[3] != 255)) {
        return false;
    }
    const uint32_t offset = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | p[7];
    const size_t   dlen   = ((size_t)p[8] << 8) | p[9];
    const bool     rgbw   = ((p[2] >> 3) & 0x07) == 3;
    const uint8_t  stride = rgbw ? 4 : 3;
    if (offset % stride != 0 || offset / stride > UINT16_MAX) {
        return false;
    }
    const size_t avail = len - hdr < dlen ? len - hdr : dlen;
    memset(out, 0, sizeof(*out));
    out->start  = (uint16_t)(offset / stride);
    out->count  = (uint16_t)(avail / stride);
    out->stride = stride;
    out->rgbw   = rgbw;
    out->data   = p + hdr;
    return out->count > 0;
}

/* WLED's own UDP realtime protocols: byte 0 the protocol, byte 1 the timeout in
 * seconds (255 for none), then 1 WARLS (index, r, g, b), 2 DRGB, 3 DRGBW, or 4
 * DNRGB (a big-endian start index, then RGB). Protocol 0 is WLED devices
 * telling each other their state, and is not pixels. */
static bool wled_udp_parse(const uint8_t *p, size_t len, wled_packet_t *out)
{
    if (len < 2) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->timeout_ms = p[1] == 255 ? WLED_NO_TIMEOUT : (int32_t)p[1] * 1000;
    out->data       = p + 2;
    switch (p[0]) {
    case 1:
        out->indexed = true;
        out->stride  = 4;
        break;
    case 2:
        out->stride = 3;
        break;
    case 3:
        out->stride = 4;
        out->rgbw   = true;
        break;
    case 4:
        if (len < 4) {
            return false;
        }
        out->start  = (uint16_t)((p[2] << 8) | p[3]);
        out->data   = p + 4;
        out->stride = 3;
        break;
    default:
        return false;
    }
    out->count = (uint16_t)((len - (size_t)(out->data - p)) / out->stride);
    return out->count > 0;
}

/* Puts a packet's pixels into the buffer the lamps are fed from, `max` pixels of
 * RGB; the rest of a longer strip falls away. A white channel is added to the
 * other three: these lamps have none of their own. */
static void wled_apply(const wled_packet_t *pk, uint8_t *rgb, size_t max)
{
    for (uint16_t k = 0; k < pk->count; k++) {
        const uint8_t *d  = pk->data + (size_t)k * pk->stride;
        size_t         px = (size_t)pk->start + k;
        if (pk->indexed) {
            px = d[0];
            d++;
        }
        if (px >= max) {
            if (pk->indexed) {
                continue;
            }
            break;
        }
        const unsigned w = pk->rgbw ? d[3] : 0;
        for (int c = 0; c < 3; c++) {
            const unsigned v = d[c] + w;
            rgb[px * 3 + c]  = (uint8_t)(v > 255 ? 255 : v);
        }
    }
}

/* >>> end host-test region: wled-parse <<< */

/* The task's own state and what /api/status shows of it. Counters are words
 * written by the task alone, so the status poll reads them without a lock. */
static TaskHandle_t s_task;
static volatile bool s_mdns_on;
static uint8_t      s_master = 255;      /* WLED's `bri` */
static uint8_t      s_px[ZB_ENT_MAX_LAMPS * 3];
static struct {
    bool     listening;
    uint32_t packets;                     /* parsed as pixels */
    uint32_t ignored;                     /* not pixels: sync, queries, garbage */
    uint32_t refused;                     /* pixels lighting.c did not take */
    uint16_t fps_in;                      /* packets in the last whole second */
    char     source[16];                  /* who sent the last one */
    const char *proto;                    /* "DDP" or "UDP" */
    int64_t  last_us;
} s_rx;

static int udp_listen(uint16_t port)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in a = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        ESP_LOGW(TAG, "Cannot listen on UDP %u (errno %d)", port, errno);
        close(fd);
        return -1;
    }
    return fd;
}

static void mdns_sync(bool on)
{
    if (on == s_mdns_on) {
        return;
    }
    if (on) {
        uint8_t mac[6];
        char    hex[13];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(hex, sizeof(hex), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
                 mac[5]);
        mdns_txt_item_t txt[] = { { "mac", hex } };
        if (mdns_service_add(NULL, "_wled", "_tcp", 80, txt, 1) == ESP_OK) {
            s_mdns_on = true;
        }
    } else {
        mdns_service_remove("_wled", "_tcp");
        s_mdns_on = false;
    }
}

static void take_packet(const uint8_t *p, size_t len, bool ddp, const struct sockaddr_in *from)
{
    wled_packet_t pk;
    if (!(ddp ? ddp_parse(p, len, &pk) : wled_udp_parse(p, len, &pk))) {
        s_rx.ignored++;
        return;
    }
    wled_apply(&pk, s_px, ZB_ENT_MAX_LAMPS);
    s_rx.packets++;
    s_rx.proto   = ddp ? "DDP" : "UDP";
    s_rx.last_us = esp_timer_get_time();
    inet_ntoa_r(from->sin_addr, s_rx.source, sizeof(s_rx.source));

    const uint16_t room = settings_wled_room();
    const int64_t  t_us = pk.timeout_ms == WLED_NO_TIMEOUT ? INT64_MAX : (int64_t)pk.timeout_ms * 1000;
    if (room == 0 || lighting_stream_put(room, s_px, ZB_ENT_MAX_LAMPS, s_master, t_us) != ESP_OK) {
        s_rx.refused++;
    }
}

static void wled_task(void *arg)
{
    (void)arg;
    int      ddp = -1, udp = -1;
    uint8_t  buf[256];
    uint32_t window_n = 0;
    int64_t  window_us = esp_timer_get_time();

    for (;;) {
        const bool on = settings_wled_on();
        mdns_sync(on);
        if (!on) {
            if (ddp >= 0) {
                close(ddp);
                ddp = -1;
            }
            if (udp >= 0) {
                close(udp);
                udp = -1;
            }
            s_rx.listening = false;
            s_rx.fps_in    = 0;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* until wled_reload */
            continue;
        }
        if (ddp < 0) {
            ddp = udp_listen(WLED_DDP_PORT);
        }
        if (udp < 0) {
            udp = udp_listen(WLED_UDP_PORT);
        }
        s_rx.listening = ddp >= 0 && udp >= 0;
        if (ddp < 0 && udp < 0) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        fd_set rd;
        FD_ZERO(&rd);
        if (ddp >= 0) {
            FD_SET(ddp, &rd);
        }
        if (udp >= 0) {
            FD_SET(udp, &rd);
        }
        struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
        const int      ready = select((ddp > udp ? ddp : udp) + 1, &rd, NULL, NULL, &tv);
        for (int k = 0; ready > 0 && k < 2; k++) {
            const int fd = k == 0 ? ddp : udp;
            if (fd < 0 || !FD_ISSET(fd, &rd)) {
                continue;
            }
            struct sockaddr_in from;
            socklen_t          flen = sizeof(from);
            const int          n    = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
            if (n > 0) {
                take_packet(buf, (size_t)n, k == 0, &from);
                window_n++;
            }
        }
        const int64_t now = esp_timer_get_time();
        if (now - window_us >= 1000000) {
            s_rx.fps_in = (uint16_t)window_n;
            window_n    = 0;
            window_us   = now;
        }
    }
}

void wled_reload(void)
{
    /* A room no longer chosen, or an interface switched off, must not go on
     * streaming: its lamps go back to how they were. */
    lighting_stream_status_t st;
    lighting_stream_status(&st);
    if (st.active && (!settings_wled_on() || st.group_id != settings_wled_room())) {
        lighting_stream_stop(st.group_id, false);
    }
    if (!s_task && settings_wled_on()) {
        if (xTaskCreate(wled_task, "wled", 4096, NULL, 4, &s_task) != pdPASS) {
            s_task = NULL;
            ESP_LOGE(TAG, "Cannot start the WLED interface");
            return;
        }
        ESP_LOGI(TAG, "WLED interface on: DDP on UDP %d, WLED UDP on %d", WLED_DDP_PORT, WLED_UDP_PORT);
    } else if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void wled_start(void)
{
    wled_reload();
}

/* ------------------------------------------------------- WLED's JSON API */

static size_t pixel_count(void)
{
    zb_ent_lamp_t lamps[ZB_ENT_MAX_LAMPS];
    const uint16_t room = settings_wled_room();
    return room ? lighting_stream_lamps(room, lamps, ZB_ENT_MAX_LAMPS) : 0;
}

static bool room_lit(void)
{
    lighting_stream_status_t st;
    lighting_stream_status(&st);
    if (st.active) {
        return true;
    }
    zbc_group_t g;
    const uint16_t room = settings_wled_room();
    return room && group_db_get(room, &g) && lighting_group_any_on(&g);
}

cJSON *wled_info_json(void)
{
    const size_t n = pixel_count();
    lighting_stream_status_t st;
    lighting_stream_status(&st);
    wifi_mgr_status_t wifi;
    wifi_manager_get_status(&wifi);

    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "ver", WLED_VER);
    cJSON_AddNumberToObject(info, "vid", WLED_VID);
    cJSON *leds = cJSON_AddObjectToObject(info, "leds");
    cJSON_AddNumberToObject(leds, "count", (double)n);
    cJSON_AddBoolToObject(leds, "rgbw", false);
    cJSON_AddNumberToObject(leds, "wv", 0);
    cJSON_AddNumberToObject(leds, "cct", 0);
    cJSON_AddNumberToObject(leds, "pwr", 0);
    cJSON_AddNumberToObject(leds, "fps", st.active ? 1000 / 50 : 0);
    cJSON_AddNumberToObject(leds, "maxpwr", 0);
    cJSON_AddNumberToObject(leds, "maxseg", 1);
    cJSON *seglc = cJSON_AddArrayToObject(leds, "seglc");
    cJSON_AddItemToArray(seglc, cJSON_CreateNumber(1));
    cJSON_AddNumberToObject(leds, "lc", 1);
    cJSON_AddBoolToObject(info, "str", false);
    cJSON_AddStringToObject(info, "name", settings_name());
    cJSON_AddNumberToObject(info, "udpport", WLED_UDP_PORT);
    cJSON_AddBoolToObject(info, "live", st.active);
    cJSON_AddNumberToObject(info, "liveseg", -1);
    cJSON_AddStringToObject(info, "lm", st.active && s_rx.proto ? s_rx.proto : "");
    cJSON_AddStringToObject(info, "lip", st.active ? s_rx.source : "");
    cJSON_AddNumberToObject(info, "ws", -1);
    cJSON_AddNumberToObject(info, "fxcount", 1);
    cJSON_AddNumberToObject(info, "palcount", 1);
    cJSON *w = cJSON_AddObjectToObject(info, "wifi");
    cJSON_AddNumberToObject(w, "rssi", wifi.rssi);
    /* WLED's own 0-100 reading of the RSSI. */
    const int signal = wifi.rssi <= -100 ? 0 : wifi.rssi >= -50 ? 100 : 2 * (wifi.rssi + 100);
    cJSON_AddNumberToObject(w, "signal", signal);
    cJSON_AddNumberToObject(w, "channel", wifi.channel);
    cJSON_AddStringToObject(info, "arch", "esp32");
    cJSON_AddNumberToObject(info, "freeheap", (double)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(info, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(info, "brand", "Elixcon");
    cJSON_AddStringToObject(info, "product", "Light Hub");
    uint8_t mac[6];
    char    hex[13];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(hex, sizeof(hex), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(info, "mac", hex);
    cJSON_AddStringToObject(info, "ip", wifi.ip);
    return info;
}

cJSON *wled_state_json(void)
{
    const size_t n = pixel_count();
    lighting_stream_status_t st;
    lighting_stream_status(&st);

    cJSON *state = cJSON_CreateObject();
    cJSON_AddBoolToObject(state, "on", room_lit());
    cJSON_AddNumberToObject(state, "bri", s_master);
    cJSON_AddNumberToObject(state, "transition", 7);
    cJSON_AddNumberToObject(state, "ps", -1);
    cJSON_AddNumberToObject(state, "pl", -1);
    cJSON_AddNumberToObject(state, "lor", 0);
    cJSON_AddBoolToObject(state, "live", st.active);
    cJSON_AddNumberToObject(state, "mainseg", 0);
    cJSON *segs = cJSON_AddArrayToObject(state, "seg");
    cJSON *seg  = cJSON_CreateObject();
    cJSON_AddNumberToObject(seg, "id", 0);
    cJSON_AddNumberToObject(seg, "start", 0);
    cJSON_AddNumberToObject(seg, "stop", (double)n);
    cJSON_AddNumberToObject(seg, "len", (double)n);
    cJSON_AddNumberToObject(seg, "grp", 1);
    cJSON_AddNumberToObject(seg, "spc", 0);
    cJSON_AddNumberToObject(seg, "of", 0);
    cJSON_AddBoolToObject(seg, "on", true);
    cJSON_AddBoolToObject(seg, "frz", false);
    cJSON_AddNumberToObject(seg, "bri", 255);
    cJSON_AddNumberToObject(seg, "cct", 127);
    cJSON_AddNumberToObject(seg, "fx", 0);
    cJSON_AddNumberToObject(seg, "sx", 128);
    cJSON_AddNumberToObject(seg, "ix", 128);
    cJSON_AddNumberToObject(seg, "pal", 0);
    cJSON_AddBoolToObject(seg, "sel", true);
    cJSON_AddBoolToObject(seg, "rev", false);
    cJSON_AddBoolToObject(seg, "mi", false);
    cJSON_AddItemToArray(segs, seg);
    cJSON *udpn = cJSON_AddObjectToObject(state, "udpn");
    cJSON_AddBoolToObject(udpn, "send", false);
    cJSON_AddBoolToObject(udpn, "recv", false);
    return state;
}

/* The settings LedFx reads before it streams: only the realtime part means
 * anything here, and it describes what this interface does. */
cJSON *wled_cfg_json(void)
{
    cJSON *cfg = cJSON_CreateObject();
    cJSON *id  = cJSON_AddObjectToObject(cfg, "id");
    cJSON_AddStringToObject(id, "name", settings_name());
    cJSON *hw  = cJSON_AddObjectToObject(cfg, "hw");
    cJSON *led = cJSON_AddObjectToObject(hw, "led");
    cJSON_AddNumberToObject(led, "total", (double)pixel_count());
    cJSON_AddNumberToObject(led, "maxpwr", 0);
    cJSON *iface = cJSON_AddObjectToObject(cfg, "if");
    cJSON *live  = cJSON_AddObjectToObject(iface, "live");
    cJSON_AddBoolToObject(live, "en", true);
    cJSON_AddBoolToObject(live, "mso", false);
    cJSON_AddNumberToObject(live, "port", 5568);
    cJSON_AddBoolToObject(live, "mc", false);
    cJSON_AddNumberToObject(live, "timeout", 25);
    cJSON_AddBoolToObject(live, "maxbri", false);
    cJSON_AddBoolToObject(live, "no-gc", true);
    cJSON_AddNumberToObject(live, "offset", 0);
    return cfg;
}

bool wled_apply_state(const cJSON *body)
{
    const uint16_t room = settings_wled_room();
    const cJSON   *bri  = cJSON_GetObjectItem(body, "bri");
    if (cJSON_IsNumber(bri)) {
        s_master = (uint8_t)(bri->valueint < 1 ? 1 : bri->valueint > 255 ? 255 : bri->valueint);
    }
    if (room == 0) {
        return cJSON_IsTrue(cJSON_GetObjectItem(body, "v"));
    }

    const cJSON *live = cJSON_GetObjectItem(body, "live");
    const cJSON *on   = cJSON_GetObjectItem(body, "on");
    const bool   off  = cJSON_IsFalse(on);
    bool         done = false;
    if (cJSON_IsFalse(live)) {
        /* Over: the room goes back to how it was, or off when the client says
         * so - Hyperion's default, unless it was told to restore the state. */
        done = lighting_stream_stop(room, off);
    }
    /* With "live": true the stream lights its own lamps; "on" then is the
     * client switching its strip on, not the room. */
    if (!done && !cJSON_IsTrue(live) && on) {
        const uint16_t ds = settings_transition_ds();
        if (cJSON_IsBool(on)) {
            lighting_group_set_onoff(room, cJSON_IsTrue(on) ? ZB_ONOFF_ON : ZB_ONOFF_OFF, ds);
        } else if (cJSON_IsString(on) && strcmp(on->valuestring, "t") == 0) {
            lighting_group_set_onoff(room, ZB_ONOFF_TOGGLE, ds);
        }
    }
    return cJSON_IsTrue(cJSON_GetObjectItem(body, "v"));
}

cJSON *wled_status_json(void)
{
    lighting_stream_status_t st;
    lighting_stream_status(&st);
    const int64_t now = esp_timer_get_time();

    cJSON *w = cJSON_CreateObject();
    cJSON_AddBoolToObject(w, "enabled", settings_wled_on());
    const uint16_t room = settings_wled_room();
    if (room) {
        cJSON_AddNumberToObject(w, "room", room);
    } else {
        cJSON_AddNullToObject(w, "room");
    }
    cJSON *pixels = cJSON_AddArrayToObject(w, "pixels");
    zb_ent_lamp_t lamps[ZB_ENT_MAX_LAMPS];
    const size_t  n = room ? lighting_stream_lamps(room, lamps, ZB_ENT_MAX_LAMPS) : 0;
    for (size_t i = 0; i < n; i++) {
        char     ieee[19];
        cJSON   *px = cJSON_CreateObject();
        zbc_ieee_to_str(lamps[i].ieee, ieee);
        cJSON_AddStringToObject(px, "ieee", ieee);
        cJSON_AddNumberToObject(px, "ep", lamps[i].ep);
        cJSON_AddItemToArray(pixels, px);
    }
    cJSON_AddBoolToObject(w, "listening", s_rx.listening);
    cJSON_AddBoolToObject(w, "live", st.active);
    cJSON_AddBoolToObject(w, "override", st.override);
    cJSON_AddBoolToObject(w, "others_off", settings_wled_others_off());
    cJSON_AddNumberToObject(w, "lights_off", st.lights_off);
    cJSON_AddNumberToObject(w, "fps_in", s_rx.fps_in);
    if (s_rx.last_us) {
        cJSON_AddStringToObject(w, "source", s_rx.source);
        cJSON_AddStringToObject(w, "protocol", s_rx.proto ? s_rx.proto : "");
        cJSON_AddNumberToObject(w, "last_packet_s_ago", (double)((now - s_rx.last_us) / 1000000));
    } else {
        cJSON_AddNullToObject(w, "source");
    }
    cJSON_AddNumberToObject(w, "packets", s_rx.packets);
    cJSON_AddNumberToObject(w, "ignored", s_rx.ignored);
    cJSON_AddNumberToObject(w, "refused", s_rx.refused);
    cJSON_AddNumberToObject(w, "sessions", st.sessions);
    cJSON_AddNumberToObject(w, "taken", st.packets);
    cJSON_AddNumberToObject(w, "dropped_by_hand", st.dropped);
    cJSON_AddNumberToObject(w, "frames", st.frames);
    cJSON_AddNumberToObject(w, "frames_failed", st.frames_failed);
    cJSON_AddNumberToObject(w, "late", st.late);
    if (st.started_us) {
        cJSON_AddNumberToObject(w, "started_s_ago", (double)((now - st.started_us) / 1000000));
    }
    static const char *const ENDS[] = { NULL, "source", "timeout", "hand", "failed" };
    const char *end = st.last_end <= LIGHTING_STREAM_END_FAILED ? ENDS[st.last_end] : NULL;
    if (end) {
        cJSON_AddStringToObject(w, "last_end", end);
    } else {
        cJSON_AddNullToObject(w, "last_end");
    }
    return w;
}

#endif /* CONFIG_ZBC_WLED */
