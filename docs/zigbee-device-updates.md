# Updating Zigbee devices

A lamp, plug or remote that supports ZCL OTA Upgrade can get new firmware from the coordinator. Open
the device on the page and press *Check for updates*. The page reads the [Zigbee2MQTT OTA
index](https://github.com/Koenkk/zigbee-OTA) from GitHub and picks the newest image for the device's
manufacturer code, image type, model and current version. It downloads the image in the browser,
strips the signing wrapper IKEA puts around its files, and uploads the plain OTA file to the
coordinator. The coordinator itself never talks to the internet. *Install from a file* takes an
`.ota` or `.zigbee` file from your computer instead.

## How an update runs

The coordinator holds one image of up to 512 KiB, in the `zb_ota` partition. Most of the collection
fits. A few large Hue lamp images do not, and the page says so.

An image is given to one named device only. Every other device that asks is told there is none, so a
stored image never updates a whole house by accident. *Install* sends that device an Image Notify,
and the device then fetches the image block by block over Zigbee, at roughly 180 bytes a second.
That takes a quarter of an hour or more. The device keeps working until it restarts onto the new
firmware at the end.

The coordinator learns what firmware a device runs from the device itself. A lamp's OTA client
attributes are read when it joins, *Check for updates* asks the device again first, and every Query
Next Image a device sends says it too. A device that has finished a download is asked again over the
next eight minutes, because it may restart onto the new firmware without joining again. *Ask all
devices to check* in *Settings* broadcasts a notify so that listening devices report their versions.
Asking installs nothing.

## Remotes

A battery remote is updated the same way, with a button press. It sleeps, and a message for it waits
at its parent for only a few seconds, so the coordinator sends the notify again whenever it hears
from the remote over the next ten minutes, which means whenever one of its buttons is pressed.
*Check for updates* (*Ask it*) waits for that press. After *Install*, press a button once more. The
press also does whatever that button is set up to do.

A remote that never answers either has no OTA client or ignores the notify. It may still ask by
itself, typically once a day, and is then served if it is the named device. Two known cases: an IKEA
BILRESA receives the notify and does not ask (it can still be updated when it checks by itself), and
an older Aqara wireless switch never picks the notify up.

## When a device does not update

- `ota` in `/api/devices` is an object for every Zigbee device, with each field `null` until the
  device has reported it since the coordinator started.
- `GET /api/status` has `zigbee.ota`: `state` (`no_partition`, `idle`, `stored`, `ready`, `writing`,
  `serving`, `stalled`, `finished`, `failed`), the `image`, the `target`, the `transfer` with its
  `offset` and `size`, and `last_query` with the answer the last asking device got. Look there
  first.
- For a remote, `waiting` lists what it is being asked, and `notifies_delivered` /
  `notifies_undelivered` tell a remote that got the question and ignored it apart from one that
  never picked it up.

## What is checked

The page is plain HTTP, where browsers withhold WebCrypto, so the index's `sha512` is not checked.
The download is checked against the index entry's size, and the OTA header against its manufacturer
code, image type and version. The device checks the image itself before it switches to it.
