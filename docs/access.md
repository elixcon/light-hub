# Access

Out of the box, anyone on your network can use the coordinator, and a banner on the page says so.
Press *Secure it* and this browser is given a key. The key is shown once, in groups of five like
`K7M2Q-9XH4T-R3WZP-6NBVA`, so write it down. From then on:

- This browser stays signed in, with the key as its cookie.
- Another device shows a lock screen. Type the key in (case, spaces and dashes do not matter), or
  use the failsafe below.
- Scripts send a key in a header. Make one key per script under *Settings → Access*:

  ```sh
  curl -H 'Authorization: Bearer K7M2Q-9XH4T-R3WZP-6NBVA' http://zigbee.local/api/devices
  ```

<img src="images/access-key.jpg" width="320" alt="The key a browser is given when the coordinator is secured, shown once">

*Settings → Access* lists every key with when it was last used, and revokes keys. Revoke the last
one and the coordinator is open to everyone again. The Wi-Fi setup page never asks for a key, and a
full factory reset erases the keys.

The lock keeps out the rest of the household network. It does not protect against someone reading
the traffic: the page is plain HTTP, so a key crosses the LAN unencrypted. The device stores only a
fingerprint of each key.

## Lost every key

Press one of the coordinator's buttons five times, about once a second (each press 0.5-2.5 s after
the last). The BOOT button on the board always works, and so do wired buttons and the on/off button
of a paired remote. For the next two minutes anyone on the network can get in, and a lock screen
that is already open unlocks by itself. The window closes as soon as a browser takes a key. Whatever
that button normally switches will switch too.

The updater does not read buttons. If every key is lost while a locked coordinator is running the
updater, only USB gets you back in.

## A network you cannot reach

If the coordinator is on a Wi-Fi network you can no longer reach, keep pressing at the same pace up
to ten. The coordinator restarts into its setup hotspot (`elixcon-XXXX`) with the stored Wi-Fi kept.
Pick a new network there, or do nothing: it goes back to the old one after 10 minutes, longer while
a phone is still connected, but never more than 30. A power cut also brings it back onto the stored
network.
