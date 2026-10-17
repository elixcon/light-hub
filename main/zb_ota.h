/* Firmware for Zigbee devices: the one image the coordinator holds, which
 * device may have it, how the transfer is going, and what each device said it
 * runs.
 *
 * The image lives in the `zb_ota` partition and is handed to the Zigbee stack
 * as a flash mapping - esp-zigbee-lib's OTA Upgrade server takes a pointer to
 * the whole file and answers the block requests itself. zb_coordinator.c does
 * that part and calls in here from its callbacks; this module never takes the
 * stack lock and never includes zb_coordinator.h.
 *
 * Nothing is served without a target. Uploading an image offers it to nobody;
 * the web UI names the one device it is for, and a device that is not that one
 * is told there is no image, whatever it asks for. A stored image is never
 * handed to every lamp of a model at once by accident.
 *
 * Which task calls what:
 *   HTTP task:   zb_ota_begin_write .. zb_ota_end_write around an upload or a
 *                removal, zb_ota_set_target, zb_ota_status, the JSON builders.
 *   Zigbee task: zb_ota_image (to attach), zb_ota_set_offered, and the
 *                zb_ota_note_* / zb_ota_admit calls from the stack's callbacks.
 * Everything shared is under one spinlock, which nothing holds across a call
 * out. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

#include "device_db.h"
#include "zb_ota_file.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opens the partition and brings back a stored image, if its length and CRC
 * in NVS still match what is in flash. After device_db_init. */
esp_err_t zb_ota_init(void);

/* Whether this flash has the partition at all. A box still on the table from
 * before it was added (see partitions.csv) has none, and takes no image. */
bool zb_ota_has_partition(void);

/* The stored image as the stack is to see it: a pointer into the flash mapping
 * and its length. False when there is none, or while one is being replaced. */
bool zb_ota_image(const void **ptr, uint32_t *len);

/* Whether the image is attached to the stack. zb_coordinator.c's to say. */
void zb_ota_set_offered(bool offered);

/* An upload or removal is starting. False, with a reason, when a transfer is
 * under way - it would be reading the flash being erased. From here to
 * zb_ota_end_write zb_ota_image answers false, so a network coming up in
 * between cannot attach the old mapping. The caller detaches from the stack
 * after this and before writing. */
bool zb_ota_begin_write(const char **why);
void zb_ota_end_write(void);

/* POST body of an upload: writes, checks and keeps it. Answers the request.
 * Between zb_ota_begin_write and zb_ota_end_write. */
esp_err_t zb_ota_handle_upload(httpd_req_t *req);

/* Forgets the stored image. Between zb_ota_begin_write and zb_ota_end_write. */
void zb_ota_forget(void);

/* The one device the image is for, or NULL for none. Naming a device starts a
 * fresh transfer record. */
void zb_ota_set_target(const uint8_t *ieee);
bool zb_ota_target(uint8_t ieee_out[ZBC_IEEE_LEN]);

/* What a device said about its firmware: a Query Next Image Request, or the
 * OTA client attributes read at interrogation (0x0002 CurrentFileVersion, 0x0007
 * ManufacturerID, 0x0008 ImageTypeID). Zigbee task. */
void zb_ota_note_client(uint16_t short_addr, uint8_t ep);   /* an OTA client endpoint in its simple descriptor */
void zb_ota_note_query(uint16_t short_addr, uint8_t ep, const zbc_ota_qni_t *q);
void zb_ota_note_attr(uint16_t short_addr, uint8_t ep, uint16_t attr_id, uint32_t value);

/* The stack's OTA server callbacks, in order. zb_ota_admit decides whether a
 * query is answered with the image; the others follow the transfer it admitted.
 * Zigbee task. */
bool zb_ota_admit(uint16_t short_addr, uint8_t ep, const zbc_ota_qni_t *q);
bool zb_ota_note_block(uint16_t short_addr, uint32_t offset);   /* false: not the admitted device */
/* True when it ended the admitted transfer; `from_version` gets the version the
 * device ran before it. */
bool zb_ota_note_end(uint16_t short_addr, uint8_t status, uint32_t *from_version);

/* What a device has told us, for the endpoint to notify and for the UI. */
typedef struct {
    uint8_t  ep;              /* 0 when unknown */
    bool     manuf_known;
    uint16_t manuf_code;
    bool     type_known;
    uint16_t image_type;
    bool     version_known;
    uint32_t file_version;
    bool     hw_known;
    uint16_t hw_version;
    int64_t  queried_us;      /* last Query Next Image, 0 = never */
} zb_ota_device_t;

bool zb_ota_device_info(const uint8_t ieee[ZBC_IEEE_LEN], zb_ota_device_t *out);

/* A notify for a sleepy device, sent again when it is next heard from - see
 * zota_wake_* in zb_ota.c. zb_ota_wake_arm: a notify was just sent to it (any
 * task). zb_ota_wake_heard: a frame from it arrived, true when a notify is due
 * (Zigbee task; it must not send from there). zb_ota_wake_take: the next device
 * one is due for (work task, which sends it). A Query Next Image from the device
 * disarms it. */
void zb_ota_wake_arm(const uint8_t ieee[ZBC_IEEE_LEN]);
bool zb_ota_wake_heard(uint16_t short_addr);
bool zb_ota_wake_take(uint8_t ieee_out[ZBC_IEEE_LEN]);
void zb_ota_wake_sent(bool ok);   /* what the stack said to the notify ota_wake_work sent */
/* A unicast notify's delivery confirmation (status 0: delivered). Zigbee task. */
void zb_ota_notify_confirmed(uint16_t short_addr, uint8_t status);

/* `zigbee.ota` in /api/status, and a device's `ota` in /api/devices. */
cJSON *zb_ota_status_json(void);
void   zb_ota_add_device_json(cJSON *device, const uint8_t ieee[ZBC_IEEE_LEN]);

#ifdef __cplusplus
}
#endif
