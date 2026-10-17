/* Installs a plain ESP-IDF application image - what ESPHome calls
 * firmware.ota.bin.
 *
 * The coordinator cannot write the partition it runs from, so this is a
 * handover: ask it to restart into the updater, wait until the updater answers
 * on the same address (its /api/status says `updater: true`), then upload to
 * that. The upload is XMLHttpRequest rather than fetch, because only XHR
 * reports upload progress, and a 2 MB image over a link shared with the
 * 802.15.4 radio takes long enough that a bar is the difference between working
 * and hung. Module state, so navigating away from Settings and back does not
 * lose the bar. */
import { toast } from "./toast.svelte.ts";
import { refresh, setPaused } from "./state.svelte.ts";

export const ota = $state({
  busy: false,
  fraction: null as number | null,   /* null = indeterminate */
  message: "",
  shown: false,
});

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

async function inUpdater(): Promise<boolean> {
  try {
    const r = await fetch("/api/status", { cache: "no-store" });
    return r.ok && (await r.json()).updater === true;
  } catch {
    return false;   /* restarting */
  }
}

async function waitForUpdater(limitMs: number): Promise<boolean> {
  const until = Date.now() + limitMs;
  while (Date.now() < until) {
    await sleep(2000);
    if (await inUpdater()) return true;
  }
  return false;
}

export async function uploadFirmware(file: File) {
  const show = (fraction: number | null, message: string) => {
    ota.shown = true;
    ota.fraction = fraction;
    ota.message = message;
  };
  const stop = (message: string) => {
    ota.busy = false;
    show(0, message);
    toast(message);
    setPaused(false);
    refresh();
  };

  ota.busy = true;
  setPaused(true);

  if (!(await inUpdater())) {
    show(null, "Restarting into the updater…");
    try {
      const r = await fetch("/api/update/prepare", { method: "POST" });
      if (!r.ok) {
        const body = await r.json().catch(() => ({}));
        stop(body.error || `The coordinator refused the update (${r.status})`);
        return;
      }
    } catch {
      stop("The coordinator did not answer");
      return;
    }
    if (!(await waitForUpdater(90_000))) {
      stop("The updater did not answer. The coordinator starts again by itself within ten minutes.");
      return;
    }
  }

  show(0, `Uploading ${file.name}…`);
  const xhr = new XMLHttpRequest();
  xhr.open("POST", "/update");
  xhr.upload.onprogress = (ev) => {
    if (!ev.lengthComputable) return;
    show(ev.loaded / ev.total, `Uploading… ${Math.round((100 * ev.loaded) / ev.total)} %`);
  };
  /* Past this point the page is talking to the updater, which has none of the
   * coordinator's API - so a failure hands over to the updater's own page,
   * which says why it is running and takes another file. */
  const fail = (message: string) => {
    show(0, `${message} Opening the updater…`);
    toast(message);
    setTimeout(() => location.reload(), 4000);
  };
  xhr.onload = () => {
    if (xhr.status === 200 && xhr.responseText.startsWith("Update Successful")) {
      /* Indeterminate while the new firmware starts; refresh() reconnects on
       * its own and reloads the page once a different build answers. */
      show(null, "Installed — starting the new firmware.");
      toast("Firmware updated");
      ota.busy = false;
      setTimeout(() => { setPaused(false); refresh(); }, 12000);
      return;
    }
    fail(xhr.getResponseHeader("X-Ota-Error") || "The updater rejected that file.");
  };
  xhr.onerror = () => fail("The connection dropped during the upload.");
  xhr.onabort = () => fail("Upload cancelled.");

  const form = new FormData();
  form.append("file", file, file.name);
  xhr.send(form);
}

/* Replacing the updater itself, from the running coordinator - the way a box
 * gets room for Zigbee device firmware without a cable. The coordinator writes
 * elixcon_light_hub_updater.bin into the updater's partition and verifies it; nothing
 * restarts. A connection lost halfway leaves an updater that does not verify,
 * which the coordinator reports and refuses to hand an update to until it is
 * uploaded again. */
export function uploadUpdater(file: File): Promise<boolean> {
  const show = (fraction: number | null, message: string) => {
    ota.shown = true;
    ota.fraction = fraction;
    ota.message = message;
  };
  ota.busy = true;
  setPaused(true);
  show(0, `Uploading ${file.name}…`);
  return new Promise((resolve) => {
    const done = (ok: boolean, message: string) => {
      ota.busy = false;
      show(ok ? 1 : 0, message);
      toast(message);
      setPaused(false);
      refresh();
      resolve(ok);
    };
    const xhr = new XMLHttpRequest();
    xhr.open("POST", "/api/updater/image");
    xhr.upload.onprogress = (ev) => {
      if (ev.lengthComputable) show(ev.loaded / ev.total, `Writing the updater… ${Math.round((100 * ev.loaded) / ev.total)} %`);
    };
    xhr.onload = () => {
      if (xhr.status === 200) { done(true, "Updater replaced and verified"); return; }
      let why = `the coordinator refused it (${xhr.status})`;
      try { why = JSON.parse(xhr.responseText).error || why; } catch { /* not JSON */ }
      done(false, `Updater not replaced: ${why}`);
    };
    xhr.onerror = () => done(false, "The connection dropped while writing the updater. Upload it again.");
    const form = new FormData();
    form.append("file", file, file.name);
    xhr.send(form);
  });
}

/* Restarts through the updater and back, which is when the updater brings the
 * partition table up to date. The page reconnects by itself; this only waits
 * for the new layout to be reported. */
export async function runUpdater() {
  ota.busy = true;
  ota.shown = true;
  ota.fraction = null;
  ota.message = "Restarting through the updater…";
  try {
    const r = await fetch("/api/updater/run", { method: "POST" });
    if (!r.ok) {
      const body = await r.json().catch(() => ({}));
      throw new Error(body.error || `the coordinator refused (${r.status})`);
    }
  } catch (err: any) {
    ota.busy = false;
    ota.fraction = 0;
    ota.message = err.message;
    toast(err.message);
    return;
  }
  setPaused(true);
  await sleep(8000);
  const until = Date.now() + 180_000;
  while (Date.now() < until) {
    try {
      const r = await fetch("/api/status", { cache: "no-store" });
      if (r.ok) {
        const s = await r.json();
        if (s.updater !== true && s.ota?.layout) {
          ota.busy = false;
          ota.fraction = 1;
          ota.message = s.ota.layout === "current"
            ? "Back, with room for Zigbee device firmware."
            : "Back, but the partition table is unchanged — the updater may be too old. Upload this build's updater first.";
          toast(ota.message);
          setPaused(false);
          refresh();
          return;
        }
      }
    } catch { /* restarting */ }
    await sleep(2000);
  }
  ota.busy = false;
  ota.fraction = 0;
  ota.message = "The coordinator has not answered for three minutes.";
  setPaused(false);
  refresh();
}
