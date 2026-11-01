/* An IEEE address as the API writes it, on its own because the node has no device
 * table. Declared in device_db.h. */
#include <stdio.h>
#include <string.h>

#include "device_db.h"

void zbc_ieee_to_str(const uint8_t ieee[ZBC_IEEE_LEN], char out[19])
{
    snprintf(out, 19, "0x%02x%02x%02x%02x%02x%02x%02x%02x",
             ieee[0], ieee[1], ieee[2], ieee[3], ieee[4], ieee[5], ieee[6], ieee[7]);
}

bool zbc_str_to_ieee(const char *str, uint8_t out[ZBC_IEEE_LEN])
{
    if (!str) {
        return false;
    }
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        str += 2;
    }
    if (strlen(str) != ZBC_IEEE_LEN * 2) {
        return false;
    }
    for (int i = 0; i < ZBC_IEEE_LEN; i++) {
        char byte[3] = { str[i * 2], str[i * 2 + 1], 0 };
        char *end    = NULL;
        long v       = strtol(byte, &end, 16);
        if (end != byte + 2) {
            return false;
        }
        out[i] = (uint8_t)v;
    }
    return true;
}
