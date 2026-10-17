import { test } from "node:test";
import assert from "node:assert/strict";
import { rankChannels, verdict, overlaps, spillDb, wifiBand, median, zigbeeMhz, FREE_MARGIN_DB,
  type WifiAp, type EnergyReading } from "../src/lib/channels.ts";

/* A quiet band as the dev board measured it, four scans a channel. */
const quiet = (over: Record<number, number[]> = {}): EnergyReading[] =>
  Array.from({ length: 16 }, (_, i) => ({ channel: 11 + i, dbm: over[11 + i] ?? [-90, -90, -90, -90] }));
const best = (aps: WifiAp[], extra: Partial<Parameters<typeof rankChannels>[0]> = {}) =>
  rankChannels({ aps, ...extra });

test("frequencies", () => {
  assert.equal(zigbeeMhz(11), 2405);
  assert.equal(zigbeeMhz(26), 2480);
  assert.deepEqual(wifiBand({ channel: 6 }), { centre: 2437, half: 10 });
  assert.deepEqual(wifiBand({ channel: 1, second: "above" }), { centre: 2422, half: 20 });
  assert.deepEqual(wifiBand({ channel: 13, second: "below" }), { centre: 2462, half: 20 });
  assert.equal(wifiBand({ channel: 14 }).centre, 2484);
});

test("Wi-Fi 6 is inside 16-19 and nothing else", () => {
  const inside = [...Array(16).keys()].map((i) => 11 + i).filter((n) => overlaps(n, { channel: 6 }));
  assert.deepEqual(inside, [16, 17, 18, 19]);
});

test("a 40 MHz network covers its second half", () => {
  const inside = [...Array(16).keys()].map((i) => 11 + i).filter((n) => overlaps(n, { channel: 1, second: "above" }));
  assert.deepEqual(inside, [11, 12, 13, 14, 15, 16, 17, 18]);
  /* The same network as 20 MHz reaches 14 at most. */
  assert.ok(!overlaps(15, { channel: 1 }));
});

test("the spill falls off with distance", () => {
  const b = wifiBand({ channel: 6 });
  assert.equal(spillDb(2437, b), 0);
  assert.equal(spillDb(2448, b), -20);
  assert.ok((spillDb(2455, b) as number) < -20);
  assert.equal(spillDb(2470, b), null);
});

test("our own network roaming between 6 and 13 leaves 11-14 and 20-22", () => {
  const aps: WifiAp[] = [{ ssid: "home", rssi: -60, channel: 6 }, { ssid: "home", rssi: -70, channel: 13 }];
  const r = best(aps, { ownSsid: "home" });
  const free = r.filter((c) => c.free).map((c) => c.channel).sort((a, b) => a - b);
  assert.deepEqual(free, [11, 12, 13, 14, 15, 20, 21, 22]);
  for (const c of r.filter((c) => [16, 17, 18, 19, 23, 24, 25, 26].includes(c.channel))) {
    assert.ok(c.ownWifi.length > 0, `channel ${c.channel} is our own Wi-Fi`);
    assert.equal(c.tier, 2);
  }
  /* Preferred and free first: 11, 15, 20 in some order, then the others. */
  assert.deepEqual(r.slice(0, 3).map((c) => c.channel).sort((a, b) => a - b), [11, 15, 20]);
});

test("the station's own channel counts when the scan missed it", () => {
  const r = best([], { ownSsid: "home", ownChannel: 1 });
  assert.deepEqual(r.find((c) => c.channel === 11)?.ownWifi, [1]);
  assert.ok(!r.find((c) => c.channel === 11)?.free);
});

test("somebody else's strong network next door takes the channel", () => {
  const r = best([{ ssid: "neighbour", rssi: -45, channel: 1 }]);
  const c15 = r.find((c) => c.channel === 15)!;
  assert.ok(!c15.free);
  assert.equal(c15.aps[0].ssid, "neighbour");
  /* Far away it does not. */
  assert.ok(r.find((c) => c.channel === 25)!.free);
});

test("a free preferred channel beats a slightly quieter other one, not a much quieter one", () => {
  const near = rankChannels({ aps: [], energy: quiet({ 14: [-92, -92, -92, -92], 15: [-90, -90, -90, -90] }) });
  const at = (n: number) => near.findIndex((c) => c.channel === n);
  assert.ok(near[0].preferred);
  assert.ok(at(15) < at(14), "15 ahead of the 2 dB quieter 14");
  const far = rankChannels({
    aps: [],
    energy: quiet(Object.fromEntries([...Array(16).keys()].map((i) => [11 + i, i + 11 === 14 ? [-96, -96, -96, -96] : [-90, -90, -90, -90]]))),
  });
  assert.ok(6 > FREE_MARGIN_DB);
  assert.equal(far[0].channel, 14);
  assert.ok(!far.find((c) => c.channel === 15)!.free);
});

test("one burst does not take a channel, a busy one does", () => {
  const r = rankChannels({ aps: [], energy: quiet({ 11: [-91, -52, -91, -91], 15: [-70, -72, -68, -75] }) });
  const c11 = r.find((c) => c.channel === 11)!, c15 = r.find((c) => c.channel === 15)!;
  assert.equal(c11.medianDbm, -91);
  assert.equal(c11.maxDbm, -52);
  assert.ok(c11.free);
  assert.ok(!c15.free);
});

test("26 goes last among the free", () => {
  const r = rankChannels({ aps: [], energy: quiet({ 26: [-95, -95, -95, -95] }) });
  const free = r.filter((c) => c.tier === 1);
  assert.equal(free[free.length - 1].channel, 26);
});

test("a tie keeps the channel in use, then other networks count", () => {
  const r = rankChannels({ aps: [], energy: quiet(), current: 20 });
  assert.equal(r[0].channel, 20);
  const r2 = rankChannels({ aps: [], energy: quiet(), networks: [{ channel: 11, pan_id: 1, ext_pan_id: "x", permit_join: false }] });
  assert.equal(r2[0].channel, 15);
});

test("median", () => {
  assert.equal(median([]), null);
  assert.equal(median([-90]), -90);
  assert.equal(median([-89, -88, -85, -62]), -86.5);
  assert.equal(median([-3, -1, -2]), -2);
});

test("verdicts", () => {
  const own: WifiAp[] = [{ ssid: "home", rssi: -60, channel: 6 }];
  const r = best(own, { ownSsid: "home" });
  assert.match(verdict(r), /^Channel (11|15|20|25) is free and is one of the preferred channels/);
  assert.match(verdict(r, r[0].channel), /^You are already on the best channel/);

  /* Wi-Fi on 1, 4, 9 and 13 covers every preferred channel. */
  const all: WifiAp[] = [1, 4, 9, 13].map((ch) => ({ ssid: "home", rssi: -60, channel: ch }));
  const r2 = best(all, { ownSsid: "home" });
  assert.ok(r2.filter((c) => c.preferred).every((c) => !c.free));
  const v = verdict(r2);
  assert.match(v, /^None of the preferred channels \(11, 15, 20, 25\) is free - the best of them, \d+, is inside your Wi-Fi/);
  assert.match(v, /Channel \d+ is the quietest/);

  /* Nothing free at all. */
  const r3 = best([3, 8, 12].map((ch) => ({ ssid: "home", rssi: -50, channel: ch, second: "above" as const })), { ownSsid: "home" });
  if (!r3[0].free) assert.match(verdict(r3), /^No channel is free/);
});
