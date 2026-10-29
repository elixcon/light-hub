/* One poll, one command chain, one map of optimistic values: the behaviour
 * CLAUDE.md's "State is never assumed" and "Commands go through a per-endpoint
 * gate" sections describe. */
import { SvelteMap } from "svelte/reactivity";
import { api, post } from "./api.ts";
import { toast } from "./toast.svelte.ts";
import type { Device, Endpoint, Group, ButtonDef, RemoteEvent, Status, StateResponse, StateUnchanged, AuthState } from "./types.ts";

export const IDLE_POLL = 2000, PAIR_POLL = 1000;

export const app = $state({
  status: null as Status | null,
  name: "Elixcon Light Hub",
  unreachable: false,
  firstLoad: true,
  pairing: false,
  capturing: false,   /* the button editor is listening for a remote press */
});

/* Reassigned per poll. Deep state, because a $state.raw object does not react
 * to its properties being reassigned - only to the variable being. */
export const data = $state({
  devices: [] as Device[],
  groups: [] as Group[],
  buttons: [] as ButtonDef[],
  remotes: [] as RemoteEvent[],
});

/* Optimistic values, cleared once the lamp agrees - on exact equality - or
 * when they get old, whichever comes first. Equality alone would leave a
 * command the lamp never carried out (a lost frame, an unplugged lamp) showing
 * on the switch and the slider indefinitely. The longest fade is 10 s and the
 * read-back that confirms it follows a few seconds later; past that the lamp's
 * own report is the truth, whatever the finger asked for. */
export const PENDING_TTL_MS = 12000;
export const pending = new SvelteMap<string, { v: unknown; at: number }>();
export const setPending = (k: string, v: unknown) => pending.set(k, { v, at: Date.now() });
export const seenIeee = new Set<string>();

let paused = false;              /* a firmware upload owns the single-threaded HTTP server */
let timer: ReturnType<typeof setTimeout> | undefined;
let bootVersion: string | null = null;
/* The revision the tables in `data` were built from, and when they arrived -
 * see /api/state?rev in docs/api.md. Between full replies the ages the tables
 * carry are moved along here, since the server is not asked for them. */
let rev: string | null = null;
let dataAt = 0;
/* One throttle per path: a shared timer would let a move on any slider cancel
 * the trailing send of every other, losing the value one finger stopped on. */
const throttles = new Map<string, { at: number; timer?: ReturnType<typeof setTimeout> }>();
let offEpoch = 0;                /* bumped on every explicit switch; stale nudges check it */
let onAuth: ((a: AuthState) => void) | null = null;
let isLocked: () => boolean = () => false;

export const setAuthHooks = (poll: (a: AuthState) => void, locked: () => boolean) => { onAuth = poll; isLocked = locked; };
export const getOffEpoch = () => offEpoch;
export function setPaused(b: boolean) { paused = b; if (b) clearTimeout(timer); }

export const key = (ieee: string, ep: number, what: string) => `${ieee}/${ep}/${what}`;
export const gkey = (id: number, what: string) => `group/${id}/${what}`;
const pendingOr = <T>(k: string, fallback: T): T => { const p = pending.get(k); return p ? (p.v as T) : fallback; };
export const value = <T>(ieee: string, ep: number, what: string, fallback: T): T => pendingOr(key(ieee, ep, what), fallback);
export const gvalue = <T>(id: number, what: string, fallback: T): T => pendingOr(gkey(id, what), fallback);

/* With a level curve in the way the lamp reports its mapped output, which is a
 * different number from the one the slider asked for and would never reconcile
 * with an optimistic value. Bind to what was asked for instead. */
export const epPct = (e: Endpoint) => (e.curves && e.curves.level ? e.input_pct : e.brightness_pct);
/* An endpoint that sends presses rather than answering commands. */
export const isRemoteEp = (e: Endpoint) => (e.features || []).includes("switch");

export function findEp(ieee: string, ep: number): { device: Device; endpoint: Endpoint } | null {
  const d = data.devices.find((x) => x.ieee === ieee);
  const e = d && d.endpoints.find((x) => x.ep === ep);
  return d && e ? { device: d, endpoint: e } : null;
}
export const findGroup = (id: number) => data.groups.find((g) => g.id === id);

/* One request here is up to three commands per light, so the gap grows with
 * the size of the group. */
export const groupInterval = (g: Group) => Math.min(600, 150 + 60 * g.members.length);

/* One command on the wire at a time.
 *
 * Two fetches issued close together can reach the coordinator in either order -
 * different sockets, and nothing promises the order they are serviced in. A
 * level arriving after an off turns the lamp straight back on, so the order the
 * user meant has to be the order the coordinator sees. The chain swallows
 * failures so one bad command cannot wedge the queue, while the promise handed
 * back still rejects so callers can report it. */
let cmdChain: Promise<unknown> = Promise.resolve();
export function sendCommand(path: string, payload: unknown): Promise<unknown> {
  const run = cmdChain.catch(() => {}).then(() => post(path, payload));
  cmdChain = run.catch(() => {});
  return run;
}

/* Dragging a slider would otherwise fire a request per pixel. Send at most one
 * every `every` ms, and always send the value the finger stopped on. */
export function sendThrottled(path: string, payload: unknown, every = 120) {
  const now = Date.now();
  const t = throttles.get(path) ?? { at: 0 };
  throttles.set(path, t);
  clearTimeout(t.timer);
  t.timer = undefined;
  if (now - t.at > every) {
    t.at = now;
    sendCommand(path, payload).catch((err) => toast(err.message));
  } else {
    t.timer = setTimeout(() => {
      t.at = Date.now();
      t.timer = undefined;
      sendCommand(path, payload).catch(() => {});
    }, every);
  }
}

/* A drag leaves a trailing send queued so the value the finger stopped on always
 * arrives. Switching a light off inside that window would otherwise let the
 * trailing level command land after the off - and move-to-level turns the lamp
 * back on. An explicit on/off is the newer intent, so it cancels the tail. */
export function cancelThrottled() {
  for (const t of throttles.values()) { clearTimeout(t.timer); t.timer = undefined; }
  offEpoch++;
}

async function switchTo(path: string, k: string, next: boolean, what: string) {
  cancelThrottled();
  setPending(k, next);
  try {
    await sendCommand(path, { state: next ? "on" : "off" });
    schedule(600);
  } catch (err: any) {
    pending.delete(k);
    toast(`Could not switch the ${what}: ${err.message}`);
  }
}

export const setOnOff = (ieee: string, ep: number, next: boolean) =>
  switchTo(`/api/devices/${ieee}/ep/${ep}/onoff`, key(ieee, ep, "on"), next, "light");

export const setGroupOnOff = (id: number, next: boolean) =>
  switchTo(`/api/groups/${id}/onoff`, gkey(id, "on"), next, "room");

/* The Adaptive Lighting button. An explicit intent, like a switch: it cancels a
 * trailing slider send and goes through the same chain, so a level still on its way
 * cannot land after it.
 *
 * Shown at once, like a switch, as the mode it starts: running, the room on, and at
 * what the sun says now, when the last poll knew that. The coordinator answers in a
 * few milliseconds and the lights start with it, but the poll that reports the mode
 * comes 600 ms after the answer - without this the button looked dead for most of a
 * second while the room was already fading in. */
export async function startAdaptive(g: Group) {
  cancelThrottled();
  const shown: [string, unknown][] = [[gkey(g.id, "adaptive"), true], [gkey(g.id, "on"), true]];
  const pct = g.adaptive?.pct;
  if (pct != null && pct > 0) shown.push([gkey(g.id, "pct"), pct]);
  /* A refusal puts back what was showing, not the poll's figures: a switch tapped
   * off a moment ago is still waiting for the lamps to agree. */
  const before = shown.map(([k]) => [k, pending.get(k)] as const);
  for (const [k, v] of shown) setPending(k, v);
  try {
    await sendCommand(`/api/groups/${g.id}/adaptive`, {});
    schedule(600);
  } catch (err: any) {
    for (const [k, p] of before) if (p) pending.set(k, p); else pending.delete(k);
    toast(`Could not start Adaptive Lighting: ${err.message}`);
  }
}

export async function openPairing() {
  try {
    await post("/api/permit_join", { duration: 180 });
    toast("Pairing open for 3 minutes");
    refresh();
  } catch (err: any) { toast(err.message); }
}

export function setLevel(ieee: string, ep: number, pct: number) {
  setPending(key(ieee, ep, "pct"), pct);
  setPending(key(ieee, ep, "on"), true);
  sendThrottled(`/api/devices/${ieee}/ep/${ep}/level`, { pct });
}

export function setGroupLevel(g: Group, pct: number) {
  setPending(gkey(g.id, "pct"), pct);
  setPending(gkey(g.id, "on"), true);
  sendThrottled(`/api/groups/${g.id}/level`, { pct }, groupInterval(g));
}

export async function refresh() {
  try {
    /* One request, not three. esp_http_server answers them one at a time, so
     * three polls every two seconds queue behind each other and spend airtime
     * the Zigbee mesh needs - see /api/state. */
    const r = await api<StateResponse | StateUnchanged>(rev ? `/api/state?rev=${encodeURIComponent(rev)}` : "/api/state");
    const { status, auth } = r;
    app.status = status;
    if (status.name && app.name !== status.name) {
      app.name = status.name;
      document.title = status.name;
    }
    /* The page reconnects across a reboot without reloading, on purpose, so an
     * upload never loses its progress bar - which also means a tab left open
     * across a firmware change goes on running the old UI against the new API.
     * Once different firmware answers, its own page is the one to show. Only a
     * change seen during this page's life counts, so it cannot loop. */
    const build = status.build || status.version;
    if (bootVersion === null) bootVersion = build;
    else if (build !== bootVersion) { location.reload(); return; }

    if ("unchanged" in r) {
      /* Nothing in the tables moved, so the optimistic values cannot have
       * settled either; only the ages tick on. */
      const now = Date.now();
      const dtS = Math.floor((now - dataAt) / 1000);
      if (dtS > 0) {
        for (const d of data.devices) {
          if (d.last_seen_s_ago >= 0) d.last_seen_s_ago += dtS;
          if (d.ota && d.ota.queried_s_ago != null) d.ota.queried_s_ago += dtS;
        }
        for (const e of data.remotes) e.ago_ms += dtS * 1000;
        for (const b of data.buttons) {
          if (b.last_event_ms != null) b.last_event_ms += dtS * 1000;
          /* And the countdowns run down: a sensor's no-motion time, a rule waiting. */
          if (b.clear_in_ms != null) b.clear_in_ms = Math.max(0, b.clear_in_ms - dtS * 1000);
          if (b.delayed) b.delayed.next_ms = Math.max(0, b.delayed.next_ms - dtS * 1000);
        }
        dataAt += dtS * 1000;
      }
      const cutoff = now - PENDING_TTL_MS;
      for (const [k, p] of pending) if (p.at < cutoff) pending.delete(k);
      app.unreachable = false;
      app.pairing = status.zigbee.permit_join_remaining_s > 0;
      onAuth?.(auth);
      schedule(app.pairing || app.capturing ? PAIR_POLL : IDLE_POLL);
      return;
    }
    const { devices, groups, buttons, remotes } = r;

    if (pending.size) {
      const settle = (k: string, v: unknown) => { const p = pending.get(k); if (p && p.v === v) pending.delete(k); };
      for (const d of devices) {
        for (const e of d.endpoints) {
          settle(key(d.ieee, e.ep, "on"), e.on);
          settle(key(d.ieee, e.ep, "pct"), epPct(e));
        }
      }
      for (const g of groups) {
        settle(gkey(g.id, "on"), g.on);
        settle(gkey(g.id, "pct"), g.brightness_pct);
        settle(gkey(g.id, "adaptive"), g.adaptive?.active);
      }
      /* Whatever the lamps still disagree with after this long is a command
       * that did not take; the poll's figures are the ones to show. */
      const cutoff = Date.now() - PENDING_TTL_MS;
      for (const [k, p] of pending) if (p.at < cutoff) pending.delete(k);
    }
    data.devices = devices;
    data.groups = groups;
    data.buttons = buttons || [];
    data.remotes = remotes || [];   /* newest first */
    rev = r.rev ?? null;
    dataAt = Date.now();
    app.unreachable = false;
    app.pairing = status.zigbee.permit_join_remaining_s > 0;
    onAuth?.(auth);
    /* Everything present on the first poll was there before this page; a device
     * that turns up on a later one is "just joined" until the page is reloaded. */
    if (app.firstLoad) for (const d of devices) seenIeee.add(d.ieee);
    app.firstLoad = false;
    schedule(app.pairing || app.capturing ? PAIR_POLL : IDLE_POLL);
  } catch {
    if (isLocked()) return;   /* the lock screen has taken over, and polls on its own */
    app.unreachable = true;
    schedule(3000);
  }
}

export function schedule(ms: number) {
  clearTimeout(timer);
  /* esp_http_server serves one request at a time, so a poll issued during a
   * firmware upload just queues behind it and times out - the UI would paint
   * "Coordinator unreachable" in the middle of a working update. */
  if (!paused) timer = setTimeout(refresh, ms);
}

