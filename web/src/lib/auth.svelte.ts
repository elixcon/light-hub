/* The lock screen, the failsafe and the one moment a key can be seen. */
import { api, post, setUnauthorizedHandler } from "./api.ts";
import { toast } from "./toast.svelte.ts";
import { refresh, setPaused, setAuthHooks } from "./state.svelte.ts";
import type { AuthState, Session } from "./types.ts";

export const auth = $state({
  locked: false,
  message: "",
  signingIn: false,
  state: null as AuthState | null,
  keyShown: null as { session: Session; kind: "browser" | "api" } | null,
});

let lockTimer: ReturnType<typeof setTimeout> | undefined;
let claiming = false;
let autoClaimed = false;

/* A label for the key list, so "Safari on iPhone" can be told from "Chrome on
 * Windows" when deciding what to revoke. Best effort - it is only a name. */
export function browserName(): string {
  const ua = navigator.userAgent;
  const browser = /Edg\//.test(ua) ? "Edge" : /OPR\//.test(ua) ? "Opera"
    : /Firefox\//.test(ua) ? "Firefox" : /Chrome\//.test(ua) ? "Chrome"
    : /Safari\//.test(ua) ? "Safari" : "Browser";
  const os = /iPhone/.test(ua) ? "iPhone" : /iPad/.test(ua) ? "iPad"
    : /Android/.test(ua) ? "Android" : /Mac OS X/.test(ua) ? "macOS"
    : /Windows/.test(ua) ? "Windows" : /Linux/.test(ua) ? "Linux" : "";
  return (os ? `${browser} on ${os}` : browser).slice(0, 31);
}

/* navigator.clipboard exists only on HTTPS or localhost, and this page is plain
 * HTTP on a LAN - so execCommand("copy"), with the text handed to the copy event
 * it fires rather than left to whatever happens to be selected.
 *
 * A hidden field appended to the body does not work: the key dialog's focus
 * trap takes the focus straight back, the copy runs with nothing selected, and
 * execCommand still answers true. `shown` - the same text on the page - is
 * selected instead, without moving the focus: Safari runs no copy without a
 * selection, and should the copy fail after all, the text is ready for the
 * user's own. "Copied" only when the event actually carried it. */
export function copyText(text: string, shown?: HTMLElement | null) {
  const sel = window.getSelection();
  if (shown && sel) {
    const r = document.createRange();
    r.selectNodeContents(shown);
    sel.removeAllRanges();
    sel.addRange(r);
  }
  let carried = false;
  const put = (e: ClipboardEvent) => {
    if (!e.clipboardData) return;
    e.clipboardData.setData("text/plain", text);
    e.preventDefault();
    carried = true;
  };
  document.addEventListener("copy", put);
  try { document.execCommand("copy"); } catch { /* not supported */ }
  document.removeEventListener("copy", put);
  toast(carried ? "Copied" : "Select the key and copy it by hand");
}

export function showLock() {
  if (auth.locked) return;
  auth.locked = true;
  auth.message = "";
  auth.keyShown = null;
  setPaused(true);
  pollLock();
}

export function unlock() {
  auth.locked = false;
  clearTimeout(lockTimer);
  setPaused(false);
  refresh();
}

/* The lock screen watches for the failsafe rather than asking anyone to reload:
 * somebody is standing at the board pressing a button, and the page they came
 * from should simply open. */
async function pollLock() {
  clearTimeout(lockTimer);
  if (!auth.locked) return;
  let a: AuthState | null = null;
  try { a = await api<AuthState>("/api/auth"); } catch { /* restarting, or the Wi-Fi blinked */ }
  if (!auth.locked) return;
  if (a && a.authorized) {
    if (a.mode === "failsafe" && !a.current) { claim(); return; }
    unlock();
    return;
  }
  lockTimer = setTimeout(pollLock, a ? 1000 : 3000);
}

/* A key for this browser: from "Secure it", or from a page that finds the
 * failsafe open. It comes back once, to be written down, and taking it closes
 * the failsafe for everyone else. */
export async function claim() {
  if (claiming) return;
  claiming = true;
  try {
    const r = await post<Session>("/api/auth/session", { name: browserName() });
    if (auth.locked) unlock(); else refresh();
    if (r.issued) auth.keyShown = { session: r, kind: "browser" };
  } catch (err: any) {
    toast(err.message);
    /* The poll said this browser was let in, so let it in even without a key
     * of its own - with every slot full, that is how somebody gets to Access to
     * revoke one. If the window has closed meanwhile, the next poll locks it. */
    if (auth.locked) unlock();
  } finally {
    claiming = false;
  }
}

export async function signInWithKey(key: string) {
  key = key.trim();
  if (!key) { auth.message = "Type the key in first."; return; }
  auth.signingIn = true;
  try {
    await post("/api/auth/session", { key });
    unlock();
    toast("Signed in");
  } catch (err: any) {
    auth.message = err.message;
  } finally {
    auth.signingIn = false;
  }
}

export async function closeFailsafe() {
  try { await post("/api/auth/lock"); } catch (err: any) { toast(err.message); }
}

/* Called on every poll. A page that got in on the failsafe takes a key while it
 * can - that is what the window is for. Once per window, so a full table is one
 * toast, not one every poll. */
function onPoll(a: AuthState) {
  auth.state = a;
  if (auth.locked) return;
  if (a.mode === "failsafe" && !a.current) {
    if (!autoClaimed) { autoClaimed = true; claim(); }
  } else {
    autoClaimed = false;
  }
}

setUnauthorizedHandler(showLock);
setAuthHooks(onPoll, () => auth.locked);
