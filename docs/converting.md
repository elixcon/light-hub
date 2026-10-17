# Converting a Shelly or ESPHome device

> [!WARNING]
> **Shelly conversions have not been tested successfully yet.** A conversion that goes wrong can
> leave the unit unable to start, and the only way back is its UART. Do not start unless you are
> prepared to open the unit and flash it over serial:
>
> - a USB-to-serial adapter with 3.3 V logic, wired to the unit's TX, RX, GND and 3.3 V pads;
> - the ESP32-C6's boot pin, GPIO9, held to GND while the unit powers up, which starts the chip's
>   ROM bootloader instead of the firmware;
> - **the unit disconnected from mains the whole time.** Many Shelly units have no isolation between
>   mains and their low-voltage side, so power it only from the adapter's 3.3 V while it is open.
>
> With that wired, save the whole flash before converting: `esptool.py --chip esp32c6 -p <port>
> read_flash 0 0x800000 shelly-backup.bin` (8 MB on a Gen4). Writing it back with `write_flash 0x0
> shelly-backup.bin` returns the unit to exactly what it shipped with. Over UART the unit is an
> ordinary ESP32-C6, so the [factory image](../README.md#from-a-release) installs the coordinator
> on it as on any blank board.

Two kinds of ready-made ESP32-C6 hardware can be turned into a coordinator over the network, without
opening them. Both conversions erase the original firmware.

## A Shelly Gen4 on its own firmware

> [!WARNING]
> Not yet tested successfully. Read the [warning at the top of this page](#converting-a-shelly-or-esphome-device)
> and have the UART wired and the flash saved before you upload anything.

A Shelly Gen4 (ESP32-C6, 8 MB) on stock firmware 2.0.0 takes third-party firmware through its own
update page. The package hands Shelly's installer the updater as the firmware, plus the unit's own
partition table, written at 0x8000 where our bootloader reads it. The unit then starts as the
updater, which converts the flash the same way it converts an ESPHome device. It is already at the
address it needs to be at, so nothing is copied.

1. Ask the unit what it is: `curl http://<ip>/rpc/Shelly.GetDeviceInfo`. `app` is its hardware code
   (`Mini1G4`, `S1G4`, ...), and `slot` must be 1. The installer writes the slot the unit is not
   running from, and only slot 0 is at the address the updater needs. A factory-fresh unit is on
   slot 0, so give it your Wi-Fi and let it install any stock update first (`Shelly.Update`).
2. Fetch the firmware package Shelly would install on it, because the partition table inside is the
   one our package has to carry: `curl -sk https://updates.shelly.cloud/update/Mini1G4` answers with
   a `.stable.url` to download.
3. Build: `idf.py build`, then `python3 tools/make-shelly-zip.py --hardware shelly-1-mini-gen4
   --stock-zip <that zip>`. The script refuses anything that would not boot, and prints what each
   part writes and which of our partitions it lands in. Read that before uploading.
4. Upload the zip on the unit's own firmware page, where it takes a file. (Over RPC it is `OTA.Start
   {size}` and then `OTA.Write {offset, data}` with base64 chunks; `/ota` itself answers 405.) The
   unit checks the package, writes it and restarts.
5. It has no stored Wi-Fi, so it opens the hotspot `elixcon-XXXX`. Join it, open
   http://192.168.4.1/, and under *Convert this device* enter your network and press *Convert*. It
   says *go*, writes our partition table and bootloader, erases what is left of the Shelly firmware
   and comes back on your network as the updater. Upload `elixcon_light_hub.bin` to it as usual
   ([Firmware updates](updates.md)), then set up the relay, the wall switch and overheat protection
   ([A converted Shelly](wired-lights.md#a-converted-shelly)).

Watch the unit's own log while it installs. First point Shelly's debug log at a machine on your
network, with `POST /rpc/Sys.SetConfig` and `{"config":{"debug":{"udp":{"addr":"<host>:5140"}}}}`,
and listen on that port. The installer says what it does to every part, and its log is the only
account of an upload that does not come back.

A package the installer refuses costs nothing, because it writes only after its checks pass. Once it
accepts one, the unit runs this firmware. The way back to Shelly's firmware is the UART pads and the
stock package from step 2, and so is the way out of a conversion interrupted by a power cut.

## A device that runs ESPHome

An ESP32-C6 with 4 MB of flash or more that runs ESPHome can be converted without opening it, as
long as its `web_server` accepts OTA uploads.

1. Upload the updater image through ESPHome, the same way as any firmware: `curl -H 'Expect:' -F
   'file=@build/updater/elixcon_light_hub_updater.bin' http://<device>/update`. ESPHome restarts
   into it. Nothing is erased yet, and until step 3 a power cut brings ESPHome back.
2. The updater does not know your Wi-Fi, so it opens the open hotspot `elixcon-XXXX`. Join it and
   open http://192.168.4.1/.
3. Under *Convert this device*, check the device and its current partitions, enter your Wi-Fi
   network and password, tick the box and press *Convert*. If the updater first has to move out of
   the way, it says so and restarts. Press *Convert* again.
4. The device rewrites its bootloader and partition table, joins your network and comes back as the
   updater, ready for the coordinator. Upload `elixcon_light_hub.bin` to it as usual ([Firmware
   updates](updates.md)).

This erases ESPHome for good. For a few seconds in step 3 the bootloader and partition table are
being rewritten, and a power cut then leaves the device unable to start until it is flashed over a
serial cable, which on a Shelly means wiring its pads. Keep the power steady until the page says
*Converted*.

## After converting

A converted device also calls itself `zigbee.local`. If another coordinator is on the same network,
give it a name of its own in *Settings*.
