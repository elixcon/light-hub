/* fetch with the coordinator's conventions: JSON in, JSON out, errors as
 * {"error": "..."}, and a 401 meaning the page has been locked out. */

let onUnauthorized: (() => void) | null = null;
export const setUnauthorizedHandler = (fn: () => void) => { onUnauthorized = fn; };

function check(res: Response, body: any) {
  /* Locked out: this browser's key was revoked, or somebody secured the
   * coordinator from another one. The server's access check says so as
   * {"error":"locked"}; any other 401 - a mistyped key on the session route -
   * is not the same thing and must not re-lock the page. */
  if (res.status === 401 && body?.error === "locked") onUnauthorized?.();
  if (!res.ok) throw new Error((body && body.error) || res.statusText);
}

export async function api<T = any>(path: string, options?: RequestInit): Promise<T> {
  const res = await fetch(path, options);
  let body: any = null;
  try { body = await res.json(); } catch { /* empty body is fine */ }
  check(res, body);
  return body as T;
}

/* The same for an answer that is a file rather than JSON: the response itself
 * when it succeeded, the {"error"} it carries when not. */
export async function apiFile(path: string, options?: RequestInit): Promise<Response> {
  const res = await fetch(path, options);
  if (!res.ok) {
    let body: any = null;
    try { body = await res.json(); } catch { /* no reason given */ }
    check(res, body);
  }
  return res;
}

const json = (method: string, payload: unknown): RequestInit => ({
  method, headers: { "Content-Type": "application/json" }, body: JSON.stringify(payload),
});

export const post = <T = any>(path: string, payload: unknown = {}) => api<T>(path, json("POST", payload));
export const patch = <T = any>(path: string, payload: unknown) => api<T>(path, json("PATCH", payload));
export const del = <T = any>(path: string) => api<T>(path, { method: "DELETE" });
