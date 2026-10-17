/* Minimal DNS responder: answers every A query with the SoftAP address so that
 * phones open the provisioning page as a captive portal. */
#pragma once

#include "esp_err.h"

esp_err_t captive_dns_start(void);
void      captive_dns_stop(void);
