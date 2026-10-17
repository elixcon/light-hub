import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { lampFace, mixColours, roomColour, lightColour, INK, MUTED, LIT, type Rgb, type LightColour, type Face } from "../src/lib/glow.ts";
import { colourOf } from "../src/lib/colorspace.ts";
import type { Endpoint, GroupMember } from "../src/lib/types.ts";

/* Contrast and OKLab written out again here rather than imported, so a fault
 * in glow.ts's own cannot pass its own test. */
const lin = (v: number) => { v /= 255; return v > 0.04045 ? ((v + 0.055) / 1.055) ** 2.4 : v / 12.92; };
const lum = (c: readonly number[]) => 0.2126 * lin(c[0]) + 0.7152 * lin(c[1]) + 0.0722 * lin(c[2]);
const contrast = (a: readonly number[], b: readonly number[]) => {
  const x = lum(a), y = lum(b);
  return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05);
};
function lab(c: readonly number[]) {
  const [r, g, b] = c.map(lin);
  const l = Math.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
  const m = Math.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
  const s = Math.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
  return [0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s, 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
    0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s];
}
const chroma = (c: Rgb) => { const [, a, b] = lab(c); return Math.hypot(a, b); };
const apart = (x: Rgb, y: Rgb) => { const a = lab(x), b = lab(y); return Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]); };

/* Every colour a lamp can report: the whole wheel at three saturations, every
 * warmth from candle to daylight - as a warmth, and as the chromaticity a room
 * that also holds a colour lamp mixes it by - and nothing known. */
const every: (LightColour | null)[] = [null];
for (let h = 0; h < 360; h += 3) for (const s of [100, 60, 25]) every.push({ rgb: colourOf({ hue: h, sat: s }) });
for (let k = 1500; k <= 7000; k += 100) every.push({ mired: 1e6 / k }, { rgb: colourOf({ kelvin: k }) });
const warmth = (k: number): LightColour => ({ mired: 1e6 / k });
const rgb = (c: Rgb): LightColour => ({ rgb: c });
const hue = (h: number, s = 100) => rgb(colourOf({ hue: h, sat: s }));

/* The gradient at t, as the browser interpolates it: in gamma-encoded sRGB. */
const along = (f: Face, t: number) => f.top.map((v, i) => v + (f.bottom[i] - v) * t);

test("the text reads on a lit card, top to foot, whatever colour the lamp is", () => {
  for (const c of every) {
    const f = lampFace(c);
    const ink = INK[f.ink];
    for (const t of [0, 0.25, 0.5, 0.75, 1]) {
      const bg = along(f, t);
      const grey = ink.map((v, i) => v * MUTED[f.ink] + bg[i] * (1 - MUTED[f.ink]));
      assert.ok(contrast(grey, bg) >= 4.5 - 1e-9, `grey text on ${JSON.stringify(c)} at ${t}: ${contrast(grey, bg)}`);
      assert.ok(contrast(ink, bg) >= 6, `text on ${JSON.stringify(c)} at ${t}`);
    }
  }
});

test("black or white, by which moves the colour least", () => {
  assert.equal(lampFace(hue(60)).ink, "dark", "yellow");
  assert.equal(lampFace(hue(120)).ink, "dark", "green");
  assert.equal(lampFace(rgb([0, 0, 255])).ink, "light", "blue");
  assert.equal(lampFace(hue(280)).ink, "light", "purple");
  /* The two are nearly even on pure red; darkened it is still red, lightened
   * far enough for black it was salmon. */
  assert.equal(lampFace(rgb([255, 0, 0])).ink, "light", "red");
  assert.equal(lampFace(warmth(2700)).ink, "dark", "a warm white");
  assert.equal(lampFace(null).ink, "dark", "the amber of lit");
});

test("a saturated colour stays saturated", () => {
  for (let h = 0; h < 360; h += 3) {
    const f = lampFace(hue(h));
    assert.ok(Math.max(chroma(f.top), chroma(f.bottom)) >= 0.12, `hue ${h}: ${f.top} ${f.bottom}`);
  }
});

test("warmth runs amber, white, blue, and the everyday whites are told apart", () => {
  const [r1, , b1] = lampFace(warmth(2200)).bottom;
  assert.ok(r1 > b1 + 60, "2200 K is amber");
  for (const end of ["top", "bottom"] as const) {
    const [r, , b] = lampFace(warmth(6500))[end];
    assert.ok(b > r + 10, `6500 K is blue at the ${end}`);
  }
  assert.ok(chroma(lampFace(warmth(4500)).top) < 0.01, "4500 K is white");
  const steps = [2000, 2200, 2700, 3000, 3500].map((k) => lampFace(warmth(k)).bottom);
  for (let i = 1; i < steps.length; i++) assert.ok(apart(steps[i - 1], steps[i]) >= 0.01, `step ${i}`);
});

test("the card and the stylesheet agree on the inks and the amber of lit", () => {
  const css = readFileSync(new URL("../src/app.css", import.meta.url), "utf8");
  const hex = (h: string) => [0, 2, 4].map((i) => parseInt(h.slice(i, i + 2), 16));
  const on = css.match(/--on: #([0-9a-f]{6});/)?.[1];
  assert.deepEqual(on && hex(on), [...LIT], "--on");
  for (const ink of ["dark", "light"] as const) {
    const block = css.match(new RegExp(`\\.lamp\\[data-lit\\]\\[data-ink="${ink}"\\] \\{([^}]*)\\}`))?.[1] ?? "";
    const fg = block.match(/--foreground: #([0-9a-f]{6})/)?.[1];
    assert.deepEqual(fg && hex(fg), [...INK[ink]], `${ink} ink`);
    const grey = block.match(/--muted-foreground: rgb\((\d+) (\d+) (\d+) \/ ([\d.]+)\)/);
    assert.ok(grey, `${ink} grey`);
    assert.deepEqual(grey.slice(1, 4).map(Number), [...INK[ink]], `${ink} grey is the ink`);
    assert.equal(Number(grey[4]), MUTED[ink], `${ink} grey's opacity`);
  }
});

test("a lamp says its colour by the mode it reported", () => {
  const ep = (o: Partial<Endpoint>) => ({ kelvin: null, hue: null, sat: null, color_mode: null, ...o }) as Endpoint;
  assert.equal(lightColour(ep({})), null);
  assert.deepEqual(lightColour(ep({ kelvin: 2500, hue: 240, sat: 100, color_mode: "color_temp" })), { mired: 400 });
  assert.deepEqual(lightColour(ep({ kelvin: 2500, hue: 240, sat: 100, color_mode: "color" })), rgb(colourOf({ hue: 240, sat: 100 })));
  assert.deepEqual(lightColour(ep({ hue: 240, sat: 100 })), rgb(colourOf({ hue: 240, sat: 100 })));
  assert.deepEqual(lightColour(ep({ kelvin: 2500 })), { mired: 400 });
});

test("a room mixes the lights that are on, and falls back to all it knows", () => {
  const red = rgb([255, 0, 0]), blue = rgb([0, 0, 255]);
  const mix = mixColours([{ c: red, weight: 50 }, { c: blue, weight: 50 }]) as { rgb: Rgb };
  const [r, g, b] = mix.rgb;
  assert.ok(r > g && b > g, "red and blue make magenta");
  assert.equal(mixColours([]), null);
  assert.deepEqual(mixColours([{ c: { mired: 500 }, weight: 30 }, { c: { mired: 200 }, weight: 10 }]), { mired: 425 },
    "whites make a white between them, by brightness");
  assert.ok("rgb" in mixColours([{ c: { mired: 400 }, weight: 1 }, { c: blue, weight: 1 }])!, "a colour among them mixes as light");

  const m = (ieee: string, on: boolean | null) => ({ ieee, ep: 1, on, brightness_pct: 50 }) as GroupMember;
  const eps: Record<string, Endpoint> = {
    a: { kelvin: null, hue: 0, sat: 100, color_mode: "color" } as Endpoint,
    b: { kelvin: null, hue: 240, sat: 100, color_mode: "color" } as Endpoint,
  };
  const find = (x: GroupMember) => eps[x.ieee];
  assert.deepEqual(roomColour([m("a", true), m("b", false)], find), rgb(colourOf({ hue: 0, sat: 100 })));
  const [r2, g2, b2] = (roomColour([m("a", false), m("b", false)], find) as { rgb: Rgb }).rgb;
  assert.ok(r2 > g2 && b2 > g2, "none lit: every known colour");
  assert.equal(roomColour([m("c", true)], find), null);
});
