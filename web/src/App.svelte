<script lang="ts">
	import { onMount } from "svelte";
	import TopBar from "$lib/components/TopBar.svelte";
	import NavBar from "$lib/components/NavBar.svelte";
	import Banners from "$lib/components/Banners.svelte";
	import Toast from "$lib/components/Toast.svelte";
	import ConfirmDialog from "$lib/components/ConfirmDialog.svelte";
	import KeyDialog from "$lib/components/KeyDialog.svelte";
	import LockScreen from "$lib/components/LockScreen.svelte";
	import Backdrop from "$lib/components/Backdrop.svelte";
	import GroupsView from "./views/GroupsView.svelte";
	import GroupView from "./views/GroupView.svelte";
	import LightsView from "./views/LightsView.svelte";
	import LightView from "./views/LightView.svelte";
	import RemotesView from "./views/RemotesView.svelte";
	import RemoteView from "./views/RemoteView.svelte";
	import ButtonView from "./views/ButtonView.svelte";
	import SensorsView from "./views/SensorsView.svelte";
	import SensorView from "./views/SensorView.svelte";
	import SettingsView from "./views/SettingsView.svelte";
	import AccessView from "./views/AccessView.svelte";
	import MatterView from "./views/MatterView.svelte";
	import WifiView from "./views/WifiView.svelte";
	import SetupView from "./views/SetupView.svelte";
	import { route, navigate } from "$lib/router.svelte.ts";
	import { app, refresh } from "$lib/state.svelte.ts";
	import { auth } from "$lib/auth.svelte.ts";

	onMount(() => { refresh(); });

	/* In the provisioning AP there is nothing but Wi-Fi to set up, and the
	 * captive portal has just opened this page. The ten-press hotspot is the
	 * exception: it keeps the stored network and the whole UI with it. */
	const setupOnly = $derived(app.status?.wifi.mode === "ap" && !app.status.wifi.temporary_hotspot);
	$effect(() => { if (setupOnly && route.name !== "setup") navigate("/setup", true); });
</script>

<Backdrop />
{#if auth.locked}
	<LockScreen />
{:else}
	{#if !setupOnly}
		<NavBar items={[
			["Rooms", "/groups", "groups group"], ["Lights", "/lights", "lights light"],
			["Remotes", "/remotes", "remotes remote remote-button"], ["Sensors", "/sensors", "sensors sensor"],
			["Settings", "/settings", "settings access matter wifi"],
		]} />
	{/if}
	<main class="mx-auto max-w-[640px] px-4 pb-28 pt-4 md:max-w-[900px] md:pb-16 md:pt-20">
		<TopBar />
		<Banners />
		{#if route.name === "setup" || setupOnly}
			<SetupView />
		{:else if route.name === "group"}
			<GroupView id={Number(route.params.id)} />
		{:else if route.name === "lights"}
			<LightsView />
		{:else if route.name === "light"}
			<LightView ieee={route.params.ieee} ep={Number(route.params.ep)} />
		{:else if route.name === "remotes"}
			<RemotesView />
		{:else if route.name === "remote"}
			<RemoteView ieee={route.params.ieee} />
		{:else if route.name === "remote-button"}
			<ButtonView id={Number(route.params.id)} />
		{:else if route.name === "sensors"}
			<SensorsView />
		{:else if route.name === "sensor"}
			<SensorView ieee={route.params.ieee} />
		{:else if route.name === "settings"}
			<SettingsView />
		{:else if route.name === "access"}
			<AccessView />
		{:else if route.name === "matter"}
			<MatterView />
		{:else if route.name === "wifi"}
			<WifiView />
		{:else}
			<GroupsView />
		{/if}
	</main>
{/if}
<Toast />
<ConfirmDialog />
<KeyDialog />
