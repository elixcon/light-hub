/* A tunable white light's mixing curves: one per LED channel, how much of its
 * full output that channel gives at each colour temperature, at full
 * brightness. x is where on the light's own range - 1 at its warm LEDs'
 * kelvin, 100 at its cold LEDs', linear in mireds, as zbc_mix_position in
 * local_lights.c - and y a percentage. The firmware evaluates them with the
 * brightness curve's monotone cubic, which curveAt mirrors. */

import { curveAt, type Point } from "./curves.ts";

export type Channel = "cold" | "neutral" | "warm";
export type MixCurves = Record<Channel, Point[] | null>;

export const CHANNEL_NAMES: Record<Channel, string> = { cold: "Cold white", neutral: "Neutral white", warm: "Warm white" };

export const channelsOf = (three: boolean): Channel[] => (three ? ["cold", "neutral", "warm"] : ["cold", "warm"]);

/* zbc_mix_position: 1 at the warm LEDs, 100 at the cold ones, linear in mireds. */
export function mixPosition(kelvin: number, coldK: number, warmK: number): number {
  const cm = 1e6 / coldK, wm = 1e6 / warmK;
  if (wm <= cm) return 50.5;
  const m = Math.min(wm, Math.max(cm, 1e6 / kelvin));
  return 1 + (99 * (wm - m)) / (wm - cm);
}

/* What a channel gives at x, as the firmware works it out; no curve is dark. */
export const channelAt = (pts: Point[] | null | undefined, x: number) =>
  pts && pts.length >= 2 ? Math.max(0, Math.min(100, curveAt(pts, x))) : 0;

const at = (x: number) => Math.max(2, Math.min(99, Math.round(x)));

/* The two formulas as curves, to start drawing from.
 *
 * Constant total: on each stretch between two LEDs, the monotone cubic through
 * three points per channel gives two curves that are mirror images with the
 * same tangents, so they add up to exactly 100 % everywhere - curved rather
 * than straight as the firmware's own formula is, but every colour temperature
 * is still equally bright. Full: each pair of neighbouring LEDs reaches full
 * half way between them. */
export function presetsFor(three: boolean, coldK: number, neutralK: number, warmK: number): Record<string, MixCurves> {
  if (!three) {
    return {
      "Constant total": { cold: [[1, 0], [100, 100]], neutral: null, warm: [[1, 100], [100, 0]] },
      "Full in the middle": { cold: [[1, 0], [51, 100], [100, 100]], neutral: null, warm: [[1, 100], [50, 100], [100, 0]] },
    };
  }
  const xn = at(mixPosition(neutralK, coldK, warmK));
  const lo = at((1 + xn) / 2), hi = at((xn + 100) / 2);
  return {
    "Constant total": {
      cold: [[1, 0], [xn, 0], [100, 100]],
      neutral: [[1, 0], [xn, 100], [100, 0]],
      warm: [[1, 100], [xn, 0], [100, 0]],
    },
    "Full in the middle": {
      cold: xn < hi ? [[1, 0], [xn, 0], [hi, 100], [100, 100]] : [[1, 0], [xn, 0], [100, 100]],
      neutral: lo < hi ? [[1, 0], [lo, 100], [hi, 100], [100, 0]] : [[1, 0], [xn, 100], [100, 0]],
      warm: lo < xn ? [[1, 100], [lo, 100], [xn, 0], [100, 0]] : [[1, 100], [xn, 0], [100, 0]],
    },
    /* The Philips Hue Being (LTC001), as reverse-engineered at
     * wejn.org/2024/12/hue-engine-reversing-the-functions: its three fitted
     * polynomials sampled to six points each, within 1.8 % of them. On the
     * Hue's own 2203-6536 K; stretched to this light's range. */
    "Philips Hue Being": {
      cold: [[1, 0], [23, 10], [50, 45], [62, 81], [68, 100], [100, 100]],
      neutral: [[1, 0], [14, 14], [53, 75], [65, 100], [77, 71], [100, 10]],
      warm: [[1, 100], [62, 100], [77, 50], [83, 32], [95, 6], [100, 0]],
    },
  };
}

/* The curves that do what a formula mix does, for switching to drawn curves. */
export function curvesFromMix(mix: string, three: boolean, coldK: number, neutralK: number, warmK: number): MixCurves {
  const p = presetsFor(three, coldK, neutralK, warmK);
  return structuredClone(mix === "full" ? p["Full in the middle"] : p["Constant total"]);
}

/* As /api/devices sends them and PATCH /api/lights takes them. */
export type MixCurvesJson = Partial<Record<Channel, { points: Point[] } | null>>;

export const curvesFromJson = (j: MixCurvesJson | null | undefined): MixCurves => ({
  cold: j?.cold?.points ?? null,
  neutral: j?.neutral?.points ?? null,
  warm: j?.warm?.points ?? null,
});

export const curvesToJson = (c: MixCurves): MixCurvesJson => ({
  cold: c.cold ? { points: c.cold } : null,
  neutral: c.neutral ? { points: c.neutral } : null,
  warm: c.warm ? { points: c.warm } : null,
});
