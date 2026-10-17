/* Remotes, as the page shows them. The coordinator keeps one button record per
 * key of a remote (see button_db.h) - which is right for the firmware, where a
 * key is what a press arrives on, and confusing on a page, where a remote is one
 * thing with several buttons. This is where the records are put back together
 * into the remote they belong to, and where remotes somebody has already met get
 * their buttons filled in rather than discovered one press at a time. */
import type { Action, ButtonDef, Device, Gesture, Rule } from "./types.ts";
import { ALL_GESTURES, GESTURES } from "./buttons.ts";
import { isRemoteEp } from "./state.svelte.ts";
import { post, patch, del } from "./api.ts";
import { hex4 } from "./format.ts";

export interface KnownKey {
  key: number;
  label: string;
  /* What each gesture does out of the box; the group is asked for when adding. */
  does: Partial<Record<Gesture, [Action, number]>>;
  /* What each gesture physically is on this key, and so the only gestures it
   * can send: a turn is a "long press" of its key as far as the firmware is
   * concerned, and nobody would call it that. */
  gestures: Partial<Record<Gesture, string>>;
}

export interface KnownRemote {
  title: string;
  /* The Basic cluster's ModelIdentifier, as /api/devices reports it. */
  models: string[];
  note?: string;
  keys: KnownKey[];
}

/* Key numbers are remote.c's: 0 the on/off press, 2 up, 1 down. Each entry
 * follows the frames the device actually sends or its zigbee2mqtt converter -
 * see the remote section of CLAUDE.md before changing one. A remote that counts
 * its own double and triple presses sends them as presses of key 0, where
 * remote.c puts them by model (remote_model). */
export const KNOWN_REMOTES: KnownRemote[] = [
  {
    title: "IKEA BILRESA scroll wheel", models: ["09BA"],
    note: "Pressing the wheel switches, a double click sets full brightness, a triple click 20 %, and turning dims.",
    keys: [
      { key: 0, label: "Wheel press", does: { short: ["toggle", 0], double: ["set", 100], triple: ["set", 20] },
        gestures: { short: "Click", double: "Double click", triple: "Triple click" } },
      { key: 2, label: "Turn up", does: { long: ["dim_up", 8] }, gestures: { long: "Turn up" } },
      { key: 1, label: "Turn down", does: { long: ["dim_down", 8] }, gestures: { long: "Turn down" } },
    ],
  },
  {
    title: "IKEA SYMFONISK sound remote", models: ["SYMFONISK Sound Controller"],
    note: "Pressing it switches, a double press sets full brightness, a triple press 20 %, and turning dims.",
    keys: [
      { key: 0, label: "Press", does: { short: ["toggle", 0], double: ["set", 100], triple: ["set", 20] },
        gestures: { short: "Press", double: "Double press", triple: "Triple press" } },
      { key: 2, label: "Turn right", does: { long: ["dim_up", 8] }, gestures: { long: "Turn right" } },
      { key: 1, label: "Turn left", does: { long: ["dim_down", 8] }, gestures: { long: "Turn left" } },
    ],
  },
  {
    title: "Philips Hue dimmer switch", models: ["RWL020", "RWL021"],
    /* Four buttons, read from endpoint 1: On, OffWithEffect, and Steps from up and
     * down - one per press, and one about every 800 ms while held, the device's
     * own sweep. remote.c makes On and Off two keys for this model only. */
    note: "On and Off switch; up and down dim a step per press and keep stepping while held.",
    keys: [
      { key: 0, label: "On", does: { short: ["on", 0] }, gestures: { short: "Press", double: "Double press" } },
      { key: 2, label: "Up", does: { short: ["dim_up", 10] }, gestures: { short: "Each step, pressed or held" } },
      { key: 1, label: "Down", does: { short: ["dim_down", 10] }, gestures: { short: "Each step, pressed or held" } },
      { key: 3, label: "Off", does: { short: ["off", 0] }, gestures: { short: "Press", double: "Double press" } },
    ],
  },
  {
    /* The same dimmer once its buttons are heard on Philips' cluster (0xFC00):
     * press, hold and release per button, so every button has a click, a hold
     * with the coordinator's own sweep, and a double made of two clicks. */
    title: "Philips Hue dimmer switch", models: ["RWL020#hue", "RWL021#hue"],
    note: "On and Off switch; up and down dim a step per press and sweep while held.",
    keys: [
      { key: 0, label: "On", does: { short: ["on", 0] },
        gestures: { short: "Press", double: "Double press", long: "Hold" } },
      { key: 2, label: "Up", does: { short: ["dim_up", 10], long: ["dim_up", 8] },
        gestures: { short: "Press", double: "Double press", long: "Hold" } },
      { key: 1, label: "Down", does: { short: ["dim_down", 10], long: ["dim_down", 8] },
        gestures: { short: "Press", double: "Double press", long: "Hold" } },
      { key: 3, label: "Off", does: { short: ["off", 0] },
        gestures: { short: "Press", double: "Double press", long: "Hold" } },
    ],
  },
  {
    title: "Philips Hue Smart Button", models: ["ROM001"],
    /* It runs its own sweep while held and always starts it downwards, so the
     * coordinator does the reversing: dim_cycle on the down key, up left alone. */
    note: "Holding it dims, and each new hold goes the other way.",
    keys: [
      { key: 0, label: "Press", does: { short: ["toggle", 0] }, gestures: { short: "Press" } },
      { key: 1, label: "Hold", does: { short: ["dim_cycle", 10] }, gestures: { short: "Each step while held" } },
    ],
  },
  {
    title: "Aqara wireless mini switch", models: ["lumi.sensor_switch.aq2", "lumi.remote.b1acn01"],
    note: "A hold dims, reversing each time, and a double press sets 40 %.",
    keys: [
      { key: 0, label: "Button", does: { short: ["toggle", 0], long: ["dim_cycle", 8], double: ["set", 40] },
        gestures: { short: "Press", long: "Hold", double: "Double press", triple: "Triple press" } },
    ],
  },
];

/* What a remote is looked up by: its model, and for a Hue dimmer whether its
 * buttons have been heard on Philips' own cluster - the coordinator reads them
 * there once they have, and then they have holds and double presses. Private,
 * so that nothing can look a remote up by its bare model and get the wrong
 * table for a dimmer: the device is what is passed around. */
const lookupModel = (d: Device): string =>
  d.endpoints.some((e) => (e.features || []).includes("hue_buttons")) ? `${d.model}#hue` : d.model;

export const knownRemote = (d?: Device | null): KnownRemote | null =>
  (d?.model && KNOWN_REMOTES.find((r) => r.models.includes(lookupModel(d)))) || null;

const GENERIC_KEYS: Record<number, string> = { 0: "Button", 1: "Down / lower button", 2: "Up / upper button", 3: "Off button" };

/* What a key is called on this remote, falling back to what the key number
 * means for any remote. */
export function keyName(key: number | null | undefined, known: KnownRemote | null): string {
  if (key === null || key === undefined) return "";
  const k = known?.keys.find((x) => x.key === key);
  return k ? k.label : GENERIC_KEYS[key] || `Button ${key}`;
}

/* The gestures a key can send, named for what they are on this remote. An
 * unknown remote or key gets short, long and double by their generic names. A
 * gesture the key cannot send is still listed while a rule is stored on it, so
 * nothing saved becomes invisible. */
export function gesturesFor(key: number | null | undefined, known: KnownRemote | null,
  events?: Partial<Record<Gesture, Rule[]>>): [Gesture, string][] {
  const k = key === null || key === undefined ? null : known?.keys.find((x) => x.key === key);
  const offered = (g: Gesture) => (k ? !!k.gestures[g] : GESTURES.some(([x]) => x === g));
  return ALL_GESTURES.filter(([g]) => offered(g) || events?.[g]?.some((r) => r.do !== "none"))
    .map(([g, label]) => [g, k?.gestures[g] ?? label]);
}

export function rulesFor(k: KnownKey, group: number): Record<Gesture, Rule[]> {
  const events: Record<Gesture, Rule[]> = { short: [], long: [], double: [], triple: [] };
  for (const [g, spec] of Object.entries(k.does) as [Gesture, [Action, number]][]) {
    events[g] = [{ group, when: "always", arg: 0, do: spec[0], value: spec[1] }];
  }
  return events;
}

export interface RemoteCard {
  ieee: string;
  ep: number;                /* where buttons set up from the page go: its first remote endpoint */
  device: Device | null;     /* null: the remote was deleted, its buttons were not */
  known: KnownRemote | null;
  buttons: ButtonDef[];
  /* Its buttons sit on more than one endpoint, so a key name alone does not
   * say which button a row is. */
  spans: boolean;
}

/* One card per remote *device*, not per endpoint: a remote with two endpoints -
 * a Hue dimmer has a second, battery endpoint - is still one thing in the hand.
 * Every paired remote, with or without buttons, plus any buttons still naming a
 * remote that is no longer paired - those must stay visible, or they would sit
 * in the table taking a slot nobody can see. */
export function remoteCards(devices: Device[], buttons: ButtonDef[]): RemoteCard[] {
  const cards = new Map<string, RemoteCard>();
  for (const d of devices) {
    const ep = d.endpoints.find(isRemoteEp)?.ep;
    if (ep !== undefined) cards.set(d.ieee, { ieee: d.ieee, ep, device: d, known: knownRemote(d), buttons: [], spans: false });
  }
  for (const b of buttons) {
    if (b.source !== "zigbee" || !b.ieee || b.ep === null) continue;
    let c = cards.get(b.ieee);
    if (!c) {
      const device = devices.find((d) => d.ieee === b.ieee) || null;
      c = { ieee: b.ieee, ep: b.ep, device, known: knownRemote(device), buttons: [], spans: false };
      cards.set(b.ieee, c);
    }
    c.buttons.push(b);
  }
  for (const c of cards.values()) {
    c.spans = new Set(c.buttons.map((b) => b.ep)).size > 1;
    /* A known remote's buttons in the order they are on it (On, Up, Down, Off),
     * which is not the order of their key numbers; anything else by number. */
    const keys = c.known?.keys ?? [];
    const rank = (b: ButtonDef) => {
      const i = keys.findIndex((k) => k.key === b.key);
      return i < 0 ? keys.length + (b.key ?? 0) : i;
    };
    c.buttons.sort((a, b) => (a.ep ?? 0) - (b.ep ?? 0) || rank(a) - rank(b));
  }
  return [...cards.values()];
}

/* Where a remote's own page is; a button on it goes back there. */
export const remotePath = (ieee: string) => `/remotes/${ieee}`;

export const remoteTitle = (c: RemoteCard) => c.device?.name || "A remote that is no longer paired";

/* What kind of remote it is: the known name, or what the device says it is. */
export const remoteKind = (c: RemoteCard) =>
  c.known ? c.known.title : [c.device?.manufacturer, c.device?.model].filter(Boolean).join(" · ") || c.ieee;

/* The latest press of any of its buttons, as an age the client keeps counting. */
export function remoteHeard(c: RemoteCard): number | null {
  const ms = c.buttons.map((b) => b.last_event_ms).filter((x): x is number => x !== null && x !== undefined);
  return ms.length ? Math.min(...ms) : null;
}

/* What a device hangs off, by the name the device list shows: "the
 * coordinator", a lamp's name, or its address for one not on record. */
export function parentName(d: Device, devices: Device[]): string | null {
  const p = d.parent;
  if (!p) return null;
  if (p.coordinator) return "the coordinator";
  return devices.find((x) => x.ieee === p.ieee)?.name || hex4(p.short_addr);
}

/* What the radio heard from it: the frames it numbered that never arrived
 * are the presses that went nowhere, and the only link reading a sleepy
 * remote gets - it is never in the neighbour table the lqa comes from. Then
 * what it hangs off, which decides how its frames reach the coordinator. */
export function remoteLink(c: RemoteCard, devices: Device[]): string | null {
  const d = c.device;
  if (!d) return null;
  const parts: string[] = [];
  if (d.rssi != null) {
    const heard = d.frames_heard ?? 0, missed = d.frames_missed ?? 0;
    parts.push(`${d.rssi} dBm · ${missed} of ${heard + missed} frames lost`);
  }
  const parent = parentName(d, devices);
  if (parent) parts.push(`child of ${parent}`);
  return parts.length ? parts.join(" · ") : null;
}

/* The lamp a group-casting remote hangs off instead of the coordinator, or
 * null when that is not the case. Such a remote's presses reach the
 * coordinator as that lamp's single unacknowledged broadcast, so a moment's
 * interference loses one - as the coordinator's own child every frame is
 * acknowledged and retried. It gets there by rejoining through the lamp after
 * losing the coordinator, typically when pressed while the board restarted. */
export function strayParent(c: RemoteCard, devices: Device[]): string | null {
  const d = c.device;
  if (!d?.groupcast || !d.parent || d.parent.coordinator) return null;
  return parentName(d, devices);
}

/* The known buttons this remote does not have yet. */
export function missingKeys(c: RemoteCard): KnownKey[] {
  return c.known ? c.known.keys.filter((k) => !c.buttons.some((b) => b.key === k.key)) : [];
}

/* One record per key, created and then filled in whole. A record that could not
 * be filled in is removed again, so a refusal - the table is full, the key is
 * taken - does not leave an empty button behind. */
export async function addKnownButtons(ieee: string, ep: number, keys: KnownKey[], group: number): Promise<number> {
  let made = 0;
  for (const k of keys) {
    const name = k.label.slice(0, 31);
    const item = await post<{ id: number }>("/api/buttons", { name });
    try {
      await patch(`/api/buttons/${item.id}`, { name, source: "zigbee", ieee, ep, key: k.key, events: rulesFor(k, group) });
    } catch (err) {
      await del(`/api/buttons/${item.id}`).catch(() => {});
      throw err;
    }
    made++;
  }
  return made;
}
