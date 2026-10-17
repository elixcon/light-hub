import { test } from "node:test";
import assert from "node:assert/strict";
import { parseBackup, blobRows, backupWarning, backupFilename } from "../src/lib/backup.ts";

/* A blob as config_backup.c writes it: the table's own header (u32 version,
 * u16 count, u16 reserved) and its rows. */
const blob = (version: number, count: number, rowBytes = 4) => {
  const b = Buffer.alloc(8 + count * rowBytes);
  b.writeUInt32LE(version, 0);
  b.writeUInt16LE(count, 4);
  return b.toString("base64");
};

const header = (extra: Record<string, unknown> = {}) => ({
  format: "elixcon-config", v: 1, firmware: "v1.2.3", name: "Basement", hostname: "zigbee",
  ieee: "0x404ccafffe42c504", pan_id: 24499, ext_pan_id: "404ccafffe42c504", created: 1760000000,
  omitted: [], ...extra,
});

const file = (entries: object[], hdr: object = header(), end: object | null = { end: true, entries: entries.length }) =>
  [hdr, ...entries, ...(end ? [end] : [])].map((o) => JSON.stringify(o)).join("\n") + "\n";

const FULL = [
  { key: "name", type: "str", value: "Basement" },
  { key: "trans_ds", type: "u16", value: 7 },
  { key: "wifi_ssid", type: "str", value: "Home" },
  { key: "wifi_pass", type: "str", value: "p\"w" },
  { key: "auth", type: "blob", value: blob(1, 2, 68) },
  { key: "devices", type: "blob", value: blob(2, 8, 256) },
  { key: "groups", type: "blob", value: blob(1, 3, 184) },
  { key: "buttons", type: "blob", value: blob(5, 1, 254) },
  { key: "curves", type: "blob", value: blob(2, 0, 112) },
];

test("a complete file is summarised", () => {
  const b = parseBackup(file(FULL));
  assert.equal(b.header.name, "Basement");
  assert.equal(b.ssid, "Home");
  assert.equal(b.keys, 2);
  /* Tables with rows, in the page's order and words; empty ones left out. */
  assert.deepEqual(b.counts, [["devices", 8], ["rooms", 3], ["button", 1]]);
  assert.match(b.text, /"end":true/);
});

test("CRLF line ends and blank lines are fine", () => {
  const text = file(FULL).replace(/\n/g, "\r\n") + "\r\n\r\n";
  assert.equal(parseBackup(text).keys, 2);
});

test("secrets left out are not mistaken for none", () => {
  const entries = FULL.filter((e) => !["wifi_ssid", "wifi_pass", "auth"].includes(e.key));
  const left = parseBackup(file(entries, header({ omitted: ["wifi", "auth"] })));
  assert.equal(left.ssid, null);
  assert.equal(left.keys, null, "keys left out read as 'none', which a restore would make true");
  const none = parseBackup(file(entries));
  assert.equal(none.keys, 0, "a coordinator with no keys");
});

test("a file cut short is refused", () => {
  assert.throws(() => parseBackup(file(FULL, header(), null)), /incomplete/);
  assert.throws(() => parseBackup(file(FULL, header(), { end: true, entries: FULL.length + 1 })), /incomplete/);
  const text = file(FULL);
  assert.throws(() => parseBackup(text.slice(0, text.length / 2)), /incomplete|damaged/);
  assert.throws(() => parseBackup(file([], header(), null)), /incomplete/);
});

test("anything else is refused with a reason", () => {
  assert.throws(() => parseBackup(""), /not a configuration file/);
  assert.throws(() => parseBackup("{}\n"), /not a configuration file/);
  assert.throws(() => parseBackup("PK\u0003\u0004 a zip"), /not a configuration file/);
  assert.throws(() => parseBackup(file(FULL, header({ v: 2 }))), /newer firmware/);
  assert.throws(() => parseBackup(file(FULL).replace('"trans_ds"', '"trans_ds')), /Line 3/);
});

test("row counts come from the blob header", () => {
  assert.equal(blobRows(blob(2, 31, 256)), 31);
  assert.equal(blobRows(blob(1, 300, 1)), 300);
  assert.equal(blobRows("AAAA"), null);
  assert.equal(blobRows("!!!"), null);
});

test("the warning names what the file will hold", () => {
  assert.match(backupWarning(true, true, true, 2)!, /Wi-Fi password.*access keys/);
  assert.match(backupWarning(true, false, true, 2)!, /Wi-Fi password/);
  assert.doesNotMatch(backupWarning(true, false, true, 2)!, /keys/);
  assert.match(backupWarning(false, true, true, 2)!, /access keys/);
  assert.equal(backupWarning(false, false, true, 2), null);
  /* Nothing to warn about that is not there. */
  assert.equal(backupWarning(true, true, false, 0), null);
  assert.match(backupWarning(false, true, true, null)!, /access keys/, "keys not known yet count as some");
});

test("the file is named for the coordinator and the day", () => {
  assert.equal(backupFilename("zigbee", new Date(2026, 9, 9, 23, 30)), "zigbee-config-2026-10-09.jsonl");
  assert.equal(backupFilename("", new Date(2026, 0, 2)), "coordinator-config-2026-01-02.jsonl");
});
