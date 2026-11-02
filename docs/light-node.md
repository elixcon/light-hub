# Light Node

A Light Node is a second firmware for an ESP32-C6: a Zigbee end device that drives wired lights and
buttons, for a place a hub's wires do not reach. It joins a hub's network, and the hub treats its
lights as ordinary lamps and its buttons as a remote's keys. It has no rooms, curves, rules or Matter
of its own, because the hub has them.

## What it does

| | |
|---|---|
| Lights | Up to 3 wired lights: on/off, dimmable, or tunable white with two or three channels. The same PWM code as a hub's [wired lights](wired-lights.md), and the same overheat hold |
| Buttons | Up to 8 wired buttons or switches. Each is a key on the hub's Remotes page, set up with the hub's rules |
| Without the hub | A button can toggle one of the node's own lights while the hub cannot be reached |
| Wi-Fi | Off in normal running. It comes on only to configure the node |
| Radio | An end device whose receiver stays on, so a command reaches it at once |

The limit of 3 lights is the hub's: it stores four endpoints per device, and the buttons take one.

Only the node's firmware is its own. It uses the same partition table and the same updater as a hub.

## Power

Wi-Fi, the web server and mDNS do not start in normal running, there is no routing, and the CPU runs
at 80 MHz rather than 160 MHz. The receiver is always on, though, and it is most of what the chip draws.
Expect a saving against a hub rather than a low-power device: going further needs a sleepy end device,
whose lights answer only when it next polls. **None of this has been measured yet.**

## Installing

Build it, then flash it over USB like a hub:

```sh
idf.py -C node build
idf.py -C node -p /dev/cu.usbmodem* flash
```

Its release files are named `elixcon_light_node-<version>`. On a board that is already a hub, its stored
lights and Wi-Fi network are kept and its Zigbee network is forgotten, so the first boot starts unpaired.
The light table keeps its first 3 lights: **take a configuration backup first if the board has more.**

The updater refuses a hub's image on a node, or a node's on a hub, because each would start on the other's
stored network. To convert a running board, add `?variant=any` to the upload:

```sh
curl -H 'Expect:' --retry 6 --retry-delay 5 --retry-all-errors \
     -F firmware=@elixcon_light_node.bin 'http://<address>/update?variant=any'
```

An updater older than this one has no such check. Going back the other way, press **Leave network** on the
node's page first, or the hub starts on the node's stored network.

## First run

A new node has no Zigbee network, so it does two things at once:

1. It opens Wi-Fi: its stored network if it has one, otherwise its setup hotspot, whose page is the usual
   captive portal.
2. It tries to join a network about every 30 seconds.

Add your lights and buttons on its page. Then on the hub press **Add a light** and **Pair a light**. It joins
within half a minute, and Wi-Fi closes two minutes after the page was last used.

A node joins **any** Zigbee network that is open for pairing, so do not leave yours open near a neighbour's.

## Configuring it later

On the hub, open one of the node's lights, or the node under Remotes, and press **Open Wi-Fi to configure
it**. The node restarts onto Wi-Fi, which blinks its lights for about a second and brings them back as they
were, and the hub shows a link to its page. The modes are:

| Situation | Wi-Fi | Zigbee |
|---|---|---|
| Paired, normal | off | on |
| Asked by the hub, a network stored | on that network | on |
| Asked by the hub, no network stored | setup hotspot | off, since a listening receiver blocks hotspot clients |
| Unpaired | stored network or hotspot | looking for a network |
| A wired button pressed ten times, 0.5-2.5 s apart | setup hotspot | off |

Wi-Fi closes after 15 minutes without a request, or at once with **Close Wi-Fi now** on the page. Ten presses is
the way back in when the hub is gone. Five open the node's page to everyone for two minutes, as on a hub. Neither
does anything to the hub's own lock, which a node's buttons cannot open.

## Buttons and the hub

A press goes to the hub, whose rules decide what it means. On the hub's Remotes page the node is a remote, and
each button is a key, found by pressing it. A click goes out as the button comes up, and a press that follows within
0.3 s makes a double, as on a hub's own buttons. A key with double-press rules waits those 0.3 s before it acts on a
click, and one without acts at once and takes two quick taps as two presses.

A button can name one of the node's lights to toggle when the hub cannot be reached. Set it on the button's
page on the node. While the hub is out, the first press takes a few seconds (6.4 s measured) because the node
is still trying to reach it, and later presses are immediate (about 30 ms). When the hub comes back, the first
press still toggles at the node, which is how the node learns the hub is back, and later presses are the hub's
again. Only short presses and switch flips work this way. A hold does nothing while the hub is out.

## Updating it

Open Wi-Fi as above, then use **Update firmware** on its page, or curl the file to it like a hub. It restarts into
its updater and comes back on Wi-Fi, so the page sees the update finish. A new image that never gets onto Zigbee or
Wi-Fi rolls back to the updater, which stays reachable and takes another file.

## What is measured, and what is not

From a node on a test board, under a hub on the same desk:

- A press through a hub rule to the node's own light: 111 ms.
- PWM at the 80 MHz CPU: exact, 9765.6 Hz, as at 160 MHz.
- The hotspot while still looking for a network, with a bad stored network being retried as well: 144 requests in a row
  from a laptop, none failed, 27 ms typically, with a pause of 2-3 s about every 32 s while it looks for a network. A
  node with no stored network has only the looking to do, though that case was not tried on its own.
- Choosing a parent: after a restart a node asks for a strong one and steps down about every 6 dB, which takes about
  2.7 s a step. It chose the hub at -39 dBm over the plug at -47 dBm on all of 9 boots, joining 5.6-10.7 s after each;
  without this it landed on either. A first join has no such demand. `zigbee.parent` in `/api/status` shows the result.
- A quick double press on real switches: the hub heard the second press 117 and 207 ms after the first click.

Open:

- **Slow and lost commands come and go, and the cause is not known.** Through a TS011F plug, 32 % of 40 on/off commands
  had their acknowledgement delayed by about 1.6 s in one run, which holds that endpoint's next command back. In about a
  minute of real button use 2 commands went undelivered and 10 were slow, and by the hub's record one press changed
  nothing for about 11 s. Later the same evening, about 640 commands through that plug had none slow, however the node
  reported and with the hub's page polling at four times its rate. With the hub as the parent, 0 of 142 were slow, but
  not at the same time, so that does not prove the parent is the cause. Interference on the Zigbee channel would fit,
  and was not shown.
- **Asking a node to open Wi-Fi just after the hub restarts** was lost once, about 20 s after a restart: the hub answered
  `202`, the node stayed on Zigbee, and its Wi-Fi state stayed off. Asking again two minutes later worked. The cause has
  not been looked into.
- **Current draw** has not been measured.
- **A node's lights are not read again when it comes back.** Brightness changed on its own page while the hub could not
  hear it, as in its hotspot, stays unknown to the hub, whose next switch-on uses the level it last knew.
- **A node cannot forget its Wi-Fi network**, or be reset, from its page. So a node with no network stored was tried only
  through a bad stored network, which reaches the same hotspot and also retries that network.
- The overheat sensor cannot be set on the node's page yet. A board that was a hub keeps its setting.
