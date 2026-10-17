/* What the updater's web server needs to know about why it is running. */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* A short code ("requested", "failed", "empty") and the sentence the page
 * shows for it. */
const char *updater_reason(void);
const char *updater_message(void);

/* The coordinator image's version, or NULL when there is no bootable one. */
const char *updater_app_version(void);

/* Somebody is using the page: a handover that has waited long enough goes back
 * to the coordinator, but not while a person is choosing a file. */
void updater_touch(void);

/* Bracket an upload: nothing restarts during one, and one that failed may have
 * left the app partition without a bootable image. */
void updater_upload_started(void);
void updater_upload_finished(void);

esp_err_t updater_http_start(void);
