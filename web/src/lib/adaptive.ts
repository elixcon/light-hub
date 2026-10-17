/* Adaptive Lighting's day, mirrored from the firmware so the page can draw it
 * while it is being edited: zbc_sun_phase and zbc_sun_stretch (main/sun.c) and
 * al_shape (main/lighting.c). The sun's own times come from the coordinator
 * (`sun` in /api/groups/{id}/day); only what is done with them is repeated here.
 * Change both together - web/test/adaptive.test.ts and test/test_sun.c,
 * test/test_alstate.c check the same numbers. The firmware's own figure for now
 * (the room's `now`) is drawn beside the mirror's, so a drift shows. */
import { pchip } from "./pchip.ts";

export interface SunDay { rise: number; noon: number; set: number; polar: number }   /* Unix seconds */
/* {earliest, latest} in minutes after local midnight, null for no bound. */
export type Window = [number | null, number | null];
export type ShapePoint = [number, number];

const DAY = 86400, HALF = 43200, MARGIN = 1800;

const ease = (u: number) => (u <= 0 ? 0 : u >= 1 ? 1 : (1 - Math.cos(Math.PI * u)) / 2);
/* C's lround: halves away from zero. */
const lround = (v: number) => Math.sign(v) * Math.round(Math.abs(v));

/* zbc_sun_phase: 1 at noon, 0 at sunrise and sunset, -1 at solar midnight. */
export function phaseAt(t: number, d: SunDay): number {
  if (d.polar) return d.polar;
  if (t >= d.rise && t <= d.set) {
    if (t <= d.noon) return ease((t - d.rise) / (d.noon - d.rise));
    return ease((d.set - t) / (d.set - d.noon));
  }
  if (t > d.set) return -ease((t - d.set) / (d.noon + HALF - d.set));
  return -ease((d.rise - t) / (d.rise - (d.noon - HALF)));
}

/* zbc_sun_stretch: sunrise and sunset moved into their windows, on the local
 * date of the day's noon, `offsetS` east of UTC; never within half an hour of
 * noon or of a solar midnight, once a window has moved them. */
export function stretch(d: SunDay, offsetS: number, rise: Window, set: Window): SunDay {
  if (d.polar) return d;
  const midnight = Math.floor((d.noon + offsetS) / DAY) * DAY - offsetS;
  const into = (t: number, w: Window, lo: number, hi: number) => {
    let v = t;
    if (w[0] !== null && v < midnight + w[0] * 60) v = midnight + w[0] * 60;
    if (w[1] !== null && v > midnight + w[1] * 60) v = midnight + w[1] * 60;
    return v === t ? t : Math.min(hi, Math.max(lo, v));
  };
  return {
    ...d,
    rise: into(d.rise, rise, d.noon - HALF + MARGIN, d.noon - MARGIN),
    set: into(d.set, set, d.noon + MARGIN, d.noon + HALF - MARGIN),
  };
}

/* zbc_al_mix */
export function mixAt(phase: number, atMidnight: number, atNoon: number): number {
  const p = Math.max(-1, Math.min(1, phase));
  return lround(atMidnight + ((atNoon - atMidnight) * (p + 1)) / 2);
}

/* Where a phase sits on a shape's x axis: 1 midnight, 50.5 sunrise/sunset, 100 noon. */
export const shapeX = (phase: number) => 1 + (99 * (Math.max(-1, Math.min(1, phase)) + 1)) / 2;

/* al_shape before its rounding: the straight line without points, else the
 * monotone cubic through the two ends and the points. Drawn from this, so a
 * chart is not a staircase of whole percents. */
export function shapeValue(phase: number, atMidnight: number, atNoon: number, pts: ShapePoint[]): number {
  if (!pts.length) return atMidnight + ((atNoon - atMidnight) * (Math.max(-1, Math.min(1, phase)) + 1)) / 2;
  const xs = [1, ...pts.map((p) => p[0]), 100];
  const ys = [atMidnight, ...pts.map((p) => p[1]), atNoon];
  return pchip(xs, ys, shapeX(phase));
}

/* al_shape: what the firmware sends, rounded as zbc_al_mix and al_shape round. */
export const shapeAt = (phase: number, atMidnight: number, atNoon: number, pts: ShapePoint[]) =>
  lround(shapeValue(phase, atMidnight, atNoon, pts));

export interface Profile {
  pct_midnight: number; pct_noon: number; k_midnight: number; k_noon: number; k_base: number;
  sunrise: Window; sunset: Window; pct_points: ShapePoint[]; k_points: ShapePoint[];
}

/* The firmware's day moved by whole days to the one whose noon is nearest `t`,
 * which is how zbc_sun_day picks it - the sun's times drift a minute or two a
 * day, which a chart of one day does not show. */
export function dayAround(sun: SunDay, t: number): SunDay {
  const k = Math.round((t - sun.noon) / DAY) * DAY;
  return { ...sun, rise: sun.rise + k, noon: sun.noon + k, set: sun.set + k };
}

/* The mode's day around `t`: stretched only when there is a zone to read the
 * limits in (`offsetS` not null), as on the coordinator. */
export function usedDay(sun: SunDay, t: number, prof: Profile, offsetS: number | null): SunDay {
  const d = dayAround(sun, t);
  return offsetS === null ? d : stretch(d, offsetS, prof.sunrise, prof.sunset);
}

/* `pct` and `offsetK` as the firmware sends them; `pctLine` unrounded, to draw. */
export interface Sample { t: number; pct: number; pctLine: number; offsetK: number; phase: number }

/* What a room running the mode is sent at `t`. */
export function sampleAt(sun: SunDay, t: number, prof: Profile, offsetS: number | null): Sample {
  const phase = phaseAt(t, usedDay(sun, t, prof, offsetS));
  const pct = Math.max(1, Math.min(100, shapeAt(phase, prof.pct_midnight, prof.pct_noon, prof.pct_points)));
  const pctLine = Math.max(1, Math.min(100, shapeValue(phase, prof.pct_midnight, prof.pct_noon, prof.pct_points)));
  return { t, pct, pctLine, offsetK: shapeAt(phase, prof.k_midnight, prof.k_noon, prof.k_points), phase };
}

/* A day sampled every `stepS`, from `from` to `to` inclusive. */
export function daySeries(sun: SunDay, from: number, to: number, prof: Profile, offsetS: number | null,
                          stepS = 600): Sample[] {
  const out: Sample[] = [];
  for (let t = from; t < to; t += stepS) out.push(sampleAt(sun, t, prof, offsetS));
  out.push(sampleAt(sun, to, prof, offsetS));
  return out;
}

/* Minutes after local midnight of a moment, in this browser's zone. */
export const localMinutes = (t: number) => {
  const d = new Date(t * 1000);
  return d.getHours() * 60 + d.getMinutes();
};

export const hhmm = (m: number | null) =>
  m === null ? "" : `${String(Math.floor(m / 60)).padStart(2, "0")}:${String(m % 60).padStart(2, "0")}`;

export function parseHhmm(s: string | null | undefined): number | null {
  const m = /^(\d{1,2}):(\d{2})$/.exec((s ?? "").trim());
  if (!m) return null;
  const h = Number(m[1]), min = Number(m[2]);
  return h <= 23 && min <= 59 ? h * 60 + min : null;
}

/* A sunrise or sunset handle dropped at `at` (local minutes) on a day whose own
 * event is at `sun`: earlier than the sun sets the window's latest, later sets
 * its earliest, and within ten minutes of the sun clears whichever bound was
 * moving it - the day follows the sun again there. A bound the drop contradicts
 * goes; the other is kept, since it may be the one that matters in the other
 * half of the year. */
export const SNAP_MIN = 10;
export function dropOn(w: Window, at: number, sun: number): Window {
  let [lo, hi] = w;
  if (at < sun - SNAP_MIN) {
    hi = at;
    if (lo !== null && lo > at) lo = null;
  } else if (at > sun + SNAP_MIN) {
    lo = at;
    if (hi !== null && hi < at) hi = null;
  } else {
    if (hi !== null && hi < sun) hi = null;
    if (lo !== null && lo > sun) lo = null;
  }
  return [lo, hi];
}

/* Shapes to start from, as fractions of the way from the midnight value to the
 * noon value, so they fit whatever those are. */
export const SHAPES: { pct: Record<string, ShapePoint[]>; k: Record<string, ShapePoint[]> } = {
  pct: {
    "Straight": [],
    "Bright until sunset": [[50, 0.85], [75, 1]],
    "Early wind-down": [[50, 0.3]],
  },
  k: {
    "Straight": [],
    "Warm evenings": [[50, 0.15]],
  },
};

export function fitShape(fractions: ShapePoint[], atMidnight: number, atNoon: number, step = 1): ShapePoint[] {
  return fractions.map(([x, f]) => [x, Math.round((atMidnight + (atNoon - atMidnight) * f) / step) * step]);
}
