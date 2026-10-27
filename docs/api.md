# REST API

The web page uses the same REST API that scripts can use. Bodies are JSON.

Once the coordinator has a key, every route except the first three below needs `Authorization:
Bearer <key>` (or the browser's cookie) and answers `401 {"error":"locked"}` without it. See
[Access](access.md).

## Examples

```sh
# Name the coordinator. The address follows the name, so it restarts and comes
# back at the hostname the response gives you: living-room-hub.local here.
curl -X POST http://zigbee.local/api/settings -d '{"name":"Living Room Hub"}'

curl -X POST http://zigbee.local/api/permit_join -d '{"duration":180}'
curl http://zigbee.local/api/devices
curl -X POST http://zigbee.local/api/devices/0x00124b0001abcdef/ep/1/onoff -d '{"state":"toggle"}'
curl -X POST http://zigbee.local/api/devices/0x00124b0001abcdef/ep/1/level -d '{"pct":30}'
curl -X POST http://zigbee.local/api/devices/0x00124b0001abcdef/ep/1/color -d '{"hue":280,"sat":90}'

curl -X POST http://zigbee.local/api/groups -d '{"name":"Living room"}'
curl -X POST http://zigbee.local/api/groups/1/members -d '{"ieee":"0x00124b0001abcdef","ep":1}'
curl -X POST http://zigbee.local/api/groups/1/level -d '{"pct":30}'

# Save the configuration, with or without the Wi-Fi password and the access
# keys, and give it back. The coordinator restarts to apply it.
curl -o zigbee-config.jsonl http://zigbee.local/api/backup
curl -o zigbee-config.jsonl 'http://zigbee.local/api/backup?omit=wifi,auth'
curl -X POST --data-binary @zigbee-config.jsonl 'http://zigbee.local/api/restore?keep=auth'
```

Examples for [curves](lighting.md#setting-curves-from-a-script) and
[buttons](buttons-and-remotes.md#setting-buttons-up-from-a-script) are on their own pages.

`{ieee}` is the 64-bit address as the API prints it, for example `0x00124b0001abcdef`. A [wired
light](wired-lights.md) has a made-up address, `0x025a42434c00` followed by its id in four hex
digits, endpoint 1.

## How commands and state work

Every command answers `202`, and the lamp's own report decides the final state. A command to a
device that has stopped answering gets `503`.

Each endpoint in `/api/devices` has a `features` list (`onoff`, `brightness`, `color_temp`, `color`)
and the lamp's physical `min_kelvin` and `max_kelvin`, so a client never has to guess what a light
can do. With a level curve active, bind a slider to `input_pct`, not `brightness_pct` (see [Response
curves](lighting.md#setting-curves-from-a-script)).

A group's `on` comes from its lights, so a light switched elsewhere still shows. Its
`brightness_pct` is the value the group was last set to, because lamps settle a percent or two apart
and an average would never match the slider. A group `toggle` is decided once for the whole group,
not per light.

The web page polls `GET /api/state` instead of the separate routes, because `esp_http_server`
answers one request at a time and each request costs airtime the Zigbee network needs. `rev` names
the state the tables were built from. Pass it back as `GET /api/state?rev=...`, and if nothing has
changed the answer is `{status, auth, rev, unchanged: true}` with no tables, which is what most
polls get. Its `status` is `/api/status` without the `CONFIG_ZBC_HEAP_WATCH` tables in `heap`:
those are only on `/api/status`.

## Access

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/api/auth` | no key needed: `mode` (`open`, `locked`, `failsafe`, `setup`), whether this caller is `authorized`, seconds left in the failsafe |
| `POST` | `/api/auth/session` | no key needed: `{"key":"…"}` signs this browser in with a key; with no body while access is open, issues a new browser key (`issued`, `key`) |
| `GET` | `/` | the web page |
| `GET` | `/api/auth/keys` | every key: `id`, `kind`, `name`, `last_used_s_ago`, and which one is `current` |
| `POST` | `/api/auth/keys` | `{"name":"Home Assistant"}` creates an API key; the `key` is in this response only |
| `DELETE` | `/api/auth/keys/{id}` | revokes a key; answers the resulting `mode` |
| `POST` | `/api/auth/lock` | closes the failsafe window early |

## Devices and lights

| Method | Path | Purpose |
|---|---|---|
| `POST` | `/api/permit_join` | `{"duration":180}` opens the network for pairing, `0` closes it |
| `GET` | `/api/devices` | every known device with per-endpoint on/off state, plus `lqa`, `rssi`, `frames_heard`, `frames_missed` and `child` (see [Link counters](radio.md#link-counters)), and `ota`: the `manufacturer_code`, `image_type`, `file_version` and `hw_version` its OTA client reported, `null` for a device without one. `source` is `zigbee`, `gpio` or `matter`; a dimmable or tunable-white wired light has `pwm_phase` (degrees, `null` while not driven); a Matter lamp has `node_id` (hex), `remote_ep`, `vendor_id`, `product_id`, `bridged`, `verified` and `needs_pairing` |
| `POST` | `/api/devices/{ieee}/ep/{ep}/onoff` | `{"state":"on"\|"off"\|"toggle"}` |
| `POST` | `/api/devices/{ieee}/ep/{ep}/level` | `{"pct":30}`: brightness, 1-100 |
| `POST` | `/api/devices/{ieee}/ep/{ep}/color_temp` | `{"kelvin":2700}` |
| `POST` | `/api/devices/{ieee}/ep/{ep}/color` | `{"hue":280,"sat":90}`: degrees and percent |
| `PATCH` | `/api/devices/{ieee}` | `{"name":"Desk lamp"}` |
| `DELETE` | `/api/devices/{ieee}` | asks a lamp to leave and forgets the device (a sleepy remote is only forgotten); a wired light gives its pin back; a Matter lamp is forgotten, and its device is told to forget the coordinator (RemoveFabric) when it was the device's last lamp |
| `GET` | `/api/devices/{ieee}/ep/{ep}/curves` | this light's response curves |
| `POST` | `/api/devices/{ieee}/ep/{ep}/curves` | replaces them: `level` points, `chroma` stops; a `null` member clears that part |
| `DELETE` | `/api/devices/{ieee}/ep/{ep}/curves` | back to passing the brightness straight through |
| `POST` | `/api/lights` | `{"name":"Porch"}` creates a wired light, answers `id`, `ieee` and `ep` |
| `PATCH` | `/api/lights/{id}` | its wiring, any subset: `name`, `gpio`, `active_low`, `power_on` (`off`/`on`/`last`), `kind` (`onoff`/`dim`/`cwww`/`cwnwww`), `pwm_hz` (100-19531) for a dimmable or tunable-white light, and for tunable white `gpio_warm`, `cold_kelvin`, `warm_kelvin`, `mix` (`constant`/`full`/`custom`) and `mix_curves` (`{"cold"|"neutral"|"warm": {"points": [[x, y], ...]} \| null}`, x 1 = warm LEDs to 100 = cold, y percent, see [wired lights](wired-lights.md#mixing)), plus `gpio_neutral` and `neutral_kelvin` for three channels; switched and removed through `/api/devices` like any lamp |

## Rooms

Rooms are called groups in the API.

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/api/groups` | every group, its lights and their state |
| `POST` | `/api/groups` | `{"name":"Living room"}` creates one, answers with its id |
| `PATCH` | `/api/groups/{id}` | `{"name":"Kitchen"}` and/or `{"adaptive":"off"\|"mode_select"\|"endpoint"}` (how Matter sees Adaptive Lighting; answers `restart_needed` when that takes a restart) and/or `{"turn_on":"last"\|"adaptive"\|"full"}` (what a Matter controller's "turn on" does to the room, dark or lit) |
| `DELETE` | `/api/groups/{id}` | deletes the group; the lights stay paired |
| `POST` | `/api/groups/{id}/members` | `{"ieee":"0x…","ep":1}` adds a light |
| `DELETE` | `/api/groups/{id}/members/{ieee}/{ep}` | removes a light |
| `POST` | `/api/groups/{id}/level` | `{"pct":30}`: brightness for the whole group |
| `POST` | `/api/groups/{id}/onoff` | `{"state":"on"\|"off"\|"toggle"}` |
| `POST` | `/api/groups/{id}/adaptive` | `{"state":"on"\|"off"}` starts or ends Adaptive Lighting for the group |
| `GET` | `/api/groups/{id}/day` | the room's Adaptive Lighting day: `day` (`k_midnight`, `k_noon`, `k_base`, `pct_midnight`, `pct_noon`, `sunrise` and `sunset` limits, `pct_points` / `k_points`, and the limits `offset_limit_k`, `base_min_k`, `base_max_k`, `max_points`; a room never set has the defaults below), `sun` (today's `rise`, `noon`, `set`, `polar`, and `al_rise` / `al_set`, where this room's limits put them; `null` without a location), `time_zone` (as in `/api/settings`), `now` (`pct` and `offset_k` the room is sent now while it runs the mode; `null` without a clock or a location) and `time_utc` |
| `POST` | `/api/groups/{id}/day` | any subset of `day`'s fields, onto the room's day: `{"k_midnight":-1500,"k_noon":500,"k_base":4000,"pct_midnight":20,"pct_noon":100}` (the defaults), `"sunrise"` and `"sunset"`, each `{"earliest":"HH:MM"\|null,"latest":"HH:MM"\|null}` (either key; `null` for the whole object clears both), local clock times that stretch the day and are refused (`400`) while there is no time zone (send `time_zone` to `/api/settings` first); and `"pct_points"` / `"k_points"`, up to 4 `[x, value]` pairs between the ends, x 2-99 ascending (1 is solar midnight, 50.5 sunrise and sunset, 100 noon), `[]` for the straight line. Answers like the `GET`; `404` for an unknown room |

## Buttons and remotes

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/api/remotes` | the last few presses from any paired remote, for learning which key a button means, and the last decoded command `frames` |
| `GET` | `/api/buttons` | every button, its source, its rules and its live pin reading |
| `POST` | `/api/buttons` | `{"name":"Kitchen switch"}` creates one, answers with its id |
| `PATCH` | `/api/buttons/{id}` | replaces the whole record: name, source and every gesture |
| `DELETE` | `/api/buttons/{id}` | deletes it and gives the pin back |
| `POST` | `/api/buttons/{id}/test` | `{"event":"short"\|"long"\|"double"\|"triple"}` runs a gesture with no finger on the pin; a long press takes `hold_ms` |

## Settings and system

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/api/status` | Wi-Fi, Zigbee network, permit-join countdown, uptime, `reset_reason` (`panic`, `task_wdt`, `brownout`, `software`, `power_on`, `usb`…), `last_crash` (null, or where the last crash was: see [development](development.md#after-a-crash)), radio counters, free heap, `thermal` (`state` `off`/`ok`/`hot`/`fault`, `temp_c`, `max_c`, `trip_c`, `release_c`, `trips`, `mv`, `held`), `wired` (the [wired lights'](wired-lights.md) fades since boot: `fade_ticks`, `fade_late` - over 20 ms after the step before - and `fade_max_gap_ms`), `wled` (with `CONFIG_ZBC_WLED`, on by default: the [WLED interface](wled.md): `enabled`, `room`, `pixels`, `listening`, `live`, `override`, `others_off`, `lights_off` (the room's other lights the running stream holds off), `fps_in`, `source`, `protocol`, `packets`, `refused`, `sessions`, `taken`, `dropped_by_hand`, `frames`, `frames_failed`, `late`, `last_end`), `zigbee.ent` (with `CONFIG_ZBC_WLED`: the entertainment frames behind it: `active`, `streams`, `frames_sent`, `frames_refused`, `sync_sent`/`sync_delivered`/`sync_undelivered`, `balance`, `rejected`, `answers_heard`, `relay_heard`), and with `CONFIG_ZBC_HEAP_WATCH` the deepest drop per route (`heap.routes`), the boot-wide lows with what was happening (`heap.lows_between_requests`, a low inside a request named by `route`) and the events to match them (`heap.marks`; with `CONFIG_ZBC_HEAP_WATCH_TASKS` also `heap.tasks`) - only here, never in `/api/state` |
| `GET` | `/api/state` | `status`, `devices`, `groups`, `buttons`, `remotes` and `auth` in one request; `?rev=` answers `unchanged` without the tables when nothing moved |
| `GET` | `/api/settings` | global settings: `backup` (whether this build has the two routes below), the fade time, `wifi_sleep`, `warm_off`, `wled` (`enabled`, `room`, `others_off`; absent from a build without `CONFIG_ZBC_WLED`), `zigbee_channel` (the channel the network is on), the coordinator's own name, Adaptive Lighting's `location`, `time_zone` (`posix`, `name`, `offset_s` now; `null` until one is set), `sun` (today's `rise`, `noon`, `set`, `polar`; each room's day is `/api/groups/{id}/day`), and `thermal` (the overheat sensor, with the pins it can use in `adc_pins`) |
| `POST` | `/api/settings` | `{"transition_ms":400}` (0-10000), `{"wifi_sleep":true}` (takes effect at once), `{"warm_off":true}`, `{"name":"Living Room Hub"}`, `{"location":{"lat":48.14,"lon":11.58}}` (`null` forgets it) and/or `{"time_zone":{"posix":"CET-1CEST,M3.5.0,M10.5.0/3","name":"Europe/Berlin"}}` (the zone the rooms' sunrise and sunset limits are read in; `null` forgets it, refused with `409` while a room has limits) and/or `{"thermal":{"gpio":3,"b":3350,"r0_ohm":10000,"rs_ohm":10000,"ntc_to":"gnd","trip_c":75}}` (any subset; `"gpio":null` switches it off; takes effect at once) and/or `{"zigbee_channel":15}` (11-26: moves the network, keeping its pairings; `zigbee.channel_move` in `/api/status` follows it; `409` while the network is not up or a move or survey runs) and/or `{"wled":{"enabled":true,"room":1,"others_off":true}}` (any of the keys; `"room":null` for none; `others_off` switches the room's other lights off while a stream runs, read when one starts; `400` in a build without `CONFIG_ZBC_WLED`); every key is optional, and `"name":""` restores the default |
| `GET` | `/api/wifi/scan` | networks in range, with `channel` and `second` (`"above"`/`"below"` for a 40 MHz network) |
| `POST` | `/api/zigbee/scan` | starts a channel survey (`202`; `409` while the network is not up or a move or survey runs) |
| `GET` | `/api/zigbee/scan` | the survey: `state` (`idle`/`running`/`done`/`failed`), `channels` (each energy scan's reading in dBm, per channel 11-26) and the other Zigbee `networks` heard |
| `POST` | `/api/wifi` | `{"ssid":"…","password":"…"}`, then reboots |
| `POST` | `/api/wifi/test` | from the setup hotspot only: `{"ssid":"…","password":"…"}` tries the network first, stores it and restarts onto it once it has an address; `GET` says how it went |
| `POST` | `/api/wifi/resume` | leaves a setup hotspot started by ten button presses, back onto the stored network; `/api/status` shows `temporary_hotspot`, `home_ssid` and `returns_in_s` while one runs |
| `POST` | `/api/restart` | restarts the coordinator, erasing nothing; `409` while a new image is on trial and Wi-Fi has not come up (the restart would roll it back). Built with `CONFIG_ZBC_CRASH_TEST`, `{"crash": "abort"}`, `{"crash": "fault"}` or `{"crash": "stack"}` crashes it instead, to test `last_crash` |
| `POST` | `/api/factory_reset` | forgets the Zigbee network, devices, Wi-Fi, Matter controllers, Matter lamps and access keys (no body, or `{"scope":"all"}`); `{"scope":"zigbee"}` keeps Wi-Fi, Matter and the keys. `400` for any other scope, `409` while the Zigbee network is not running, as in the setup hotspot, because leaving it needs the stack - nothing is erased in either case |
| `GET` | `/api/backup` | (with `CONFIG_ZBC_CONFIG_BACKUP`, on by default; `/api/settings` says `"backup"`) the configuration as a file (JSON Lines, `application/x-ndjson`): settings, devices and their names, rooms, curves, buttons, wired lights, Matter lamps, Adaptive Lighting, the Wi-Fi network with its password in plain text, and the access keys as their hashes. Not the Zigbee network, its channel, or the Matter controllers. `?omit=wifi,auth` leaves either secret out, and the file's header says so (`omitted`); any other name answers `400` |
| `POST` | `/api/restore` | (with `CONFIG_ZBC_CONFIG_BACKUP`) a file from `GET /api/backup` as the raw body, up to 64 KB. Checked whole and staged; then the coordinator restarts and the boot replaces the configuration with the file's - a setting or table the file does not have is removed. `?keep=wifi,auth` keeps this coordinator's Wi-Fi or keys, and so does a file that left them out; a file with no Wi-Fi never removes it. Answers `{"ok":true,"addresses_kept":…,"entries":…,"skipped":…,"wifi":…,"auth":…}`: `addresses_kept` false means the file is from another Zigbee network, and its devices are offline until paired again here. `400` with the reason for a file it will not restore (cut short, damaged, a table from a newer firmware), `409` while the Zigbee network is not up, `413` above 64 KB, `507` without room in flash to stage it - nothing changes in any of those |

## WLED

Only while the [WLED interface](wled.md) is on; `404` otherwise, and in a build without `CONFIG_ZBC_WLED`. No key: WLED clients send none.

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/json`, `/json/si` | `state` and `info` together (`/json` adds `effects` and `palettes`) |
| `GET` | `/json/info` | WLED's info: `ver`, `vid`, `leds.count` (the pixels), `udpport` (21324), `live`, `name`, `wifi`, `mac`, `ip` |
| `GET` | `/json/state` | `on` (the room is lit), `bri`, `live`, one segment of all the pixels |
| `GET` | `/json/cfg`, `/json/eff`, `/json/pal`, `/json/fxdata`, `/json/nodes` | what clients read before they stream |
| `GET` | `/ws` | Only with `CONFIG_ZBC_WLED_WS`: WLED's WebSocket, which the WLED apps need to show the device online. It sends `{"state", "info"}` when it opens, takes the same commands as `POST /json/state`, and answers each with the new state. |
| `POST`/`PUT` | `/json/state` | `{"live":false}` ends a stream (the room goes back, or off with `"on":false`); `{"on":true\|false\|"t"}` switches the room while nothing streams; `{"bri":128}` scales the stream; `{"v":true}` answers with the state |

Pixels arrive by DDP on UDP 4048, or by WLED's own UDP protocols on UDP 21324 (WARLS, DRGB, DRGBW,
DNRGB; byte 1 the timeout in seconds, 255 for none).

## Matter

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/api/matter` | pairing code, QR payload, which controllers it is in (`ours` marks the coordinator's own fabric) and each one's `subscriptions` (`live`, `kept` to resume after a restart, the controller's intervals, `resume_tries`, `live_after_s`), whether pairing is open, the last commands controllers sent, and `controller`: its own `fabric_index`, each paired Matter device with its subscription (`online`, `drops`, `reports`, `last_error`), and the `pairing` in progress (`state`, `step`, `error`, `lamps`, `heap_low` per step) |
| `POST` | `/api/matter/window` | opens pairing for 15 minutes |
| `POST` | `/api/matter/reset` | asks every Matter lamp to forget the coordinator, forgets every Matter controller and its own fabric, then reboots |
| `POST` | `/api/matter/lamps/pair` | `{"code":"3497-011-2332"}` or `{"code":"MT:…"}` pairs a Matter device in pairing mode, `202`; progress in `/api/matter` `controller.pairing`. `409` while one runs, or when the coordinator has no fabric slot left |
| `POST` | `/api/matter/nodes/{node}/rescan` | reads a paired Matter device's lights again, `202` |

## Firmware

See [Firmware updates](updates.md) and [Updating Zigbee devices](zigbee-device-updates.md).

| Method | Path | Purpose |
|---|---|---|
| `POST` | `/api/update/prepare` | restarts into the updater, `202`; poll `/api/status` until it says `"updater": true`, then upload |
| `POST` | `/update` | multipart firmware upload, ESPHome's shape; the coordinator answers `503` and restarts into the updater, which takes the retried upload |
| `POST` | `/api/updater/image` | multipart `elixcon_light_hub_updater.bin`: replaces and checks the updater while the coordinator runs; `/api/status` `ota.updater_valid` says whether it is whole |
| `POST` | `/api/updater/run` | restarts through the updater and straight back, `202`; how a coordinator on the old partition table gets the new one (`ota.layout`) |
| `GET` | `/api/zigbee/ota` | the stored Zigbee device image, its target, the transfer and the last query: the same object as `zigbee.ota` in `/api/status` |
| `POST` | `/api/zigbee/ota/image` | multipart upload of one plain Zigbee OTA file (`0x0BEEF11E` header, up to 512 KiB); `409` while a device is downloading |
| `DELETE` | `/api/zigbee/ota/image` | forgets the image |
| `POST` | `/api/zigbee/ota/notify` | `{"ieee":"0x…"}` gives the image to that device and asks it to fetch it; `{}` asks every listening device to report its firmware, giving nothing |
| `POST` | `/api/zigbee/ota/target` | `{"ieee":"0x…"\|null}` names or clears the device, without asking it |
| `POST` | `/api/zigbee/ota/ask` | `{"ieee":"0x…"}` makes the device report the firmware it runs (`ota` in `/api/devices`); offers nothing and needs no image. A sleepy remote answers once woken (for ten minutes) |
