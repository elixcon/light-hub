/* Choosing a Zigbee channel: which of 11-26 is clear of the Wi-Fi around it, and
 * of other Zigbee networks, as the coordinator's own scans saw them.
 *
 * Two kinds of evidence. The access points from /api/wifi/scan predict how much
 * of each one spills onto a Zigbee channel - a Wi-Fi channel is 20 MHz wide (40
 * with a second half), a Zigbee channel 2 MHz. The energy scan from
 * /api/zigbee/scan measures it: everything on the air, Bluetooth and microwave
 * ovens included, but only at the coordinator. The measurement decides where it
 * exists, the prediction otherwise.
 *
 * Our own Wi-Fi is a different matter from anybody else's: the coordinator
 * shares one antenna between the two radios, and a Zigbee channel inside the
 * network it is associated with is fought over by its own two halves. Every
 * channel our SSID is seen on counts - an access point may roam between them. */

export const ZIGBEE_CHANNELS = Array.from({ length: 16 }, (_, i) => 11 + i);
/* The Zigbee Light Link channels: Touchlink looks there first, and a Hue bridge
 * uses nothing else. Any Zigbee 3.0 device works on all sixteen, but some find
 * a network faster on these. */
export const PREFERRED = [11, 15, 20, 25];
/* Some devices cannot use 26, and some regions limit power there. */
export const LAST_RESORT = 26;

/* Free means: spill from other access points below this... */
export const FREE_PREDICTED_DBM = -85;
/* ...and a median energy within this much of the quietest channel's. On the dev
 * board (2026-10-05) a quiet band read from about -92 dBm at 11 to -88 at 26, a
 * tilt of the receiver's as much as anything, while a Wi-Fi network on top of a
 * channel lifted it 10-30 dB. */
export const FREE_MARGIN_DB = 5;

export const zigbeeMhz = (n: number) => 2405 + 5 * (n - 11);
export const wifiMhz = (c: number) => (c === 14 ? 2484 : 2407 + 5 * c);

export interface WifiAp {
  ssid: string;
  rssi: number;
  channel: number;
  second?: "above" | "below" | null;
}
/* One channel of /api/zigbee/scan: each energy scan's reading, in dBm. */
export interface EnergyReading {
  channel: number;
  dbm: number[];
}
export interface ZigbeeNet {
  channel: number;
  pan_id: number;
  ext_pan_id: string;
  permit_join: boolean;
}

/* Where an access point's signal sits: the centre and half-width of what it
 * occupies, in MHz. A 40 MHz one is its primary channel and the 20 MHz beside it. */
export function wifiBand(ap: Pick<WifiAp, "channel" | "second">): { centre: number; half: number } {
  const c = wifiMhz(ap.channel);
  if (ap.second === "above") return { centre: c + 10, half: 20 };
  if (ap.second === "below") return { centre: c - 10, half: 20 };
  return { centre: c, half: 10 };
}

/* How much weaker an access point is at `mhz` than inside its band, in dB: the
 * 802.11 OFDM transmit mask, in units of the half-width (flat to h-1, -20 dBr at
 * h+1, -28 at 2h, -40 at 3h, nothing past it). A mask is a limit, not a measured
 * spectrum, so this errs towards caution. null for nothing at all. */
export function spillDb(mhz: number, band: { centre: number; half: number }): number | null {
  const d = Math.abs(mhz - band.centre), h = band.half;
  const pts: [number, number][] = [[h - 1, 0], [h + 1, -20], [2 * h, -28], [3 * h, -40]];
  if (d <= pts[0][0]) return 0;
  for (let i = 1; i < pts.length; i++) {
    const [x0, y0] = pts[i - 1], [x1, y1] = pts[i];
    if (d <= x1) return y0 + ((y1 - y0) * (d - x0)) / (x1 - x0);
  }
  return null;
}

/* The rule the coordinator's own log uses: a Zigbee channel whose 2 MHz reach
 * into a Wi-Fi channel's band is inside it - 11 MHz from a 20 MHz channel's
 * centre. */
export function overlaps(n: number, ap: Pick<WifiAp, "channel" | "second">): boolean {
  const b = wifiBand(ap);
  return Math.abs(zigbeeMhz(n) - b.centre) <= b.half + 1;
}

export interface ChannelRating {
  channel: number;
  mhz: number;
  preferred: boolean;
  /* Our own network's Wi-Fi channels this one is inside. */
  ownWifi: number[];
  /* Other access points that reach it, strongest first, as they arrive here. */
  aps: { ssid: string; channel: number; dbm: number }[];
  predictedDbm: number | null;
  /* The middle reading of the energy scans - one burst does not move it - and
   * the highest. */
  medianDbm: number | null;
  maxDbm: number | null;
  networks: number;
  free: boolean;
  /* 0 free and preferred, 1 free, 2 the rest. */
  tier: 0 | 1 | 2;
}

export interface Survey {
  aps: WifiAp[];
  ownSsid?: string | null;
  /* The channel the station is associated on, in case the scan missed it. */
  ownChannel?: number | null;
  energy?: EnergyReading[] | null;
  networks?: ZigbeeNet[] | null;
  /* The Zigbee channel in use, which wins a tie: a move costs something. */
  current?: number | null;
}

const sumDbm = (dbm: number[]) =>
  dbm.length ? 10 * Math.log10(dbm.reduce((s, v) => s + 10 ** (v / 10), 0)) : null;

export function median(v: number[]): number | null {
  if (!v.length) return null;
  const s = [...v].sort((a, b) => a - b), m = s.length >> 1;
  return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2;
}

/* Every channel, best first. */
export function rankChannels(sv: Survey): ChannelRating[] {
  const own = sv.ownSsid ? sv.aps.filter((a) => a.ssid === sv.ownSsid) : [];
  const ownBands: Pick<WifiAp, "channel" | "second">[] = [...own];
  if (sv.ownChannel && !own.some((a) => a.channel === sv.ownChannel)) ownBands.push({ channel: sv.ownChannel });
  const others = sv.aps.filter((a) => !own.includes(a));
  const energy = new Map((sv.energy ?? []).filter((e) => e.dbm.length).map((e) => [e.channel, e.dbm]));
  const quietest = energy.size ? Math.min(...[...energy.values()].map((d) => median(d) as number)) : null;

  const rated = ZIGBEE_CHANNELS.map((n): ChannelRating => {
    const mhz = zigbeeMhz(n);
    const ownWifi = [...new Set(ownBands.filter((b) => overlaps(n, b)).map((b) => b.channel))].sort((a, b) => a - b);
    const aps = others
      .map((a) => ({ a, s: spillDb(mhz, wifiBand(a)) }))
      .filter((x): x is { a: WifiAp; s: number } => x.s !== null)
      .map(({ a, s }) => ({ ssid: a.ssid, channel: a.channel, dbm: Math.round(a.rssi + s) }))
      .sort((x, y) => y.dbm - x.dbm);
    const predictedDbm = sumDbm(aps.map((a) => a.dbm));
    const e = energy.get(n);
    const medianDbm = e ? median(e) : null, maxDbm = e ? Math.max(...e) : null;
    const networks = (sv.networks ?? []).filter((z) => z.channel === n).length;
    const free = ownWifi.length === 0 &&
      (predictedDbm === null || predictedDbm < FREE_PREDICTED_DBM) &&
      (quietest === null || medianDbm === null || medianDbm <= quietest + FREE_MARGIN_DB);
    const preferred = PREFERRED.includes(n);
    return { channel: n, mhz, preferred, ownWifi, aps, predictedDbm, medianDbm, maxDbm, networks, free,
      tier: free ? (preferred ? 0 : 1) : 2 };
  });

  const cmp = (a: number | null, b: number | null) => (a ?? -Infinity) - (b ?? -Infinity);
  return rated.sort((a, b) =>
    a.tier - b.tier ||
    Number(a.ownWifi.length > 0) - Number(b.ownWifi.length > 0) ||
    Number(a.channel === LAST_RESORT) - Number(b.channel === LAST_RESORT) ||
    cmp(a.medianDbm, b.medianDbm) ||
    cmp(a.maxDbm, b.maxDbm) ||
    cmp(a.predictedDbm, b.predictedDbm) ||
    a.networks - b.networks ||
    Number(b.channel === sv.current) - Number(a.channel === sv.current) ||
    a.channel - b.channel);
}

/* Why a channel is not free, in a few words. */
export function whyNotFree(r: ChannelRating, ranked: ChannelRating[]): string {
  if (r.ownWifi.length) return `inside your Wi-Fi on channel ${r.ownWifi.join(" and ")}`;
  if (r.predictedDbm !== null && r.predictedDbm >= FREE_PREDICTED_DBM && r.aps.length)
    return `Wi-Fi “${r.aps[0].ssid || "hidden"}” on channel ${r.aps[0].channel} reaches it`;
  const quietest = Math.min(...ranked.map((x) => x.medianDbm ?? Infinity));
  if (r.medianDbm !== null && Number.isFinite(quietest))
    return `${Math.round(r.medianDbm - quietest)} dB busier than the quietest channel`;
  return "busy";
}

/* One sentence for the top of the result. */
export function verdict(ranked: ChannelRating[], current?: number | null): string {
  const best = ranked[0];
  const list = PREFERRED.join(", ");
  if (!best.free) {
    return `No channel is free. Channel ${best.channel} is the least busy${best.channel === current ? ", and it is the one in use" : ""}.`;
  }
  if (best.preferred) {
    return best.channel === current
      ? `You are already on the best channel: ${best.channel} is free and one of the preferred channels (${list}).`
      : `Channel ${best.channel} is free and is one of the preferred channels (${list}).`;
  }
  const pref = ranked.find((r) => r.preferred) as ChannelRating;
  const lead = `None of the preferred channels (${list}) is free - the best of them, ${pref.channel}, is ${whyNotFree(pref, ranked)}.`;
  const tail = "Most devices work on any channel, but some find a network faster on the preferred ones.";
  return best.channel === current
    ? `${lead} You are already on the quietest channel, ${best.channel}. ${tail}`
    : `${lead} Channel ${best.channel} is the quietest. ${tail}`;
}
