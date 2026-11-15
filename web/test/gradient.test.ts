import { test } from "node:test";
import assert from "node:assert/strict";
import { gradientAt, gradientCss, addStop, removeStop, flipStops, stopAt, sameStops, DEFAULT_GRADIENT,
  GRADIENT_MAX, GRADIENT_MIN, type GradColour } from "../src/lib/gradient.ts";
import { hsToXy } from "../src/lib/colorspace.ts";

const near = (a: number, b: number, tol = 1e-6) => Math.abs(a - b) <= tol;
const rgb: GradColour[] = [{ hue: 0, sat: 100 }, { hue: 120, sat: 100 }, { hue: 240, sat: 100 }];

/* The same checks test/test_gradient.c makes of zbc_gradient_at. */
test("a blend is its colours where they sit, and a straight line in xy between", () => {
  const [r, g, b] = rgb.map((c) => hsToXy(c.hue, c.sat));
  for (const [pos, want] of [[0, r], [0.5, g], [1, b], [-1, r], [7, b]] as [number, [number, number]][]) {
    const got = gradientAt(rgb, pos);
    assert.ok(near(got[0], want[0]) && near(got[1], want[1]), `at ${pos}`);
  }
  const q = gradientAt(rgb, 0.25);
  assert.ok(near(q[0], (r[0] + g[0]) / 2) && near(q[1], (r[1] + g[1]) / 2), "a quarter along is halfway between the first two");
  const t = gradientAt(rgb, 0.75);
  assert.ok(near(t[0], (g[0] + b[0]) / 2) && near(t[1], (g[1] + b[1]) / 2), "three quarters is halfway between the last two");
});

test("one colour is the whole strip, and none is nothing", () => {
  const one = hsToXy(40, 60);
  for (const pos of [0, 0.3, 1]) assert.deepEqual(gradientAt([{ hue: 40, sat: 60 }], pos), one);
  assert.deepEqual(gradientAt([], 0.5), [0, 0]);
});

test("stops are spread evenly from one end to the other", () => {
  assert.equal(stopAt(0, 3), 0);
  assert.equal(stopAt(1, 3), 50);
  assert.equal(stopAt(2, 3), 100);
  assert.equal(stopAt(1, 5), 25);
  assert.equal(stopAt(0, 1), 50);
});

test("adding a stop leaves the strip as it was", () => {
  let c = rgb;
  for (let n = 4; n <= GRADIENT_MAX; n++) {
    const next = addStop(c);
    assert.equal(next.length, n);
    /* The ends are the ends, and every new stop is on the old strip. */
    assert.deepEqual(next[0], c[0]);
    assert.deepEqual(next[n - 1], c[c.length - 1]);
    for (let i = 0; i < n; i++) {
      const was = gradientAt(c, i / (n - 1)), is = hsToXy(next[i].hue, next[i].sat);
      assert.ok(near(was[0], is[0], 0.01) && near(was[1], is[1], 0.01), `${n} stops: stop ${i} moved`);
    }
    c = next;
  }
  assert.equal(addStop(c), c, "five is the most");
});

test("removing and flipping", () => {
  assert.deepEqual(removeStop(rgb, 1), [rgb[0], rgb[2]]);
  const two = removeStop(rgb, 1);
  assert.equal(removeStop(two, 0), two, `${GRADIENT_MIN} is the fewest`);
  assert.deepEqual(flipStops(rgb), [rgb[2], rgb[1], rgb[0]]);
  assert.ok(sameStops(flipStops(flipStops(rgb)), rgb));
  assert.ok(!sameStops(rgb, two) && !sameStops(rgb, flipStops(rgb)));
});

test("the preview starts and ends in the stops' colours", () => {
  const css = gradientCss(rgb);
  assert.match(css, /^linear-gradient\(to right, rgb\(255, 0, 0\) 0\.0%/);
  assert.match(css, /rgb\(0, 255, 0\) 50\.0%/);
  assert.match(css, /rgb\(0, 0, 255\) 100\.0%\)$/);
});

test("the default is a gradient the firmware takes", () => {
  assert.ok(DEFAULT_GRADIENT.length >= GRADIENT_MIN && DEFAULT_GRADIENT.length <= GRADIENT_MAX);
  for (const c of DEFAULT_GRADIENT) {
    assert.ok(Number.isInteger(c.hue) && c.hue >= 0 && c.hue < 360 && Number.isInteger(c.sat) && c.sat >= 0 && c.sat <= 100);
  }
});
