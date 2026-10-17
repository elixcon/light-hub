import { test } from "node:test";
import assert from "node:assert/strict";
import { presetsFor, mixPosition, channelAt, channelsOf, curvesFromJson, curvesToJson, curvesFromMix } from "../src/lib/mix.ts";
import type { Point } from "../src/lib/curves.ts";

/* zbc_mix_curve_valid: what the firmware stores. */
function valid(pts: Point[] | null) {
  if (pts === null) return true;
  if (pts.length < 2 || pts.length > 6 || pts[0][0] !== 1 || pts[pts.length - 1][0] !== 100) return false;
  return pts.every(([x, y], i) => Number.isInteger(x) && Number.isInteger(y) && y >= 0 && y <= 100 &&
    (i === 0 || x > pts[i - 1][0]));
}

const ranges: [number, number, number][] = [[6500, 4000, 2700], [6500, 3000, 2700], [5000, 4900, 2000], [10000, 4000, 1000]];

test("every preset is a curve the firmware takes", () => {
  for (const three of [false, true]) {
    for (const [c, n, w] of ranges) {
      for (const [name, curves] of Object.entries(presetsFor(three, c, n, w))) {
        for (const ch of channelsOf(three)) {
          assert.ok(curves[ch], `${name}: no ${ch} curve`);
          assert.ok(valid(curves[ch]), `${name} ${ch} at ${c}/${n}/${w} K: ${JSON.stringify(curves[ch])}`);
        }
      }
    }
  }
});

test("constant total adds up to 100 % at every colour temperature", () => {
  for (const three of [false, true]) {
    for (const [c, n, w] of ranges) {
      const p = presetsFor(three, c, n, w)["Constant total"];
      for (let x = 1; x <= 100; x += 0.5) {
        const sum = channelsOf(three).reduce((s, ch) => s + channelAt(p[ch], x), 0);
        assert.ok(Math.abs(sum - 100) < 1e-6, `${three ? 3 : 2} channels ${c}/${n}/${w} K: ${sum} at ${x}`);
      }
    }
  }
});

test("the ends of a constant total are one set of LEDs alone", () => {
  const p = presetsFor(true, 6500, 4000, 2700)["Constant total"];
  assert.deepEqual([channelAt(p.cold, 1), channelAt(p.neutral, 1), channelAt(p.warm, 1)], [0, 0, 100]);
  assert.deepEqual([channelAt(p.cold, 100), channelAt(p.neutral, 100), channelAt(p.warm, 100)], [100, 0, 0]);
  const xn = Math.round(mixPosition(4000, 6500, 2700));
  assert.equal(channelAt(p.neutral, xn), 100);
});

test("full in the middle puts both neighbours at full between them", () => {
  const p = presetsFor(false, 6500, 4000, 2700)["Full in the middle"];
  assert.ok(channelAt(p.cold, 51) === 100 && channelAt(p.warm, 50) === 100);
});

test("the position on the curves is the firmware's", () => {
  assert.equal(mixPosition(2700, 6500, 2700), 1);
  assert.equal(mixPosition(6500, 6500, 2700), 100);
  assert.equal(mixPosition(2000, 6500, 2700), 1);         /* clamped to the LEDs */
  assert.equal(mixPosition(9000, 6500, 2700), 100);
  const mid = 2e6 / (1e6 / 6500 + 1e6 / 2700);            /* half way in mireds */
  assert.ok(Math.abs(mixPosition(mid, 6500, 2700) - 50.5) < 1e-9);
});

test("the Hue preset is the article's lamp: warm full at its warm end, cold full at its cold end", () => {
  const p = presetsFor(true, 6536, 3876, 2203)["Philips Hue Being"];
  assert.deepEqual([channelAt(p.cold, 1), channelAt(p.warm, 1)], [0, 100]);
  assert.deepEqual([channelAt(p.cold, 100), channelAt(p.warm, 100)], [100, 0]);
  /* All three near full together a little cold of the middle, around 3900 K. */
  const x = mixPosition(3900, 6536, 2203);
  for (const ch of ["cold", "neutral", "warm"] as const) assert.ok(channelAt(p[ch], x) > 80, `${ch} at 3900 K`);
});

test("curves go to the API and back unchanged", () => {
  const c = curvesFromMix("full", true, 6500, 4000, 2700);
  assert.deepEqual(curvesFromJson(curvesToJson(c)), c);
  assert.deepEqual(curvesFromJson(null), { cold: null, neutral: null, warm: null });
});
