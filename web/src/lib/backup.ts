/* The configuration file, as GET /api/backup writes it and POST /api/restore
 * takes it back (main/config_backup.c): JSON Lines - a header, one line per
 * setting or table, and a last line with the count. Read here before anything
 * is sent, so a wrong or truncated file is refused with a reason, and the
 * restore dialog can say what the file holds. Pure, for node --test. */

/* The two parts of a backup that are secrets, and that a download can leave
 * out and a restore can keep. */
export type Secret = "wifi" | "auth";

export interface BackupHeader {
  format: string;
  v: number;
  firmware: string;
  name: string;
  hostname: string;
  ieee: string | null;
  pan_id: number | null;
  ext_pan_id: string | null;
  created: number | null;
  omitted: Secret[];
  warning?: string;
}

export interface BackupSummary {
  header: BackupHeader;
  /* The Wi-Fi network the file gives, or null: left out, or the coordinator
   * had none. */
  ssid: string | null;
  /* How many access keys it gives; null when they were left out, 0 when the
   * coordinator had none - which a restore would make true here. */
  keys: number | null;
  /* Rows per table, by what the page calls them, for the ones that have any. */
  counts: [string, number][];
  text: string;
}

export interface RestoreResult {
  ok: boolean;
  addresses_kept: boolean;
  entries: number;
  skipped: number;
  wifi: boolean;
  auth: boolean;
}

/* The largest file a coordinator writes is ~30 KB; the API takes 64. */
export const BACKUP_MAX_BYTES = 64 * 1024;

const TABLES: [string, string, string][] = [
  ["devices", "device", "devices"],
  ["groups", "room", "rooms"],
  ["curves", "curve", "curves"],
  ["buttons", "button", "buttons"],
  ["lights", "wired light", "wired lights"],
  ["mlights", "Matter lamp", "Matter lamps"],
];

/* A table's row count from the start of its blob: u32 version, u16 count,
 * little-endian. Every table but Adaptive Lighting's (u16, u16) has it. */
export function blobRows(b64: string): number | null {
  try {
    const head = atob(b64.slice(0, 12));
    return head.length >= 6 ? head.charCodeAt(4) | (head.charCodeAt(5) << 8) : null;
  } catch {
    return null;
  }
}

export function parseBackup(text: string): BackupSummary {
  const lines = text.split("\n").map((l) => l.trim()).filter((l) => l !== "");
  const parse = (line: string | undefined, n: number) => {
    try { return JSON.parse(line ?? ""); }
    catch { throw new Error(n === 1 ? "This is not a configuration file." : `Line ${n} of the file is damaged.`); }
  };
  const header = parse(lines[0], 1) as BackupHeader;
  if (header?.format !== "elixcon-config") throw new Error("This is not a configuration file.");
  if (header.v !== 1) throw new Error("This configuration file is from a newer firmware.");
  if (!Array.isArray(header.omitted)) header.omitted = [];

  const end = lines.length > 1 ? parse(lines[lines.length - 1], lines.length) : null;
  const entries = lines.slice(1, -1).map((l, i) => parse(l, i + 2));
  if (!end?.end || end.entries !== entries.length) {
    throw new Error("The file is incomplete: it was cut short somewhere.");
  }

  const value = (key: string) => entries.find((e) => e?.key === key)?.value;
  const ssid = typeof value("wifi_ssid") === "string" && value("wifi_ssid") !== "" ? value("wifi_ssid") : null;
  const auth = value("auth");
  const keys = header.omitted.includes("auth") ? null : typeof auth === "string" ? (blobRows(auth) ?? 0) : 0;
  const counts: [string, number][] = [];
  for (const [key, one, many] of TABLES) {
    const v = value(key);
    const n = typeof v === "string" ? blobRows(v) : null;
    if (n) counts.push([n === 1 ? one : many, n]);
  }
  return { header, ssid, keys, counts, text };
}

/* What the file will hold that needs keeping private, in words, or null.
 * `ssid` is whether the coordinator has a network to put in it, `keys` how many
 * access keys it has (null while not known). */
export function backupWarning(withWifi: boolean, withKeys: boolean, ssid: boolean, keys: number | null): string | null {
  const wifi = withWifi && ssid;
  const auth = withKeys && keys !== 0;
  if (wifi && auth) {
    return "The file will contain your Wi-Fi password in plain text and the fingerprints of this coordinator's access keys. Anyone who has it can join your Wi-Fi network. Keep it as private as a password.";
  }
  if (wifi) {
    return "The file will contain your Wi-Fi password in plain text. Anyone who has it can join your Wi-Fi network. Keep it as private as a password.";
  }
  if (auth) {
    return "The file will contain the fingerprints of this coordinator's access keys. They cannot be turned back into keys, but a coordinator restored from the file accepts the same ones. Keep it private.";
  }
  return null;
}

/* "zigbee-config-2026-10-09.jsonl", dated by this browser's own calendar. */
export function backupFilename(hostname: string, now: Date): string {
  const pad = (n: number) => String(n).padStart(2, "0");
  const day = `${now.getFullYear()}-${pad(now.getMonth() + 1)}-${pad(now.getDate())}`;
  return `${hostname || "coordinator"}-config-${day}.jsonl`;
}
