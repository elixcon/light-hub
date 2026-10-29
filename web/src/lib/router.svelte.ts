/* Hash routes: the coordinator serves one HTML route, and the fragment never
 * reaches it, so the phone's back button works and a reload lands on the same
 * view. */
/* A detail view's name starts with its list view's name less the final "s" -
 * that prefix is how StaggeredMenu marks the current section. */
export type RouteName = "groups" | "group" | "lights" | "light" | "remotes" | "remote" | "remote-button"
  | "sensors" | "sensor" | "settings" | "access" | "matter" | "wifi" | "setup";

export interface Route { name: RouteName; params: Record<string, string> }

function parse(hash: string): Route {
  const parts = hash.replace(/^#\/?/, "").split("/").filter(Boolean).map(decodeURIComponent);
  const [a, b, c] = parts;
  switch (a) {
    case "groups":  return b ? { name: "group", params: { id: b } } : { name: "groups", params: {} };
    case "lights":  return b && c ? { name: "light", params: { ieee: b, ep: c } } : { name: "lights", params: {} };
    /* "buttons" is an older name for this view, kept so bookmarks still land
     * on it. */
    case "remotes":
    case "buttons":
      /* A remote by its IEEE address (a trailing endpoint, from older links, is
       * ignored); a button by its number. */
      if (b && b.startsWith("0x")) return { name: "remote", params: { ieee: b } };
      return b ? { name: "remote-button", params: { id: b } } : { name: "remotes", params: {} };
    case "sensors": return b ? { name: "sensor", params: { ieee: b } } : { name: "sensors", params: {} };
    case "settings":
      if (b === "access" || b === "matter" || b === "wifi") return { name: b, params: {} };
      return { name: "settings", params: {} };
    case "setup":   return { name: "setup", params: {} };
    default:        return { name: "groups", params: {} };
  }
}

export const route = $state<Route>(parse(location.hash));

window.addEventListener("hashchange", () => {
  const r = parse(location.hash);
  route.name = r.name;
  route.params = r.params;
  window.scrollTo(0, 0);
});

export function navigate(path: string, replace = false) {
  const hash = "#" + (path.startsWith("/") ? path : "/" + path);
  if (replace) history.replaceState(null, "", hash);
  else location.hash = hash;
  if (replace) { const r = parse(hash); route.name = r.name; route.params = r.params; }
}

export const back = () => history.back();
