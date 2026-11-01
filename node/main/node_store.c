#include "node_store.h"

#include "nvs.h"

#define NS "node"

static bool get_u8(const char *key, uint8_t *v)
{
    nvs_handle_t nvs;
    if (nvs_open(NS, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    const bool ok = nvs_get_u8(nvs, key, v) == ESP_OK;
    nvs_close(nvs);
    return ok;
}

static void set_u8(const char *key, uint8_t v)
{
    nvs_handle_t nvs;
    if (nvs_open(NS, NVS_READWRITE, &nvs) == ESP_OK) {
        if (nvs_set_u8(nvs, key, v) == ESP_OK) {
            nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
}

static bool take_u8(const char *key, uint8_t *v)
{
    nvs_handle_t nvs;
    if (nvs_open(NS, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    const bool ok = nvs_get_u8(nvs, key, v) == ESP_OK;
    if (ok && nvs_erase_key(nvs, key) == ESP_OK) {
        nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ok;
}

bool node_store_paired(void)
{
    uint8_t v = 0;
    return get_u8("paired", &v) && v;
}

void node_store_set_paired(bool paired)
{
    set_u8("paired", paired ? 1 : 0);
}

bool node_store_foreign(void)
{
    uint8_t v = 0;
    return !(get_u8("role", &v) && v == 1);
}

void node_store_claim(void)
{
    set_u8("role", 1);
}

void node_store_forget_claim(void)
{
    set_u8("role", 0);
}

void node_store_set_ask(node_ask_t ask, uint8_t minutes)
{
    set_u8("ask_min", minutes);
    set_u8("ask", (uint8_t)ask);
}

node_ask_t node_store_take_ask(uint8_t *minutes)
{
    uint8_t ask = 0, m = 0;
    take_u8("ask_min", &m);
    if (minutes) {
        *minutes = m;
    }
    return take_u8("ask", &ask) ? (node_ask_t)ask : NODE_ASK_NONE;
}

void node_store_set_resume(void)
{
    set_u8("resume", 1);
}

bool node_store_take_resume(void)
{
    uint8_t v = 0;
    return take_u8("resume", &v) && v;
}

uint32_t node_store_endpoints(void)
{
    nvs_handle_t nvs;
    uint32_t     v = 0;
    if (nvs_open(NS, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u32(nvs, "eps", &v);
        nvs_close(nvs);
    }
    return v;
}

void node_store_set_endpoints(uint32_t digest)
{
    nvs_handle_t nvs;
    if (nvs_open(NS, NVS_READWRITE, &nvs) == ESP_OK) {
        if (nvs_set_u32(nvs, "eps", digest) == ESP_OK) {
            nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
}
