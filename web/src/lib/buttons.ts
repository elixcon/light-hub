/* The button editor's vocabulary. Wire names are the C side's; see
 * http_server.c's COND_NAMES / ACTION_NAMES / EVENT_NAMES / PULL_NAMES. */
import type { Action, Gesture, Pull, Rule, When, Group, Device } from "./types.ts";

/* What a pin, or a remote nobody has described, can send. */
export const GESTURES: [Gesture, string][] = [["short", "Short press"], ["long", "Long press"], ["double", "Double press"]];
/* Every gesture a button stores. A triple press is only ever counted by a remote
 * that counts its own presses, so it is offered only on a known remote's key. */
export const ALL_GESTURES: [Gesture, string][] = [...GESTURES, ["triple", "Triple press"]];
/* Mirrors CONFIG_ZBC_BUTTON_RULES: the rules of one button, all its gestures
 * together. */
export const MAX_RULES = 12;
/* Mirrors ZBC_BTN_MAX_SWEEPS: the rooms and lights one long press can dim. */
export const MAX_SWEEPS = 4;

/* The condition carries the word "when" so a row reads as one sentence:
 * "When under 30 %  ·  Kitchen  ·  Set to 80 %". */
export const WHENS: [When, string][] = [["always", "Always"], ["off", "When off"], ["on", "When on"],
  ["below", "When under"], ["above", "When over"], ["sun_down", "When the sun is down"]];

/* How long a rule can wait, as offered; any other number of seconds the API
 * holds shows as itself. "at once" is 0. Mirrors delay_s in button_db.h. */
export const DELAYS = [0, 30, 60, 120, 300, 600, 900, 1800, 3600];
export const MAX_DELAY_S = 65535;

/* 90 → "1 min 30 s", 3600 → "1 h". */
export function delayLabel(s: number): string {
  if (s < 60) return `${s} s`;
  if (s < 3600) return s % 60 ? `${Math.floor(s / 60)} min ${s % 60} s` : `${s / 60} min`;
  const m = Math.round((s % 3600) / 60);
  return m ? `${Math.floor(s / 3600)} h ${m} min` : `${Math.floor(s / 3600)} h`;
}
export const ACTIONS: [Action, string][] = [["none", "Do nothing"], ["toggle", "Toggle"], ["on", "Turn on"],
  ["off", "Turn off"], ["set", "Set to"], ["dim_cycle", "Dim, reversing"],
  ["dim_up", "Dim brighter"], ["dim_down", "Dim dimmer"], ["adaptive", "Adaptive Lighting"]];
export const PULLS: [Pull, string][] = [["up", "Pull-up"], ["down", "Pull-down"], ["none", "No pull"]];

/* Pins the coordinator will refuse, so the page never offers a choice that is
 * going to bounce, plus the ones worth a word before someone solders to them. */
export const PIN_MAX = 30;
export const PIN_BLOCKED = [24, 25, 26, 27, 28, 29, 30];
export const PIN_NOTE: Record<number, string> = { 4: "strapping pin", 5: "strapping pin", 8: "strapping pin",
  9: "BOOT button on most boards", 12: "USB D−, disconnects USB serial (Shelly: wall switch)",
  13: "USB D+, disconnects USB serial", 15: "strapping pin" };
export const PINS = Array.from({ length: PIN_MAX + 1 }, (_, p) => p).filter((p) => !PIN_BLOCKED.includes(p));

/* A remote's keys are told apart by what they send, not by anything it
 * advertises, so there is nothing to enumerate - only names for what has been
 * seen. Key 0 is a remote with one button; 1 and 2 are the halves of a rocker. */
const KEY_LABELS: Record<number, string> = { 0: "single button", 1: "lower button", 2: "upper button" };
export const keyLabel = (k: number | null | undefined) =>
  k === null || k === undefined ? "" : KEY_LABELS[k] || `button ${k}`;

export const isDimAction = (a: Action) => a === "dim_up" || a === "dim_down" || a === "dim_cycle";
export const takesValue = (a: Action) => a === "set" || isDimAction(a);

export const blankRule = (): Rule => ({ group: 0, ieee: null, ep: null, and: false, when: "always", arg: 30, do: "none", value: 60, delay: 0 });

/* A dim moves while a button is held, so it never waits. */
export const canWait = (a: Action) => a !== "none" && !isDimAction(a);

/* One light a rule can act on: every endpoint that switches and is not a
 * remote's, as a room's member list offers them. `k` is its key in a select. */
export interface LightChoice { ieee: string; ep: number; k: string; name: string; kind: string; dims: boolean }

export const lightKey = (ieee: string, ep: number) => `${ieee}/${ep}`;

export function lightChoices(devices: Device[]): LightChoice[] {
  const rows: LightChoice[] = [];
  for (const d of devices) {
    const switchable = d.endpoints.filter((e) => e.features.includes("onoff") && !e.features.includes("switch"));
    for (const e of switchable) {
      const dims = e.features.includes("brightness");
      rows.push({
        ieee: d.ieee, ep: e.ep, k: lightKey(d.ieee, e.ep), dims,
        name: d.name + (switchable.length > 1 ? ` · endpoint ${e.ep}` : ""),
        kind: dims ? "dimmable" : e.curves?.level ? "on/off · switch point" : "on/off only",
      });
    }
  }
  return rows;
}

/* Whether the rule names something: a room, or a light. */
export const hasTarget = (r: Rule) => r.group > 0 || (!!r.ieee && !!r.ep);
export const isLightRule = (r: Rule) => !!r.ieee && !!r.ep;

/* What the rule acts on, by name. */
export function targetName(r: Rule, groups: Group[], devices: Device[]): string {
  if (isLightRule(r)) {
    const l = lightChoices(devices).find((x) => x.k === lightKey(r.ieee!, r.ep!));
    return l ? l.name : "a light that is gone";
  }
  const g = groups.find((x) => x.id === r.group);
  return g ? g.name : "a room that is gone";
}

/* The action alone, lower case where it starts with a verb so it can follow an
 * "and"; `lead` capitalises it. A text that starts with a name is left as the
 * name was typed. */
function actionText(r: Rule, groups: Group[], devices: Device[], lead: boolean): string | null {
  const a = bareActionText(r, groups, devices, lead);
  return a && r.delay ? `${a} after ${delayLabel(r.delay)}` : a;
}

function bareActionText(r: Rule, groups: Group[], devices: Device[], lead: boolean): string | null {
  const name = targetName(r, groups, devices);
  const g = isLightRule(r) ? null : groups.find((x) => x.id === r.group);
  const verb = (s: string) => (lead ? s[0].toUpperCase() + s.slice(1) : s);
  switch (r.do) {
    case "on": return verb(`turn on ${name}`);
    case "off": return verb(`turn off ${name}`);
    case "toggle": return verb(`toggle ${name}`);
    case "set": return `${name} to ${r.value} %`;
    case "dim_up": return verb(`dim ${name} brighter`);
    case "dim_down": return verb(`dim ${name} dimmer`);
    case "dim_cycle": return verb(`dim ${name}`);
    /* A rule somebody made is shown even when the group has since stopped offering
     * Adaptive Lighting - hiding it would hide why the button does nothing. */
    case "adaptive": return `${name} with Adaptive Lighting${g && g.adaptive?.expose === "off" ? " (off for this room)" : ""}`;
    default: return null;
  }
}

/* The condition a case's head puts on it. `subject` names the target, which a
 * case of several rules needs said: "while it is off" would not say which. */
function condText(r: Rule, subject: string | null): string {
  const it = subject ?? "it";
  switch (r.when) {
    case "below": return subject ? ` when ${subject} is under ${r.arg} %` : ` when under ${r.arg} %`;
    case "above": return subject ? ` when ${subject} is over ${r.arg} %` : ` when over ${r.arg} %`;
    case "on": return ` while ${it} is on`;
    case "off": return ` while ${it} is off`;
    case "sun_down": return " while the sun is down";
    default: return "";
  }
}

/* One rule as a sentence, condition included. */
export function ruleText(r: Rule, groups: Group[], devices: Device[]): string | null {
  const a = actionText(r, groups, devices, true);
  return a && a + condText(r, null);
}

/* A gesture's rules read out: the rules of a case joined with "and", the cases
 * with ", otherwise". */
export function eventText(rules: Rule[] | undefined, groups: Group[], devices: Device[]): string | null {
  const cases: string[] = [];
  const list = rules || [];
  for (let head = 0; head < list.length;) {
    let end = head + 1;
    while (end < list.length && list[end].and) end++;
    const members = list.slice(head, end);
    const texts: string[] = [];
    for (const r of members) {
      if (!hasTarget(r)) continue;
      /* Capitalised only at the start of the whole sentence. */
      const t = actionText(r, groups, devices, cases.length === 0 && texts.length === 0);
      if (t) texts.push(t);
    }
    if (texts.length) {
      const h = list[head];
      const subject = members.length > 1 ? targetName(h, groups, devices) : null;
      cases.push(texts.join(" and ") + condText(h, subject));
    }
    head = end;
  }
  return cases.length ? cases.join(", otherwise ") : null;
}

/* The rows of one gesture as they are saved. A row with no target is dropped,
 * and so is one that does nothing - except a head with rules joined to it,
 * which is how a case says "when the kitchen is off, [leave it], and switch the
 * reading lamp on". A head that is dropped hands its condition to the first of
 * its joined rules that survives, which becomes the head, so the rules after it
 * do not fall into the case before. The first rule never joins anything. */
export function cleanRules(rules: Rule[]): Rule[] {
  const out: Rule[] = [];
  let orphaned: Rule | null = null;       /* a dropped head whose case goes on */
  rules.forEach((r, i) => {
    const joinedAfter = i + 1 < rules.length && !!rules[i + 1].and;
    const keep = hasTarget(r) && (r.do !== "none" || (!r.and && joinedAfter));
    if (!keep) {
      if (!r.and) orphaned = r;
      return;
    }
    let row: Rule = { ...r };
    if (!r.and) orphaned = null;
    else if (orphaned) {
      row = { ...row, and: false, when: orphaned.when, arg: orphaned.arg };
      orphaned = null;
    }
    if (out.length === 0) row.and = false;
    const light = isLightRule(row);
    out.push({
      group: light ? 0 : row.group, ieee: light ? row.ieee! : null, ep: light ? row.ep! : null,
      and: !!row.and,
      /* A joined rule's condition is not read; saved as what it means. */
      when: row.and ? "always" : row.when, arg: row.and ? 0 : row.arg | 0,
      do: row.do, value: row.value | 0,
      delay: canWait(row.do) ? Math.max(0, Math.min(MAX_DELAY_S, (row.delay ?? 0) | 0)) : 0,
    });
  });
  return out;
}

/* How many of the button's rules its rows take up while being edited - every
 * row, filled in or not, except the one empty row a gesture with nothing on it
 * shows. Counted this way so the page stops offering rows before the pool is
 * full, not after somebody has filled them in. */
export const rowsUsed = (events: Partial<Record<string, Rule[]>>) =>
  Object.values(events).reduce((n, rules) => {
    const rows = rules || [];
    const empty = rows.length === 1 && !hasTarget(rows[0]) && rows[0].do === "none";
    return n + (empty ? 0 : rows.length);
  }, 0);

export function remoteName(ieee: string | null, devices: Device[]): string {
  const d = ieee && devices.find((x) => x.ieee === ieee);
  return d ? d.name : "a remote that is gone";
}
