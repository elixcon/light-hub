/* wifi_manager.c stops Zigbee before it opens the setup hotspot, because the
 * 802.15.4 receiver would otherwise hold the antenna. The updater never starts
 * Zigbee, so there is nothing to stop. */
#include "zb_coordinator.h"

esp_err_t zb_coordinator_stop(void)
{
    return ESP_OK;
}
