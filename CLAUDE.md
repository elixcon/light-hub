# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

ESP32-C6 firmware (ESP-IDF v5.5.5, C with some C++ for Matter) that runs a Zigbee coordinator on
the 802.15.4 radio, joins Wi-Fi on the same chip, bridges each room to Matter and serves a Svelte
web UI. Target: 4 MB flash, built with `-Os`. User-facing docs are in `docs/`; `docs/development.md`
has the source layout, the crash-decoding procedure and the shared-radio settings.

Source comments and `web/README.md` point at sections of this file by name ("State is never
assumed", "Commands go through a per-endpoint gate", "Move/Stop", the remote section). Keep those
headings when editing.

## Commands

```sh
. ~/esp/esp-idf/export.sh          # every new shell; needs zopfli on PATH too
idf.py build                       # builds the coordinator AND the updater (subproject)
idf.py -p /dev/cu.usbmodem* flash monitor   # bootloader, full partition table, both apps
idf.py app-flash                   # coordinator only
idf.py updater-flash               # updater only
idf.py -C updater build            # the updater on its own
idf.py menuconfig                  # options under "Elixcon Light Hub" (main/Kconfig.projbuild)
```

Never run `idf.py erase-flash` unless the user explicitly wants to lose the Zigbee network, paired
devices, keys and Wi-Fi credentials.

Host tests (plain `cc`, no ESP-IDF, about a second):

```sh
make -C test                       # build and run all
make -C test test_gate && test/test_gate   # one test
make -C test clean
```

Web UI (`web/`, Node):

```sh
npm install
npm run build        # writes ../main/www/index.html, then the 150 KB gzipped size gate
npm run dev          # HMR, proxies /api and /update to http://zigbee.local (ZBC_TARGET=http://host:port)
npm run dev:mock     # HMR against mock/server.mjs, no hardware needed
npm run check        # svelte-check
npm test             # all; one file: node --test test/curves.test.ts
```

CI (`.github/workflows/firmware.yml`) builds from `sdkconfig.defaults`. `sdkconfig` is local and
gitignored, so put new defaults in `sdkconfig.defaults`. CI also fails if the committed
`main/www/index.html` differs from what `web/` builds.

Release files (factory image, USB zip, app and updater images, ELF): `tools/make-release.sh build
dist` after `idf.py build`. A `v*` tag makes CI publish them as a GitHub release. The firmware
version is `git describe`, so tag first.

## Architecture

### Two images, one app partition

`partitions.csv` has a 1 MiB `updater` (factory) and one `app` (ota_0) instead of two OTA slots. An
update restarts into the updater (`updater/`), which writes `app` and restarts into it, and an image
that never confirms rolls back to the updater. The root `CMakeLists.txt` builds `updater/` as an
ExternalProject. `components/partition_layout` makes the coordinator build against a derived table
without the updater row, `build/partitions-app.csv`, generated at every configure and never
committed. The root CMake then swaps the full table back into the flash targets. The data partitions'
offsets must never move: a USB flash of a new table has to keep every stored setting.
`updater/main/convert.c` rewrites other firmwares' tables (Shelly, ESPHome) and this project's older
one.

The updater compiles shared modules (`wifi_manager.c`, `settings.c`, `auth.c`, `ota.c`, ...) from
`../../main` by path. Never copy them, and keep them free of Zigbee and Matter dependencies.
`updater/main/stubs.c` stubs what it lacks.

### Layers

- `http_server.c` is transport: REST under `/api` (see `docs/api.md`) plus the embedded page. One
  request at a time (`esp_http_server`), so the UI polls one route, `GET /api/state?rev=`, which
  answers `unchanged` when `state_rev` has not moved. Anything the page shows must call
  `zbc_state_bump()` when it changes, or it shows stale.
- `lighting.c` is policy. It turns "room to N %" into per-lamp commands through response curves
  (`curve_db.c`, monotone cubic `zbc_pchip`), fades, Warm while off, and Adaptive Lighting
  (`sun.c`, `tz.c`). It dispatches to Zigbee (`zb_coordinator.c`), wired lights (`local_lights.c`,
  LEDC and MCPWM PWM, with `ntc.c`/`thermal.c` overheat hold) or Matter lamps (`matter_ctl.cpp`).
  `zb_coordinator` knows nothing of groups or curves.
- `zb_coordinator.c` handles the stack task, network formation, device interrogation (capabilities
  from clusters and ColorCapabilities, never a model list), reporting, the command gate, settle
  reads, remote frames, the Zigbee OTA server and the worker task.
- `*_db.c` are NVS-backed registries: devices keyed by IEEE address, groups of (device, endpoint),
  curves, buttons, wired lights, Matter lamps, Adaptive Lighting. `config_backup.c` saves and
  restores them, and the restore is applied at boot before any table loads.
- Matter is C++. `matter_bridge.cpp` exposes one dimmable light per group, and
  `matter_fabric.cpp`/`matter_commission.cpp`/`matter_ctl.cpp` are the controller for Wi-Fi Matter
  lamps (`matter_priv.h` is internal and runs on the CHIP thread; `matter_ctl.h` is the C API). No
  Thread: the radio runs Zigbee.
- Leaf modules have no ESP-IDF dependencies and compile whole in host tests: `remote.c`,
  `colorspace.c`, `sun.c`, `tz.c`, `ntc.c`, `zb_ota_file.c`, `light_math.h`. Keep them that way.
- `main/CMakeLists.txt` links several `-Wl,--wrap=` hooks: the 802.15.4 driver (`radio_ed.c`
  restarts energy scans the driver abandons, `radio_coex.c` counts frames and adjusts send
  priority), `esp_panic_handler` (`crash_log.c`), and the heap per-task hooks (`heap_watch.c`).
- `main.c` fixes the startup order: NVS, crash log, config restore, registries, Wi-Fi, HTTP, Zigbee,
  Matter.

### Threading rules

- The Zigbee stack lock (`esp_zigbee_lock_acquire`) guards every SDK call. Public `zb_*` functions
  take it themselves and may be called from any task. Zigbee callbacks already hold it, so they
  must not take it, block or send.
- Nothing that takes the stack lock may run on the esp_timer task. All esp_timer callbacks share
  one task with the 10 ms button sampler, whose debounce and gesture timing depend on that rate.
  Timers only notify a task (`work_post` in `zb_coordinator.c`, `al_task` in `lighting.c`).
  Timer callbacks use spinlocks, never mutexes.
- The Matter controller code runs on the CHIP thread or under the CHIP stack lock.

## Design rules

### State is never assumed

Every command answers `202`. The lamp's report or read-back decides the state, never the fact that a
command was sent. After a command, settle reads (keyed per short address and endpoint, re-armed from
the transition time) check where the lamp ended up and correct it a bounded number of times
(`corrections` in `/api/status`). A device that stops answering is marked gone, gets `503`, and a
command to it is treated as a probe. The UI mirrors this in `web/src/lib/state.svelte.ts`: an
optimistic `pending` value clears on exact agreement or after `PENDING_TTL_MS`.

### Commands go through a per-endpoint gate

Every state-changing Zigbee command is a `zbc_cmd_t` that passes through a gate with two channels
per endpoint (level/on-off and colour), one frame in flight each, released by the APS delivery
confirmation. An undelivered command is re-sent from its slot. The UI likewise keeps one command on
the wire at a time (`sendCommand`) and throttles sliders per path. The gate and settle scheduler
are host-tested (`test_gate`, `test_settle`), because their failure on hardware shows up only as a
temperamental lamp.

### Rooms are fanned out, not group-cast

A group is a coordinator-side member list. Commands go to each member's own route rather than as a
ZCL group-cast, so a lamp that is reset and rejoins needs nothing re-applied.

### Move/Stop

Never send an edge-triggered Move/Stop to a lamp: if the Stop is lost the lamp runs to a rail, and
some lamps ignore Stop. Holds (`buttons.c` `run_hold`) and Matter Move (`matter_bridge.cpp`) send
repeated steps. Each one aims one interval ahead of the sweep, with a transition of exactly that
interval, so the lamp keeps moving and a lost frame is repaired by the next one.

### Remotes

Everything model-specific about a remote lives in `remote.c` (decoding by model via
`remote_model()`), and `web/src/lib/remotes.ts` mirrors its key numbers (0 on/off, 2 up, 1 down).
Each entry must follow the frames the device actually sends or its zigbee2mqtt converter.

### Wired-light PWM

`zbc_pwm_place_n` spreads a light's channel pulses evenly over the period. A pulse that would wrap
past the period's end is emitted as its complement with the GPIO output inverted, so LEDC is never
given `hpoint + duty > max`.

Channels 6-11 are MCPWM (numbering in `light_math.h`). An operator has two comparators for its two
outputs, so a pulse cannot sit anywhere: timers count up and down, and each output is centred on its
timer's zero or peak (`zbc_mcpwm_out`, `slot.peak`). One timer per operator; units at one frequency
are synced to the first at the phase the plan gives (`zbc_mcpwm_unit_deg`). `zbc_pwm_plan` gives
LEDC the lights while any split lets it, so a set LEDC alone can run is planned exactly as before.
An MCPWM timer's period is LEDC's for its frequency to the tick (`zbc_mcpwm_peak`). Rounded to the
hertz instead, a 4882 Hz light on it slid past LEDC lights at 9765 Hz 1.8 times a second, which
lights sharing a power supply can show as flicker.
`mc_start_locked` makes and deletes a 160 MHz timer before any other: the driver's group clock
divider is set by the first timer and shared, and without that one a later timer is refused or
runs at a resolution its counts were not worked out for. `mc_set_timer_locked` remakes a unit's
timer only after the old one has stopped at its zero: one remade while the old still ran keeps the
old divider.

### Shared radio

Wi-Fi and Zigbee share one antenna. `esp_coex_wifi_i154_enable()` must be called (the updater
calls it too). Leave the 802.15.4 coexistence priorities at their defaults, and keep
`CONFIG_ESP_PHY_RF_CAL_FULL`, channels 1-13 and `CONFIG_LWIP_DHCP_DOES_NOT_CHECK_OFFERED_IP`.
`docs/development.md` explains each one.

### Zigbee SDK

The build uses esp-zigbee-lib 2.0.4, whose API is `ezb_*`. Most online examples use the 1.x
`esp_zb_*` API, which does not apply here. The reference is
`managed_components/espressif__esp-zigbee-lib/include/ezbee/`. esp_matter is pinned to 1.6.0
(the release built against IDF 5.5.5).

### Host-test regions

Code under test is extracted at test time from between `/* >>> host-test region: NAME <<< */` and
`/* >>> end host-test region: NAME <<< */` markers, never copied. Moving or renaming a marker
breaks `make -C test`. To test more code: mark the region, add it to `REGIONS` and a
`FROM_<name>` line in `test/Makefile`, add a test target, and `#include "<name>.inc"` against stubs.
Regions inside `.cpp` files must stay plain C (no namespaces), because they compile as C on the
host.

### Web UI

- `main/www/index.html` is the shipped artefact and is committed, so firmware builds never need
  Node. Rebuild and commit it with every UI change. Even comment edits can change Tailwind's output.
- The gzipped page must stay at or under 150 KB (`web/scripts/size-check.sh`, same zopfli as
  CMake). No bits-ui and no tailwind-merge. `cn` is plain `clsx`, so a component's classes and a
  caller's `class` must never set the same property (`web/test/classes.test.ts`).
- `src/lib/components/ui/*` were generated by shadcn-svelte and then hand-edited. Do not re-run
  `npx shadcn-svelte add` over them.
- `pchip.ts`, `curves.ts`, `colorspace.ts`, `adaptive.ts` and `tz.ts` mirror C code. The C is the
  authority; change both sides together and keep the web tests matching the C host tests.

## Debugging on hardware

Opening the serial port resets the board, so diagnostics go through `GET /api/status`: the radio
counters (`cmd_undelivered`, `cmd_slow`, `corrections`, ...; read them as rates), and `last_crash`
from `crash_log.c`. Decode crash addresses against the matching `build/elixcon_light_hub.elf`,
subtracting one from `ra` and each `stack` entry (see `docs/development.md`). A `Stack protection
fault` with `guard.sp_in_bounds` true is the stack guard's false alarm (esp-idf#14456), not an
overflow: do not grow a stack for it.
`CONFIG_ZBC_CRASH_TEST` enables a test crash via `POST /api/restart`, and `CONFIG_ZBC_HEAP_WATCH`
records per-request heap lows.
