/* A gradient along a light strip: a few colours, evenly spaced, the strip
 * blending between them. Mirrors main/gradient.c - the C is the authority, and
 * test/gradient.test.ts keeps the two in step - for the editor's preview and
 * for the list of stops it edits. */

import { hsToXy, xyToHs, xyToRgb } from "./colorspace.ts";

export type GradColour = { hue: number; sat: number };

/* ZBC_GRADIENT_MAX; one colour is not a gradient but a colour. */
export const GRADIENT_MIN = 2;
export const GRADIENT_MAX = 5;

/* What the editor opens with on a strip that shows none: left, middle, right. */
export const DEFAULT_GRADIENT: GradColour[] = [
  { hue: 28, sat: 88 }, { hue: 330, sat: 78 }, { hue: 235, sat: 85 },
];

/* zbc_gradient_at: the colour at `pos` along the strip, 0 its start and 1 its
 * end - a straight line in CIE xy between the two colours either side. */
export function gradientAt(colors: GradColour[], pos: number): [number, number] {
  const n = colors.length;
  if (n === 0) return [0, 0];
  if (n === 1) return hsToXy(colors[0].hue, colors[0].sat);
  const f = Math.max(0, Math.min(1, pos)) * (n - 1);
  const i = Math.min(Math.floor(f), n - 2), t = f - i;
  const a = hsToXy(colors[i].hue, colors[i].sat), b = hsToXy(colors[i + 1].hue, colors[i + 1].sat);
  return [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t];
}

/* Where stop i of n sits, in percent of the strip. */
export const stopAt = (i: number, n: number) => (n <= 1 ? 50 : (i * 100) / (n - 1));

/* The strip as CSS, sampled finely enough that the browser's own blend between
 * two samples - which is not the lamp's - does not show. */
export function gradientCss(colors: GradColour[], steps = 24): string {
  const parts: string[] = [];
  for (let k = 0; k <= steps; k++) {
    const [r, g, b] = xyToRgb(...gradientAt(colors, k / steps));
    parts.push(`rgb(${r}, ${g}, ${b}) ${((k * 100) / steps).toFixed(1)}%`);
  }
  return `linear-gradient(to right, ${parts.join(", ")})`;
}

/* One stop more: the same strip read at one more point, so adding a stop changes
 * nothing until it is given a colour. */
export function addStop(colors: GradColour[]): GradColour[] {
  if (colors.length >= GRADIENT_MAX) return colors;
  const n = colors.length + 1;
  return Array.from({ length: n }, (_, i) => xyToHs(...gradientAt(colors, i / (n - 1))));
}

export function removeStop(colors: GradColour[], i: number): GradColour[] {
  if (colors.length <= GRADIENT_MIN) return colors;
  return colors.filter((_, k) => k !== i);
}

export const flipStops = (colors: GradColour[]): GradColour[] => [...colors].reverse();

export const sameStops = (a: GradColour[], b: GradColour[]) =>
  a.length === b.length && a.every((c, i) => c.hue === b[i].hue && c.sat === b[i].sat);
