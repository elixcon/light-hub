# Web UI

The coordinator's page, built with Svelte 5, shadcn-svelte and Tailwind 4 into
**one file**: `npm run build` writes `../main/www/index.html`, which
`main/CMakeLists.txt` gzips (zopfli) and embeds in the firmware.
The built file is committed, so `idf.py build` and `make -C test` never need Node.

```sh
npm install            # once
npm run build          # -> ../main/www/index.html, then the size gate
npm run dev            # HMR against http://zigbee.local (ZBC_TARGET=http://host:port to point elsewhere)
npm run dev:mock       # HMR against mock/server.mjs - a stand-in coordinator with lamps, a relay and a remote
npm run check          # svelte-check
npm test               # the math ports (pchip, switch points, the colour path), lamp card colours, class clashes, the QR code and Zigbee OTA matching
```

**Rebuild and commit `main/www/index.html` with every UI change.** The source
of truth for what ships is that file; `web/` is how it is made. Tailwind also
picks up class-like words from comments, so even a comment edit can change the
build.

## The size gate

The page is embedded in the coordinator image, which has to fit its partition.
`scripts/size-check.sh` runs after every build and fails when the gzipped page is
over 150 KB - the same `zopfli` CMake uses, so the number is the one that lands
in flash. bits-ui is not used: its five components were a fifth of the page.
Check the gate's number before adding any component library back for one widget.

## Local edits to generated shadcn-svelte components

`src/lib/components/ui/*` came from `npx shadcn-svelte@latest add ...` and were
then edited. Re-running `add` over them would undo this:

- `utils.ts`: `cn` is `clsx`, not `clsx` + `tailwind-merge` (7 KB). So nothing
  drops a class another overrides: a component's base, variant and size, and a
  caller's `class`, must never set the same property, or the one Tailwind emits
  later wins. `test/classes.test.ts` checks Button, Badge, Card, Input and
  Switch and every place they are used. A variant (`hover:`, `data-new:`)
  outranks a plain class by rule, so that is the way to override on purpose;
  otherwise wrap the component (a fixed-width `<div>` around an `<Input>`).
- `button.svelte`, `badge.svelte`: `tailwind-variants` replaced by plain class maps.
- `slider`, `switch`, `progress`, `label`, `dialog`: native elements behind
  shadcn's props, with no bits-ui underneath - a range input, a `role="switch"`
  button, `<dialog>`. `add` for any of them brings bits-ui back.
  - The slider handles touch itself, because iOS moves a range input only when the
    thumb is dragged; its fill is painted through `--range`, and both controls are
    sized for a finger.
  - The dialog is controlled only (`open` plus `onOpenChange`), and has no Portal,
    Overlay, Trigger or Close part.

## Where things are

- `src/lib/state.svelte.ts` - the poll, the optimistic `pending` map, `sendCommand()`
  (one command on the wire at a time), `sendThrottled()`, `cancelThrottled()` and
  `offEpoch`. This is the behaviour CLAUDE.md's "State is never assumed" and
  "Commands go through a per-endpoint gate" sections describe.
- `src/lib/pchip.ts`, `curves.ts`, `colorspace.ts` - mirrors of `zbc_pchip`,
  `chroma_eval` and `colorspace.c`. The C is the authority; do not simplify one side
  without the other. `test/curves.test.ts` holds the invariants.
- `src/lib/adaptive.ts` - mirror of Adaptive Lighting's day (`zbc_sun_phase`,
  `zbc_sun_stretch`, `al_shape`) for the Settings page's "today" chart; `tz.ts`
  writes the browser's time zone as the POSIX rule `main/tz.c` reads. The C is the
  authority. `test/adaptive.test.ts` and `test/tz.test.ts` check the same numbers as
  `test/test_sun.c`, `test_alstate.c` and `test_tz.c`.
- `src/lib/remotes.ts` - the known remotes, and the per-key button records put
  back together into one card per remote.
- `src/lib/zbota.ts`, `zbota.svelte.ts` - Zigbee device firmware: matching the
  Zigbee2MQTT index, downloading, unwrapping and uploading.
- `src/lib/qr.ts` - the Matter pairing QR code; `test/qr.test.ts` checks it
  against vectors generated with Python `segno`.
- `src/lib/router.svelte.ts` - hash routes (`#/groups`, `#/lights/{ieee}/{ep}`, ...).
  The coordinator serves one HTML route; the fragment never reaches it.
- `src/lib/components/StaggeredMenu.svelte` - the menu, CSS transitions only.
- `src/views/*` - one file per page.
