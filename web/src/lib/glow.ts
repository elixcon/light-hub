/* A lit lamp's card is filled with the light's colour, as it last reported
 * it, and its text is black or white - whichever the colour has to be moved
 * less for. A colour that neither reads well on (a pure red, a mid blue) is
 * made just light or dark enough that the grey text keeps 4.5:1; so is the
 * darker foot of the card's gradient, where the brightness figure sits.
 * test/glow.test.ts holds that for every colour a lamp can report.
 *
 * A white is not drawn as its chromaticity. sRGB's white is 6500 K, so every
 * warm white would be one orange and a daylight lamp would be grey. Warmth is
 * drawn instead the way lighting apps draw it: amber, through white at 4500 K,
 * to sky blue, its strength counted in mireds (equal steps look equal). */

import { colourOf, rgbCss, srgbExpand as expand, srgbCompress as compress } from "./colorspace.ts";
import type { Endpoint, GroupMember } from "./types.ts";

export type Rgb = [number, number, number]; /* gamma-encoded sRGB, 0..255 */
/* What a light gives off: a colour, or a white by its warmth in mireds. */
export type LightColour = { rgb: Rgb } | { mired: number };

/* Warmth: white at `neutral` (4500 K), full amber at `warm` (2000 K) and
 * beyond, full blue at `cool` (6500 K) and beyond, in mireds, with the hue of
 * each side. `curve` < 1 lifts the middle, so the everyday whites, 2200 to
 * 3000 K, are clearly warm. 4000 K stays faintly warm, which it is next to
 * daylight. */
const WARMTH = { neutral: 222, warm: 500, cool: 154, warmHue: 78, coolHue: 250, curve: 0.5 } as const;

/* The text colours of a lit card and the grey of its secondary text - ink at
 * this opacity - and the amber of a lamp that has said no colour. app.css sets
 * the same values (`.lamp[data-ink]`, `--on`); the test holds them together. */
export const INK = { dark: [28, 27, 23], light: [255, 255, 255] } as const;
export const MUTED = { dark: 0.72, light: 0.8 } as const;
export const LIT: Rgb = [245, 179, 1];
type Ink = keyof typeof INK;

/* How far the gradient runs above and below the colour, in OKLab lightness,
 * and the most chroma a card takes: a lamp's pure green is neon on a screen. */
const UP = 0.06, DOWN = 0.16, MAX_CHROMA = 0.25;

/* Linear sRGB to OKLab and back (Ottosson's matrices). */
function toLab([r, g, b]: number[]): [number, number, number] {
  const l = Math.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
  const m = Math.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
  const s = Math.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
  return [
    0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
    1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
    0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s,
  ];
}
function fromLab(L: number, a: number, b: number): [number, number, number] {
  const l = (L + 0.3963377774 * a + 0.2158037573 * b) ** 3;
  const m = (L - 0.1055613458 * a - 0.0638541728 * b) ** 3;
  const s = (L - 0.0894841775 * a - 1.2914855480 * b) ** 3;
  return [
    4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
    -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
    -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s,
  ];
}
const inGamut = (c: number[]) => c.every((v) => v >= -1e-6 && v <= 1 + 1e-6);

/* What a light is giving off, as far as it has said: its colour or its warmth,
 * whichever mode it reported, and the other if it reported no mode. Null for a
 * lamp that has said neither - an on/off plug, or one not read yet. */
export function lightColour(e: Endpoint): LightColour | null {
  const hs = e.hue != null && e.sat != null ? { hue: e.hue, sat: e.sat } : null;
  if (e.color_mode === "color" && hs) return { rgb: colourOf(hs) };
  if (e.kelvin != null && e.kelvin > 0) return { mired: 1e6 / e.kelvin };
  return hs ? { rgb: colourOf(hs) } : null;
}

/* Several lights as the light they make together, each by its brightness.
 * Whites make a white between them, averaged in mireds, the scale warmth is
 * drawn on. Once a colour is among them they are added as light adds, in
 * linear sRGB, a white as its own chromaticity; only the hue and chroma of
 * that sum are used, so its scale does not matter. */
export function mixColours(parts: { c: LightColour; weight: number }[]): LightColour | null {
  const live = parts.filter((p) => p.weight > 0);
  const total = live.reduce((t, p) => t + p.weight, 0);
  if (!total) return null;
  if (live.every((p) => "mired" in p.c))
    return { mired: live.reduce((t, p) => t + (p.c as { mired: number }).mired * p.weight, 0) / total };
  const sum = [0, 0, 0];
  for (const { c, weight } of live) {
    const rgb = "rgb" in c ? c.rgb : colourOf({ kelvin: 1e6 / c.mired });
    rgb.forEach((v, i) => (sum[i] += expand(v / 255) * weight));
  }
  const top = Math.max(...sum);
  return top > 0 ? { rgb: sum.map((v) => Math.round(compress(v / top) * 255)) as Rgb } : null;
}

/* A room's colour: the lights in it that are on, or - while none is known to
 * be, as just after the room was switched - every light whose colour is known,
 * since that is what it comes back in. `find` looks a member's endpoint up. */
export function roomColour(members: GroupMember[], find: (m: GroupMember) => Endpoint | null | undefined): LightColour | null {
  const parts = (lit: boolean) => members.flatMap((m) => {
    if (lit && m.on !== true) return [];
    const e = find(m);
    const c = e ? lightColour(e) : null;
    return c ? [{ c, weight: lit ? Math.max(1, m.brightness_pct ?? 100) : 1 }] : [];
  });
  return mixColours(parts(true)) ?? mixColours(parts(false));
}

/* Whether the grey - ink at its opacity over `bg`, composited as the browser
 * does in gamma-encoded sRGB - keeps WCAG 4.5:1 against `bg`. */
function greyReads(ink: Ink, bg: Rgb): boolean {
  const luminance = (c: readonly number[]) => {
    const [r, g, b] = c.map((v) => expand(v / 255));
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
  };
  const a = MUTED[ink], grey = luminance(INK[ink].map((v, i) => v * a + bg[i] * (1 - a))), back = luminance(bg);
  return (Math.max(grey, back) + 0.05) / (Math.min(grey, back) + 0.05) >= 4.5;
}

/* `x` if `ok(x)`, else the value nearest `x` towards `safe` (where `ok` holds)
 * that still does. 12 halvings: the result is rounded to 8-bit sRGB, and it
 * stays on the passing side however many there are. */
function bisect(x: number, safe: number, ok: (v: number) => boolean): number {
  if (ok(x)) return x;
  let lo = x, hi = safe;
  for (let i = 0; i < 12; i++) { const mid = (lo + hi) / 2; if (ok(mid)) hi = mid; else lo = mid; }
  return hi;
}

/* OKLCH to sRGB at a hue angle in radians, losing chroma where sRGB cannot show
 * that much at that lightness, rather than clipping a channel. */
function lch(L: number, C: number, h: number): Rgb {
  const ua = Math.cos(h), ub = Math.sin(h);
  const c = bisect(C, 0, (v) => inGamut(fromLab(L, ua * v, ub * v)));
  return fromLab(L, ua * c, ub * c).map((v) => Math.round(compress(Math.min(1, Math.max(0, v))) * 255)) as Rgb;
}

/* The colour a card shows for a light before its text is fitted: a colour as
 * itself, a warmth on the amber-white-blue scale, and for a lamp that has said
 * neither, the amber of "lit" (`--on`). Warmer is deeper and more saturated;
 * the golden hue is the warm white of lighting apps, not the orange of 2000 K's
 * chromaticity. */
function display(c: LightColour | null): Rgb {
  if (!c) return LIT;
  if ("rgb" in c) return c.rgb;
  const W = WARMTH, m = c.mired, warm = m >= W.neutral;
  const f = Math.min(1, warm ? (m - W.neutral) / (W.warm - W.neutral) : (W.neutral - m) / (W.neutral - W.cool)) ** W.curve;
  return warm ? lch(0.95 - 0.13 * f, 0.165 * f, (W.warmHue * Math.PI) / 180)
    : lch(0.95 - 0.05 * f, 0.09 * f, (W.coolHue * Math.PI) / 180);
}

export type Face = { top: Rgb; bottom: Rgb; ink: Ink };

/* A lit card: the gradient and the text colour. Hue is never touched; only
 * lightness, and chroma where the gamut runs out. */
export function lampFace(c: LightColour | null): Face {
  const [L0, a0, b0] = toLab(display(c).map((v) => expand(v / 255)));
  const C = Math.min(MAX_CHROMA, Math.hypot(a0, b0)), h = Math.atan2(b0, a0);
  /* The lightness nearest `want` that `ink`'s grey reads on, and the colour there. */
  const fit = (ink: Ink, want: number, chroma: number) =>
    bisect(want, ink === "dark" ? 1 : 0, (v) => greyReads(ink, lch(v, chroma, h)));
  const at = (ink: Ink, want: number, k: number) => lch(fit(ink, want, C * k), C * k, h);
  /* Black needs no move on a light colour, and then white cannot need less. */
  const dark = fit("dark", L0, C), light = dark === L0 ? L0 : fit("light", L0, C);
  const ink: Ink = Math.abs(dark - L0) <= Math.abs(light - L0) ? "dark" : "light";
  const L = ink === "dark" ? dark : light;
  return { top: at(ink, Math.min(0.97, L + UP), 0.95), bottom: at(ink, L - DOWN, 0.9), ink };
}

/* A `.lamp` card's attributes, to spread on it: its ink and its gradient. */
export function lampAttrs(c: LightColour | null): { "data-ink": Ink; style: string } {
  const face = lampFace(c);
  return { "data-ink": face.ink, style: `--lamp-top: ${rgbCss(face.top)}; --lamp-bottom: ${rgbCss(face.bottom)}` };
}
