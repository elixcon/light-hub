import { test } from "node:test";
import assert from "node:assert/strict";
import { dropOn, fitShape, mixAt, phaseAt, sampleAt, shapeAt, shapeX, SHAPES, stretch,
         type Profile, type SunDay, type Window } from "../src/lib/adaptive.ts";

/* The same cases as test/test_sun.c and test/test_alstate.c, so the page's
 * drawing of the day is the firmware's. */
const utc = (y: number, m: number, d: number, hh: number, mm: number) => Date.UTC(y, m - 1, d, hh, mm) / 1000;
/* Munich on 10 October 2026, to the minute: sunrise 05:25 UTC, noon 11:03, sunset 16:40. */
const MUNICH: SunDay = { rise: utc(2026, 10, 10, 5, 25), noon: utc(2026, 10, 10, 11, 3), set: utc(2026, 10, 10, 16, 40), polar: 0 };
const NONE: Window = [null, null];
const CEST = 7200, CET = 3600;

test("the phase hits its marks", () => {
  assert.ok(Math.abs(phaseAt(MUNICH.noon, MUNICH) - 1) < 1e-9);
  assert.ok(Math.abs(phaseAt(MUNICH.rise, MUNICH)) < 1e-9);
  assert.ok(Math.abs(phaseAt(MUNICH.set, MUNICH)) < 1e-9);
  assert.ok(Math.abs(phaseAt(MUNICH.noon + 43200, MUNICH) + 1) < 1e-9);
  assert.ok(Math.abs(phaseAt(MUNICH.noon - 43200, MUNICH) + 1) < 1e-9);
  assert.ok(phaseAt(MUNICH.set + 3600, MUNICH) < 0, "an hour after sunset is night");
  assert.equal(phaseAt(0, { ...MUNICH, polar: -1 }), -1);
});

test("stretch moves sunrise and sunset into their windows, as zbc_sun_stretch does", () => {
  assert.deepEqual(stretch(MUNICH, CEST, NONE, NONE), MUNICH, "no limits leave the day alone");
  const rise: Window = [null, 6 * 60 + 30], set: Window = [21 * 60, null];
  const d = stretch(MUNICH, CEST, rise, set);
  assert.equal(d.rise, utc(2026, 10, 10, 4, 30), "no later than 06:30 CEST");
  assert.equal(d.set, utc(2026, 10, 10, 19, 0), "no earlier than 21:00 CEST");
  assert.equal(d.noon, MUNICH.noon);
  const w = stretch(MUNICH, CET, rise, set);
  assert.equal(w.set, utc(2026, 10, 10, 20, 0), "an hour later in UTC on winter time");
  assert.equal(w.rise, MUNICH.rise, "06:30 CET is after the sun's 05:25 UTC");
  const late = stretch(MUNICH, CEST, [8 * 60, null], [null, 18 * 60]);
  assert.equal(late.rise, utc(2026, 10, 10, 6, 0));
  assert.equal(late.set, utc(2026, 10, 10, 16, 0));
  const edge = stretch(MUNICH, CEST, [null, 0], [null, 12 * 60]);
  assert.equal(edge.rise, MUNICH.noon - 43200 + 1800, "half an hour after solar midnight at the most");
  assert.equal(edge.set, MUNICH.noon + 1800, "half an hour after noon at the least");
  assert.equal(stretch(MUNICH, CEST, [23 * 60, null], NONE).rise, MUNICH.noon - 1800);
  const polar = { ...MUNICH, polar: -1 };
  assert.deepEqual(stretch(polar, CET, rise, set), polar, "a polar night is not stretched");
  /* 22:00 local: well brighter with the evening stretched. */
  assert.ok(phaseAt(utc(2026, 10, 10, 20, 0), d) > phaseAt(utc(2026, 10, 10, 20, 0), MUNICH) + 0.3);
});

test("no points is the two-value profile exactly; points are passed through", () => {
  for (let i = -1000; i <= 1000; i++) {
    const ph = i / 1000;
    assert.equal(shapeAt(ph, 20, 100, []), mixAt(ph, 20, 100));
    assert.equal(shapeAt(ph, -1500, 500, []), mixAt(ph, -1500, 500));
  }
  assert.equal(mixAt(0, -1500, 500), -500);
  assert.equal(mixAt(0, 20, 100), 60);
  const bright = fitShape(SHAPES.pct["Bright until sunset"], 20, 100);
  assert.deepEqual(bright, [[50, 88], [75, 100]]);
  const pts: [number, number][] = [[50, 90], [75, 100]];
  const phaseOf = (x: number) => ((x - 1) / 99) * 2 - 1;
  assert.equal(shapeAt(-1, 20, 100, pts), 20);
  assert.equal(shapeAt(1, 20, 100, pts), 100);
  assert.equal(shapeAt(phaseOf(50), 20, 100, pts), 90);
  assert.equal(shapeAt(phaseOf(75), 20, 100, pts), 100);
  assert.equal(shapeAt(phaseOf(90), 20, 100, pts), 100, "a flat top stays flat");
  assert.ok(shapeAt(0, 20, 100, pts) > shapeAt(0, 20, 100, []) + 25);
  assert.equal(shapeAt(phaseOf(50), -1500, 500, [[50, -1300]]), -1300);
  assert.equal(shapeX(0), 50.5);
});

test("a sample is clamped to 1-100 % and follows the stretched day", () => {
  const prof: Profile = { pct_midnight: 20, pct_noon: 100, k_midnight: -1500, k_noon: 500, k_base: 4000,
    sunrise: [null, 6 * 60 + 30], sunset: [21 * 60, null], pct_points: [], k_points: [] };
  const t = utc(2026, 10, 10, 20, 0);
  const plain = sampleAt(MUNICH, t, prof, null);
  const stretched = sampleAt(MUNICH, t, prof, CEST);
  assert.equal(plain.pct, mixAt(phaseAt(t, MUNICH), 20, 100), "without a zone the limits do nothing");
  assert.ok(stretched.pct > plain.pct + 10, "with one, 22:00 is brighter");
  /* A day later the same moment of day uses the next day's sun. */
  assert.equal(sampleAt(MUNICH, t + 86400, prof, CEST).pct, stretched.pct);
});

test("dropping a handle sets the bound that makes it so", () => {
  const sunrise = 7 * 60 + 25, sunset = 18 * 60 + 40;
  assert.deepEqual(dropOn(NONE, 6 * 60 + 30, sunrise), [null, 390], "earlier than the sun: latest");
  assert.deepEqual(dropOn(NONE, 8 * 60, sunrise), [480, null], "later than the sun: earliest");
  assert.deepEqual(dropOn([null, 390], sunrise + 5, sunrise), [null, null], "back on the sun: follow it");
  assert.deepEqual(dropOn([300, 390], 6 * 60, sunrise), [300, 360], "the other bound is kept");
  assert.deepEqual(dropOn([400, null], 6 * 60, sunrise), [null, 360], "unless the drop contradicts it");
  assert.deepEqual(dropOn(NONE, 21 * 60, sunset), [1260, null], "a later evening: earliest");
  assert.deepEqual(dropOn([1260, 1320], sunset, sunset), [null, 1320], "a summer limit stays");
});
