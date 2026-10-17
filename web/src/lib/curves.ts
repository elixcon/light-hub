/* Mirrors zbc_pchip() in curve_db.c and the switch-point encoding lighting.c
 * reads. The chart, the preview strip and the test-slider readout are all drawn
 * from this, so they show what the lamp will actually do rather than an
 * approximation of it; the two sides agree to about 1e-4. Do not simplify. */

import { pchip } from "./pchip.ts";
import { colourOf, pathAt, type Stop, type KelvinStop, type ColourStop } from "./colorspace.ts";

export { pchip };

export type Point = [number, number];

export const curveAt = (pts: Point[], x: number) =>
  pchip(pts.map((p) => p[0]), pts.map((p) => p[1]), x);

export const DEFAULT_MIN_K = 2000, DEFAULT_MAX_K = 6500;
export const MAX_POINTS = 6;   /* ZBC_CURVE_MAX_POINTS */
export const MAX_STOPS = 8;    /* ZBC_CHROMA_MAX_STOPS */

export const PRESETS: { level: Record<string, Point[]>; chroma: Record<string, Stop[]> } = {
  level: {
    "Linear":         [[1, 1], [100, 100]],
    "Perceptual":     [[1, 1], [25, 6], [50, 20], [75, 50], [100, 100]],
    "Gentle":         [[1, 1], [50, 35], [100, 70]],
    "Off below 40 %": [[1, 0], [39, 0], [40, 20], [100, 100]],
  },
  /* One list, filtered by what the lamp can do: a preset with a warmth stop
   * needs colour temperature, one with a colour stop needs colour. The last two
   * need both, and are the point of the whole thing. */
  chroma: {
    "Dim to warm":  [{ at: 1, kelvin: 2000 }, { at: 50, kelvin: 2700 }, { at: 100, kelvin: 4000 }],
    "Candlelight":  [{ at: 1, kelvin: 1800 }, { at: 100, kelvin: 2300 }],
    "Daylight":     [{ at: 1, kelvin: 2700 }, { at: 50, kelvin: 4000 }, { at: 100, kelvin: 6000 }],
    "Sunset":       [{ at: 1, hue: 15, sat: 95 }, { at: 50, hue: 30, sat: 80 }, { at: 100, hue: 45, sat: 35 }],
    "Amber":        [{ at: 1, hue: 25, sat: 90 }, { at: 100, hue: 40, sat: 40 }],
    "Cool":         [{ at: 1, hue: 250, sat: 60 }, { at: 100, hue: 210, sat: 20 }],
    "Ember to day": [{ at: 1, hue: 12, sat: 90 }, { at: 35, kelvin: 2200 }, { at: 100, kelvin: 5000 }],
    "Night to day": [{ at: 1, hue: 280, sat: 85 }, { at: 25, kelvin: 2000 }, { at: 100, kelvin: 4500 }],
  },
};

const isKelvin = (s: Stop): s is KelvinStop => (s as KelvinStop).kelvin !== undefined;

const kelvinSpread = (stops: Stop[]) => {
  const k = stops.filter(isKelvin).map((s) => s.kelvin);
  return k.length ? Math.max(...k) - Math.min(...k) : 0;
};

/* The chroma presets this lamp can actually show, fitted to it. A preset needs
 * colour temperature for a warmth stop and colour for a colour stop, and its
 * warmth stops are clamped to the lamp's own range - the firmware would clamp
 * them anyway, so this shows what the lamp will really do. A preset that loses
 * more than half its warmth range to that is left out: "Candlelight" on a lamp
 * that starts at 2700 K would be one flat colour, not candlelight. */
export function chromaPresetsFor(canCt: boolean, canColor: boolean, minK: number, maxK: number): Record<string, Stop[]> {
  const out: Record<string, Stop[]> = {};
  for (const [name, stops] of Object.entries(PRESETS.chroma)) {
    if (!stops.every((s) => (isKelvin(s) ? canCt : canColor))) continue;
    const fitted = stops.map((s) =>
      isKelvin(s) ? { ...s, kelvin: Math.round(Math.min(maxK, Math.max(minK, s.kelvin))) } : { ...s });
    if (kelvinSpread(fitted) < kelvinSpread(stops) / 2) continue;
    out[name] = fitted;
  }
  return out;
}

export const clonePoints = (pts: Point[]): Point[] => pts.map((p) => [p[0], p[1]]);
export const clonePath = (stops: Stop[]): Stop[] => stops.map((s) => ({ ...s }));

/* Default stops for a lamp that has no path yet: the identity where there is
 * one, so switching a mapping on does not change the light until it is moved. */
export function defaultPath(canCt: boolean, minK: number, maxK: number): Stop[] {
  if (canCt) return [{ at: 1, kelvin: Math.round(minK) }, { at: 100, kelvin: Math.round(maxK) }];
  return [{ at: 1, hue: 30, sat: 70 }, { at: 100, hue: 45, sat: 30 }];
}
export const defaultLevel = (): Point[] => [[1, 1], [100, 100]];

/* A switch point is stored as an ordinary level curve that is 0 below it and
 * 100 from it on; the firmware reads any output above 0 as On. Monotone cubic
 * interpolation keeps both flat stretches flat, so whole percentages land on
 * exactly 0 or 100. x has to be strictly ascending from 1 to 100, hence the
 * cases at either end. */
export function switchPoints(at: number): Point[] {
  at = Math.max(1, Math.min(100, Math.round(at)));
  if (at === 1) return [[1, 100], [100, 100]];
  if (at === 2) return [[1, 0], [2, 100], [100, 100]];
  if (at === 100) return [[1, 0], [99, 0], [100, 100]];
  return [[1, 0], [at - 1, 0], [at, 100], [100, 100]];
}

/* The lowest input that lights it, or null when nothing is set. */
export function switchPointOf(points: Point[] | null | undefined): number | null {
  if (!points) return null;
  const p = points.find((q) => q[1] > 0);
  return p ? p[0] : 100;
}

export const switchLabel = (at: number | null | undefined) =>
  at === null || at === undefined
    ? "Not set · follows the room's on/off only"
    : at <= 1 ? "On whenever its room is on" : `On from ${at} % room brightness`;

/* Chart geometry, shared by the SVG and its hit-testing. */
export const CW = 300, CH = 180, CPAD = 16;
export const cx = (x: number) => CPAD + ((x - 1) / 99) * (CW - 2 * CPAD);
export const cy = (v: number, lo = 0, hi = 100) =>
  CH - CPAD - ((v - lo) / Math.max(1e-6, hi - lo)) * (CH - 2 * CPAD);
export const invx = (px: number) =>
  Math.max(1, Math.min(100, Math.round(1 + ((px - CPAD) / (CW - 2 * CPAD)) * 99)));
export const invy = (py: number, lo = 0, hi = 100) =>
  lo + ((CH - CPAD - py) / (CH - 2 * CPAD)) * (hi - lo);

/* The path itself, at full brightness - what colour the lamp is showing across
 * the room range, with the level curve deliberately left out of it so the stops
 * stay readable at the dim end. */
export function chromaGradient(stops: Stop[]): string {
  const sw: string[] = [];
  for (let i = 0; i <= 60; i++) {
    const pct = 1 + (99 * i) / 60;
    const [r, g, b] = colourOf(pathAt(stops, pct));
    sw.push(`rgb(${r},${g},${b}) ${(i / 60) * 100}%`);
  }
  return `linear-gradient(to right, ${sw.join(", ")})`;
}

export type { Stop, KelvinStop, ColourStop };
