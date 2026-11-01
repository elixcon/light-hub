<script lang="ts">
	import * as Card from "$lib/components/ui/card";
	import { Button } from "$lib/components/ui/button";
	import { Input } from "$lib/components/ui/input";
	import { Label } from "$lib/components/ui/label";
	import { Progress } from "$lib/components/ui/progress";
	import Upload from "@lucide/svelte/icons/upload";
	import WifiForm from "$lib/components/WifiForm.svelte";
	import { app, refresh } from "$lib/state.svelte.ts";
	import { post } from "$lib/api.ts";
	import { toast } from "$lib/toast.svelte.ts";
	import { confirm } from "$lib/confirm.svelte.ts";
	import { ota, uploadFirmware } from "$lib/ota.svelte.ts";

	const s = $derived(app.status);
	const z = $derived(s?.zigbee);
	let name = $state("");
	let named = false;
	$effect(() => { if (s && !named) { name = s.name; named = true; } });
	let fileInput: HTMLInputElement;

	async function rename() {
		if (!name.trim() || name === s?.name) return;
		try { await post("/api/settings", { name: name.trim() }); toast("Renamed — it restarts onto its new name"); }
		catch (err: any) { toast(err.message); }
	}
	function pickFile() {
		const file = fileInput.files?.[0];
		fileInput.value = "";
		if (file) uploadFirmware(file);
	}
	async function close() {
		if (!(await confirm("Close Wi-Fi and go back to Zigbee alone? This page goes away. To come back, ask from the hub, or press a wired button ten times in a steady rhythm.", { confirmLabel: "Close Wi-Fi" }))) return;
		try { await post("/api/node/close"); toast("Closing Wi-Fi"); }
		catch (err: any) { toast(err.message); }
	}
	async function leave() {
		if (!(await confirm("Forget this node's Zigbee network? It stops being on the hub, and looks for a network to join. Remove it from the hub too.", { confirmLabel: "Leave network" }))) return;
		try { await post("/api/node/leave"); toast("Leaving the network"); }
		catch (err: any) { toast(err.message); }
	}
	async function restart() {
		try { await post("/api/restart"); toast("Restarting — the page reconnects by itself"); }
		catch (err: any) { toast(err.message); }
		refresh();
	}
</script>

<Card.Root class="mt-4">
	<Card.Header><Card.Title>Zigbee</Card.Title></Card.Header>
	<Card.Content class="flex flex-col gap-3">
		<dl class="text-muted-foreground grid grid-cols-[auto_1fr] gap-x-4 gap-y-1 text-sm">
			<dt>Network</dt>
			<dd class="text-foreground">{z?.network_up ? `on channel ${z.channel}` : "looking for one to join"}</dd>
			{#if z?.network_up && z.parent}
				<dt>Connected through</dt>
				<dd class="text-foreground">{z.parent.coordinator ? "the hub itself" : "a router"}, {z.parent.rssi} dBm</dd>
			{/if}
			{#if s?.node?.hub_down}<dt>Hub</dt><dd class="text-destructive">could not be reached by the last press</dd>{/if}
		</dl>
		<div class="flex flex-wrap gap-2">
			<Button variant="outline" onclick={close}>Close Wi-Fi now</Button>
			{#if s?.node?.paired}<Button variant="destructive" onclick={leave}>Leave network</Button>{/if}
		</div>
	</Card.Content>
</Card.Root>

<Card.Root class="mt-4">
	<Card.Header><Card.Title>Name</Card.Title></Card.Header>
	<Card.Content class="flex flex-col gap-2">
		<Label for="nname">What this node is called on your network</Label>
		<Input id="nname" bind:value={name} maxlength={31} onchange={rename} />
	</Card.Content>
</Card.Root>

<Card.Root class="mt-4">
	<Card.Header><Card.Title>Wi-Fi</Card.Title><Card.Description>Used only while configuring: normally this node has Wi-Fi off. Saving a network restarts it onto that one.</Card.Description></Card.Header>
	<Card.Content><WifiForm /></Card.Content>
</Card.Root>

<Card.Root class="mt-4">
	<Card.Header><Card.Title>Firmware</Card.Title><Card.Description>Version {s?.version ?? "unknown"}, updater {s?.ota?.updater ?? "unknown"}. Upload the node's own elixcon_light_node.bin. It restarts into its updater to install it, and comes back on Wi-Fi.</Card.Description></Card.Header>
	<Card.Content class="flex flex-col gap-3">
		<input type="file" accept=".bin,application/octet-stream" hidden bind:this={fileInput} onchange={pickFile} />
		<div class="flex flex-wrap gap-2">
			<Button variant="outline" disabled={ota.busy || s?.ota?.updater_valid === false} onclick={() => fileInput.click()}><Upload /> Update firmware</Button>
			<Button variant="ghost" onclick={restart}>Restart</Button>
		</div>
		{#if ota.shown}
			{#if ota.fraction === null}
				<div class="bg-muted h-2 w-full overflow-hidden rounded-full"><div class="bg-primary h-full w-1/3 animate-pulse rounded-full"></div></div>
			{:else}
				<Progress value={ota.fraction * 100} class="h-2" />
			{/if}
			<div class="text-muted-foreground text-xs">{ota.message}</div>
		{/if}
	</Card.Content>
</Card.Root>
