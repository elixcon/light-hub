/* What the Wi-Fi form offers from a scan. The scan answers with every access point it
 * heard, which the Zigbee channel survey needs, but a network with two access points
 * is one choice here, at its strongest, and a hidden one has no name to choose. Its
 * list is keyed by name, and Svelte throws on a repeated key: the suggestions then
 * stay empty on any network with more than one access point. */
export function namedNetworks<T extends { ssid: string; rssi: number }>(scan: T[]): T[] {
  const seen = new Set<string>();
  return [...scan].sort((a, b) => b.rssi - a.rssi).filter((n) => {
    if (!n.ssid || seen.has(n.ssid)) return false;
    seen.add(n.ssid);
    return true;
  });
}
