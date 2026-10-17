#pragma once

/* CHIP's project configuration, named by CONFIG_CHIP_PROJECT_CONFIG in
 * sdkconfig.defaults and read by the whole Matter build before CHIP's own
 * defaults. Only plain defines: CHIPConfig.h and SystemConfig.h both include it.
 *
 * The example device instance info provider this build uses answers Basic
 * Information's VendorName and ProductName from these two; CHIP's defaults are
 * "TEST_VENDOR" and "TEST_PRODUCT". The vendor and product ids stay the test
 * ones, to match the test attestation certificates; a name is only a name. */
#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME  "Elixcon"
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME "Light Hub"

/* Sessions this box sets up itself (main/matter_ctl.cpp): one per paired Matter
 * lamp or hub, all asked for at boot, besides the bridge's own subscription
 * resumption to its controllers. A pending setup is cheap; the CASE clients that
 * do the work are not, and stay at CHIP's two - a setup that finds none free is
 * failed and tried again a few seconds later. */
#define CHIP_CONFIG_DEVICE_MAX_ACTIVE_DEVICES 8

/* After a restart the bridge resumes its controllers' subscriptions itself: CHIP
 * keeps each one in nvs_matter, sets up CASE to the subscriber and reports on it
 * again, and that new session is what the controller's commands then use. Without
 * it a controller sees nothing until its own max interval runs out, and its first
 * commands go to a session the bridge no longer has.
 *
 * CHIP makes the first attempt as soon as esp_matter::start() returns, which here
 * is before the station has associated, so the lookup of the controller goes
 * nowhere and fails (after 45 s, see below). The next attempt is 300 s later by
 * default (then 10, 10, 15 minutes...): a controller was out of touch for about
 * six minutes after every restart, unless it gave up on its subscription sooner by
 * itself. 10 s steps instead - 10, 20, 20, 30, 40, 60 s... -
 * and a subscriber that is really gone is dropped after eleven failures (~26
 * minutes; it subscribes again by itself when it comes back). Also used when a
 * controller stops acknowledging reports. CHIPConfig.h's two conditions hold:
 * 10 + fib(10) * 10 = 560 < 600 < 10 + fib(11) * 10 = 900. */
#define CHIP_CONFIG_SUBSCRIPTION_TIMEOUT_RESUMPTION_MIN_RETRY_INTERVAL_SECS   10
#define CHIP_CONFIG_SUBSCRIPTION_TIMEOUT_RESUMPTION_WAIT_TIME_MULTIPLIER_SECS 10
#define CHIP_CONFIG_SUBSCRIPTION_TIMEOUT_RESUMPTION_MAX_RETRY_INTERVAL_SECS   600

/* How long a node's address may take. The ESP32 DNS-SD backend asks once - SRV and
 * TXT for 3 s, then AAAA for 3 s if the answer carried no address - so a lookup
 * that succeeds is done in about 6 s. One that finds nothing is reported without
 * the node it was for (Discovery_ImplPlatform passes PeerId()), CHIP cannot match
 * it to the lookup, and the lookup runs to this limit: 45 s by default, for an
 * answer that is never coming. That is what an attempt made before Wi-Fi is up
 * costs, for the resumption above and for matter_ctl's sessions to its lamps. */
#define CHIP_CONFIG_ADDRESS_RESOLVE_MAX_LOOKUP_TIME_MS 10000
