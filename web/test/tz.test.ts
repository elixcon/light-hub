import { test } from "node:test";
import assert from "node:assert/strict";
import { browserPosixTz, offsetEast } from "../src/lib/tz.ts";

/* The rules the page writes for these zones. test/test_tz.c reads exactly
 * these strings on the firmware side and checks the instants either side of
 * every transition, so the two ends meet here. Node takes a TZ set at runtime. */
const ZONES: [string, string][] = [
  ["Europe/Berlin", "STD-1DST-2,M3.5.0/2,M10.5.0/3"],
  ["America/New_York", "STD5DST4,M3.2.0/2,M11.1.0/2"],
  ["Australia/Sydney", "STD-10DST-11,M10.1.0/2,M4.1.0/3"],
  ["Asia/Kolkata", "STD-5:30"],
  ["UTC", "STD0"],
];

test("each zone's rule is the one test_tz.c reads", () => {
  const was = process.env.TZ;
  try {
    for (const [zone, rule] of ZONES) {
      process.env.TZ = zone;
      assert.equal(browserPosixTz(Date.UTC(2026, 5, 1)), rule, zone);
      assert.equal(browserPosixTz(Date.UTC(2027, 0, 15)), rule, `${zone} the next year`);
    }
  } finally {
    if (was === undefined) delete process.env.TZ; else process.env.TZ = was;
  }
});

test("offsets are seconds east, as tz.c answers them", () => {
  const was = process.env.TZ;
  try {
    process.env.TZ = "Europe/Berlin";
    assert.equal(offsetEast(Date.UTC(2026, 0, 1)), 3600);
    assert.equal(offsetEast(Date.UTC(2026, 6, 1)), 7200);
    process.env.TZ = "America/New_York";
    assert.equal(offsetEast(Date.UTC(2026, 0, 1)), -18000);
  } finally {
    if (was === undefined) delete process.env.TZ; else process.env.TZ = was;
  }
});
