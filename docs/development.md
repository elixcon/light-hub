# Development

## Building

The toolchain is ESP-IDF v6.0.3:

```sh
# once
git clone -b v6.0.3 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v6
~/esp/esp-idf-v6/install.sh esp32c6
python3 ~/esp/esp-idf-v6/tools/idf_tools.py install cmake ninja   # macOS: IDF 6 leaves them to you

# in every new shell
. ~/esp/esp-idf-v6/export.sh

idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

The build also needs `zopfli` (`brew install zopfli`, or `apt install zopfli`), which compresses the
embedded web page.

One build produces both images, the coordinator and the updater, and `idf.py flash` writes the
bootloader, the partition table and both of them. `idf.py app-flash` writes only the coordinator and
`idf.py updater-flash` only the updater. Never use `idf.py erase-flash` unless you mean to lose the
Zigbee network, the paired devices, the keys and the Wi-Fi credentials.

Options worth checking with `idf.py menuconfig`, under *Elixcon Light Hub*: the Zigbee channel for a
new network (default 20), the maximum number of devices, and an optional password for the setup
hotspot.

The built web page, `main/www/index.html`, is committed, so `idf.py build` never needs Node. Its
source is the Svelte 5 project in `web/`; see [web/README.md](../web/README.md) for building it.

## Tests

The logic that is hard to exercise on hardware has host tests: the per-endpoint command gate, the
settle scheduler, device reachability, the button and remote gesture machines, the dim sweep, the
fade-in and warm-off decisions, the Matter Move model, the remote decoders, the updater's boot
decision, the partition table conversion plans and the stack scan of a crash record.

```sh
make -C test        # no ESP-IDF, no hardware, a couple of seconds
```

The code under test is extracted from the source files at build time, never copied, so the tests
cannot drift out of step with what ships. `test/Makefile` lists the regions. The web page has its
own tests (`npm test` in `web/`).

## After a crash

There is no core dump partition, and the panic handler prints only to the serial port. So the
coordinator keeps its own record: `crash_log.c` copies the registers, the task, the abort message, the
heap and the code addresses found on the stack into RTC memory before the restart, and the next boot
stores it in NVS. `GET /api/status` shows it as `last_crash` until another crash replaces it:

```json
"last_crash": {"count": 2, "this_boot": true, "reset_reason": "panic", "at": 1791463278,
  "captured": true, "uptime_s": 55, "version": "fc8f985-dirty", "build": "3035886d9",
  "reason": "abort", "message": "abort() was called at PC 0x42023315 on core 0",
  "task": "crash", "pc": "0x40806ac2", "ra": "0x40806a86", "sp": "0x40870d70",
  "mcause": 2, "mtval": "0x00000000", "free": 65600, "min_free": 46832,
  "stack": ["0x4080ea58", "0x42023318"]}
```

`at` is Unix time (null if the box had no clock). `captured` is false when the reset reason says
crash but the panic handler left nothing, such as a watchdog that reset the chip directly. `free` and
`min_free` are the internal heap at the crash and its low point. During a request, while
`CONFIG_ZBC_HEAP_WATCH` measures it, `min_free` is that request's low instead. The addresses are
`pc`, `ra`, the abort message's PC and `stack`. `stack` holds return addresses nearest the fault
first, with some noise. They mean something only against the ELF of the build that crashed. Give
addr2line `pc` and the abort message's PC as they are, and `ra` and each `stack` entry **minus one**.
Those are return addresses, and a call to a function that never returns is the last instruction of
its caller, so the address after it belongs to the next function. The record above, a test crash,
decodes as `panic_abort` <- `esp_system_abort` <- `abort` <- `crash_task`. Undecremented, `ra` and
the first stack entry name `esp_vApplicationTickHook` and `__assert_func`, which never ran:

```sh
riscv32-esp-elf-addr2line -pfiaC -e elixcon_light_hub.elf 0x40806ac2 0x42023315 0x40806a85 0x4080ea57 0x42023317
```

`stack_bounds` is the crashed task's stack, lowest and highest address, null outside a task.

A `Stack protection fault` comes from the hardware stack guard, which holds sp to the running
stack's bounds. Its interrupt is taken an instruction or more after sp left them, so `pc` and `sp`
are where the code had got to by then. Such a record has `guard`, null in any other: `pc`, the
instruction after the one that moved sp (give it to addr2line as it is), the `bounds` it was held to,
and `sp_in_bounds`. A real overflow leaves sp below them, as the test crash does:

```json
"task": "crash", "pc": "0x4202a6d0", "sp": "0x40873620", "stack_bounds": ["0x40873a84", "0x40874680"],
"guard": {"pc": "0x4202a6ce", "bounds": ["0x40873a84", "0x40874680"], "sp_in_bounds": false}
```

`guard.bounds` other than `stack_bounds` are the interrupt stack's (`CONFIG_FREERTOS_ISR_STACKSIZE`).
`sp_in_bounds: true` with `guard.pc` in task code is the guard going off with sp in range, right
after an instruction that loads sp from memory (`lw sp,…`), as a function with a variable-length
array does on its way out. That is [esp-idf#14456](https://github.com/espressif/esp-idf/issues/14456),
open and without a known cause. In October 2026 it restarted two boxes from the Zigbee library's
link status (`nwk_link_mngr_send_link_status_cmd`, every 15 s), with `zigbee_main` using some 500
bytes of its 6 KB stack. Such a record does not mean a stack is too small. Records stored before
`stack_bounds` and `guard` existed show both as null.

So keep `build/elixcon_light_hub.elf` of every image that goes onto a box; a release has it as
`elixcon_light_hub-<version>.elf`, the `version` in the record. `build` matches the
"ELF file SHA256" `idf.py build` prints. To test the record, build with `CONFIG_ZBC_CRASH_TEST`. Then
`POST /api/restart` takes `{"crash": "abort"}`, `{"crash": "fault"}` or `{"crash": "stack"}` (an
overflow the stack guard catches) and crashes half a second later.

## Continuous integration

`.github/workflows/firmware.yml` runs the host tests and the web page's tests, checks that the
committed `main/www/index.html` is what `web/` builds, and alongside them builds both images in the
`espressif/idf:v6.0.3` container, from `sdkconfig.defaults`. Every push uploads the images as a
workflow artifact, and a `v*` tag also publishes them as a GitHub release. Options set only in a
local `sdkconfig`, such as `CONFIG_ZBC_HEAP_WATCH`, are off in those builds. `sdkconfig` itself is
not committed.

`tools/make-release.sh [build-dir] [out-dir]` makes the release files from a build: the factory
image, the USB zip, the coordinator and updater images, the ELF and `SHA256SUMS`. CI runs it, and
it works the same locally after `idf.py build`. The version in the file names is the one in the
image, which comes from `git describe`, so tag before building a release.

## The shared radio

Wi-Fi and Zigbee share one antenna, and these settings decide whether that works at all. They live
in `sdkconfig.defaults`, `zb_coordinator.c` and `wifi_manager.c`, and each one prevents a hard
failure.

- The application must call `esp_coex_wifi_i154_enable()`. Nothing in ESP-IDF, OpenThread or the
  Zigbee SDK calls it. Without it, whichever stack starts first keeps the antenna: start Zigbee at
  boot and the Zigbee network forms while Wi-Fi never finds an access point; start it later and
  Wi-Fi connects while the Zigbee network never forms. The updater calls it too, although it has no
  Zigbee, because without it the updater's Wi-Fi hears nothing after a hardware reset.
- Leave the 802.15.4 coexistence priorities alone. Raising `txrx` above the IDF default starves
  Wi-Fi so badly that a SoftAP stops beaconing.
- `CONFIG_ESP_PHY_RF_CAL_FULL`: stored partial calibration data can go stale and leave the receiver
  deaf while the transmitter still works. Scans return nothing, though a phone a metre away can
  still talk to the SoftAP.
- Scan channels 1-13 (`esp_wifi_set_country`), not the 1-11 of world safe mode.
- `CONFIG_LWIP_DHCP_DOES_NOT_CHECK_OFFERED_IP`: lwIP ARP-probes the address it has just been given,
  and access points that proxy ARP for their clients answer that probe, so the client declines a
  good lease. The symptom is a router that shows a lease for a device that never gets an address.

[CLAUDE.md](../CLAUDE.md) has the design notes behind these and the rest of the firmware.

## Zigbee SDK version

The firmware is built against esp-zigbee-lib 2.0.5 (ESP-IDF 6.0.x), whose API is the `ezb_*` family.
Most Zigbee material online still shows the 1.x `esp_zb_*` API, which does not apply here. The
reference is the headers in `managed_components/espressif__esp-zigbee-lib/include/ezbee/`.

## Source layout

```
main/
  main.c             startup order: NVS, event loop, Wi-Fi, HTTP, Zigbee, Matter
  wifi_manager.c     stored credentials -> STA, otherwise the provisioning AP; mDNS
  captive_dns.c      answers every DNS query with the AP address
  zb_coordinator.c   stack task, signals, device interrogation, the command gate and
                     settle reads, remote frames, the Zigbee OTA server
  radio_ed.c         restarts an energy scan the 802.15.4 driver never finishes
  settings.c         global preferences, persisted in NVS
  crash_log.c        the last crash: registers, task and stack, kept for the next boot
  device_db.c        device registry, keyed by IEEE address, persisted in NVS
  group_db.c         named groups of (device, endpoint) members, persisted in NVS
  al_db.c            how each group exposes Adaptive Lighting, and which run it, in NVS
  button_db.c        buttons: their source, and all their rules in one pool, in NVS
  light_db.c         wired lights and their wiring, in NVS
  mlight_db.c        Matter lamps: the device and endpoint each one is, in NVS
  remote.c           what a Zigbee remote's frames mean, as a press; no dependencies
  colorspace.c       Planckian locus and hue/saturation <-> CIE xy, no dependencies
  curve_db.c         per-light response curves and the monotone cubic that reads them
  lighting.c         turns "light the room to N %" into commands: curves, fades,
                     fan-out, Adaptive Lighting, and Zigbee, wired or Matter lamps
  sun.c              the sun's day for Adaptive Lighting, and stretching it to clock times; no dependencies
  tz.c               POSIX time zone rules, for Adaptive Lighting's sunrise and sunset limits; no dependencies
  local_lights.c     drives the wired lights: relays, dimmable and tunable-white PWM
  ntc.c, thermal.c   the NTC reading and the overheat hold on the wired lights
  buttons.c          samples the pins, tells the gestures apart, runs the winning rule
  matter_bridge.cpp  the Matter bridge: one light per group
  matter_fabric.cpp  the coordinator's own Matter fabric, for the lamps it controls
  matter_commission.cpp  pairing a Matter device, and finding the lamps on it
  matter_ctl.cpp     Matter lamps: the command gate, subscriptions, the C API
  http_server.c      REST API and the embedded page
  auth.c             access keys and the failsafe window
  ota.c              POST /update: the handover to the updater, the multipart parser and flash
                     writer the updater uses, confirming a trial image, replacing the updater
  zb_ota.c           the one Zigbee device image: storage, which device may have it, progress
  zb_ota_file.c      the Zigbee OTA file header and Query Next Image; no dependencies
  auth_http.c        the key a request carries; shared with the updater
  www/index.html     the entire web interface, built from web/ and embedded in the firmware
updater/             the updater, the factory app that writes the coordinator's partition
  main/updater.c     why it is running: start the coordinator, wait for a file, or stay
  main/convert.c     rewriting another firmware's partition table, or this project's older one
  main/updater_http.c  its page, status, sign-in and POST /update
components/partition_layout/  builds the coordinator against partitions.csv less the updater row
tools/make-shelly-zip.py  packages the bootloader, the table and the updater for a stock Shelly Gen4
tools/make-release.sh     the release files from a build
web/                 the web interface's source (Svelte 5); `npm run build` writes main/www/index.html
test/                host tests: `make -C test` extracts the marked regions from the sources
                     and runs them; see test/Makefile for the list
docs/                these documents
```
