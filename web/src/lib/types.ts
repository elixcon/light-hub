/* The JSON the coordinator sends. Field names are the C side's; see
 * http_server.c's *_to_json functions. */

import type { MixCurvesJson } from "./mix.ts";

export type Feature = "onoff" | "brightness" | "color_temp" | "color" | "switch" | "hue_buttons";

export interface Endpoint {
  ep: number;
  device_id: number;
  features: Feature[];
  on: boolean | null;
  brightness_pct: number | null;
  kelvin: number | null;
  min_kelvin: number | null;
  max_kelvin: number | null;
  hue: number | null;
  sat: number | null;
  color_mode: "color_temp" | "color" | null;
  curves: { level: boolean; chroma: boolean };
  input_pct: number | null;
}

/* An end device's parent, from the coordinator's own neighbour table
 * ("table", looked at every five minutes) or a router's ("router", every
 * hour).
 * `lqi` is the parent's reading of the link; `timeout_s` how long the
 * coordinator keeps its own child without hearing from it, and
 * `timeout_left_s` how much of that is left. */
export interface DeviceParent {
  coordinator: boolean;
  short_addr: number;
  ieee: string | null;
  source: "table" | "router";
  lqi: number | null;
  changed_s_ago: number;
  timeout_s?: number;
  timeout_left_s?: number;
}

export interface Device {
  ieee: string;
  /* null for a row restored from another network's backup, until it joins. */
  short_addr: number | null;
  name: string;
  manufacturer: string;
  model: string;
  online: boolean;
  last_seen_s_ago: number;
  lqa: number | null;
  /* The receiving side, from frames the device itself sent: the last one's
   * RSSI in dBm (the last hop's), how many arrived, how many of its sequence
   * numbers never did. A sleepy remote gets no lqa - it is never in the
   * neighbour table - so this is the only link reading it has. `child` is
   * whether it hangs off the coordinator itself; null until looked. */
  rssi?: number | null;
  frames_heard?: number;
  frames_missed?: number;
  child?: boolean | null;
  /* Whether it has sent to a group since the coordinator started, and - for an
   * end device - what it hangs off. A group-casting remote that is a lamp's
   * child reaches the coordinator as that lamp's one unacknowledged broadcast
   * per press, so the two together say whether its presses can go missing. */
  groupcast?: boolean;
  parent?: DeviceParent | null;
  /* What its OTA Upgrade client said it runs; null for a wired light only.
   * Fields stay null until the device has said since the coordinator started:
   * a lamp at interrogation or when asked, a remote only when it asks for
   * firmware - which it does when woken after /api/zigbee/ota/ask. */
  ota?: DeviceOta | null;
  endpoints: Endpoint[];
  /* "gpio" is a light wired to a pin of the board; the rest are its wiring.
   * "matter" is one light of a Matter device paired to this box. */
  source?: "zigbee" | "gpio" | "matter";
  id?: number;
  kind?: LightKind;
  /* The only pin of an on/off or dimmable light, the cold white of a tunable white one. */
  gpio?: number | null;
  gpio_warm?: number | null;
  active_low?: boolean;
  power_on?: PowerOn;
  pwm_hz?: number;
  /* Where in the PWM period a driven dimmable or tunable white light starts, in
   * degrees: lights sharing a frequency are spread apart. Null while it is not driven. */
  pwm_phase?: number | null;
  cold_kelvin?: number;
  warm_kelvin?: number;
  mix?: LightMix;
  /* A three-channel tunable white light's neutral white. */
  gpio_neutral?: number | null;
  neutral_kelvin?: number;
  /* Mix "custom": one curve per channel over the light's colour temperatures
   * (lib/mix.ts); null for a channel with none. Kept whatever the mix. */
  mix_curves?: MixCurvesJson;
  /* A wired light held off by the overheat protection: switching on is refused. */
  held?: boolean;
  /* A Matter lamp: the device it is on (a hex string - it does not fit a JS
   * number), its endpoint there, and what the device says it is. `verified`
   * is whether its certificate was checked, which it never is yet. After a
   * Matter reset the lamp is still listed, but `needs_pairing`. */
  node_id?: string;
  remote_ep?: number;
  vendor_id?: number;
  product_id?: number;
  bridged?: boolean;
  verified?: boolean;
  needs_pairing?: boolean;
}

export interface DeviceOta {
  ep: number | null;
  manufacturer_code: number | null;
  image_type: number | null;
  file_version: number | null;
  hw_version: number | null;
  queried_s_ago: number | null;
}

/* The coordinator's one Zigbee device image and what is happening to it. */
export type ZbOtaState = "no_partition" | "writing" | "idle" | "stored" | "ready" | "serving" | "stalled"
  | "finished" | "failed";

export interface ZbOtaStatus {
  state: ZbOtaState;
  room: number;
  image: { manufacturer_code: number; image_type: number; file_version: number; stack_version: number;
    size: number; header: string; crc: string } | null;
  error: string | null;
  target: string | null;
  transfer: { ieee: string; short_addr: number; from_version: number; to_version: number; device_version: number | null; offset: number;
    size: number; started_s_ago: number; progress_s_ago: number; status: number | null } | null;
  queries: number;
  refused: number;
  last_query: { ieee: string | null; short_addr: number; manufacturer_code: number; image_type: number;
    file_version: number; s_ago: number; answer: string } | null;
  /* Sleepy devices a notify is held for until they are next heard from. */
  waiting?: { ieee: string; s_left: number; notifies: number; last_s_ago: number }[];
  wake_notifies_sent?: number;
  wake_notifies_refused?: number;
  notifies_delivered?: number;
  notifies_undelivered?: number;
  last_notify?: { short_addr: number; status: number; s_ago: number } | null;
}

export type PowerOn = "off" | "on" | "last";
export type LightKind = "onoff" | "dim" | "cwww" | "cwnwww";
export type LightMix = "constant" | "full" | "custom";

export interface GroupMember {
  ieee: string;
  ep: number;
  name: string;
  online: boolean;
  brightness: boolean;
  on: boolean | null;
  brightness_pct: number | null;
}

export type AdaptiveReason = "off" | "on" | "no_clock" | "no_location";
/* How a group offers Adaptive Lighting: not at all, as a Mode Select on its Matter
 * light, or as a second on/off light for Apple Home. */
export type AdaptiveExpose = "off" | "mode_select" | "endpoint";

/* What a Matter controller's "turn on" does to a room, dark or lit: back as it was, start
 * Adaptive Lighting (where the room offers it), or full brightness. */
export type TurnOn = "last" | "adaptive" | "full";

export interface AdaptiveStatus {
  expose: AdaptiveExpose;
  active: boolean;       /* the mode is running, including while it waits */
  reason: AdaptiveReason;
  pct: number | null;    /* what the sun says now; null without a clock or a location */
  offset_k: number | null;
}

export interface Group {
  id: number;
  name: string;
  members: GroupMember[];
  on: boolean | null;
  brightness_pct: number | null;
  turn_on?: TurnOn;
  adaptive?: AdaptiveStatus;
}

export type Gesture = "short" | "long" | "double" | "triple";
export type When = "always" | "off" | "on" | "below" | "above";
export type Action = "none" | "on" | "off" | "toggle" | "set" | "dim_up" | "dim_down" | "dim_cycle" | "adaptive";
export type Pull = "none" | "up" | "down";
/* A push button springs back; a switch stays put, and every flip is one short press. */
export type InputKind = "button" | "switch";

/* A rule acts on a room (`group`) or on one light (`ieee` and `ep`, `group` 0).
 * `and` joins it to the rule above: it runs whenever that one does, and its own
 * condition is not read. */
export interface Rule {
  group: number;
  ieee?: string | null;
  ep?: number | null;
  and?: boolean;
  when: When;
  arg: number;
  do: Action;
  value: number;
}

export interface ButtonDef {
  id: number;
  name: string;
  source: "gpio" | "zigbee";
  ieee: string | null;
  ep: number | null;
  key: number | null;
  gpio: number | null;
  inverted: boolean;
  pull: Pull;
  input?: InputKind;
  pressed: boolean | null;
  level: number | null;
  last_event_ms: number | null;
  events: Record<Gesture, Rule[]>;
}

export interface RemoteEvent {
  seq: number;
  ieee: string;
  ep: number;
  key: number;
  event: string;
  ago_ms: number;
  name?: string;
}

export interface WifiStatus {
  mode: "ap" | "sta";
  connected: boolean;
  ssid: string;
  ip: string;
  hostname: string;
  rssi: number;
  channel: number;
  weak_uplink: boolean;
  temporary_hotspot: boolean;
  home_ssid?: string;
  returns_in_s?: number;
}

/* A move of the Zigbee network to another channel: announced to the routers,
 * then the coordinator follows; `boot` when it was put back after a restart. */
export interface ChannelMove {
  state: "idle" | "announced" | "moving" | "moved" | "failed";
  from?: number;
  to?: number;
  boot?: boolean;
  s_ago?: number;
}

/* GET /api/zigbee/scan: the band as the coordinator's own radio measured it. */
export interface ChannelSurveyAnswer {
  state: "idle" | "running" | "done" | "failed";
  error: string | null;
  passes: number;
  s_ago: number | null;
  channels: { channel: number; dbm: number[] }[];
  networks: { channel: number; pan_id: number; ext_pan_id: string; permit_join: boolean }[];
}

export interface Status {
  name: string;
  wifi: WifiStatus;
  zigbee: {
    network_up: boolean;
    pan_id: number;
    channel: number;
    permit_join_remaining_s: number;
    ieee: string;
    radio: Record<string, number>;
    ota?: ZbOtaStatus;
    channel_move?: ChannelMove;
  };
  device_count: number;
  uptime_s: number;
  version: string;
  build: string;
  ota?: { partition: string; pending_verify: boolean; updater?: string | null;
    /* The updater image verifies; the coordinator hands no update to one that does not. */
    updater_valid?: boolean;
    updater_problem?: string | null;
    /* "old" until the updater has made room for Zigbee device firmware. */
    layout?: "current" | "old" };
  heap?: { free: number; min_free: number; largest: number };
  thermal?: ThermalStatus;
  wled?: WledStatus;
}

/* What the WLED interface is doing: its pixels in order (the room's Hue colour
 * lamps), whether a client is streaming, and from where. */
export interface WledStatus {
  enabled: boolean;
  room: number | null;
  pixels: { ieee: string; ep: number }[];
  listening: boolean;
  live: boolean;
  /* A hand ended the stream; the client is ignored until it pauses. */
  override: boolean;
  /* The setting, and how many of the room's other lights the running stream
   * holds switched off. */
  others_off?: boolean;
  lights_off?: number;
  fps_in: number;
  source: string | null;
  protocol?: string;
  last_packet_s_ago?: number;
  packets: number;
  frames: number;
  sessions: number;
  last_end: "source" | "timeout" | "hand" | "failed" | null;
}

/* Overheat protection: "off" without a sensor; "hot" holds until release_c;
 * "fault" is a sensor that reads as broken, which holds the lights off too. */
export type ThermalState = "off" | "ok" | "hot" | "fault";
export interface ThermalStatus {
  state: ThermalState;
  temp_c: number | null;
  max_c: number | null;
  trip_c: number;
  release_c: number;
  trips: number;
  mv: number;
  held: boolean;
}
export interface ThermalConfig {
  gpio: number | null;
  b: number;
  r0_ohm: number;
  rs_ohm: number;
  ntc_to: "gnd" | "3v3";
  trip_c: number;
  /* Pins the sensor can be on: the chip's analog inputs. */
  adc_pins?: number[];
}

export type AuthMode = "locked" | "setup" | "open" | "failsafe";

export interface AuthState {
  mode: AuthMode;
  authorized: boolean;
  keys: number;
  failsafe_left_s: number;
  current: { id: number; name: string } | null;
}

export interface AuthKey {
  id: number;
  kind: "api" | "browser";
  name: string;
  last_used_s_ago: number;
  current: boolean;
}

export interface Session {
  id: number;
  name: string;
  issued: boolean;
  key?: string;
}

export type MatterPairState = "idle" | "running" | "done" | "failed";

/* The box as a controller of Matter lamps: its own fabric, each paired device
 * with its subscription, and the pairing in progress. */
export interface MatterController {
  running: boolean;
  fabric_index: number;      /* 0 until the first pairing makes it */
  fabric_error: string;
  node_id: string;
  nodes: { node_id: string; lamps: number; online: boolean; subscribed: boolean; drops: number;
    reports: number; last_seen_s_ago: number; down_s: number; last_error: string }[];
  pairing: { state: MatterPairState; step: string; error: string; node_id: string; lamps: number; s_ago: number };
}

export interface MatterStatus {
  running: boolean;
  fabrics: number;
  /* `ours` is this box's own fabric, for its Matter lamps - not a controller. */
  fabric_list: { index: number; vendor_id: string; label: string; ours?: boolean }[];
  window_open: boolean;
  groups: number;
  manual_code: string;
  qr_code: string;
  controller?: MatterController;
}

export interface AdaptiveProfile {
  k_midnight: number;
  k_noon: number;
  k_base: number;
  pct_midnight: number;
  pct_noon: number;
  /* Limits on the sun's day, "HH:MM" in the coordinator's time zone or null. */
  sunrise?: { earliest: string | null; latest: string | null };
  sunset?: { earliest: string | null; latest: string | null };
  /* The shape's points between midnight (x 1) and noon (x 100), [x, value]. */
  pct_points?: [number, number][];
  k_points?: [number, number][];
  max_points?: number;
  offset_limit_k?: number;
  base_min_k?: number;
  base_max_k?: number;
}

/* GET and POST /api/groups/{id}/day: a room's Adaptive Lighting day. */
export interface RoomDay {
  day: AdaptiveProfile;
  /* Unix seconds; null until the coordinator's clock has been set over SNTP. */
  time_utc: number | null;
  /* The sun's day around now, and where this room's day puts its sunrise and
   * sunset (al_rise, al_set: moved into its limits). Null without a location. */
  sun: { rise: number; noon: number; set: number; polar: number; al_rise: number; al_set: number } | null;
  /* The zone the limits are read in; null until the page has sent one. */
  time_zone: { posix: string; name: string | null; offset_s: number } | null;
  /* What the room is sent now while it runs the mode, by the coordinator's own reckoning. */
  now: { pct: number; offset_k: number } | null;
}

export interface Settings {
  /* The build has /api/backup and /api/restore (CONFIG_ZBC_CONFIG_BACKUP);
   * absent from firmware older than them. */
  backup?: boolean;
  transition_ms: number;
  transition_max_ms: number;
  /* Wi-Fi dozes between beacons so the Zigbee radio can hear a remote. */
  wifi_sleep?: boolean;
  /* A light is left at its warmest white while it is switched off, so it does
   * not come back on cold. */
  warm_off?: boolean;
  /* The channel the Zigbee network is on; posting another moves it there. */
  zigbee_channel?: number;
  name: string;
  hostname: string;
  name_max_len: number;
  location?: { lat: number; lon: number } | null;
  /* Unix seconds; null until the coordinator's clock has been set over SNTP. */
  time_utc?: number | null;
  /* The sun's day around now. Where a room's day puts its sunrise and sunset is
   * that room's (RoomDay). */
  sun?: { rise: number; noon: number; set: number; polar: number } | null;
  /* The zone the limits are read in; null until the page has sent one. */
  time_zone?: { posix: string; name: string | null; offset_s: number } | null;
  thermal?: ThermalConfig;
  /* The WLED interface: one room's Hue colour lamps as a WLED strip. Absent
   * from a build without it (CONFIG_ZBC_WLED). */
  wled?: { enabled: boolean; room: number | null; others_off?: boolean };
}

export interface StateResponse {
  status: Status;
  devices: Device[];
  groups: Group[];
  buttons: ButtonDef[];
  remotes: RemoteEvent[];
  auth: AuthState;
  rev: string;
}

/* What `/api/state?rev=` answers when nothing has changed: the tables are
 * left out and the page keeps the ones it has. */
export interface StateUnchanged {
  status: Status;
  auth: AuthState;
  rev: string;
  unchanged: true;
}
