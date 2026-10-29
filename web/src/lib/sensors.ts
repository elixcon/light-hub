/* Motion sensors. A sensor is a button whose source is one endpoint of a paired
 * sensor (button_db.h), with two events where a button has gestures, so its
 * rules are edited the way a button's are. This puts the two tables back
 * together: one card per sensor endpoint, with its record or none yet. */
import type { ButtonDef, Device, Endpoint, Rule, SensorEvent } from "./types.ts";
import { mmss } from "./format.ts";
import { blankRule } from "./buttons.ts";

export const isSensorEp = (e: Endpoint) => (e.features || []).includes("motion");
export const isSensor = (d: Device) => d.endpoints.some(isSensorEp);

export const SENSOR_EVENTS: [SensorEvent, string][] = [["motion", "When motion is detected"], ["clear", "When there has been no motion"]];

/* Mirrors ZBC_SENSOR_CLEAR_MAX_S / _DEFAULT_S. The page offers whole minutes
 * from one, above the firmware's 30 s: a Xiaomi or Aqara sensor reports at most
 * once a minute. */
export const CLEAR_MAX_S = 3600;
export const CLEAR_DEFAULT_S = 120;

export const sensorPath = (ieee: string) => `/sensors/${ieee}`;

export interface SensorCard {
  ieee: string;
  ep: number;
  device: Device | null;      /* null: the sensor was deleted, its record was not */
  button: ButtonDef | null;   /* null: not set up yet */
}

/* One card per sensor endpoint - nearly always one per sensor - plus any record
 * still naming a sensor that is no longer paired, which would otherwise hold a
 * button slot nobody can see. */
export function sensorCards(devices: Device[], buttons: ButtonDef[]): SensorCard[] {
  const cards: SensorCard[] = [];
  for (const d of devices) {
    for (const e of d.endpoints.filter(isSensorEp)) cards.push({ ieee: d.ieee, ep: e.ep, device: d, button: null });
  }
  for (const b of buttons) {
    if (b.source !== "sensor" || !b.ieee || b.ep === null) continue;
    const c = cards.find((x) => x.ieee === b.ieee && x.ep === b.ep);
    if (c) c.button = b;
    else cards.push({ ieee: b.ieee, ep: b.ep, device: devices.find((d) => d.ieee === b.ieee) || null, button: b });
  }
  return cards;
}

/* What the room looks like to the sensor's rules, in a few words. `elapsed` is
 * how long ago the numbers arrived: the poll brings new ones only when
 * something changes, so the page counts down between them itself. */
export function sensorState(b: ButtonDef | null, elapsed = 0): string {
  if (!b) return "Not set up yet";
  const left = b.clear_in_ms != null ? Math.max(0, Math.ceil((b.clear_in_ms - elapsed) / 1000)) : null;
  const due = left !== null ? ` · no-motion rules in ${mmss(left)}` : "";
  if (b.occupied == null) return `Not heard since the coordinator started${due}`;
  return b.occupied ? `Motion${due}` : "No motion";
}

/* What the last event left waiting, or null. */
export function waitingText(b: ButtonDef | null, elapsed = 0): string | null {
  const d = b?.delayed;
  if (!d || !d.count) return null;
  const left = mmss(Math.max(0, Math.ceil((d.next_ms - elapsed) / 1000)));
  return d.count === 1 ? `1 action waiting, due in ${left}` : `${d.count} actions waiting, the first due in ${left}`;
}

/* The rules a sensor is set up with: the room on with motion, off without. */
export function setupRules(group: number): Record<SensorEvent, Rule[]> {
  const rule = (d: "on" | "off"): Rule => ({ ...blankRule(), group, arg: 0, do: d });
  return { motion: [rule("on")], clear: [rule("off")] };
}
