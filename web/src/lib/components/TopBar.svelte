<script lang="ts">
	import { app } from "$lib/state.svelte.ts";
	import { hex4, plural } from "$lib/format.ts";

	/* The chips lie on the aurora. In the dark theme a 10 % tint left teal-300
	 * text at about 3:1 there, and ink at 80 % keeps both kinds above 4.5:1
	 * where it is brightest. In the light theme teal-600 on its own 10 % tint
	 * was 4.2:1 even on plain sand; on white it is 4.9:1. */
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

<header class="pr-12 pt-[env(safe-area-inset-top)]">
	<h1 class="text-2xl font-semibold tracking-tight">{app.name}</h1>
	<div class="mt-1.5 flex flex-wrap gap-1.5 text-xs">
		{#each chips as c, i (i)}
			<span class="bg-card dark:bg-background/80 rounded-full px-2 py-0.5 {c.bad ? 'text-destructive font-medium' : 'text-primary'}">{c.text}</span>
		{/each}
	</div>
</header>
