/* A stand-in coordinator for looking at the UI without rebooting the board:
 * the JSON shapes of http_server.c, a little state, no radio. */
import http from "node:http";
import { parseHhmm, sampleAt, usedDay } from "../src/lib/adaptive.ts";

const devices = [
  { ieee: "0x001788010bd6f2a1", short_addr: 0x046b, name: "Sofa strip", manufacturer: "Signify Netherlands B.V.", model: "LST002", online: true, last_seen_s_ago: 4, lqa: 114,
    endpoints: [{ ep: 11, device_id: 0x0210, features: ["onoff", "brightness", "color_temp", "color"], on: true, brightness_pct: 61, kelvin: 2700, min_kelvin: 2000, max_kelvin: 6500, hue: 30, sat: 60, color_mode: "color_temp", curves: { level: true, chroma: true }, input_pct: 40 }] },
  { ieee: "0x7cb03eaa0a0c5e12", short_addr: 0xa2b4, name: "Desk spot", manufacturer: "OSRAM", model: "PAR16 50 TW", online: true, last_seen_s_ago: 9, lqa: 98,
    endpoints: [{ ep: 3, device_id: 0x0220, features: ["onoff", "brightness", "color_temp"], on: false, brightness_pct: 30, kelvin: 3000, min_kelvin: 2700, max_kelvin: 6500, hue: null, sat: null, color_mode: "color_temp", curves: { level: false, chroma: false }, input_pct: null }] },
  { ieee: "0x00158d0002a1b2c3", short_addr: 0x4b86, name: "Hall relay", manufacturer: "Sonoff", model: "ZBMINI", online: false, last_seen_s_ago: 900, lqa: 60,
    endpoints: [{ ep: 1, device_id: 0x0100, features: ["onoff"], on: null, brightness_pct: null, kelvin: null, min_kelvin: null, max_kelvin: null, hue: null, sat: null, color_mode: null, curves: { level: false, chroma: false }, input_pct: null }] },
  { ieee: "0x00178801095e4149", short_addr: 0x4149, name: "Hue button", manufacturer: "Signify Netherlands B.V.", model: "ROM001", online: true, last_seen_s_ago: 120, lqa: null,
    endpoints: [{ ep: 1, device_id: 0x0830, features: ["switch"], on: null, brightness_pct: null, kelvin: null, min_kelvin: null, max_kelvin: null, hue: null, sat: null, color_mode: null, curves: { level: false, chroma: false }, input_pct: null }] },
  /* A Matter lamp behind a hub: a made-up address, and where it really is. */
  { ieee: "0x025a42434d000001", short_addr: 0, name: "Kitchen ceiling", manufacturer: "Signify Netherlands B.V.", model: "Hue color lamp", online: true, last_seen_s_ago: 20, lqa: null,
    source: "matter", id: 1, node_id: "80106DE83124FF1E", remote_ep: 12, vendor_id: 0x100b, product_id: 3, bridged: true, verified: false, needs_pairing: false, ota: null,
    endpoints: [{ ep: 1, device_id: 0, features: ["onoff", "brightness", "color_temp", "color"], on: true, brightness_pct: 80, kelvin: 3000, min_kelvin: 2000, max_kelvin: 6500, hue: 30, sat: 50, color_mode: "color_temp", curves: { level: false, chroma: false }, input_pct: null }] },
];
/* A pairing walks through the firmware's steps, a second or so each. */
const PAIR_STEPS = ["find", "pase", "arm", "csr", "root", "noc", "case", "complete", "read"];
let pairing = { state: "idle", step: "idle", error: "", node_id: "", lamps: 0, s_ago: 0, started: 0 };
function pairingNow() {
  if (pairing.state !== "running") return pairing;
  const i = Math.floor((Date.now() - pairing.started) / 700);
  if (i < PAIR_STEPS.length) return { ...pairing, step: PAIR_STEPS[i] };
  pairing = { ...pairing, state: "done", step: "done", node_id: "C09E35354A5B1A02", lamps: 1 };
  return pairing;
}
/* A light on a pin: listed as a device under a made-up address, wired through /api/lights. */
const wiredEp = () => ({ ep: 1, device_id: 0, features: ["onoff"], on: false, brightness_pct: null, kelvin: null, min_kelvin: null, max_kelvin: null, hue: null, sat: null, color_mode: null, curves: { level: false, chroma: false }, input_pct: null });
const wired = (id, name, gpio) => refreshWired({ ieee: `0x025a42434c00${id.toString(16).padStart(4, "0")}`, short_addr: 0, name, manufacturer: "", model: "", online: false, last_seen_s_ago: 0, lqa: null,
  source: "gpio", id, kind: "onoff", gpio, gpio_warm: null, active_low: false, power_on: "off", pwm_hz: 4882, pwm_phase: null, cold_kelvin: 6500, warm_kelvin: 2700, gpio_neutral: null, neutral_kelvin: 4000, mix: "constant", mix_curves: { cold: null, neutral: null, warm: null }, endpoints: [wiredEp()] });
/* What http_server.c derives from the wiring: the model line and, for tunable white, the abilities. */
function refreshWired(d) {
  const e = d.endpoints[0], three = d.kind === "cwnwww", cwww = d.kind === "cwww" || three, dims = cwww || d.kind === "dim";
  d.online = d.gpio != null && (!cwww || d.gpio_warm != null) && (!three || d.gpio_neutral != null);
  d.model = !d.online ? "" : three ? `GPIO ${d.gpio} + ${d.gpio_neutral} + ${d.gpio_warm}` : cwww ? `GPIO ${d.gpio} + ${d.gpio_warm}` : `GPIO ${d.gpio}`;
  e.features = cwww ? ["onoff", "brightness", "color_temp"] : dims ? ["onoff", "brightness"] : ["onoff"];
  e.min_kelvin = cwww ? d.warm_kelvin : null; e.max_kelvin = cwww ? d.cold_kelvin : null;
  e.color_mode = cwww ? "color_temp" : null;
  e.brightness_pct = dims ? e.brightness_pct ?? 60 : null;
  e.kelvin = cwww ? e.kelvin ?? 4000 : null;
  return d;
}
/* What each device's OTA client said. The PAR16 and the Hue button run versions
 * the real Zigbee2MQTT index has newer images for; the strip has not said yet. */
devices[0].ota = { ep: 11, manufacturer_code: null, image_type: null, file_version: null, hw_version: null, queried_s_ago: null };
devices[1].ota = { ep: 3, manufacturer_code: 0x110c, image_type: 3, file_version: 0x01020509, hw_version: null, queried_s_ago: 3600 };
devices[2].ota = { ep: null, manufacturer_code: null, image_type: null, file_version: null, hw_version: null, queried_s_ago: null };
devices[3].ota = { ep: 1, manufacturer_code: 0x1015, image_type: 913, file_version: 0x00020003, hw_version: 1, queried_s_ago: 86000 };
/* A remote that has said nothing since boot: asking it answers only after the
 * "press" the page asks for, faked as eight seconds. The identity is made up. */
devices.push({ ieee: "0xec1bbdfffe397735", short_addr: 0x7c21, name: "Sound controller", manufacturer: "IKEA of Sweden", model: "SYMFONISK Sound Controller", online: true, last_seen_s_ago: 3000, lqa: null,
  endpoints: [{ ep: 1, device_id: 0x0820, features: ["switch"], on: null, brightness_pct: null, kelvin: null, min_kelvin: null, max_kelvin: null, hue: null, sat: null, color_mode: null, curves: { level: false, chroma: false }, input_pct: null }],
  ota: { ep: null, manufacturer_code: null, image_type: null, file_version: null, hw_version: null, queried_s_ago: null } });
/* A Hue dimmer as an older build recorded it: its battery endpoint 2 taken for
 * a second remote. The page must still show one remote. */
const remoteEp = (ep, device_id) => ({ ep, device_id, features: ["switch"], on: null, brightness_pct: null, kelvin: null, min_kelvin: null, max_kelvin: null, hue: null, sat: null, color_mode: null, curves: { level: false, chroma: false }, input_pct: null });
devices.push({ ieee: "0x00178801086a4064", short_addr: 0x938b, name: "Hallway dimmer", manufacturer: "Philips", model: "RWL021", online: true, last_seen_s_ago: 600, lqa: null,
  endpoints: [remoteEp(1, 0x0830), remoteEp(2, 0x000c)],
  ota: { ep: null, manufacturer_code: null, image_type: null, file_version: null, hw_version: null, queried_s_ago: null } });
const sleepy = (d) => d.endpoints.every((e) => e.features.includes("switch"));
devices.push(wired(1, "Porch lamp", 4));
devices.push(Object.assign(wired(2, "Desk strip", 18), { kind: "cwww", gpio_warm: 19, pwm_hz: 9765, pwm_phase: 0 }));
refreshWired(devices[devices.length - 1]);
devices.push(Object.assign(wired(3, "Shelf strip", 20), { kind: "dim", pwm_hz: 4882, pwm_phase: 0 }));
refreshWired(devices[devices.length - 1]);
devices.push(Object.assign(wired(4, "Ceiling panel", 21), { kind: "cwnwww", gpio_neutral: 22, gpio_warm: 23, pwm_hz: 4882, pwm_phase: 0 }));
refreshWired(devices[devices.length - 1]);
const groups = [
  { id: 1, name: "Living room", members: [{ ieee: devices[0].ieee, ep: 11, name: "Sofa strip", online: true, brightness: true, on: true, brightness_pct: 61 }, { ieee: devices[1].ieee, ep: 3, name: "Desk spot", online: true, brightness: true, on: false, brightness_pct: 30 }], on: true, brightness_pct: 40 },
  { id: 2, name: "Hall", members: [{ ieee: devices[2].ieee, ep: 1, name: "Hall relay", online: false, brightness: false, on: null, brightness_pct: null }], on: null, brightness_pct: null },
];
const buttons = [
  { id: 1, name: "BOOT button", source: "gpio", ieee: null, ep: null, key: null, gpio: 9, inverted: true, pull: "up", pressed: false, level: 1, last_event_ms: null,
    events: { short: [{ group: 1, when: "always", arg: 30, do: "toggle", value: 60 }], long: [{ group: 1, when: "always", arg: 30, do: "dim_cycle", value: 6 }], double: [{ group: 1, when: "below", arg: 30, do: "set", value: 80 }, { group: 1, when: "always", arg: 30, do: "set", value: 30 }] } },
  { id: 2, name: "Hue button", source: "zigbee", ieee: devices[3].ieee, ep: 1, key: 0, gpio: null, inverted: false, pull: "none", pressed: null, level: null, last_event_ms: 45000,
    events: { short: [{ group: 1, when: "always", arg: 30, do: "toggle", value: 60 }], long: [], double: [] } },
];
const curves = {
  [`${devices[0].ieee}/11`]: { level: { points: [[1, 1], [25, 6], [50, 20], [75, 50], [100, 100]] }, chroma: { stops: [{ at: 1, hue: 12, sat: 90 }, { at: 35, kelvin: 2200 }, { at: 100, kelvin: 5000 }] } },
};
let permit = 0, seq = 10, boot = Date.now();
const remotes = [{ seq: 9, ieee: devices[3].ieee, ep: 1, key: 0, event: "click", ago_ms: 45000, name: "Hue button" }];
const auth = { mode: "open", authorized: true, keys: 0, failsafe_left_s: 0, current: null };
/* The one Zigbee device image. MOCK_LAYOUT=old shows a box whose updater has
 * not made room yet. A notified device "downloads" it over a minute. */
const zota = { layout: process.env.MOCK_LAYOUT === "old" ? "old" : "current", image: null, target: null, transfer: null, queries: 3, refused: 3, error: null,
  last_query: { ieee: devices[1].ieee, short_addr: 0xa2b4, manufacturer_code: 0x110c, image_type: 3, file_version: 0x01020509, s_ago: 3600, answer: "no image stored" } };
const ROOM = 524288;
function zotaStatus() {
  const t = zota.transfer, now = Date.now();
  if (t && t.status === null) {
    t.offset = Math.min(t.size, Math.round(((now - t.started) / 60000) * t.size));
    if (t.offset >= t.size) { t.status = 0; zota.target = null; const d = devices.find((x) => x.ieee === t.ieee); if (d?.ota) d.ota.file_version = t.to_version; }
  }
  const state = zota.layout === "old" ? "no_partition" : t && t.status === null ? "serving" : !zota.image ? "idle" : t ? (t.status === 0 ? "finished" : "failed") : "ready";
  return { state, room: zota.layout === "old" ? 0 : ROOM, image: zota.image, error: zota.error, target: zota.target,
    transfer: t && { ieee: t.ieee, short_addr: t.short_addr, from_version: t.from_version, to_version: t.to_version, device_version: devices.find((x) => x.ieee === t.ieee)?.ota?.file_version ?? null, offset: t.offset, size: t.size,
      started_s_ago: Math.round((now - t.started) / 1000), progress_s_ago: 0, status: t.status },
    queries: zota.queries, refused: zota.refused, last_query: zota.last_query };
}
const rawBody = (req) => new Promise((r) => { const parts = []; req.on("data", (c) => parts.push(c)); req.on("end", () => r(Buffer.concat(parts))); });

/* GET /mock/ap puts the page in setup-hotspot mode, where a network is checked
 * before it is saved: password "wrong" is refused, a name not in the scan is not
 * found, anything else joins after 3 s. */
let wifiMode = "sta";
let wifiCheck = null;
const wifiCheckState = () => {
  if (!wifiCheck) return { state: "idle", ssid: "", reason: 0, ip: null };
  const c = wifiCheck, done = Date.now() - c.at > 3000;
  const state = !done ? "checking" : c.password === "wrong" ? "wrong_password"
    : !["elixcon", "Guest"].includes(c.ssid) ? "not_found" : "joined";
  const reason = { wrong_password: 15, not_found: 201 }[state] ?? 0;
  return { state, ssid: c.ssid, reason, ip: state === "joined" ? "192.168.1.77" : null };
};
/* The Zigbee channel and moving it (zb_move_channel): announced for 9 s, then
 * the coordinator follows. The survey runs 12 s; its readings are the dev
 * board's of 2026-10-05, with a busier 25 for the Wi-Fi on 13. */
let zbChannel = 20;
let zbMove = null;
const moveNow = () => {
  if (!zbMove) return { state: "idle" };
  const t = (Date.now() - zbMove.at) / 1000;
  if (t >= 10 && zbChannel !== zbMove.to) zbChannel = zbMove.to;
  const state = t < 9 ? "announced" : t < 10 ? "moving" : "moved";
  return { state, from: zbMove.from, to: zbMove.to, boot: false, s_ago: Math.round(state === "moved" ? t - 10 : t) };
};
let zbSurvey = null;
const SURVEY_DBM = { 11: [-92, -92, -92, -91], 12: [-91, -92, -91, -91], 13: [-91, -90, -91, -91], 14: [-90, -90, -90, -90],
  15: [-88, -89, -90, -90], 16: [-89, -89, -89, -89], 17: [-89, -89, -89, -88], 18: [-88, -90, -89, -88],
  19: [-88, -89, -89, -89], 20: [-85, -89, -89, -89], 21: [-88, -89, -89, -90], 22: [-88, -88, -96, -89],
  23: [-88, -87, -88, -88], 24: [-86, -88, -84, -85], 25: [-75, -80, -72, -62], 26: [-86, -87, -88, -86] };
const surveyNow = () => {
  if (!zbSurvey) return { state: "idle", error: null, passes: 0, s_ago: null, channels: [], networks: [] };
  const t = (Date.now() - zbSurvey) / 1000, done = t >= 12, passes = Math.min(4, Math.floor(t / 2.7));
  return { state: done ? "done" : "running", error: null, passes: done ? 4 : passes, s_ago: Math.round(done ? t - 12 : t),
    channels: Object.entries(SURVEY_DBM).map(([c, d]) => ({ channel: Number(c), dbm: d.slice(0, done ? 4 : passes) })),
    networks: done ? [{ channel: 11, pan_id: 6754, ext_pan_id: "0xdddddddddddddddd", permit_join: false },
      { channel: 15, pan_id: 2072, ext_pan_id: "0x2380c446750c8a81", permit_join: false },
      { channel: 20, pan_id: 21086, ext_pan_id: "0x98a316fffe717cfc", permit_join: false }] : [] };
};
const status = () => ({ name: "Living Room Hub", wifi: { mode: wifiMode, connected: true, ssid: "elixcon", ip: "192.168.1.42", hostname: "living-room-hub", rssi: -61, channel: 6, weak_uplink: false, temporary_hotspot: false },
  zigbee: { network_up: true, pan_id: 0x5fb3, channel: (moveNow(), zbChannel), channel_move: moveNow(), permit_join_remaining_s: Math.max(0, Math.round((permit - Date.now()) / 1000)), ieee: "0x404ccafffe5a1b2c", radio: { cmd_undelivered: 0, read_undelivered: 1, went_quiet: 0, corrections: 0, gate_timeouts: 0 }, ota: zotaStatus() },
  device_count: devices.length, uptime_s: Math.round((Date.now() - boot) / 1000) + 5000, version: "mock-1", build: "deadbeef", ota: { partition: "app", pending_verify: false, updater: "mock-updater-1", updater_valid: true, updater_problem: null, layout: zota.layout }, heap: { free: 120000, min_free: 84000, largest: 82000 },
  thermal: thermalCfg.gpio === null ? { state: "off", temp_c: null, max_c: null, trip_c: thermalCfg.trip_c, release_c: thermalCfg.trip_c - 5, trips: 0, mv: -1, held: false }
    : { state: 41.3 >= thermalCfg.trip_c ? "hot" : "ok", temp_c: 41.3, max_c: 52, trip_c: thermalCfg.trip_c, release_c: thermalCfg.trip_c - 5, trips: 0, mv: 1120, held: 41.3 >= thermalCfg.trip_c } });
let thermalCfg = { gpio: null, b: 3350, r0_ohm: 10000, rs_ohm: 10000, ntc_to: "gnd", trip_c: 75, adc_pins: [0, 1, 2, 3, 4, 5, 6] };

/* Adaptive Lighting: a fixed sun (Munich in October, in the mock's own time
 * zone), the profile, its limits and the zone, and "now" from the page's own
 * mirror of the firmware - a stand-in, so the dot always sits on the line. */
let location = { lat: 48.137, lon: 11.575 };
let adaptive = { k_midnight: -1500, k_noon: 500, k_base: 4000, pct_midnight: 20, pct_noon: 100,
  sunrise: { earliest: null, latest: null }, sunset: { earliest: null, latest: null }, pct_points: [], k_points: [] };
let timeZone = null;
function sunToday() {
  const d = new Date(), at = (h, m) => new Date(d.getFullYear(), d.getMonth(), d.getDate(), h, m).getTime() / 1000;
  return { rise: at(7, 25), noon: at(13, 3), set: at(18, 40), polar: 0 };
}
function adaptiveNow() {
  const now = Math.floor(Date.now() / 1000), sun = sunToday();
  const prof = { ...adaptive, sunrise: [parseHhmm(adaptive.sunrise.earliest), parseHhmm(adaptive.sunrise.latest)],
    sunset: [parseHhmm(adaptive.sunset.earliest), parseHhmm(adaptive.sunset.latest)] };
  const off = timeZone ? -new Date().getTimezoneOffset() * 60 : null;
  const used = usedDay(sun, now, prof, off), s = sampleAt(sun, now, prof, off);
  return { time_utc: now, sun: { ...sun, al_rise: used.rise, al_set: used.set },
    time_zone: timeZone && { ...timeZone, offset_s: -new Date().getTimezoneOffset() * 60 },
    adaptive_now: { pct: s.pct, offset_k: s.offsetK } };
}
const settings = () => ({ backup: true, transition_ms: 800, transition_max_ms: 10000, name: "Living Room Hub", hostname: "living-room-hub",
  name_max_len: 31, thermal: thermalCfg, zigbee_channel: zbChannel, location,
  adaptive: { ...adaptive, offset_limit_k: 3000, base_min_k: 1500, base_max_k: 8000, max_points: 4 }, ...adaptiveNow() });

const json = (res, code, body) => { res.writeHead(code, { "Content-Type": "application/json", "Cache-Control": "no-store" }); res.end(JSON.stringify(body)); };
const body = (req) => new Promise((r) => { let s = ""; req.on("data", (c) => (s += c)); req.on("end", () => { try { r(JSON.parse(s || "{}")); } catch { r({}); } }); });
const findEp = (ieee, ep) => { const d = devices.find((x) => x.ieee === ieee); return d && d.endpoints.find((e) => e.ep === Number(ep)); };
const syncGroup = (g) => { for (const m of g.members) { const e = findEp(m.ieee, m.ep); if (e) { m.on = e.on; m.brightness_pct = e.brightness_pct; } } g.on = g.members.some((m) => m.on) ? true : g.members.every((m) => m.on === null) ? null : false; };

http.createServer(async (req, res) => {
  const u = new URL(req.url, "http://x"); const p = u.pathname.split("/").filter(Boolean); const m = req.method;
  await new Promise((r) => setTimeout(r, 40));
  if (u.pathname === "/api/state") { groups.forEach(syncGroup); return json(res, 200, { status: status(), devices, groups, buttons, remotes, auth }); }
  if (u.pathname === "/api/status") return json(res, 200, status());
  if (u.pathname === "/api/auth") return json(res, 200, auth);
  if (u.pathname === "/api/auth/keys" && m === "GET") return json(res, 200, { keys: [{ id: 1, kind: "browser", name: "Chrome on macOS", last_used_s_ago: 3, current: true }, { id: 2, kind: "api", name: "Home Assistant", last_used_s_ago: -1, current: false }], max: 8 });
  if (u.pathname === "/api/auth/keys" && m === "POST") return json(res, 201, { id: 3, name: (await body(req)).name, issued: true, key: "K7M2Q-9XH4T-R3WZP-6NBVA" });
  if (u.pathname === "/api/auth/session") { auth.mode = "locked"; auth.keys = 1; auth.current = { id: 1, name: "Chrome" }; return json(res, 201, { id: 1, name: "Chrome on macOS", issued: true, key: "K7M2Q-9XH4T-R3WZP-6NBVA" }); }
  if (u.pathname === "/api/settings" && m === "GET") return json(res, 200, settings());
  if (u.pathname === "/api/settings" && m === "POST") { const b = await body(req);
    if (b.zigbee_channel !== undefined) {
      const st = moveNow().state;
      if (st === "announced" || st === "moving" || (zbSurvey && Date.now() - zbSurvey < 12000)) return json(res, 409, { error: "a channel move or survey is in progress" });
      if (b.zigbee_channel !== zbChannel) zbMove = { at: Date.now(), from: zbChannel, to: b.zigbee_channel };
    }
    if (b.adaptive) {
      const win = (w, was) => w === undefined ? was : w === null ? { earliest: null, latest: null } : { ...was, ...w };
      const next = { ...adaptive, ...b.adaptive, sunrise: win(b.adaptive.sunrise, adaptive.sunrise), sunset: win(b.adaptive.sunset, adaptive.sunset) };
      const zone = b.time_zone !== undefined ? b.time_zone : timeZone;
      const limits = [next.sunrise.earliest, next.sunrise.latest, next.sunset.earliest, next.sunset.latest].some((v) => v !== null);
      if (limits && !zone) return json(res, 400, { error: "sunrise and sunset limits are local clock times: send time_zone with them" });
      adaptive = next;
    }
    if (b.time_zone !== undefined) timeZone = b.time_zone;
    if (b.location !== undefined) location = b.location;
    if (b.thermal) thermalCfg = { ...thermalCfg, ...b.thermal, adc_pins: thermalCfg.adc_pins }; return json(res, 200, { ...settings(), transition_ms: b.transition_ms ?? 800,
      ...(b.name !== undefined ? { name: b.name, hostname: (b.name || "living-room-hub").toLowerCase().replace(/[^a-z0-9]+/g, "-") } : {}) }); }
  if (u.pathname === "/api/matter") return json(res, 200, { running: true, fabrics: 3,
    fabric_list: [{ index: 1, vendor_id: "0x1349", label: "Home", ours: false }, { index: 2, vendor_id: "0x1384", label: "", ours: false }, { index: 3, vendor_id: "0xFFF1", label: "Elixcon Light Hub lamps", ours: true }],
    window_open: true, groups: 2, group_list: [], manual_code: "34970112332", qr_code: "MT:Y.K9042C00KA0648G00",
    controller: { running: true, fabric_index: 3, fabric_error: "", node_id: "E0B853D7E2D7B9E5",
      nodes: [{ node_id: "80106DE83124FF1E", lamps: 1, online: true, subscribed: true, drops: 0, reports: 12, last_seen_s_ago: 20, down_s: 0, last_error: "" }],
      pairing: pairingNow() } });
  if (u.pathname === "/api/matter/window") return json(res, 200, { ok: true });
  if (u.pathname === "/api/matter/lamps/pair" && m === "POST") {
    const code = String((await body(req)).code ?? "").replace(/[\s-]/g, "");
    if (!/^(\d{11}|\d{21}|MT:.+)$/.test(code)) return json(res, 400, { error: "that is not a Matter pairing code" });
    if (pairingNow().state === "running") return json(res, 409, { error: "a pairing is already running" });
    /* A code ending in 0 stands for a device that is not in pairing mode. */
    pairing = { state: "running", step: "find", error: "", node_id: "", lamps: 0, s_ago: 0, started: Date.now() };
    if (code.endsWith("0")) pairing = { ...pairing, state: "failed", step: "failed", error: "no device in pairing mode with that code was found on the network" };
    return json(res, 202, { ok: true });
  }
  if (p[0] === "api" && p[1] === "matter" && p[2] === "nodes" && p[4] === "rescan") return json(res, 202, { ok: true });
  if (u.pathname === "/mock/ap") { wifiMode = "ap"; return json(res, 200, { ok: true }); }
  if (u.pathname === "/api/wifi/test" && m === "POST") {
    if (wifiMode !== "ap") return json(res, 409, { error: "a network can only be checked from the setup hotspot" });
    const b = await body(req); wifiCheck = { ssid: b.ssid, password: b.password, at: Date.now() }; return json(res, 202, { ok: true });
  }
  if (u.pathname === "/api/wifi/test") return json(res, 200, wifiCheckState());
  if (u.pathname === "/api/wifi" && m === "POST") return json(res, 200, { ok: true });
  if (u.pathname === "/api/wifi/scan") return json(res, 200, { networks: [{ ssid: "elixcon", rssi: -61, channel: 6, second: null, open: false },
    { ssid: "elixcon", rssi: -79, channel: 13, second: null, open: false }, { ssid: "Guest", rssi: -75, channel: 11, second: null, open: true },
    { ssid: "FRITZ!Box 7590 XY", rssi: -83, channel: 1, second: "above", open: false }] });
  if (u.pathname === "/api/zigbee/scan" && m === "POST") {
    if (zbSurvey && Date.now() - zbSurvey < 12000) return json(res, 409, { error: "the Zigbee network is not up, or a channel move or survey is in progress" });
    zbSurvey = Date.now(); return json(res, 202, { ok: true });
  }
  if (u.pathname === "/api/zigbee/scan") return json(res, 200, surveyNow());
  if (u.pathname === "/api/permit_join") { permit = Date.now() + (await body(req)).duration * 1000; return json(res, 202, { ok: true }); }
  if (p[0] === "api" && p[1] === "devices") {
    const ieee = p[2], e = findEp(ieee, p[4]);
    if (p[3] === "ep" && p[5] === "curves") {
      const k = `${ieee}/${p[4]}`;
      if (m === "GET") return json(res, 200, { level: curves[k]?.level ?? null, chroma: curves[k]?.chroma ?? null, input_pct: e?.input_pct ?? null });
      const b = await body(req); curves[k] = b; if (e) e.curves = { level: !!b.level, chroma: !!b.chroma }; return json(res, 200, { ok: true });
    }
    if (p[3] === "ep" && e) {
      const b = await body(req);
      if (p[5] === "onoff") e.on = b.state === "toggle" ? !e.on : b.state === "on";
      if (p[5] === "level") { e.on = true; if (e.curves.level) { e.input_pct = b.pct; e.brightness_pct = Math.round(b.pct * 0.6); } else e.brightness_pct = b.pct; }
      if (p[5] === "color_temp") { e.kelvin = b.kelvin; e.color_mode = "color_temp"; }
      if (p[5] === "color") { e.hue = b.hue; e.sat = b.sat; e.color_mode = "color"; }
      return json(res, 202, { ok: true });
    }
    if (m === "PATCH") { const d = devices.find((x) => x.ieee === ieee); if (d) d.name = (await body(req)).name; return json(res, 200, { ok: true }); }
    if (m === "DELETE") { const i = devices.findIndex((x) => x.ieee === ieee); if (i >= 0) devices.splice(i, 1); return json(res, 200, { ok: true }); }
  }
  if (p[0] === "api" && p[1] === "groups") {
    if (!p[2] && m === "POST") { const g = { id: groups.length + 1, name: (await body(req)).name, members: [], on: null, brightness_pct: null }; groups.push(g); return json(res, 201, { id: g.id }); }
    const g = groups.find((x) => x.id === Number(p[2])); if (!g) return json(res, 404, { error: "no such group" });
    if (p[3] === "onoff") { const b = await body(req); for (const mm of g.members) { const e = findEp(mm.ieee, mm.ep); if (e) e.on = b.state === "on"; } return json(res, 202, { ok: true }); }
    if (p[3] === "level") { const b = await body(req); g.brightness_pct = b.pct; for (const mm of g.members) { const e = findEp(mm.ieee, mm.ep); if (e && mm.brightness) { e.on = true; e.brightness_pct = b.pct; e.input_pct = b.pct; } } return json(res, 202, { ok: true }); }
    if (p[3] === "members" && m === "POST") { const b = await body(req); const d = devices.find((x) => x.ieee === b.ieee); const e = findEp(b.ieee, b.ep); g.members.push({ ieee: b.ieee, ep: b.ep, name: d.name, online: d.online, brightness: e.features.includes("brightness"), on: e.on, brightness_pct: e.brightness_pct }); return json(res, 200, { ok: true }); }
    if (p[3] === "members" && m === "DELETE") { g.members = g.members.filter((x) => !(x.ieee === p[4] && x.ep === Number(p[5]))); return json(res, 200, { ok: true }); }
    if (m === "PATCH") { g.name = (await body(req)).name; return json(res, 200, { ok: true }); }
    if (m === "DELETE") { groups.splice(groups.indexOf(g), 1); return json(res, 200, { ok: true }); }
  }
  if (p[0] === "api" && p[1] === "lights") {
    if (!p[2] && m === "POST") { const id = Math.max(0, ...devices.filter((d) => d.source === "gpio").map((d) => d.id)) + 1; const d = wired(id, (await body(req)).name, null); devices.push(d); return json(res, 201, { id, ieee: d.ieee, ep: 1 }); }
    const d = devices.find((x) => x.source === "gpio" && x.id === Number(p[2])); if (!d) return json(res, 404, { error: "unknown light" });
    if (m === "PATCH") {
      const b = await body(req);
      if (b.gpio === 9) return json(res, 400, { error: "that pin is watched for the failsafe unlock, so it cannot drive a light" });
      if (b.gpio != null && buttons.some((x) => x.gpio === b.gpio)) return json(res, 409, { error: "a button already uses that pin" });
      if (b.pwm_hz != null && (b.pwm_hz < 100 || b.pwm_hz > 19531)) return json(res, 400, { error: "pwm_hz must be 100-19531" });
      const next = { ...d, ...b };
      if (next.warm_kelvin >= next.cold_kelvin) return json(res, 400, { error: "the warm white has to be warmer - fewer kelvin - than the cold white" });
      if ((next.kind === "cwww" || next.kind === "cwnwww") && next.gpio != null && next.gpio === next.gpio_warm) return json(res, 400, { error: "the cold and warm white need two different pins" });
      if (next.kind === "cwnwww" && (next.neutral_kelvin <= next.warm_kelvin || next.neutral_kelvin >= next.cold_kelvin)) return json(res, 400, { error: "the neutral white has to lie between the warm and the cold white" });
      if (b.mix_curves) b.mix_curves = { ...d.mix_curves, ...b.mix_curves };
      Object.assign(d, b); refreshWired(d); return json(res, 200, { ok: true });
    }
  }
  if (p[0] === "api" && p[1] === "buttons") {
    if (!p[2] && m === "POST") { const b = { id: buttons.length + 1, name: (await body(req)).name, source: "gpio", ieee: null, ep: null, key: null, gpio: null, inverted: true, pull: "up", pressed: null, level: null, last_event_ms: null, events: { short: [], long: [], double: [], triple: [] } }; buttons.push(b); return json(res, 201, b); }
    const b = buttons.find((x) => x.id === Number(p[2])); if (!b) return json(res, 404, { error: "no such button" });
    if (p[3] === "test") { return json(res, 202, { ok: true }); }
    if (m === "PATCH") { Object.assign(b, await body(req)); return json(res, 200, { ok: true }); }
    if (m === "DELETE") { buttons.splice(buttons.indexOf(b), 1); return json(res, 200, { ok: true }); }
  }
  if (u.pathname === "/api/zigbee/ota" && m === "GET") return json(res, 200, zotaStatus());
  if (u.pathname === "/api/zigbee/ota/image" && m === "POST") {
    if (zota.layout === "old") return json(res, 409, { error: "this flash has no zb_ota partition yet - see Settings" });
    const buf = await rawBody(req); const at = buf.indexOf(Buffer.from([0x1e, 0xf1, 0xee, 0x0b]));
    if (at < 0) { zota.error = "not a Zigbee OTA file (no 0x0BEEF11E header)"; return json(res, 400, { error: zota.error }); }
    const total = buf.readUInt32LE(at + 52);
    if (total > ROOM) return json(res, 400, { error: `the image is ${Math.ceil(total / 1024)} KB, and the coordinator holds up to ${ROOM / 1024} KB` });
    zota.image = { manufacturer_code: buf.readUInt16LE(at + 10), image_type: buf.readUInt16LE(at + 12), file_version: buf.readUInt32LE(at + 14), stack_version: 2, size: total,
      header: buf.subarray(at + 20, at + 52).toString("latin1").replace(/\0.*$/s, ""), crc: "0badc0de" };
    zota.error = null; if (zota.transfer?.status !== null) zota.transfer = null;
    return json(res, 200, { ok: true, ota: zotaStatus() });
  }
  if (u.pathname === "/api/zigbee/ota/image" && m === "DELETE") { zota.image = null; zota.transfer = null; return json(res, 200, { ok: true }); }
  if (u.pathname === "/api/zigbee/ota/target") { zota.target = (await body(req)).ieee ?? null; return json(res, 200, zotaStatus()); }
  if (u.pathname === "/api/zigbee/ota/notify") {
    const b = await body(req);
    if (b.ieee) {
      if (!zota.image) return json(res, 409, { error: "no image is stored" });
      zota.target = b.ieee;
      const d = devices.find((x) => x.ieee === b.ieee);
      /* The device asks a few seconds later, and gets it if it matches. */
      setTimeout(() => {
        zota.queries++;
        const ok = d?.ota && zota.target === b.ieee && d.ota.manufacturer_code === zota.image?.manufacturer_code && d.ota.image_type === zota.image?.image_type;
        zota.last_query = { ieee: b.ieee, short_addr: d?.short_addr ?? 0, manufacturer_code: d?.ota?.manufacturer_code ?? 0, image_type: d?.ota?.image_type ?? 0, file_version: d?.ota?.file_version ?? 0, s_ago: 0, answer: ok ? "image offered" : "the image is for another manufacturer or model" };
        if (ok) zota.transfer = { ieee: b.ieee, short_addr: d.short_addr, from_version: d.ota.file_version, to_version: zota.image.file_version, offset: 0, size: zota.image.size, started: Date.now(), status: null };
        else zota.refused++;
      }, 4000);
    }
    return json(res, 202, zotaStatus());
  }
  if (u.pathname === "/api/zigbee/ota/ask") {
    const asked = (await body(req)).ieee;
    const d = devices.find((x) => x.ieee === asked);
    if (!d) return json(res, 404, { error: "unknown device" });
    setTimeout(() => {
      if (!d.ota) return;
      d.ota.queried_s_ago = 0;
      if (d.ota.manufacturer_code == null && d.ieee === devices[0].ieee) Object.assign(d.ota, { manufacturer_code: 0x100b, image_type: 0x10f, file_version: 0x01001502 });
      if (d.ota.manufacturer_code == null && d.ieee === "0xec1bbdfffe397735") Object.assign(d.ota, { ep: 1, manufacturer_code: 0x117c, image_type: 0x11c4, file_version: 0x00010012 });
    }, sleepy(d) ? 8000 : 2000);
    return json(res, 202, zotaStatus());
  }
  if (u.pathname === "/api/updater/image") { await rawBody(req); return json(res, 200, { ok: true }); }
  /* The configuration file: a small one in the real shape, secrets as asked. */
  if (u.pathname === "/api/backup") {
    const omit = (u.searchParams.get("omit") ?? "").split(",").filter(Boolean);
    if (omit.some((x) => x !== "wifi" && x !== "auth")) return json(res, 400, { error: "omit takes wifi and auth" });
    const rows = (v, n, size) => { const b = Buffer.alloc(8 + n * size); b.writeUInt32LE(v, 0); b.writeUInt16LE(n, 4); return b.toString("base64"); };
    const lines = [{ key: "name", type: "str", value: "Living Room Hub" }, { key: "trans_ds", type: "u16", value: 8 }];
    if (!omit.includes("wifi")) lines.push({ key: "wifi_ssid", type: "str", value: "Home" }, { key: "wifi_pass", type: "str", value: "secret" });
    if (!omit.includes("auth")) lines.push({ key: "auth", type: "blob", value: rows(1, 2, 68) });
    lines.push({ key: "devices", type: "blob", value: rows(2, devices.length, 256) }, { key: "groups", type: "blob", value: rows(1, groups.length, 184) });
    const head = { format: "elixcon-config", v: 1, firmware: "mock", name: "Living Room Hub", hostname: "living-room-hub",
      ieee: status().zigbee.ieee, pan_id: status().zigbee.pan_id, ext_pan_id: "404ccafffe42c504", created: Math.floor(Date.now() / 1000), omitted: omit };
    res.writeHead(200, { "Content-Type": "application/x-ndjson", "Content-Disposition": 'attachment; filename="living-room-hub-config.jsonl"' });
    return res.end([head, ...lines, { end: true, entries: lines.length }].map((o) => JSON.stringify(o)).join("\n") + "\n");
  }
  if (u.pathname === "/api/restore" && m === "POST") {
    const text = String(await rawBody(req));
    if (!text.startsWith('{"format":"elixcon-config"')) return json(res, 400, { error: "this is not a configuration file" });
    const keep = (u.searchParams.get("keep") ?? "").split(",").filter(Boolean);
    return json(res, 200, { ok: true, addresses_kept: true, entries: text.trim().split("\n").length - 2, skipped: 0, wifi: !keep.includes("wifi"), auth: !keep.includes("auth") });
  }
  if (u.pathname === "/api/updater/run") { setTimeout(() => { zota.layout = "current"; }, 5000); return json(res, 202, { ok: true }); }
  if (u.pathname === "/mock/press") { seq++; remotes.unshift({ seq, ieee: devices[3].ieee, ep: 1, key: 1, event: "click", ago_ms: 0, name: "Hue button" }); return json(res, 200, { ok: true }); }
  json(res, 404, { error: "not found" });
}).listen(Number(process.env.MOCK_PORT) || 8765, function () { console.log(`mock on ${this.address().port}`); });
