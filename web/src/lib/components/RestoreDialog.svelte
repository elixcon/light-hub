<script lang="ts">
	/* What a configuration file holds, the two secrets it may replace, and the
	 * restart that carries it out. Open while `file` is set. */
	import * as Dialog from "$lib/components/ui/dialog";
	import { Button } from "$lib/components/ui/button";
	import { Label } from "$lib/components/ui/label";
	import { Switch } from "$lib/components/ui/switch";
	import { app } from "$lib/state.svelte.ts";
	import { toast } from "$lib/toast.svelte.ts";
	import { backup, restoreBackup } from "$lib/backup.svelte.ts";
	import type { BackupSummary, Secret } from "$lib/backup.ts";

	let { file = $bindable() }: { file: BackupSummary | null } = $props();

	const s = $derived(app.status);
	const h = $derived(file?.header);
	const wifiLeftOut = $derived(!!h?.omitted.includes("wifi"));
	const keysLeftOut = $derived(!!h?.omitted.includes("auth"));
	let wifi = $state(true);
	let keys = $state(true);
	/* Each file starts from what restoring it means: its Wi-Fi and its keys -
	 * except a file with no keys, where "its keys" is no protection at all,
	 * which nobody should get by not reading a switch. */
	$effect(() => {
		if (!file) return;
		wifi = !!file.ssid;
		keys = file.keys !== null && file.keys > 0;
	});

	const sameIeee = (a: string | null | undefined, b: string | null | undefined) =>
		!!a && !!b && a.toLowerCase() === b.toLowerCase();
	const otherBox = $derived(!!h?.ieee && !!s?.zigbee.ieee && !sameIeee(h.ieee, s.zigbee.ieee));
	/* The coordinator decides with the extended PAN as well; this is for words. */
	const otherNetwork = $derived(!h || h.pan_id === null || !s?.zigbee.network_up || h.pan_id !== s.zigbee.pan_id);
	const made = $derived(h?.created ? new Date(h.created * 1000).toLocaleString([], { dateStyle: "medium", timeStyle: "short" }) : "an unknown date");
	const contents = $derived(file?.counts.length ? file.counts.map(([what, n]) => `${n} ${what}`).join(", ") : "no devices, rooms or buttons");
	const leavesNetwork = $derived(wifi && !!file?.ssid && !!s?.wifi.ssid && file.ssid !== s.wifi.ssid);

	async function restore() {
		if (!file) return;
		const keep: Secret[] = [];
		if (!wifi) keep.push("wifi");
		if (!keys) keep.push("auth");
		try {
			const r = await restoreBackup(file, keep);
			file = null;
			toast(r.addresses_kept ? "Restored — the coordinator is restarting"
				: "Restored — the coordinator is restarting. Pair the lamps and remotes again; they keep their names and rooms.");
		} catch (err: any) {
			toast(err.message);
		}
	}
</script>

<Dialog.Root open={!!file} onOpenChange={(o) => { if (!o && !backup.busy) file = null; }}>
	<Dialog.Content showCloseButton={false}>
		<Dialog.Header>
			<Dialog.Title>Restore configuration</Dialog.Title>
			<Dialog.Description>
				From “{h?.name}” ({h?.hostname}), firmware {h?.firmware}, made {made}: {contents}.
			</Dialog.Description>
		</Dialog.Header>
		<div class="flex flex-col gap-3 text-sm">
			{#if otherBox}<p class="text-destructive">This file is from another coordinator.</p>{/if}
			{#if otherNetwork}
				<p>It was made on another Zigbee network, or before this one was formed. Lamps and remotes have to be paired again here, and then find their names, rooms, curves and buttons waiting.</p>
			{/if}
			<p class="text-muted-foreground">The settings, devices, rooms, curves, buttons, wired lights and Matter lamps become what the file says, and the coordinator restarts. The Zigbee network and the Matter controllers stay as they are.</p>

			<div class="flex items-center justify-between gap-4">
				<Label for="rswifi">Wi-Fi network</Label>
				<Switch id="rswifi" checked={wifi} disabled={!file?.ssid} aria-label="Restore the Wi-Fi network" onCheckedChange={(on) => (wifi = on)} />
			</div>
			<div class="text-muted-foreground -mt-2 text-xs">
				{#if wifiLeftOut}Left out when this file was made — the coordinator keeps its own.
				{:else if !file?.ssid}Not in this file — the coordinator keeps its own.
				{:else if wifi}“{file.ssid}”, with its password.{#if leavesNetwork}{" "}That is not the network the coordinator is on now: this page will lose it after the restart.{/if}
				{:else}The coordinator keeps “{s?.wifi.ssid}”.{/if}
			</div>

			<div class="flex items-center justify-between gap-4">
				<Label for="rskeys">{file?.keys === 0 && !keysLeftOut ? "Remove access protection" : "Access keys"}</Label>
				<Switch id="rskeys" checked={keys} disabled={keysLeftOut} aria-label="Restore the access keys" onCheckedChange={(on) => (keys = on)} />
			</div>
			<div class="text-muted-foreground -mt-2 text-xs">
				{#if keysLeftOut}Left out when this file was made — this coordinator keeps its keys.
				{:else if file?.keys === 0}The file has no access keys: with this on, anyone on the network can use the coordinator after the restart.
				{:else if keys}The file's {file?.keys === 1 ? "key replaces" : `${file?.keys} keys replace`} this coordinator's, and only {file?.keys === 1 ? "it opens" : "they open"} it afterwards. Without one, five presses of a button open it again.
				{:else}This coordinator keeps its own keys.{/if}
			</div>
		</div>
		<Dialog.Footer class="flex-row justify-end gap-2">
			<Button variant="ghost" disabled={backup.busy} onclick={() => (file = null)}>Cancel</Button>
			<Button variant="destructive" disabled={backup.busy} onclick={restore}>{backup.busy ? "Restoring…" : "Restore and restart"}</Button>
		</Dialog.Footer>
	</Dialog.Content>
</Dialog.Root>
