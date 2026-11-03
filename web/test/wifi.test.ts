import { test } from "node:test";
import assert from "node:assert/strict";
import { namedNetworks } from "../src/lib/wifi.ts";

const net = (ssid: string, rssi: number, channel = 1) => ({ ssid, rssi, channel, open: false });

test("a network with two access points is one choice, at its strongest", () => {
  const got = namedNetworks([net("Home", -70, 1), net("Other", -60, 6), net("Home", -40, 11)]);
  assert.deepEqual(got.map((n) => [n.ssid, n.rssi, n.channel]), [["Home", -40, 11], ["Other", -60, 6]]);
});

test("strongest first", () => {
  assert.deepEqual(namedNetworks([net("A", -80), net("B", -50), net("C", -65)]).map((n) => n.ssid), ["B", "C", "A"]);
});

test("a hidden network has no name to offer, however many there are", () => {
  assert.deepEqual(namedNetworks([net("", -30), net("", -35), net("A", -60)]).map((n) => n.ssid), ["A"]);
});

test("every name stays unique, which is what the list is keyed by", () => {
  const got = namedNetworks([net("A", -1), net("A", -2), net("B", -3), net("A", -4), net("B", -5)]);
  assert.equal(new Set(got.map((n) => n.ssid)).size, got.length);
});

test("the scan itself is left alone, since the channel survey reads it too", () => {
  const scan = [net("A", -80), net("A", -50)];
  namedNetworks(scan);
  assert.deepEqual(scan.map((n) => n.rssi), [-80, -50]);
});

test("nothing heard", () => {
  assert.deepEqual(namedNetworks([]), []);
});
