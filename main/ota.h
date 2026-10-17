/* Over-the-air firmware update, shaped like ESPHome's web_server OTA platform.
 *
 * The wire format is deliberately not ours: `POST /update` with a
 * multipart/form-data body whose first file part is a plain ESP-IDF application
 * image, answered with 200 text/plain "Update Successful!" or "Update Failed!".
 * That is byte for byte what an ESPHome device accepts, so the same browser
 * upload, the same `curl -F`, and the same scripts work against this box.
 *
 * The file ESPHome calls firmware.ota.bin is exactly what `idf.py build` leaves
 * in build/elixcon_light_hub.bin - an app image, 0xE9 then an esp_app_desc_t.
 * firmware.factory.bin is a merged flash image (bootloader first) and is
 * rejected; only the app-descriptor check can tell the two apart.
 *
 * There is one app partition, and a running app cannot overwrite itself. So the
 * coordinator hands an update over: it restarts into the updater (updater/, the
 * factory app), which is where the upload is actually written. This file is
 * compiled into both.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The one-shot flag the coordinator leaves for the updater: an update was asked
 * for, so wait for it rather than starting the coordinator straight back up.
 * Read and erased by updater/main/updater.c. */
#define OTA_NVS_NAMESPACE    "zbc"
#define OTA_NVS_UPDATER_FLAG "upd_wait"

/* Arms the confirmation of a freshly written image. Call once at startup, after
 * the HTTP server is up and before app_main can return. */
void ota_init(void);

/* POST /update in the updater: parses the upload and writes the app partition. */
esp_err_t ota_handle_update(httpd_req_t *req);

/* The coordinator's two routes, both of which restart into the updater.
 * POST /api/update/prepare answers 202 for the web UI, which then waits for the
 * updater to answer. POST /update answers 503 with Retry-After, keeping
 * ESPHome's body, so a client that retries - `curl --retry` - sends the same
 * file again and it reaches the updater. */
esp_err_t ota_handle_prepare(httpd_req_t *req);
esp_err_t ota_handle_handoff(httpd_req_t *req);

/* The partition label the running image came from: "app" or "updater". */
const char *ota_running_label(void);

/* The updater's version, or NULL when the flash has no updater. */
const char *ota_updater_version(void);

/* True while the running image is on trial and has not been confirmed yet. */
bool ota_pending_verify(void);

/* Whether the updater partition holds an image that verifies - checksum and
 * hash, not just a descriptor. Verified once and kept; the coordinator refuses
 * to hand an update over while this is false. */
bool ota_updater_valid(void);

/* Why the updater cannot be trusted, or NULL: its image does not verify, or it
 * was built for a partition table that is neither the one on flash nor the one
 * set with ota_set_own_table - an updater that would take this flash for a
 * foreign firmware's and start nothing. */
const char *ota_updater_problem(void);

/* The coordinator build's own full partition table, embedded at build time, so
 * an updater carrying it counts as knowing this flash. */
void ota_set_own_table(const uint8_t *table, size_t len);

/* The coordinator's routes for replacing the updater, which is how a box gets a
 * new partition table without a cable (see partitions.csv). Both answer JSON.
 * POST /api/updater/image writes an uploaded elixcon_light_hub_updater.bin into the factory
 * partition and verifies it; POST /api/updater/run restarts through the updater
 * without asking it to wait, so it comes straight back. */
esp_err_t ota_handle_updater_image(httpd_req_t *req);
esp_err_t ota_handle_updater_run(httpd_req_t *req);

/* ---------------------------------------------------------------- uploads */

/* Takes the bytes of the file part, in order, in pieces of any size. Returns
 * NULL to go on, or why the upload is refused - which ends it. */
typedef const char *(*ota_sink_fn)(void *ctx, const uint8_t *data, size_t len);

/* Whether the request is multipart/form-data with a usable boundary. */
bool ota_is_multipart(httpd_req_t *req);

/* Reads a multipart/form-data body to the end of its first file part and hands
 * that part's bytes to `sink`. Ordinary form fields before it are skipped. NULL
 * when the whole part arrived; otherwise why not, the sink's own reasons
 * included. Nothing is answered: the caller owns the response. */
const char *ota_multipart_receive(httpd_req_t *req, ota_sink_fn sink, void *ctx);

/* An ota_sink_fn writing into a partition from offset 0, erasing ahead of
 * itself a block at a time. The first `head_want` bytes are held in `head` until
 * `check` passes, so a wrong file never reaches flash. Zero what is not set. */
typedef struct {
    const esp_partition_t *part;
    size_t                 limit;      /* most bytes the file may have; 0 = the partition */
    const char            *too_big;    /* the reason when it has more; NULL for a plain one */
    uint8_t               *head;       /* head_want bytes of the caller's */
    size_t                 head_want;
    size_t                 head_len;
    const char *(*check)(void *check_ctx, const uint8_t *head, size_t len);
    void                  *check_ctx;
    size_t                 written;    /* bytes in flash so far */
    size_t                 erased;     /* bytes from the start erased so far */
} ota_part_sink_t;

const char *ota_part_sink_write(void *ctx, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
