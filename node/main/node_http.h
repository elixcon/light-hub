/* The node's web server, only in a mode with Wi-Fi: the same JSON the hub's page reads
 * for what is wired to a board, and little else. */
#pragma once

#include "esp_err.h"

esp_err_t node_http_start(void);
