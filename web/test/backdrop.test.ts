import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

/* Headings, hints and empty-list texts lie straight on the room
 * (lib/components/Backdrop.svelte). Its veil is what keeps them readable, and
 * that is a property of two dozen hand-picked colours: this holds the grey text
 * at WCAG 4.5:1 on every one of them, under the veil, by day and at night. The
 * blur only ever mixes these colours, so what passes sharp passes blurred. */

const read = (p: string) => readFileSync(new URL(`../src/${p}`, import.meta.url), "utf8");
const svelte = read("lib/components/Backdrop.svelte"), css = read("app.css");

type Rgb = number[];
const hex = (h: string): Rgb => [1, 3, 5].map((i) => parseInt(h.slice(i, i + 2), 16));
const over = (top: Rgb, alpha: number, under: Rgb): Rgb => top.map((v, i) => v * alpha + under[i] * (1 - alpha));
const luminance = (c: Rgb) => {
  const [r, g, b] = c.map((v) => { v /= 255; return v <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4; });
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
};
const contrast = (a: Rgb, b: Rgb) => {
  const [hi, lo] = [luminance(a), luminance(b)].sort((x, y) => y - x);
  return (hi + 0.05) / (lo + 0.05);
};

/* The two themes' halves of a stylesheet: before and inside the dark media rule. */
const themes = (text: string) => {
  const at = text.indexOf("@media (prefers-color-scheme: dark)");
  assert.ok(at > 0, "a dark theme");
  return { day: text.slice(0, at), night: text.slice(at) };
};
const room = themes(svelte.slice(svelte.indexOf("<style>"))), page = themes(css);

/* The lamp's light is translucent: three circles on the wall, which stack to
 * this much at their middle, and a candle. Nothing lays it on thicker. */
const LAMP = 0.6;

test("the lamp's light is never thicker than the contrast check allows for", () => {
  const stack = [...svelte.matchAll(/<circle cx="80" cy="42" r="\d+" opacity="([\d.]+)"/g)].map((m) => Number(m[1]));
  assert.equal(stack.length, 3, "the three circles of light on the wall");
  assert.ok(1 - stack.reduce((left, a) => left * (1 - a), 1) <= LAMP + 1e-9, "the circles, stacked");
  const lit = [...svelte.matchAll(/<(\w+) style:fill="var\(--lamp\)"([^>]*)>/g)].filter((m) => m[1] !== "g");
  assert.ok(lit.length >= 2, "the pool on the floor and the candle");
  for (const m of lit) {
    const a = Number(m[2].match(/opacity="([\d.]+)"/)?.[1] ?? 1);
    assert.ok(a <= LAMP, `<${m[1]}> at ${a}`);
  }
});

for (const theme of ["day", "night"] as const) {
  test(`grey text reads on every colour of the room, ${theme}`, () => {
    const colours = Object.fromEntries([...room[theme].matchAll(/--(\w+): (#[0-9a-f]{6});/g)].map((m) => [m[1], hex(m[2])]));
    const veil = room[theme].match(/--veil: rgb\((\d+) (\d+) (\d+) \/ ([\d.]+)\)/);
    const grey = page[theme].match(/--muted-foreground: (#[0-9a-f]{6});/);
    assert.ok(veil && grey && Object.keys(colours).length >= 14, "the palette, the veil and the grey");
    colours.lamp = over(colours.lamp, LAMP, colours.wall);
    for (const [name, c] of Object.entries(colours)) {
      const seen = over(veil.slice(1, 4).map(Number), Number(veil[4]), c);
      const ratio = contrast(hex(grey[1]), seen);
      assert.ok(ratio >= 4.5, `${name}: ${ratio.toFixed(2)}:1`);
    }
  });
}
