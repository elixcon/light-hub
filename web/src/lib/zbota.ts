/* Firmware for Zigbee devices, from the Zigbee2MQTT OTA index
 * (github.com/Koenkk/zigbee-OTA). The browser does the internet part: it reads
 * the index, picks the image for a device, downloads it and strips any wrapper,
 * and uploads a plain OTA file to the coordinator, which never talks to GitHub.
 *
 * Pure functions only, so `npm test` runs them without a browser. The flow that
 * uses them is in zbota.svelte.ts. */

export const INDEX_URL = "https://raw.githubusercontent.com/Koenkk/zigbee-OTA/master/index.json";

/* The fields this page uses; the index has a few more. */
export interface IndexEntry {
  fileName: string;
  fileVersion: number;
  fileSize: number;
  url: string;
  imageType: number;
  manufacturerCode: number;
  sha512?: string;
  otaHeaderString?: string;
  modelId?: string;
  manufacturerName?: string[];
  hardwareVersionMin?: number;
  hardwareVersionMax?: number;
  minFileVersion?: number;
  maxFileVersion?: number;
  releaseNotes?: string;
}

/* What a device told the coordinator (a device's `ota` in /api/devices), and
 * the Basic cluster strings it was interrogated for. */
export interface DeviceFirmware {
  manufacturer_code: number | null;
  image_type: number | null;
  file_version: number | null;
  hw_version: number | null;
}

export interface MatchInput {
  ota: DeviceFirmware;
  manufacturer: string;
  model: string;
}

export const OTA_MAGIC = 0x0beef11e;
export const OTA_HEADER_MIN = 56;

/* Zigbee file versions are opaque 32-bit numbers, and every vendor reads them
 * differently, so they are shown the way they are sent. */
export const versionText = (v: number | null | undefined) =>
  v == null ? "unknown" : `0x${(v >>> 0).toString(16).padStart(8, "0")}`;

/* Whether a device has asked for firmware since it was asked to, going by its
 * `queried_s_ago` - which the poll ages between answers, so a query made after
 * the ask is at most that many seconds old. Two seconds of slack for the whole
 * seconds both ends round to. */
export const askedSince = (queriedSAgo: number | null | undefined, askedAtMs: number, nowMs: number) =>
  queriedSAgo != null && queriedSAgo <= (nowMs - askedAtMs) / 1000 + 2;

/* Whether an index entry is firmware for this device, newer than what it runs.
 * The same rules as zigbee-herdsman-converters: manufacturer code and image type
 * must match, and where the entry names a model, a manufacturer name, a range of
 * hardware versions or of current firmware versions, those must match too.
 * Several Aqara models share one manufacturer code and image type, so an entry
 * with a model id never matches a device whose model is not known. */
export function entryApplies(e: IndexEntry, dev: MatchInput): boolean {
  const o = dev.ota;
  if (o.manufacturer_code == null || o.image_type == null || o.file_version == null) return false;
  if (e.manufacturerCode !== o.manufacturer_code || e.imageType !== o.image_type) return false;
  if (e.modelId !== undefined && e.modelId !== dev.model) return false;
  if (e.manufacturerName !== undefined && !e.manufacturerName.includes(dev.manufacturer)) return false;
  if (e.hardwareVersionMin !== undefined || e.hardwareVersionMax !== undefined) {
    /* An entry for some hardware revisions only: without the device's own
     * revision there is no telling, so no. */
    if (o.hw_version == null) return false;
    if (e.hardwareVersionMin !== undefined && o.hw_version < e.hardwareVersionMin) return false;
    if (e.hardwareVersionMax !== undefined && o.hw_version > e.hardwareVersionMax) return false;
  }
  const current = o.file_version >>> 0;
  if (e.minFileVersion !== undefined && current < e.minFileVersion) return false;
  if (e.maxFileVersion !== undefined && current > e.maxFileVersion) return false;
  return (e.fileVersion >>> 0) > current;
}

/* The newest image for a device, or null. */
export function newestFor(entries: IndexEntry[], dev: MatchInput): IndexEntry | null {
  let best: IndexEntry | null = null;
  for (const e of entries) {
    if (entryApplies(e, dev) && (!best || (e.fileVersion >>> 0) > (best.fileVersion >>> 0))) best = e;
  }
  return best;
}

export interface OtaHeader {
  offset: number;          /* where the OTA file starts in what was downloaded */
  headerLength: number;
  manufacturerCode: number;
  imageType: number;
  fileVersion: number;
  headerString: string;
  totalSize: number;
}

/* Where the OTA file is in a download, and what its header says. Most files are
 * the OTA file itself; IKEA's signed ones put a signature block ("NGIS") in
 * front, and the file proper is found by its magic number. Only the first 4 KiB
 * are searched - a wrapper is a header, not a container. Throws with the reason
 * when there is no usable file. */
export function findOtaFile(buf: Uint8Array): OtaHeader {
  const view = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  const limit = Math.min(buf.byteLength - OTA_HEADER_MIN, 4096);
  for (let off = 0; off <= limit; off++) {
    if (view.getUint32(off, true) !== OTA_MAGIC) continue;
    if (view.getUint16(off + 4, true) !== 0x0100) continue;
    const headerLength = view.getUint16(off + 6, true);
    const totalSize = view.getUint32(off + 52, true);
    if (headerLength < OTA_HEADER_MIN || totalSize < headerLength) continue;
    if (off + totalSize > buf.byteLength) {
      throw new Error(`the download is shorter than its OTA header says (${buf.byteLength - off} of ${totalSize} bytes)`);
    }
    let headerString = "";
    for (let i = 0; i < 32; i++) {
      const c = buf[off + 20 + i];
      if (c === 0) break;
      headerString += c >= 0x20 && c < 0x7f ? String.fromCharCode(c) : ".";
    }
    return {
      offset: off,
      headerLength,
      manufacturerCode: view.getUint16(off + 10, true),
      imageType: view.getUint16(off + 12, true),
      fileVersion: view.getUint32(off + 14, true),
      headerString,
      totalSize,
    };
  }
  throw new Error("no Zigbee OTA file in it (no 0x0BEEF11E header)");
}

/* The plain OTA file, exactly `totalSize` bytes: what the coordinator takes. */
export function trimOta(buf: Uint8Array): { file: Uint8Array; header: OtaHeader } {
  const header = findOtaFile(buf);
  return { file: buf.subarray(header.offset, header.offset + header.totalSize), header };
}

/* A downloaded image against the entry it was picked from, so a changed or
 * mislabelled file is caught before it reaches a lamp. */
export function checkAgainstEntry(h: OtaHeader, e: IndexEntry): string | null {
  if (h.manufacturerCode !== e.manufacturerCode) return "the file is for another manufacturer than the index says";
  if (h.imageType !== e.imageType) return "the file is for another image type than the index says";
  if (h.fileVersion >>> 0 !== e.fileVersion >>> 0) return "the file is another version than the index says";
  return null;
}

export const kb = (bytes: number) => `${Math.ceil(bytes / 1024)} KB`;
