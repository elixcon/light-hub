/* See zb_ota_file.h. A leaf: no ESP-IDF, compiled whole by test/test_zbota.c. */
#include "zb_ota_file.h"

#include <string.h>

/* Field control bits of the OTA header. */
#define FC_SECURITY_VERSION (1u << 0)
#define FC_DESTINATION      (1u << 1)
#define FC_HW_VERSIONS      (1u << 2)

/* Image types from 0xFFC0 are security credentials, configuration and logs,
 * and 0xFFFF is the wildcard; none of them is firmware. */
#define IMAGE_TYPE_FIRST_SPECIAL 0xFFC0u

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

const char *zbc_ota_header_check(const uint8_t *hdr, size_t have, uint32_t file_len, zbc_ota_hdr_t *out)
{
    if (!hdr || !out || have < ZBC_OTA_HDR_MIN) {
        return "too short to be a Zigbee OTA file";
    }
    if (rd32(hdr) != ZBC_OTA_FILE_ID) {
        return "not a Zigbee OTA file (no 0x0BEEF11E header)";
    }
    memset(out, 0, sizeof(*out));
    out->hdr_version   = rd16(hdr + 4);
    out->hdr_length    = rd16(hdr + 6);
    out->hdr_fc        = rd16(hdr + 8);
    out->manuf_code    = rd16(hdr + 10);
    out->image_type    = rd16(hdr + 12);
    out->file_version  = rd32(hdr + 14);
    out->stack_version = rd16(hdr + 18);
    out->total         = rd32(hdr + 52);

    if (out->hdr_version != 0x0100) {
        return "unsupported OTA header version";
    }
    size_t need = ZBC_OTA_HDR_MIN;
    need += (out->hdr_fc & FC_SECURITY_VERSION) ? 1 : 0;
    need += (out->hdr_fc & FC_DESTINATION) ? 8 : 0;
    const size_t hw_at = need;
    need += (out->hdr_fc & FC_HW_VERSIONS) ? 4 : 0;

    if (out->hdr_length < need) {
        return "the OTA header is shorter than its own fields";
    }
    if (out->total < out->hdr_length) {
        return "the OTA header says the file is shorter than the header";
    }
    if (file_len != 0 && out->total != file_len) {
        return "the file is not the length its OTA header says";
    }
    if (out->manuf_code == 0xFFFF) {
        return "the OTA header has no manufacturer code";
    }
    if (out->image_type >= IMAGE_TYPE_FIRST_SPECIAL) {
        return "not a firmware image (special OTA image type)";
    }
    if (have < need) {
        return "too short to be a Zigbee OTA file";
    }
    if (out->hdr_fc & FC_HW_VERSIONS) {
        out->hw_range = true;
        out->hw_min   = rd16(hdr + hw_at);
        out->hw_max   = rd16(hdr + hw_at + 2);
    }

    /* 32 bytes, zero-padded or not terminated at all. Only printable ASCII is
     * kept, so the string can go into JSON and a log line as it is. */
    size_t n = 0;
    for (; n < 32 && hdr[20 + n] != '\0'; n++) {
        const uint8_t c   = hdr[20 + n];
        out->hdr_string[n] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    out->hdr_string[n] = '\0';
    return NULL;
}

bool zbc_ota_qni_parse(const uint8_t *p, size_t n, zbc_ota_qni_t *out)
{
    if (!p || !out || n < 9) {
        return false;
    }
    const bool hw = (p[0] & 0x01) != 0;
    if (p[0] & ~0x01u) {
        return false;   /* no other field control bit is defined */
    }
    if (n < (hw ? 11u : 9u)) {
        return false;
    }
    out->manuf_code   = rd16(p + 1);
    out->image_type   = rd16(p + 3);
    out->file_version = rd32(p + 5);
    out->hw_present   = hw;
    out->hw_version   = hw ? rd16(p + 9) : 0;
    return true;
}
