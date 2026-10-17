/* Updating a Zigbee device: find its image in the Zigbee2MQTT index, download
 * it here in the browser, strip any wrapper, upload the plain file to the
 * coordinator, and ask the device to fetch it. The device then pulls it block by
 * block over Zigbee, which takes minutes - that part is the coordinator's, and
 * its progress comes back in every poll as status.zigbee.ota.
 *
 * GitHub answers raw.githubusercontent.com with Access-Control-Allow-Origin: *,
 * which is what makes this possible from a page served by the coordinator. The
 * page is plain HTTP, so WebCrypto is not available to check the index's
 * sha512; the download is checked against the entry's size and the OTA header
 * against its manufacturer, image type and version instead.
 *
 * Module state, so leaving the page and coming back keeps the bar. */
import { toast } from "./toast.svelte.ts";
import { app, data, refresh, setPaused } from "./state.svelte.ts";
import { post, del } from "./api.ts";
import { INDEX_URL, askedSince, checkAgainstEntry, kb, newestFor, trimOta, versionText, type IndexEntry } from "./zbota.ts";
import type { Device } from "./types.ts";

export const zbota = $state({
  busy: false,
  ieee: null as string | null,        /* the device the bar belongs to */
  fraction: null as number | null,    /* null = indeterminate */
  message: "",
  asking: null as string | null,      /* a sleepy device being waited on to wake and answer */
});

let index: IndexEntry[] | null = null;
let indexLoading: Promise<IndexEntry[]> | null = null;

/* 567 KB, fetched once per page load and only when somebody asks. */
export function loadIndex(): Promise<IndexEntry[]> {
  if (index) return Promise.resolve(index);
  indexLoading ??= fetch(INDEX_URL, { cache: "no-cache" })
    .then(async (r) => {
      if (!r.ok) throw new Error(`the firmware index answered ${r.status}`);
      index = (await r.json()) as IndexEntry[];
      return index;
    })
    .catch((err) => {
      indexLoading = null;
      throw new Error(err instanceof TypeError ? "could not reach GitHub for the firmware index" : err.message);
    });
  return indexLoading;
}

export type UpdateCheck =
  | { kind: "unknown" }                       /* the device has not said what it runs */
  | { kind: "no_answer" }                     /* a remote was asked and did not wake up to answer */
  | { kind: "current" }
  | { kind: "available"; entry: IndexEntry; fits: boolean };

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

/* How long a remote is given to be woken. The coordinator holds the question
 * for ten minutes, but somebody looking at a spinner is not. */
const WAKE_WAIT_MS = 90_000;

/* Makes a device say what it runs now, and waits for the answer: what is on
 * record can be missing, or stale - a device that has just updated need not
 * rejoin. A lamp answers in seconds. A sleepy remote hears the question only
 * once it is woken, which is what `zbota.asking` tells the page to ask for.
 * Resolves with the device as the poll now has it, and whether it answered. */
export async function askDevice(d: Device): Promise<{ device: Device; answered: boolean }> {
  const askedAt = Date.now();
  await post("/api/zigbee/ota/ask", { ieee: d.ieee });
  const sleepy = isSleepy(d);
  if (sleepy) zbota.asking = d.ieee;
  try {
    const until = askedAt + (sleepy ? WAKE_WAIT_MS : 12_000);
    while (Date.now() < until) {
      await sleep(1500);
      await refresh();
      const now = data.devices.find((x) => x.ieee === d.ieee);
      if (now && askedSince(now.ota?.queried_s_ago, askedAt, Date.now())) return { device: now, answered: true };
    }
  } finally {
    if (zbota.asking === d.ieee) zbota.asking = null;
  }
  return { device: data.devices.find((x) => x.ieee === d.ieee) ?? d, answered: false };
}

export async function checkForUpdate(device: Device): Promise<UpdateCheck> {
  /* Ask first, or the version compared is whatever was on record. */
  const [{ device: d, answered }] = await Promise.all([askDevice(device), loadIndex()]);
  const o = d.ota;
  if (!o || o.manufacturer_code == null || o.image_type == null || o.file_version == null) {
    return { kind: !answered && isSleepy(d) ? "no_answer" : "unknown" };
  }
  const entry = newestFor(await loadIndex(), {
    ota: { manufacturer_code: o.manufacturer_code, image_type: o.image_type, file_version: o.file_version, hw_version: o.hw_version },
    manufacturer: d.manufacturer,
    model: d.model,
  });
  if (!entry) return { kind: "current" };
  const room = app.status?.zigbee.ota?.room ?? 0;
  return { kind: "available", entry, fits: room > 0 && entry.fileSize <= room };
}

const show = (ieee: string, fraction: number | null, message: string) => {
  zbota.ieee = ieee;
  zbota.fraction = fraction;
  zbota.message = message;
};

/* GitHub sends nothing for this long: give up rather than leave the page busy
 * with nothing on show. */
const DOWNLOAD_STALL_MS = 20_000;

async function download(entry: IndexEntry, ieee: string): Promise<Uint8Array> {
  show(ieee, null, `Downloading ${entry.fileName} from GitHub…`);
  const abort = new AbortController();
  let stalled = false;
  let timer = 0;
  const arm = () => {
    clearTimeout(timer);
    timer = window.setTimeout(() => { stalled = true; abort.abort(); }, DOWNLOAD_STALL_MS);
  };
  arm();
  try {
    const r = await fetch(entry.url, { signal: abort.signal });
    if (!r.ok || !r.body) throw new Error(`the image download answered ${r.status}`);
    const total = entry.fileSize;
    const buf = new Uint8Array(total + 4096);
    const reader = r.body.getReader();
    let got = 0;
    for (;;) {
      arm();
      const { done, value } = await reader.read();
      if (done) break;
      if (got + value.byteLength > buf.byteLength) throw new Error("the image is larger than the index says");
      buf.set(value, got);
      got += value.byteLength;
      show(ieee, (0.5 * got) / total, `Downloading ${entry.fileName}… ${Math.round((100 * got) / total)} %`);
    }
    if (got !== total) throw new Error(`the download has ${got} bytes, the index says ${total}`);
    return buf.subarray(0, got);
  } catch (err: any) {
    if (stalled) throw new Error(`GitHub sent nothing for ${DOWNLOAD_STALL_MS / 1000} s`);
    if (err instanceof TypeError) throw new Error("could not reach GitHub for the image");
    throw err;
  } finally {
    clearTimeout(timer);
  }
}

/* Resolves once the coordinator has stored and checked the file. The upload is
 * XMLHttpRequest for its progress events; the server answers one request at a
 * time, so the poll waits. */
function upload(file: Uint8Array, name: string, ieee: string, from: number): Promise<void> {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open("POST", "/api/zigbee/ota/image");
    xhr.upload.onprogress = (ev) => {
      if (!ev.lengthComputable) return;
      const f = ev.loaded / ev.total;
      show(ieee, from + (0.95 - from) * f, `Handing it to the coordinator… ${Math.round(100 * f)} %`);
    };
    xhr.onload = () => {
      if (xhr.status === 200) { resolve(); return; }
      let why = `the coordinator refused the image (${xhr.status})`;
      try { why = JSON.parse(xhr.responseText).error || why; } catch { /* not JSON */ }
      reject(new Error(why));
    };
    xhr.onerror = () => reject(new Error("the connection to the coordinator dropped during the upload"));
    const form = new FormData();
    form.append("file", new Blob([file as BlobPart], { type: "application/octet-stream" }), name);
    xhr.send(form);
  });
}

/* The stored image is already this one: no need to send it again. */
function alreadyStored(entry: { manufacturerCode: number; imageType: number; fileVersion: number }): boolean {
  const img = app.status?.zigbee.ota?.image;
  return !!img && img.manufacturer_code === entry.manufacturerCode && img.image_type === entry.imageType &&
    img.file_version >>> 0 === entry.fileVersion >>> 0;
}

async function run(d: Device, work: () => Promise<void>) {
  if (zbota.busy) { toast("Another firmware image is on its way already"); return; }
  zbota.busy = true;
  try {
    await work();
    show(d.ieee, 1, `Asking ${d.name} to fetch it…`);
    await post("/api/zigbee/ota/notify", { ieee: d.ieee });
    show(d.ieee, null, isSleepy(d)
      ? "Waiting for the remote to ask for it — press one of its buttons to wake it."
      : "Waiting for the device to ask for it…");
    toast(`Firmware offered to ${d.name}`);
  } catch (err: any) {
    show(d.ieee, 0, `Not installed: ${err.message}`);
    toast(`Not installed: ${err.message}`);
  } finally {
    zbota.busy = false;
    setPaused(false);
    refresh();
  }
}

/* A device whose endpoints are all remote keys sleeps. The coordinator sends
 * it the notify again whenever it is heard from in the next ten minutes, so a
 * press is what delivers it. The same test as device_listens in
 * zb_coordinator.c. */
export const isSleepy = (d: Device) => d.endpoints.length > 0 && d.endpoints.every((e) => e.features.includes("switch"));

export function installEntry(d: Device, entry: IndexEntry) {
  return run(d, async () => {
    if (alreadyStored(entry)) return;
    const raw = await download(entry, d.ieee);
    const { file, header } = trimOta(raw);
    const bad = checkAgainstEntry(header, entry);
    if (bad) throw new Error(bad);
    show(d.ieee, 0.5, "Handing it to the coordinator…");
    setPaused(true);
    await upload(file, entry.fileName, d.ieee, 0.5);
  });
}

/* A file from this computer. Checked against what the device says it runs, so
 * a lamp is not offered another model's firmware by a slip of the file picker. */
export function installFile(d: Device, picked: File) {
  return run(d, async () => {
    show(d.ieee, null, `Reading ${picked.name}…`);
    const { file, header } = trimOta(new Uint8Array(await picked.arrayBuffer()));
    const o = d.ota;
    if (o && o.manufacturer_code != null && o.manufacturer_code !== header.manufacturerCode) {
      throw new Error(`the file is for manufacturer 0x${header.manufacturerCode.toString(16)}, the device is 0x${o.manufacturer_code.toString(16)}`);
    }
    if (o && o.image_type != null && o.image_type !== header.imageType) {
      throw new Error(`the file is image type 0x${header.imageType.toString(16)}, the device wants 0x${o.image_type.toString(16)}`);
    }
    const room = app.status?.zigbee.ota?.room ?? 0;
    if (room && file.byteLength > room) throw new Error(`the image is ${kb(file.byteLength)}, the coordinator holds ${kb(room)}`);
    if (alreadyStored({ manufacturerCode: header.manufacturerCode, imageType: header.imageType, fileVersion: header.fileVersion })) return;
    setPaused(true);
    await upload(file, picked.name, d.ieee, 0.1);
  });
}

export async function askAgain(d: Device) {
  try {
    await post("/api/zigbee/ota/notify", { ieee: d.ieee });
    toast(isSleepy(d) ? `Asked ${d.name} again — press one of its buttons to wake it` : `Asked ${d.name} again`);
  }
  catch (err: any) { toast(err.message); }
  refresh();
}

export async function cancelTarget() {
  try { await post("/api/zigbee/ota/target", { ieee: null }); toast("No device will be given the image"); }
  catch (err: any) { toast(err.message); }
  zbota.ieee = null;
  refresh();
}

export async function askEveryone() {
  try { await post("/api/zigbee/ota/notify", {}); toast("Asked every listening device to check for firmware"); }
  catch (err: any) { toast(err.message); }
  refresh();
}

export async function removeImage() {
  try { await del("/api/zigbee/ota/image"); toast("Image removed"); }
  catch (err: any) { toast(err.message); }
  zbota.ieee = null;
  refresh();
}

/* What status.zigbee.ota says about one device, in words, or null when the
 * image has nothing to do with it. */
export function transferText(ieee: string, sleepy = false): { fraction: number | null; text: string; done: boolean } | null {
  const s = app.status?.zigbee.ota;
  if (!s) return null;
  const t = s.transfer;
  if (t && t.ieee === ieee) {
    const pct = t.size ? Math.floor((100 * t.offset) / t.size) : 0;
    if (s.state === "serving") return { fraction: t.size ? t.offset / t.size : null, text: `Sending ${versionText(t.to_version)} over Zigbee… ${pct} %`, done: false };
    if (s.state === "stalled") return { fraction: t.size ? t.offset / t.size : null, text: `Stalled at ${pct} % — the device has not asked for more in ${Math.floor(t.progress_s_ago / 60)} min.`, done: false };
    if (t.status === 0) {
      if (t.device_version != null && t.device_version >>> 0 === t.to_version >>> 0) {
        return { fraction: 1, text: `Updated: the device now runs ${versionText(t.to_version)}.`, done: true };
      }
      return { fraction: 1, text: `The device has ${versionText(t.to_version)} and restarts onto it. The coordinator asks it again over the next few minutes to confirm.`, done: true };
    }
    if (t.status !== null) return { fraction: 0, text: `The device stopped the update (status 0x${t.status.toString(16).padStart(2, "0")}). Its old firmware keeps running.`, done: true };
  }
  if (s.target === ieee && s.image) {
    return { fraction: null, done: false, text: sleepy
      ? `Waiting for the remote to ask for ${versionText(s.image.file_version)} — press one of its buttons to wake it. The offer is repeated each time it wakes for ten minutes.`
      : `Waiting for the device to ask for ${versionText(s.image.file_version)}…` };
  }
  return null;
}
