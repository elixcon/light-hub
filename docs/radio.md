# Radio and Wi-Fi

The ESP32-C6 shares a single 2.4 GHz radio and antenna between Wi-Fi and Zigbee (802.15.4). Most of
the surprises in this project come from that. When lamps misbehave, check the link before suspecting
anything else.

## Choosing the Zigbee channel

Check this before anything else. Zigbee channel *n* sits at `2405 + 5(n-11)` MHz, and a Wi-Fi
channel takes up its centre frequency plus or minus 10 MHz. If the two overlap, they fight over the
antenna, and the symptoms look like broken lamps: poor link quality at close range, commands that
arrive only sometimes, a light that switches and then switches back. The coordinator says so in its
log once it knows both numbers, and `/api/status` reports the Wi-Fi channel next to the Zigbee one.
`GET /api/wifi/scan` reports the channel of every network in range, so the choice can be made
against what is actually on the air.

| Wi-Fi channel | avoid Zigbee | good Zigbee |
|---|---|---|
| 1 (2412 MHz) | 11-14 | 15-26 |
| 6 (2437 MHz) | 16-19 | 11-14, 20-26 |
| 11 (2462 MHz) | 21-24 | 11-19, 25-26 |
| 13 (2472 MHz) | 23-26 | 11-22 |

The default channel, 20 (2450 MHz), suits Wi-Fi on channels 1-6 and 12-13 but collides with 7-10.

## Moving the network

*Settings → Zigbee network* moves a formed network to another channel and keeps every pairing (`POST
/api/settings` with `{"zigbee_channel":15}`). The coordinator tells the lamps, gives the message
about ten seconds to spread, and then follows. Lamps answer again within seconds.

Sleepy remotes do not hear the announcement. They find the network again when a press goes
unanswered, and one that does not (often an Aqara remote) has to be paired again. So does a lamp
that had no power during the move. The channel is remembered across restarts, and a Zigbee-only
factory reset forms the new network on it.

*Find the best channel* on the same page surveys the band in about 15 seconds: a Wi-Fi scan for the
networks around, and four energy scans and a beacon scan by the coordinator's own Zigbee radio
(`POST`, then poll `GET /api/zigbee/scan`). The page then ranks the channels. It leaves out every
channel inside your own Wi-Fi network's channels (all the ones it is seen on, since an access point
may roam) and every channel another network spills onto. It keeps channels whose measured energy is
within 5 dB of the quietest one, and lists the preferred channels 11, 15, 20 and 25 first when they
are free. Those are the Zigbee Light Link channels, where some devices find a network faster. While
the radio measures, remotes are not heard, and Wi-Fi is slow for a second at a time.

## Wi-Fi sleep

Zigbee only listens while Wi-Fi is not using the antenna. The arbiter gives ordinary 802.15.4
receive the lowest priority of anything, so with a Wi-Fi that never sleeps, Zigbee is deaf whenever
Wi-Fi is listening. A lamp survives that, because the coordinator retries what it sends to it. A
remote does not, because it sends each press once.

*Settings → Wi-Fi → Let Wi-Fi doze between beacons* (`wifi_sleep` in `/api/settings`, on by default)
hands the radio to Zigbee between the access point's beacons. The price is that the page answers a
few tens of milliseconds later. Compare `frames_missed` with it on and off before you turn it off.

## Link counters

`/api/status` has `zigbee.radio`, running totals since boot:

```
cmd_undelivered    the stack gave up getting a command to a lamp
read_undelivered   ...or a read
cmd_slow           a command did get there, but took over half a second: it
cmd_slow_ms        needed retries, and that lamp's next frame waited it out;
                   cmd_slow_ms is their time added up
went_quiet         a lamp stopped answering, so its state is unknown
corrections        a lamp was somewhere it had not been asked to be
gate_timeouts      a delivery confirmation never arrived at all
slots_short        more lamps were commanded at once than the coordinator has
                   slots for, so one went out unsequenced or lost its read-back
sends_skipped      commands and reads not sent to a device that has stopped
                   answering (an unplugged lamp gets one probe every 30 s)
frames_heard       frames devices sent of their own accord (reports, a remote's
                   presses) that arrived
frames_missed      ...and the sequence numbers between them that never did
table_entries      the coordinator's own neighbour table at its last walk, and
table_children     how many of those are attached to the coordinator itself; a
                   remote that is not among them is a router's child, and its
                   presses cross that router's link as well as its own
router_tables_asked, router_tables_answered, router_tables_unanswered
                   the routers' neighbour tables asked for to find each
                   remote's parent (one request per page), how many were
                   answered, and how many the stack gave up on unanswered
energy_scans, energy_scans_done, energy_scans_restarted, energy_scans_refused,
energy_scans_refused_coex, energy_scans_given_up
                   the radio's energy scans (forming, and the channel survey):
                   asked for, finished, started again, refused the antenna
                   (by the Wi-Fi arbiter), and reported unmeasured after 1.5 s
```

`frames_heard` and `frames_missed` are the receiving side, which none of the others cover, and it is
the side a remote lives on: a press is one frame, and the remote does not repeat it. They come from
the ZCL sequence number every frame carries. A device numbers what it sends, so a gap is a frame
lost on the air.

The same readings are given per device in `/api/devices`: `frames_heard`, `frames_missed`, `rssi`
(of the last frame heard, in dBm, so the last hop's) and `child`, which says whether the device is
attached to the coordinator itself. A sleepy remote never appears in the neighbour table, so these
are the only link readings it gets. Lamps also have `lqa`, from the neighbour table. It swings
widely (0-123) while traffic still gets through, so compare ranges, not single readings.

### Frame-level counters

`zigbee.radio.mac` is one level further down: frame by frame, as the radio saw them.

- `tx_done` counts frames sent (and acknowledged, where an ack was asked for).
- `tx_no_ack`, `tx_bad_ack`, `tx_coex` (Wi-Fi took the antenna mid-send), `tx_cca_busy` and
  `tx_other` count the ones that failed. The stack retries each of them.
- `rx` counts frames received.
- `rx_coex`, `rx_ack_coex` and `tx_ack_coex` are the radio's own count of Wi-Fi taking the antenna
  mid-way: while receiving or just listening (so `rx_coex` rises with Wi-Fi activity even when
  nothing was lost), while receiving the acknowledgement to a frame we sent, and while acknowledging
  a frame we received, which the sender then has to repeat.

The coordinator's sends bid for the antenna one step above its listening. In testing that took
`tx_coex` from over a third of sends to a few percent without slowing Wi-Fi. Listening stays at the
lowest priority, because raising it starves Wi-Fi.

All of these are counts, not rates. Sample twice and divide by the uptime in between. A rate that
climbs after moving a lamp, changing the channel or changing a radio setting is the measurement
worth having, and it does not need the serial port, which resets the board.

## Wi-Fi joining

If a wrong Wi-Fi password is saved from *Settings*, the coordinator reopens its setup hotspot after
a few failed attempts, so it cannot lock you out, and keeps trying the stored network from there
every minute. Once a join has failed twice, the serial log lists every network the radio can hear,
which tells a bad password apart from a network that is not there at all. An association that never
turns into a DHCP lease is retried after 20 seconds.

There is no country setting, on purpose. The radio scans channels 1-13 with the country left
unclaimed and 802.11d on, so the regulatory domain comes from the access point's own beacon once
associated, the way a phone behaves. ESP-IDF's default world safe mode scans only channels 1-11,
which makes an access point on channel 12 or 13 invisible: every join attempt would end in "no AP
found".

While the setup hotspot runs, Zigbee is stopped. Getting onto the network is the only thing that
matters then, and there are no lights to serve yet.
