import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

/* Headings, hints and empty-list texts lie straight on the room
 * (lib/components/Backdrop.svelte). Its veil is what keeps them readable, and
 * that is a property of a dozen hand-picked colours a theme: this holds the grey text
 * at WCAG 4.5:1 on every one of them, under the veil, by day and at night. The
 * blur and the lamps' translucent light only ever mix these colours, so what
 * passes sharp and at full strength passes as drawn. */

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

for (const theme of ["day", "night"] as const) {
  test(`grey text reads on every colour of the room, ${theme}`, () => {
    const colours = Object.fromEntries([...room[theme].matchAll(/--(\w+): (#[0-9a-f]{6});/g)].map((m) => [m[1], hex(m[2])]));
    const veil = room[theme].match(/--veil: rgb\((\d+) (\d+) (\d+) \/ ([\d.]+)\)/);
    const grey = page[theme].match(/--muted-foreground: (#[0-9a-f]{6});/);
    assert.ok(veil && grey && Object.keys(colours).length >= 12, "the palette, the veil and the grey");
    for (const [name, c] of Object.entries(colours)) {
      const seen = over(veil.slice(1, 4).map(Number), Number(veil[4]), c);
      const ratio = contrast(hex(grey[1]), seen);
      assert.ok(ratio >= 4.5, `${name}: ${ratio.toFixed(2)}:1`);
    }
  });
}
