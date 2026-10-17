/* Mirrors colorspace.c and the chromaticity half of curve_db.c.
 *
 * A path can run from a colour to a colour temperature, and the only space in
 * which those are the same kind of thing is CIE xy. These are the same
 * conversions the coordinator uses, so the editor shows what the lamp will do
 * rather than an approximation of it. The C is the authority; the two agree to
 * within one unit of hue, saturation or kelvin. Do not simplify either side
 * without the other. */

import { pchip } from "./pchip.ts";

export type KelvinStop = { at: number; kelvin: number };
export type ColourStop = { at: number; hue: number; sat: number };
export type Stop = KelvinStop | ColourStop;
export type PathValue = { kelvin: number } | { hue: number; sat: number } | { x: number; y: number };

export function kelvinToXy(k: number): [number, number] {
  k = Math.max(1667, Math.min(25000, k)); /* the fit's own range */
  const t2 = k * k, t3 = t2 * k;
  const x = k <= 4000
    ? -0.2661239e9 / t3 - 0.2343589e6 / t2 + 0.8776956e3 / k + 0.179910
    : -3.0258469e9 / t3 + 2.1070379e6 / t2 + 0.2226347e3 / k + 0.240390;
  const x2 = x * x, x3 = x2 * x;
  const y = k <= 2222 ? -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683
          : k <= 4000 ? -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867
          :              3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
  return [x, y];
}

export const srgbExpand = (v: number) => (v > 0.04045 ? Math.pow((v + 0.055) / 1.055, 2.4) : v / 12.92);
export const srgbCompress = (v: number) => (v <= 0.0031308 ? 12.92 * v : 1.055 * Math.pow(v, 1 / 2.4) - 0.055);

/* HSV at value 1, as gamma-encoded sRGB in 0..1. Value is pinned because xy
 * carries no brightness - the lamp's own level is what sets that. */
function hsvUnit(h: number, s: number): [number, number, number] {
  h = ((h % 360) + 360) % 360;
  s = Math.max(0, Math.min(1, s / 100));
  const c = s, m = 1 - c, k = c * (1 - Math.abs(((h / 60) % 2) - 1));
  let r: number, g: number, b: number;
  if (h < 60)       [r, g, b] = [c, k, 0];
  else if (h < 120) [r, g, b] = [k, c, 0];
  else if (h < 180) [r, g, b] = [0, c, k];
  else if (h < 240) [r, g, b] = [0, k, c];
  else if (h < 300) [r, g, b] = [k, 0, c];
  else              [r, g, b] = [c, 0, k];
  return [r + m, g + m, b + m];
}

export function hsToXy(h: number, s: number): [number, number] {
  const [r, g, b] = hsvUnit(h, s).map(srgbExpand);
  const X = r * 0.4124 + g * 0.3576 + b * 0.1805;
  const Y = r * 0.2126 + g * 0.7152 + b * 0.0722;
  const Z = r * 0.0193 + g * 0.1192 + b * 0.9505;
  const sum = X + Y + Z || 1;
  return [X / sum, Y / sum];
}

/* xy back to gamma-encoded sRGB in 0..1, with the maximum channel at 1. Lifting
 * all three channels off a negative rather than clamping each keeps the hue: see
 * colorspace.c. */
function xyToUnit(x: number, y: number): [number, number, number] {
  if (y < 1e-4) return [0, 0, 0];
  const X = x / y, Z = (1 - x - y) / y;
  let r =  3.2406 * X - 1.5372 - 0.4986 * Z;
  let g = -0.9689 * X + 1.8758 + 0.0415 * Z;
  let b =  0.0557 * X - 0.2040 + 1.0570 * Z;
  const low = Math.min(r, g, b);
  if (low < 0) { r -= low; g -= low; b -= low; }
  const top = Math.max(r, g, b);
  if (top <= 0) return [0, 0, 0];
  return [r / top, g / top, b / top].map(srgbCompress) as [number, number, number];
}

export const xyToRgb = (x: number, y: number): [number, number, number] =>
  xyToUnit(x, y).map((v) => Math.round(v * 255)) as [number, number, number];

export function xyToHs(x: number, y: number): { hue: number; sat: number } {
  const [r, g, b] = xyToUnit(x, y);
  const lo = Math.min(r, g, b), c = 1 - lo; /* the maximum is 1 by construction */
  let h = 0;
  if (c >= 1e-6) {
    if (r >= g && r >= b) h = 60 * ((((g - b) / c) + 6) % 6);
    else if (g >= b)      h = 60 * ((b - r) / c + 2);
    else                  h = 60 * ((r - g) / c + 4);
  }
  return { hue: Math.round(h) % 360, sat: Math.round(c * 100) };
}

/* McCamy's approximation, used only here: it gives a colour stop a sensible
 * kelvin to start from when it is switched to a warmth stop, so nobody has to
 * invent a number for a stop they already placed by eye. */
export function xyToCct(x: number, y: number): number {
  const n = (x - 0.3320) / (0.1858 - y);
  return Math.round(437 * n * n * n + 3601 * n * n + 6861 * n + 5517);
}

export const isKelvinStop = (s: Stop | PathValue): s is KelvinStop =>
  (s as KelvinStop).kelvin !== undefined;
export const stopXy = (s: Stop): [number, number] =>
  isKelvinStop(s) ? kelvinToXy(s.kelvin) : hsToXy(s.hue, s.sat);

/* The mirror of chroma_eval(): the segment decides. Two stops of the same kind
 * interpolate in their own space over the whole run of that kind; two of
 * different kinds are joined by a straight line across the chromaticity diagram.
 * Hue is unwrapped so a path from 350 to 10 takes the short way across red. A
 * stop where the two kinds meet is read as the warmth, as the firmware does. */
export function pathAt(stops: Stop[], at: number): PathValue | null {
  const n = stops.length;
  if (n < 2) return null;
  for (const s of stops) if (s.at === at && isKelvinStop(s)) return { kelvin: s.kelvin };
  let i = 0;
  while (i < n - 2 && at > stops[i + 1].at) i++;

  if (isKelvinStop(stops[i]) === isKelvinStop(stops[i + 1])) {
    let a = i, b = i + 1;
    const kind = isKelvinStop(stops[i]);
    while (a > 0 && isKelvinStop(stops[a - 1]) === kind) a--;
    while (b < n - 1 && isKelvinStop(stops[b + 1]) === kind) b++;
    const run = stops.slice(a, b + 1);
    const xs = run.map((s) => s.at);

    if (kind) return { kelvin: Math.round(pchip(xs, run.map((s) => (s as KelvinStop).kelvin), at)) };

    const cs = run as ColourStop[];
    const hues = [cs[0].hue];
    for (let j = 1; j < cs.length; j++) {
      let d = cs[j].hue - cs[j - 1].hue;
      while (d > 180) d -= 360;
      while (d < -180) d += 360;
      hues.push(hues[j - 1] + d);
    }
    let h = pchip(xs, hues, at) % 360;
    if (h < 0) h += 360;
    const s = pchip(xs, cs.map((r) => r.sat), at);
    return { hue: Math.round(h) % 360, sat: Math.round(Math.max(0, Math.min(100, s))) };
  }

  const [x0, y0] = stopXy(stops[i]), [x1, y1] = stopXy(stops[i + 1]);
  const span = stops[i + 1].at - stops[i].at;
  const t = Math.max(0, Math.min(1, span > 0 ? (at - stops[i].at) / span : 0));
  return { x: x0 + (x1 - x0) * t, y: y0 + (y1 - y0) * t };
}

/* Everything the editor paints goes through this one converter. Drawing a
 * warmth segment with a different approximation from a blended one would leave a
 * visible seam exactly where the whole feature is meant to look smooth. */
export function colourOf(r: PathValue | null | undefined): [number, number, number] {
  if (!r) return [255, 220, 170];
  const [x, y] = isKelvinStop(r) ? kelvinToXy(r.kelvin)
               : "hue" in r ? hsToXy(r.hue, r.sat)
               : [(r as { x: number; y: number }).x, (r as { x: number; y: number }).y];
  return xyToRgb(x, y);
}

/* Any path value as chromaticity. */
export const xyOf = (v: PathValue): [number, number] =>
  isKelvinStop(v) ? kelvinToXy(v.kelvin)
  : "hue" in v ? hsToXy(v.hue, v.sat)
  : [(v as { x: number; y: number }).x, (v as { x: number; y: number }).y];

export const rgbCss = ([r, g, b]: [number, number, number]) => `rgb(${r}, ${g}, ${b})`;

/* Approximate a colour temperature as a screen colour, for slider gradients. */
export function kelvinToCss(k: number): string {
  const t = k / 100;
  let r: number, g: number, b: number;
  if (t <= 66) { r = 255; g = 99.47 * Math.log(t) - 161.12; }
  else { r = 329.7 * Math.pow(t - 60, -0.133); g = 288.12 * Math.pow(t - 60, -0.0755); }
  if (t >= 66) b = 255;
  else if (t <= 19) b = 0;
  else b = 138.52 * Math.log(t - 10) - 305.04;
  const c = (v: number) => Math.max(0, Math.min(255, Math.round(v)));
  return `rgb(${c(r)}, ${c(g)}, ${c(b)})`;
}

/* The wheel is painted conic-gradient(from 90deg, red, magenta, blue, cyan,
 * green, yellow, red): red sits at east and hue rises anticlockwise. Screen y
 * grows downward, hence the minus. */
export function wheelCursor(hue: number, sat: number): [number, number] {
  const rad = (sat / 100) * 50;
  const ang = (hue * Math.PI) / 180;
  return [50 + rad * Math.cos(ang), 50 - rad * Math.sin(ang)];
}

/* The inverse: a pointer position relative to the wheel's box, to hue and sat. */
export function wheelPick(dx: number, dy: number, radius: number): { hue: number; sat: number } {
  const dist = Math.min(1, Math.hypot(dx, dy) / radius);
  let hue = Math.round((Math.atan2(-dy, dx) * 180) / Math.PI);
  if (hue < 0) hue += 360;
  return { hue, sat: Math.round(dist * 100) };
}
