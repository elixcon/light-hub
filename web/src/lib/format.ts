export function ago(ms: number): string {
  if (ms < 1500) return "just now";
  if (ms < 60000) return `${Math.round(ms / 1000)} s ago`;
  if (ms < 3600000) return `${Math.round(ms / 60000)} min ago`;
  return "a while ago";
}

export function agoS(s: number): string {
  if (s < 0) return "never";
  if (s < 60) return "just now";
  if (s < 3600) return `${Math.floor(s / 60)} min ago`;
  if (s < 86400) return `${Math.floor(s / 3600)} h ago`;
  return `${Math.floor(s / 86400)} days ago`;
}

export const usedAgo = (s: number) => (s < 0 ? "not used since the coordinator restarted" : `used ${agoS(s)}`);

/* null means "not known yet", which is not the same as off. */
export const stateLabel = (on: boolean | null | undefined) => (on == null ? "state unknown" : on ? "On" : "Off");

export const plural = (n: number, word: string) => `${n} ${word}${n === 1 ? "" : "s"}`;

export const mmss = (s: number) => `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`;

/* 11 digits, shown the way controllers print them. */
export const matterCode = (code: string) =>
  code.length === 11 ? `${code.slice(0, 4)}-${code.slice(4, 7)}-${code.slice(7)}` : code;

export const hex4 = (n: number) => `0x${n.toString(16).padStart(4, "0")}`;
