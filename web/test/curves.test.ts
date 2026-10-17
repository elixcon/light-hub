import { test } from "node:test";
import assert from "node:assert/strict";
import { pchip } from "../src/lib/pchip.ts";
import { curveAt, switchPoints, chromaPresetsFor } from "../src/lib/curves.ts";
import { pathAt, kelvinToXy, hsToXy, xyToHs, colourOf, type Stop } from "../src/lib/colorspace.ts";

function rnd(seed: number) {
  let s = seed >>> 0;
  return () => ((s = (s * 1664525 + 1013904223) >>> 0) / 2 ** 32);
}

test("pchip is bounded by its points and monotone for monotone data", () => {
  const r = rnd(7);
  for (let run = 0; run < 300; run++) {
    const n = 2 + Math.floor(r() * 5);
    const xs = [1];
    while (xs.length < n) xs.push(xs[xs.length - 1] + 1 + Math.floor(r() * (99 / n)));
    xs[n - 1] = Math.max(xs[n - 1], xs[n - 2] + 1);
    const ys = [r() * 100];
    for (let i = 1; i < n; i++) ys.push(Math.min(100, ys[i - 1] + r() * 30));
    let last = -1;
    for (let t = 1; t <= xs[n - 1]; t += 0.25) {
      const v = pchip(xs, ys, t);
      assert.ok(v >= Math.min(...ys) - 1e-9 && v <= Math.max(...ys) + 1e-9, `bounded at ${t}`);
      assert.ok(v >= last - 1e-9, `monotone at ${t}`);
      last = v;
    }
    for (let i = 0; i < n; i++) assert.ok(Math.abs(pchip(xs, ys, xs[i]) - ys[i]) < 1e-9, "through points");
  }
});

test("pchip of the identity is the identity", () => {
  for (let t = 1; t <= 100; t++) assert.ok(Math.abs(curveAt([[1, 1], [100, 100]], t) - t) < 1e-9);
});

test("switch points give exactly 0 or 100 at every whole percentage", () => {
  for (let T = 1; T <= 100; T++) {
    const pts = switchPoints(T);
    for (let t = 1; t <= 100; t++) {
      const v = Math.round(curveAt(pts, t) * 1e6) / 1e6;
      assert.equal(v, t >= T ? 100 : 0, `T=${T} t=${t} -> ${v}`);
    }
  }
});

test("chroma presets fit the lamp's warmth range and drop the ones it cannot show", () => {
  /* Two lamp profiles: an OSRAM PAR16 that is warmth-only from 2702 K, and a
   * Hue LST002 that does colour and 2000-6535 K. */
  const par16 = chromaPresetsFor(true, false, 2702, 6535);
  assert.deepEqual(Object.keys(par16), ["Dim to warm", "Daylight"]);
  assert.deepEqual(par16["Dim to warm"].map((s) => (s as { kelvin: number }).kelvin), [2702, 2702, 4000]);

  const lst002 = chromaPresetsFor(true, true, 2000, 6535);
  assert.ok("Candlelight" in lst002 && "Night to day" in lst002);
  assert.deepEqual(lst002["Candlelight"].map((s) => (s as { kelvin: number }).kelvin), [2000, 2300]);

  const colourOnly = chromaPresetsFor(false, true, 2000, 6500);
  assert.deepEqual(Object.keys(colourOnly), ["Sunset", "Amber", "Cool"]);
});

test("pathAt passes exactly through every stop", () => {
  const paths: Stop[][] = [
    [{ at: 1, kelvin: 2000 }, { at: 50, kelvin: 2700 }, { at: 100, kelvin: 4000 }],
    [{ at: 1, hue: 350, sat: 90 }, { at: 60, hue: 10, sat: 60 }, { at: 100, hue: 45, sat: 30 }],
    [{ at: 1, hue: 12, sat: 90 }, { at: 35, kelvin: 2200 }, { at: 70, kelvin: 3300 }, { at: 100, kelvin: 5000 }],
  ];
  /* Compared as the colour the lamp shows, because the reading of a stop on a
   * kind boundary is a kelvin while the reading either side of it is xy - the
   * same point on the diagram, which is the continuity the C side is built on. */
  for (const p of paths) for (const s of p) {
    const v = pathAt(p, s.at)!;
    assert.deepEqual(colourOf(v), colourOf(s));
  }
});

test("a stop where the kinds meet is read as a warmth", () => {
  /* The two readings name the same colour, so only the kind of command tells
   * them apart - and a colour temperature is what the run above the stop will
   * be sending anyway. Mirrors chroma_eval() in main/curve_db.c. */
  const p: Stop[] = [{ at: 1, hue: 15, sat: 83 }, { at: 10, kelvin: 2000 }, { at: 100, kelvin: 4000 }];
  assert.deepEqual(pathAt(p, 10), { kelvin: 2000 });
  assert.ok("x" in pathAt(p, 9)!, "the blend below it is still xy");

  /* Even a lone warmth stop with a mixed segment on either side. */
  const lone: Stop[] = [{ at: 1, hue: 15, sat: 83 }, { at: 40, kelvin: 3000 }, { at: 100, hue: 220, sat: 70 }];
  assert.deepEqual(pathAt(lone, 40), { kelvin: 3000 });
  assert.ok("x" in pathAt(lone, 41)!, "and the blend above it");
});

test("a mixed path is continuous at the kind boundary", () => {
  const p: Stop[] = [{ at: 1, hue: 280, sat: 85 }, { at: 25, kelvin: 2000 }, { at: 100, kelvin: 4500 }];
  const before = pathAt(p, 24.99)! as { x: number; y: number };
  const [x, y] = kelvinToXy(2000);
  assert.ok(Math.abs(before.x - x) < 1e-3 && Math.abs(before.y - y) < 1e-3);
});

test("hue/sat survives a round trip through xy", () => {
  for (let h = 0; h < 360; h += 15) for (const s of [30, 60, 95]) {
    const [x, y] = hsToXy(h, s);
    const back = xyToHs(x, y);
    const dh = Math.min(Math.abs(back.hue - h), 360 - Math.abs(back.hue - h));
    assert.ok(dh <= 1 && Math.abs(back.sat - s) <= 1, `h=${h} s=${s} -> ${JSON.stringify(back)}`);
  }
});
