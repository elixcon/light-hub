# Firmware updates

After the first USB flash, the coordinator updates itself over the network. Use *Settings → Update
firmware* on the page, which shows a progress bar and reconnects by itself after the restart, or
upload the file with curl:

```sh
curl -H 'Expect:' --retry 6 --retry-delay 5 --retry-all-errors \
     -F 'file=@build/elixcon_light_hub.bin' http://zigbee.local/update
# Update Successful!
```

The upload is the one ESPHome's `web_server` OTA platform uses, so the file and the request have the
same shape as for any ESPHome device.

The file is a plain application image: `elixcon_light_hub.bin` from a release or from `build/`, what
ESPHome calls `firmware.ota.bin`. A merged `*-factory.bin` (bootloader first) is refused, with
`X-Ota-Error` saying why. Both start with the same `0xE9` magic byte, and only the app descriptor
tells them apart.

## Why curl needs the retry flags

The coordinator runs from its only app partition and cannot overwrite it. So the first attempt
restarts it into the updater, a small second firmware, and answers `503` with `Retry-After`. curl
waits and sends the file again, this time to the updater, which installs it and starts the new
coordinator. The whole command takes under a minute, and for about a minute of that the lights do
not respond. The page does the same handover by itself.

Once the coordinator is [secured](access.md), add the key: `curl -H 'Expect:' -H 'Authorization:
Bearer K7M2Q-9XH4T-R3WZP-6NBVA' --retry … -F 'file=@…' …`. The updater accepts the same keys, and a
browser signed in to the coordinator is signed in to the updater too.

`-H 'Expect:'` saves a second. curl announces `Expect: 100-continue` for bodies over 1 KiB, and
`esp_http_server` does not handle it, so curl waits out its own one-second timeout before it sends
anything. Browsers never send the header, so the page is not affected.

## When a new image does not work

A freshly uploaded image boots once on trial, and so does the first boot after a USB flash. It is
confirmed once the coordinator has been on Wi-Fi (its stored network, or a new one entered on the
setup page), or after five minutes, so a coordinator that has fallen back to its setup hotspot for a
good reason is not rolled back at the next power cut.

A build that cannot get onto the network is undone by the bootloader, and it goes back to the
updater, not to the previous firmware: there is room for only one copy of the coordinator. The
updater stays reachable at the usual address, says why it is running, and takes another upload.
Until it gets one, the lights, Zigbee and Matter are off.

An update that is started with no file following goes back to the coordinator by itself after ten
minutes.

## Partition table changes

A change to the partition table normally needs a USB flash: `idf.py -p /dev/cu.usbmodem* flash`, or
`write_flash @flash_args` with the files from a release. Never use `erase-flash`. The data
partitions keep their offsets and sizes on purpose, so the paired devices, their names, the rooms,
the curves, the keys and the Wi-Fi credentials all survive a flash. It writes the bootloader, the
table, the updater and the coordinator. `idf.py updater-flash` writes only the updater.

A coordinator on the previous table, from before 512 KiB was taken off the end of `app` for Zigbee
device firmware, gets the current one without a cable, in three steps:

1. Install the new coordinator as usual. It runs on the old table, and `/api/status` says `"layout":
   "old"`.
2. Replace the updater with `elixcon_light_hub_updater.bin` from the same build: *Settings →
   Firmware → Replace updater*, or `curl -H 'Expect:' -F
   'file=@build/updater/elixcon_light_hub_updater.bin' http://<box>/api/updater/image`. The
   coordinator keeps running while it writes and checks the image. A connection lost halfway leaves
   `"updater_valid": false`, and the coordinator then refuses to hand any update to that updater
   until it is uploaded again. The same happens with an updater from a build with a different
   partition table, which would not recognise the flash; `updater_problem` says which.
3. *Settings → Zigbee device firmware → Restart through the updater*, or `POST /api/updater/run`.
   The updater recognises the old table as this project's own with a longer `app`, writes the new
   table and starts the coordinator, which comes back with `"layout": "current"`. Writing the table
   is one flash sector, and it is the only step a power cut could turn into a cable job.

The updater only ever rewrites a table whose other rows are all unchanged and whose new rows lie in
the space `app` gave up (`cv_relayout_check`, host-tested). It leaves anything else alone. If the
installed coordinator is larger than the new `app`, the updater keeps the old table.
