# Matter

The coordinator works with Matter in two directions. As a bridge, it shows your rooms to Apple Home,
Home Assistant or Alexa. As a controller, it adds Matter lamps you already have and uses them like
Zigbee lamps.

## The bridge

The coordinator is a Matter-over-Wi-Fi bridge, and every room is one dimmable light on it, named
after the room. Switching and dimming from a controller go through the same path as the web page, so
response curves and fade times apply, and a change made on the page shows up in the controller
within a second or two.

1. Open *Settings → Matter*. It shows the pairing QR code and the 11-digit pairing code.
2. In the controller, add a Matter accessory and scan the code or type in the digits. Pairing runs
   over your network, not Bluetooth, so the phone has to be on the same Wi-Fi as the coordinator.
3. The controller warns that the accessory is uncertified, because it uses Matter's test
   certificates. Carry on anyway.
4. For a second ecosystem, share the bridge from the first one ("Turn On Pairing Mode" in Apple
   Home, *Share device* in Home Assistant) and use the code it gives you. That is the dependable way
   to add it to Alexa.

<img src="images/matter.jpg" width="320" alt="Settings → Matter: the controllers the bridge is in, and its pairing code and QR code">

The Matter page lists every controller the bridge is in. There is room for five, and Apple Home
takes two: one named after your home, and an unnamed one for Apple's own keychain. Both come from
the same pairing, so two entries after adding the bridge to Apple Home alone is expected. Once a
Matter lamp is paired (below), the coordinator's own fabric takes one more, which leaves four.

The pairing window is open for 15 minutes after a boot while nothing has been added yet. *Allow
pairing* opens it again. Rooms made or deleted later appear and disappear in the controllers by
themselves.

Each room also decides what a controller's "turn on" does: go back to the last brightness, start
[Adaptive Lighting](lighting.md#adaptive-lighting), or go to full. The last two apply to a room that
is already on as well, so "turn on" brings a room dimmed to 10 % to full or into the mode. "Set to
30 %" is not a turn on, and goes to 30 % in every case, though controllers send an On along with it.
How the controllers see Adaptive Lighting is set on the room's page too.

*Reset Matter* forgets every controller and leaves Zigbee lamps, wired lights, rooms and buttons
alone. Matter lamps are asked to forget the coordinator first and then have to be paired again. They
stay in their rooms, curves and buttons, marked *needs pairing again*.

`GET /api/matter` lists the last commands each controller sent, which is where to look when two
controllers behave differently with the same light.

### Devices on their own

A Zigbee light, remote or motion sensor can also be shared by itself, beside the rooms. Turn on
*Show in Matter on its own* on its page. It is off unless you turn it on, and the accessory appears
in the controllers, or disappears from them, within a few seconds.

- **A light** becomes the kind of Matter light it can be: on/off, dimmable, colour temperature or
  full colour, from the clusters the lamp has. Commands go through the same path as the light's own
  page, so its response curves and the fade time still apply. A colour set from a controller stays
  until the next brightness change. On a lamp with a warmth curve, that change sets the colour
  from the curve again. A light in a room is shared both ways: the room still switches all its
  lights, and the light alone switches only itself.
- **A remote** becomes one switch per button that is set up on its page. Buttons that are not set
  up are not shared. A controller can run automations on a single, a double and a long press of
  each. To tell a single press from a double, every single press of a shared remote then waits
  300 ms, its own rules on the coordinator included, as a button with a double-press rule already
  does. A triple press is not reported. The remote's own rules keep running.
- **A motion sensor** becomes an occupancy sensor. It has to be set up on its page first, because
  its no-motion time is what decides when it clears, for the controllers as for its own rules.

A device the coordinator has marked as gone shows as *No Response* in the controllers. For a light,
that also happens after ten minutes without a word from it. A remote or a sensor sleeps between
presses, so silence alone does not mark it.

A colour change that sweeps (a controller's colour loop, or a held colour-temperature button) is
not carried out. The light keeps its colour, and the controller's slider goes back to it.

What runs out first is memory. Every shared endpoint takes some for as long as it is shared: a
remote's button under 1 KB, a light 4 to 10 KB, and a colour light the most. Controllers need
plenty on top while they pair or subscribe. So the coordinator shares nothing new while it has less
than 48 KB free, and a few lights are about what a coordinator with several rooms has room for.
Beyond that there is room for 12 shared endpoints in all, a light or a sensor taking one and a
remote one per button (`CONFIG_ZBC_MAX_MATTER_EXPOSED`). The Matter page says when something asked
for is not shared, and why. A light that turns out to be able to do more than it could when it was shared, colour
discovered later for instance, gets it at the next restart, under the same identity.

## Matter lamps

The coordinator is also a Matter controller. A Matter lamp you already have in Apple Home, Home
Assistant or another app becomes a light here, like a Zigbee lamp: it goes into rooms, takes
response curves, fades, *Warm while off* and Adaptive Lighting, answers buttons and remotes, and is
shared back out through the bridge. It stays in its own app as well.

Lamps on Wi-Fi, lamps on Thread (reached through your border router, such as a HomePod) and hubs are
all added the same way. A hub such as a Hue Bridge brings every lamp behind it.

1. In Apple Home, open the lamp (or the hub), go to its settings and choose *Turn On Pairing Mode*.
   Other apps call this sharing or linking to another service. It shows a code that works for a few
   minutes.
2. On the *Lights* page choose *Add a Matter lamp* and type in the code. The coordinator finds the
   device on your network, pairs it and reads which of its endpoints are lights. On Wi-Fi this takes
   a few seconds.

Pairing is over the network only. The device's certificate is not checked, and the light's page says
so. A lamp added to a hub later appears after *Rescan device* on any of the hub's lamps.

Removing the last lamp of a device tells the device to forget the coordinator. If the device is
unplugged at the time, also remove the coordinator in the device's own app, or the entry stays in
the lamp's fabric table, which usually has room for five.

A Matter lamp's state comes from a subscription, one per device however many lamps it has, so a
change made in another app shows here at once. A device whose subscription has been down for 30 s
answers commands with `503`, and the coordinator tries it again at least once a minute until it
answers.

Re-pairing a device makes new lights, so its rooms and curves have to be set up again. A rescan adds
lamps that a hub has gained but does not remove ones it has dropped.
