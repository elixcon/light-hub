# The WLED interface

The coordinator can pretend to be a WLED light strip. Hyperion.ng, HyperHDR and LedFx can then drive
one room's Hue colour lamps: a TV's ambilight, or lights that follow music. Each lamp is one pixel.

The lamps are not sent ordinary Zigbee commands, which manage a few per second at best. They get
Philips' own entertainment frames, the ones a Hue Bridge uses for Hue Sync. Each frame carries every
lamp's colour in one radio message, twenty times a second, and the lamps glide from one frame to the
next.

## Setting it up

1. *Settings → WLED interface*: switch it on and pick the room. The card lists the lamps that become
   pixels, in order, and the room's other lights. *Switch the other lights off* decides what happens to
   those while a stream runs (below).
2. In the client, add a WLED device at the coordinator's address (`zigbee.local`, or its IP). LedFx
   and HyperHDR also find it by themselves.
3. Set the client's LED count to the number of pixels on the card. Lay the LEDs out where the lamps
   stand around the screen.

| Client | Protocol | Notes |
| --- | --- | --- |
| Hyperion.ng | DDP (its default) | "Restore original state" decides what happens when it stops (below). |
| HyperHDR | WLED's UDP protocol (its default) | The same restore option. |
| LedFx | DDP (its default) | Found by mDNS. With no "live: false" it sends, a stream ends 2.5 s after its last frame. |
| WLED app (iOS, Android) | WebSocket `/ws` | Only in a build with `CONFIG_ZBC_WLED_WS` (off by default, 7.4 KB). Found by mDNS and shown online. Its power button switches the room. Its effects, colours and presets do nothing here, and its brightness slider only scales a stream. It shows what the room was doing at its last command, not live. |

**WLED has no keys.** While the interface is on, anything on your network can switch the chosen
room and stream to it, as with a real WLED. It is off by default, and nothing about it answers while
it is off.

The interface is a build option, `CONFIG_ZBC_WLED` (*Elixcon Light Hub* in `idf.py menuconfig`), on
by default. A build without it has no *WLED interface* card, opens no UDP port and has no `/json`
routes, and saves about 16 KB of flash. Its settings are kept, in flash and in a backup, for a build
that has it.

## What takes part

- **Hue colour lamps only, up to ten.** Philips' frames reach no other lamp. White-ambiance Hues
  cannot join a Hue entertainment area either.
- **The room's other lights** (wired lights, Matter lamps, other Zigbee lamps, an eleventh Hue) are
  left as they are, unless *Switch the other lights off* is on. Then a stream switches them off as it
  starts, with the fade and *Warm while off* like any other off, and keeps them off while it runs, so
  Adaptive Lighting cannot light them again. When it ends they come back on if they were on, at their
  brightness, or where Adaptive Lighting has the room now. Switching one on by hand while the stream
  runs keeps the stream going: that light is yours again, and the end of the stream leaves it alone.
  The setting is read when a stream starts.
- **The pixel order is the room's member order**, as listed on the card.

## How a stream behaves

- **It starts with the first frame.** It ends when the client says so (`"live": false`), or when no
  frame has arrived for the client's own timeout. That is 2.5 s for DDP, and whatever the client
  gives for WLED's protocol. A client that gives none is cut off after 60 s without frames.
- **At the end the room goes back to how it was.** Lamps that were dark are switched off, with *Warm
  while off* if that is on. Lamps that were lit come back at their brightness and colour, through
  their curves. In a room running Adaptive Lighting they come back where the mode has the room now.
  If the client ends with `"on": false`, the default for Hyperion and HyperHDR unless *restore
  original state* is set, the whole room is switched off instead, as a WLED would be.
- **The hand wins.** A switch, a remote, the page or a Matter controller used on the room, or on one
  of its streaming lamps, ends the stream at once and does what it was asked. Lights the stream
  switched off stay off then: a command to the room reaches them anyway, and one to a single lamp is
  not about them. The client is then
  ignored until it stops sending for its timeout. So pressing Off while the TV streams keeps the room
  off until the TV is switched off and on again.
- **Black is dark**, not the dimmest glow. The lamp keeps its colour while dark, so the next frame
  does not flash through white.
- **Adaptive Lighting keeps running** through a stream. It stays away from the streaming lamps, and
  from the lights the stream switched off, until the stream is over.

## What it costs

- **Remote presses.** While a client streams over Wi-Fi, the radio spends most of its time on Wi-Fi.
  Zigbee only gets to receive in the gaps. On the development board about one remote press in four
  was lost, at 15 and at 30 frames a second alike. Without a stream none were. Wall buttons wired to
  the coordinator, the page and Matter are not affected. A press that is lost can simply be repeated.
- **Memory.** About 5 KB from the first time the interface is switched on (a task and its two
  sockets), and about 5 KB more while a stream runs. That matters on a converted Shelly.
- **Airtime.** One frame per 50 ms to the first lamp, which repeats it for the others. It ran for 30
  minutes without a lost frame or a leak.

## Checking on it

`/api/status` has two members:

- `wled`: whether it is listening, the pixels, whether a stream is `live`, its `source` and
  `protocol`, frames in per second (`fps_in`), the other lights the stream holds off (`lights_off`),
  and how the last stream ended (`last_end`: `source`, `timeout`, `hand` or `failed`).
- `zigbee.ent`: the radio side. That is frames sent and refused, the start and stop messages
  delivered, and `balance` (0 when the lamps took the start).

The full list is in the [REST API](api.md#settings-and-system).
