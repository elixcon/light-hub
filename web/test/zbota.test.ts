import { test } from "node:test";
import assert from "node:assert/strict";
import { askedSince, entryApplies, newestFor, findOtaFile, trimOta, checkAgainstEntry, versionText,
  type IndexEntry, type MatchInput } from "../src/lib/zbota.ts";

const entry = (over: Partial<IndexEntry> = {}): IndexEntry => ({
  fileName: "f.ota", fileVersion: 0x03197310, fileSize: 1000, url: "https://x/f.ota",
  imageType: 0x00a0, manufacturerCode: 0x1189, ...over,
});
const device = (over: Partial<MatchInput["ota"]> = {}, model = "PAR16 RGBW", manufacturer = "LEDVANCE"): MatchInput => ({
  ota: { manufacturer_code: 0x1189, image_type: 0x00a0, file_version: 0x03000000, hw_version: null, ...over },
  model, manufacturer,
});

test("an image applies only to the same manufacturer and image type, and only when newer", () => {
  assert.equal(entryApplies(entry(), device()), true);
  assert.equal(entryApplies(entry({ manufacturerCode: 0x1188 }), device()), false);
  assert.equal(entryApplies(entry({ imageType: 0x00a1 }), device()), false);
  assert.equal(entryApplies(entry(), device({ file_version: 0x03197310 })), false, "the same version");
  assert.equal(entryApplies(entry(), device({ file_version: 0x03197311 })), false, "an older image");
});

test("versions compare unsigned", () => {
  assert.equal(entryApplies(entry({ fileVersion: 0x90000000 }), device({ file_version: 0x7fffffff })), true);
  assert.equal(entryApplies(entry({ fileVersion: 0x7fffffff }), device({ file_version: 0x90000000 })), false);
  assert.equal(versionText(0x90000001), "0x90000001");
  assert.equal(versionText(null), "unknown");
});

test("nothing applies to a device that has not said what it runs", () => {
  assert.equal(entryApplies(entry(), device({ manufacturer_code: null })), false);
  assert.equal(entryApplies(entry(), device({ image_type: null })), false);
  assert.equal(entryApplies(entry(), device({ file_version: null })), false);
});

test("a model id, when the entry has one, must be the device's", () => {
  assert.equal(entryApplies(entry({ modelId: "PAR16 RGBW" }), device()), true);
  assert.equal(entryApplies(entry({ modelId: "lumi.plug" }), device()), false);
  assert.equal(entryApplies(entry({ modelId: "PAR16 RGBW" }), device({}, "")), false, "model not known");
});

test("a manufacturer name list, when the entry has one, must contain the device's", () => {
  assert.equal(entryApplies(entry({ manufacturerName: ["OSRAM", "LEDVANCE"] }), device()), true);
  assert.equal(entryApplies(entry({ manufacturerName: ["OSRAM"] }), device()), false);
});

test("hardware version ranges need the device's hardware version", () => {
  const e = entry({ hardwareVersionMin: 2, hardwareVersionMax: 4 });
  assert.equal(entryApplies(e, device({ hw_version: null })), false);
  assert.equal(entryApplies(e, device({ hw_version: 1 })), false);
  assert.equal(entryApplies(e, device({ hw_version: 2 })), true);
  assert.equal(entryApplies(e, device({ hw_version: 4 })), true);
  assert.equal(entryApplies(e, device({ hw_version: 5 })), false);
  assert.equal(entryApplies(entry({ hardwareVersionMax: 3 }), device({ hw_version: 3 })), true);
});

test("current version ranges are honoured", () => {
  const e = entry({ minFileVersion: 0x03000000, maxFileVersion: 0x03100000 });
  assert.equal(entryApplies(e, device({ file_version: 0x02ffffff })), false);
  assert.equal(entryApplies(e, device({ file_version: 0x03000000 })), true);
  assert.equal(entryApplies(e, device({ file_version: 0x03100000 })), true);
  assert.equal(entryApplies(e, device({ file_version: 0x03100001 })), false);
});

test("the newest applicable entry wins", () => {
  const list = [entry({ fileVersion: 0x03100000 }), entry({ fileVersion: 0x03197310 }),
    entry({ fileVersion: 0x04000000, modelId: "other" }), entry({ fileVersion: 0x03150000 })];
  assert.equal(newestFor(list, device())?.fileVersion, 0x03197310);
  assert.equal(newestFor(list, device({ file_version: 0x03197310 })), null);
});

function otaFile(size: number, version = 0x03197310, str = "A60S_RGBW"): Uint8Array {
  const b = new Uint8Array(size);
  const v = new DataView(b.buffer);
  v.setUint32(0, 0x0beef11e, true);
  v.setUint16(4, 0x0100, true);
  v.setUint16(6, 56, true);
  v.setUint16(10, 0x1189, true);
  v.setUint16(12, 0x00a0, true);
  v.setUint32(14, version, true);
  for (let i = 0; i < str.length; i++) b[20 + i] = str.charCodeAt(i);
  v.setUint32(52, size, true);
  for (let i = 56; i < size; i++) b[i] = i & 0xff;
  return b;
}

test("a plain OTA file is found at offset 0 and kept whole", () => {
  const f = otaFile(500);
  const { file, header } = trimOta(f);
  assert.equal(header.offset, 0);
  assert.equal(file.byteLength, 500);
  assert.equal(header.headerString, "A60S_RGBW");
  assert.equal(header.fileVersion, 0x03197310);
});

test("a wrapper in front and a tail behind are cut off", () => {
  const inner = otaFile(300);
  const wrapped = new Uint8Array(100 + 300 + 64);
  wrapped.set([0x4e, 0x47, 0x49, 0x53], 0);        /* "NGIS" */
  wrapped.set(inner, 100);
  wrapped.fill(0xee, 400);
  const { file, header } = trimOta(wrapped);
  assert.equal(header.offset, 100);
  assert.deepEqual(Array.from(file), Array.from(inner));
});

test("a false magic number inside a wrapper is skipped", () => {
  const inner = otaFile(200);
  const wrapped = new Uint8Array(40 + 200);
  new DataView(wrapped.buffer).setUint32(8, 0x0beef11e, true);   /* magic, but header version 0 */
  wrapped.set(inner, 40);
  assert.equal(findOtaFile(wrapped).offset, 40);
});

test("a truncated download and a file without a header are refused", () => {
  const f = otaFile(500).subarray(0, 499);
  assert.throws(() => findOtaFile(f), /shorter than its OTA header/);
  assert.throws(() => findOtaFile(new Uint8Array(1000)), /no Zigbee OTA file/);
  assert.throws(() => findOtaFile(new Uint8Array(10)), /no Zigbee OTA file/);
});

test("the downloaded header must agree with the index entry", () => {
  const h = findOtaFile(otaFile(100));
  assert.equal(checkAgainstEntry(h, entry()), null);
  assert.match(checkAgainstEntry(h, entry({ fileVersion: 1 })) ?? "", /version/);
  assert.match(checkAgainstEntry(h, entry({ imageType: 1 })) ?? "", /image type/);
  assert.match(checkAgainstEntry(h, entry({ manufacturerCode: 1 })) ?? "", /manufacturer/);
});

test("an answer counts only when it came after the ask", () => {
  const asked = 1_000_000;
  assert.equal(askedSince(null, asked, asked + 30_000), false, "never asked");
  assert.equal(askedSince(undefined, asked, asked + 30_000), false, "no record");
  assert.equal(askedSince(5, asked, asked + 30_000), true, "asked 25 s after");
  assert.equal(askedSince(31, asked, asked + 30_000), true, "rounding slack");
  assert.equal(askedSince(40, asked, asked + 30_000), false, "an answer from before the ask");
  assert.equal(askedSince(3600, asked, asked + 1_500), false, "yesterday's query");
});
