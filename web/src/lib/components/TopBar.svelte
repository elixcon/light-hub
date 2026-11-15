<script lang="ts">
	import PageHeader from "$lib/components/PageHeader.svelte";
	import { app } from "$lib/state.svelte.ts";
	import { hex4, plural } from "$lib/format.ts";

	interface Chip { text: string; bad: boolean }
	const chips = $derived.by((): Chip[] => {
		const s = app.status;
		if (app.unreachable) return [{ text: "Coordinator unreachable", bad: true }];
		if (!s) return [{ text: "Connecting…", bad: false }];
		const out: Chip[] = [];
		const w = s.wifi, z = s.zigbee;
		if (w.mode === "ap") out.push({ text: w.temporary_hotspot ? "Setup hotspot" : "Setup mode — no Wi-Fi yet", bad: true });
		else if (w.connected && w.weak_uplink) out.push({ text: `${w.ssid} · ${w.ip} · weak signal ${w.rssi} dBm`, bad: true });
		else out.push({ text: w.connected ? `${w.ssid} · ${w.ip}` : "Wi-Fi reconnecting…", bad: !w.connected });
		out.push(z.network_up ? { text: `PAN ${hex4(z.pan_id)} · ch ${z.channel}`, bad: false } : { text: "Zigbee starting…", bad: true });
		out.push({ text: plural(s.device_count, "device"), bad: false });
		return out;
	});
</script>

<PageHeader {chips} />
