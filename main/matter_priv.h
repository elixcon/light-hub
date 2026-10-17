/* The three parts of the Matter controller, as they see each other: our fabric
 * (matter_fabric.cpp), pairing and endpoint discovery (matter_commission.cpp),
 * and control and state (matter_ctl.cpp). matter_ctl.h is the C face the rest
 * of the firmware sees.
 *
 * C++, and CHIP task only: everything declared here is called on the CHIP thread,
 * or with the CHIP stack lock held, unless it says otherwise. */
#pragma once

#include <crypto/CHIPCryptoPAL.h>
#include <lib/core/CHIPError.h>
#include <lib/core/DataModelTypes.h>
#include <lib/core/NodeId.h>
#include <lib/support/Span.h>
#include <transport/Session.h>

#include "matter_ctl.h"

namespace mctl {

/* ------------------------------------------------------ matter_fabric.cpp */

/* At start: finds our fabric in the Server's FabricTable by its root public key.
 * Not an error when there is none yet - the first pairing makes it. */
void              fabric_load();
/* Makes our fabric if there is none. CHIP_ERROR_BUSY while the Server's own
 * fail-safe is armed (a controller is adding the bridge right now: the pending
 * fabric state is shared), CHIP_ERROR_NO_MEMORY with no fabric slot left. */
CHIP_ERROR        fabric_ensure();
chip::FabricIndex fabric_index();          /* kUndefinedFabricIndex when none */
/* Why the last attempt to make it failed, "" when it did not. */
const char *      fabric_error();
chip::NodeId      fabric_own_node();       /* our node id on it, 0 when none */
uint16_t          fabric_vendor_id();      /* what we tell a lamp we are */
/* Our root certificate, Matter TLV encoding - what AddTrustedRootCertificate sends. */
CHIP_ERROR        fabric_root_cert(chip::MutableByteSpan & out);
/* A lamp's operational certificate for `pub`, signed by our root, Matter TLV. */
CHIP_ERROR        fabric_issue_noc(const chip::Crypto::P256PublicKey & pub, chip::NodeId node,
                                   chip::MutableByteSpan & out);
/* The identity protection key's epoch key, which AddNOC hands the lamp. */
chip::ByteSpan    fabric_ipk();
/* A fresh random operational node id for a lamp. */
chip::NodeId      fabric_new_node_id();

/* -------------------------------------------------- matter_commission.cpp */

/* The pairing's progress, copied out for mctl_get_status. Any task. */
void commission_status(mctl_status_t * out);
/* Starts a pairing from a parsed code. Any task; the work happens elsewhere. */
esp_err_t commission_start(uint32_t pin, bool short_disc, uint16_t disc);
bool      commission_busy();

/* Reads a device's endpoints over an established session and puts what it finds
 * into mlight_db, keeping the names of rows it already has. `done` gets the number
 * of lamps found, or an error. One at a time: CHIP_ERROR_BUSY otherwise. */
typedef void (*discover_done_fn)(chip::NodeId node, CHIP_ERROR err, int lamps);
CHIP_ERROR discover_start(chip::NodeId node, uint8_t lamp_fabric, const chip::SessionHandle & session,
                          discover_done_fn done);

/* --------------------------------------------------------- matter_ctl.cpp */

/* Re-reads mlight_db: lamps and devices come and go to match. */
void ctl_reload_now();

} // namespace mctl
