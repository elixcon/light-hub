/* Downloading the configuration and giving it back; the file itself is read in
 * backup.ts. */
import { apiFile } from "./api.ts";
import { setPaused } from "./state.svelte.ts";
import { BACKUP_MAX_BYTES, backupFilename, parseBackup, type BackupSummary, type RestoreResult, type Secret } from "./backup.ts";

export const backup = $state({ busy: false });

/* Fetched rather than linked, so a locked page shows the lock screen instead
 * of saving {"error":"locked"} as the file - and read through before it is
 * saved, so a download cut short is an error here rather than a file that
 * fails on the day it is needed. */
export async function downloadBackup(hostname: string, omit: Secret[]) {
  backup.busy = true;
  try {
    const res = await apiFile(`/api/backup${omit.length ? `?omit=${omit.join(",")}` : ""}`);
    const text = await res.text();
    parseBackup(text);
    const url = URL.createObjectURL(new Blob([text], { type: "application/x-ndjson" }));
    const a = document.createElement("a");
    a.href = url;
    a.download = backupFilename(hostname, new Date());
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 10_000);
  } finally {
    backup.busy = false;
  }
}

export async function readBackup(file: File): Promise<BackupSummary> {
  if (file.size > BACKUP_MAX_BYTES) throw new Error("This file is too large to be a configuration file.");
  return parseBackup(await file.text());
}

/* The file as it was read, so what is restored is exactly what was shown. The
 * coordinator restarts once it has staged it; the poll finds it again. */
export async function restoreBackup(file: BackupSummary, keep: Secret[]): Promise<RestoreResult> {
  backup.busy = true;
  setPaused(true);           /* one request at a time on the server */
  try {
    const res = await apiFile(`/api/restore${keep.length ? `?keep=${keep.join(",")}` : ""}`, {
      method: "POST",
      headers: { "Content-Type": "application/x-ndjson" },
      body: file.text,
    });
    return (await res.json()) as RestoreResult;
  } finally {
    setPaused(false);
    backup.busy = false;
  }
}
