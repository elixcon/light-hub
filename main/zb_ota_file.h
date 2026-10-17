/* The Zigbee OTA Upgrade file header, and the one OTA command a device sends
 * that says what firmware it runs.
 *
 * A leaf: nothing but the C library, so test/test_zbota.c compiles it whole.
 * Everything is parsed byte by byte, little-endian, with no packed structs -
 * the same code runs on the C6 and on the host.
 *
 * ZCL 7 section 11.4: a file is a header (magic 0x0BEEF11E, at least 56 bytes,
 * longer by whichever optional fields its field control announces) followed by
 * sub-elements, and `total_image_size` in the header is the length of the whole
 * file. The coordinator serves the file byte for byte; what is inside the
 * sub-elements is the device's business. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZBC_OTA_FILE_ID  0x0BEEF11Eu
#define ZBC_OTA_HDR_MIN  56u
/* With every optional field: security credential version (1), upgrade file
 * destination (8), minimum and maximum hardware version (2 + 2). */
#define ZBC_OTA_HDR_MAX  (ZBC_OTA_HDR_MIN + 1u + 8u + 4u)

typedef struct {
    uint16_t hdr_version;
    uint16_t hdr_length;
    uint16_t hdr_fc;
    uint16_t manuf_code;
    uint16_t image_type;
    uint32_t file_version;
    uint16_t stack_version;
    char     hdr_string[33];   /* printable ASCII only, NUL-terminated */
    uint32_t total;            /* the whole file, header included */
    bool     hw_range;
    uint16_t hw_min;
    uint16_t hw_max;
} zbc_ota_hdr_t;

/* Query Next Image Request (client to server, command 0x01). */
typedef struct {
    uint16_t manuf_code;
    uint16_t image_type;
    uint32_t file_version;
    bool     hw_present;
    uint16_t hw_version;
} zbc_ota_qni_t;

/* Checks the first `have` bytes of a file. NULL when they are a usable header,
 * with `out` filled; otherwise why not. `have` must cover ZBC_OTA_HDR_MIN, and
 * the optional fields the field control announces when there are any.
 * `file_len` is the file's length when known, 0 when not yet. */
const char *zbc_ota_header_check(const uint8_t *hdr, size_t have, uint32_t file_len, zbc_ota_hdr_t *out);

/* A Query Next Image Request payload, or false when it is not one. */
bool zbc_ota_qni_parse(const uint8_t *p, size_t n, zbc_ota_qni_t *out);

#ifdef __cplusplus
}
#endif
