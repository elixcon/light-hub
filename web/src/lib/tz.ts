/* The browser's time zone as a POSIX TZ rule, for Adaptive Lighting's sunrise
 * and sunset limits - the one thing on the coordinator that needs the local
 * clock. main/tz.c reads what this writes: `STD<off>[DST<off>,Mm.w.d/t,Mm.w.d/t]`,
 * names fixed (they mean nothing to the arithmetic), offsets west of UTC as
 * POSIX has them. web/test/tz.test.ts and test/test_tz.c assert the same strings.
 *
 * Worked out from the browser's own offsets over the year rather than from a
 * table of zones: the transitions are found to the minute, and each is written
 * as "the n-th (or last) weekday of the month at the wall time before it". That
 * is every rule in use in Europe, the Americas and Australia. A zone whose year
 * does not have exactly two changes (none, or a standard offset that moved) is
 * written as the offset in force now. */

const DAY_MS = 86_400_000;
const MIN_MS = 60_000;

/* Seconds east of UTC at a moment, in this browser's zone. */
export const offsetEast = (ms: number) => -new Date(ms).getTimezoneOffset() * 60;

/* The first minute with the new offset, between two moments either side of a change. */
function transition(lo: number, hi: number): number {
  const before = offsetEast(lo);
  while (hi - lo > MIN_MS) {
    const mid = lo + Math.floor((hi - lo) / 2 / MIN_MS) * MIN_MS;
    if (offsetEast(mid) === before) lo = mid;
    else hi = mid;
  }
  return hi;
}

/* [-]h[:mm[:ss]] */
function hms(s: number): string {
  const sign = s < 0 ? "-" : "";
  s = Math.abs(s);
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
  const pad = (v: number) => String(v).padStart(2, "0");
  return sign + h + (m || sec ? `:${pad(m)}` : "") + (sec ? `:${pad(sec)}` : "");
}

/* A change as a rule: which weekday of which month, at the wall time it happens
 * at in the offset in force until then. Week 5 is the last of the month. */
function rule(atMs: number, eastBefore: number): string {
  const wall = new Date(atMs + eastBefore * 1000);   /* read with the UTC getters */
  const y = wall.getUTCFullYear(), mo = wall.getUTCMonth(), d = wall.getUTCDate();
  const inMonth = new Date(Date.UTC(y, mo + 1, 0)).getUTCDate();
  const week = d + 7 > inMonth ? 5 : Math.ceil(d / 7);
  const secs = wall.getUTCHours() * 3600 + wall.getUTCMinutes() * 60 + wall.getUTCSeconds();
  return `M${mo + 1}.${week}.${wall.getUTCDay()}/${hms(secs)}`;
}

/* The rule for this browser's zone, from the year `nowMs` falls in. */
export function browserPosixTz(nowMs = Date.now()): string {
  const y = new Date(nowMs).getUTCFullYear();
  const start = Date.UTC(y, 0, 1), end = Date.UTC(y + 1, 0, 1);
  const changes: { at: number; before: number; after: number }[] = [];
  let prev = offsetEast(start);
  for (let t = start + DAY_MS; t <= end; t += DAY_MS) {
    const o = offsetEast(t);
    if (o !== prev) {
      changes.push({ at: transition(t - DAY_MS, t), before: prev, after: o });
      prev = o;
    }
  }
  if (changes.length !== 2) return `STD${hms(-offsetEast(nowMs))}`;
  const std = Math.min(changes[0].before, changes[0].after);
  const dst = Math.max(changes[0].before, changes[0].after);
  const [into, out] = changes[0].after === dst ? changes : [changes[1], changes[0]];
  return `STD${hms(-std)}DST${hms(-dst)},${rule(into.at, std)},${rule(out.at, dst)}`;
}

/* The zone's IANA name, only ever shown - "Europe/Berlin". Empty when the
 * browser does not say, or says something the coordinator would refuse. */
export function browserZoneName(): string {
  let name = "";
  try { name = Intl.DateTimeFormat().resolvedOptions().timeZone ?? ""; } catch { /* old browser */ }
  return /^[A-Za-z0-9/_+-]{1,47}$/.test(name) ? name : "";
}

/* What the page sends as `time_zone`. */
export const browserZone = () => ({ posix: browserPosixTz(), name: browserZoneName() || null });
