<script lang="ts">
	/* The Light Node's page, up only while its Wi-Fi is: what is wired to it, its
	 * network and its firmware. Rooms, curves and what a button does are the hub's.
	 * The light editor and the Wi-Fi form are the hub's own components. */
	import { onMount } from "svelte";
	import Toast from "$lib/components/Toast.svelte";
	import ConfirmDialog from "$lib/components/ConfirmDialog.svelte";
	import LockScreen from "$lib/components/LockScreen.svelte";
	import StatusChips from "$lib/components/StatusChips.svelte";
	import LightView from "../views/LightView.svelte";
	import NodeHome from "./NodeHome.svelte";
	import NodeButton from "./NodeButton.svelte";
	import NodeSettings from "./NodeSettings.svelte";
	import { route } from "$lib/router.svelte.ts";
	import { app, refresh, stopPollingWhileHidden } from "$lib/state.svelte.ts";
	import { auth } from "$lib/auth.svelte.ts";

	onMount(() => { stopPollingWhileHidden(); refresh(); });

	const node = $derived(app.status?.node);
	const minutes = $derived(node && node.window_left_s >= 0 ? Math.ceil(node.window_left_s / 60) : null);
	const chips = $derived.by(() => {
		const s = app.status;
		if (app.unreachable) return [{ text: "Node unreachable", bad: true }];
		if (!s) return [{ text: "Connecting…", bad: false }];
		const out = [];
		out.push(s.zigbee.network_up
			? { text: `On Zigbee, channel ${s.zigbee.channel}`, bad: false }
			: { text: "Looking for a hub", bad: true });
		if (s.wifi.mode === "ap") out.push({ text: "Setup hotspot", bad: false });
		else if (s.wifi.connected) out.push({ text: `${s.wifi.ssid} · ${s.wifi.ip}`, bad: false });
		else out.push({ text: "Wi-Fi connecting…", bad: true });
		if (minutes !== null) out.push({ text: `Wi-Fi closes in ${minutes} min`, bad: false });
		return out;
	});
</script>

{#if auth.locked}
	<LockScreen />
{:else}
	<main class="mx-auto max-w-[640px] px-4 pb-16 pt-4 md:max-w-[900px]">
		<header class="pt-[env(safe-area-inset-top)]">
			<div class="flex items-start justify-between gap-3">
				<h1 class="text-2xl font-semibold tracking-tight">{app.name}</h1>
				<nav class="flex shrink-0 gap-3 pt-1.5 text-sm">
					<a href="#/lights" class="text-primary underline-offset-4 hover:underline">Wiring</a>
					<a href="#/settings" class="text-primary underline-offset-4 hover:underline">Settings</a>
				</nav>
			</div>
			<StatusChips {chips} />
		</header>
		{#if route.name === "light"}
			<LightView ieee={route.params.ieee} ep={Number(route.params.ep)} node />
		{:else if route.name === "remote-button"}
			<NodeButton id={Number(route.params.id)} />
		{:else if route.name === "settings" || route.name === "wifi"}
			<NodeSettings />
		{:else}
			<NodeHome />
		{/if}
	</main>
{/if}
<Toast />
<ConfirmDialog />
