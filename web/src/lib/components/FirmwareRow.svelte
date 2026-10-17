<script lang="ts">
	/* A Zigbee device's firmware: what it runs, whether the Zigbee2MQTT index has
	 * something newer, and the update on its way. See zbota.svelte.ts. */
	import { Button } from "$lib/components/ui/button";
	import { Progress } from "$lib/components/ui/progress";
	import { app } from "$lib/state.svelte.ts";
	import { kb, versionText } from "$lib/zbota.ts";
	import { zbota, checkForUpdate, installEntry, installFile, askAgain, cancelTarget, isSleepy, transferText,
		type UpdateCheck } from "$lib/zbota.svelte.ts";
	import type { Device } from "$lib/types.ts";

	let { device }: { device: Device } = $props();

	const o = $derived(device.ota ?? null);
	const status = $derived(app.status?.zigbee.ota);
	const oldLayout = $derived(app.status?.ota?.layout === "old");
	const known = $derived(!!o && o.manufacturer_code != null && o.image_type != null && o.file_version != null);
	const sleepy = $derived(isSleepy(device));
	const remote = $derived(transferText(device.ieee, sleepy));
	const waking = $derived(zbota.asking === device.ieee);
	const local = $derived(zbota.ieee === device.ieee && zbota.message ? zbota : null);

	let check = $state<UpdateCheck | null>(null);
	let checking = $state(false);
	let checkError = $state("");
	let fileInput = $state<HTMLInputElement>();

	async function runCheck() {
		checking = true;
		checkError = "";
		try { check = await checkForUpdate(device); }
		catch (err: any) { checkError = err.message; check = null; }
		checking = false;
	}
	function pickFile() {
		const f = fileInput?.files?.[0];
		if (fileInput) fileInput.value = "";
		if (f) installFile(device, f);
	}
</script>

{#snippet wake()}
	<div class="bg-muted h-2 w-full overflow-hidden rounded-full"><div class="bg-primary h-full w-1/3 animate-pulse rounded-full"></div></div>
	<p class="text-xs">Press one of its buttons now to wake it — it does whatever that button is set up to do.</p>
{/snippet}

{#if o}
	<div class="flex flex-col gap-2 text-sm">
		<div>
			<span class="text-muted-foreground">Firmware</span>
			<span class="ml-2 font-mono text-xs">{versionText(o.file_version)}</span>
			{#if o.manufacturer_code != null && o.image_type != null}
				<span class="text-muted-foreground ml-1 text-xs">· manufacturer 0x{o.manufacturer_code.toString(16).padStart(4, "0")}, image 0x{o.image_type.toString(16).padStart(4, "0")}</span>
			{/if}
		</div>

		{#if oldLayout}
			<p class="text-muted-foreground text-xs">Updating it needs room on the coordinator first — see Settings, Zigbee device firmware.</p>
		{:else if status?.state === "no_partition"}
			<p class="text-muted-foreground text-xs">This coordinator has no room for device firmware.</p>
		{:else if !known}
			{#if waking}
				{@render wake()}
			{:else if check?.kind === "no_answer"}
				<p class="text-muted-foreground text-xs">It did not ask. Press <em>Ask it</em> and then one of its buttons again; a remote that never asks does not take firmware over Zigbee.</p>
			{:else if sleepy}
				<p class="text-muted-foreground text-xs">It has not said what firmware it runs since the coordinator started. A remote says so when it is woken: press <em>Ask it</em>, then one of its buttons.</p>
			{:else if check?.kind === "unknown"}
				<p class="text-muted-foreground text-xs">It did not answer. A device that never does may not take firmware over Zigbee.</p>
			{:else}
				<p class="text-muted-foreground text-xs">It has not said what firmware it runs yet.</p>
			{/if}
			{#if checkError}<p class="text-destructive text-xs">{checkError}</p>{/if}
			<div><Button variant="outline" size="sm" disabled={checking} onclick={runCheck}>{checking ? "Asking…" : sleepy ? "Ask it" : "Check for updates"}</Button></div>
		{:else}
			{#if remote && !(local && zbota.busy)}
				{#if remote.fraction === null}
					<div class="bg-muted h-2 w-full overflow-hidden rounded-full"><div class="bg-primary h-full w-1/3 animate-pulse rounded-full"></div></div>
				{:else if !remote.done}
					<Progress value={remote.fraction * 100} class="h-2" />
				{/if}
				<p class="text-muted-foreground text-xs">{remote.text}</p>
				{#if !remote.done}
					<div class="flex flex-wrap gap-2">
						{#if status?.state !== "serving"}<Button variant="outline" size="sm" onclick={() => askAgain(device)}>Ask again</Button>{/if}
						<Button variant="ghost" size="sm" onclick={cancelTarget}>Cancel</Button>
					</div>
				{/if}
			{:else if local}
				{#if local.fraction === null}
					<div class="bg-muted h-2 w-full overflow-hidden rounded-full"><div class="bg-primary h-full w-1/3 animate-pulse rounded-full"></div></div>
				{:else if local.fraction > 0}
					<Progress value={local.fraction * 100} class="h-2" />
				{/if}
				<p class="text-muted-foreground text-xs">{local.message}</p>
			{/if}

			{#if !zbota.busy && !(remote && !remote.done)}
				{#if waking}
					{@render wake()}
				{:else if check?.kind === "available"}
					<p class="text-xs">
						{versionText(check.entry.fileVersion)} is available ({kb(check.entry.fileSize)}).
						{#if check.entry.releaseNotes?.startsWith("http")}<a class="text-primary underline" href={check.entry.releaseNotes} target="_blank" rel="noreferrer">Release notes</a>{:else if check.entry.releaseNotes}<span class="text-muted-foreground">{check.entry.releaseNotes}</span>{/if}
					</p>
					{#if !check.fits}
						<p class="text-muted-foreground text-xs">It is larger than the {kb(status?.room ?? 0)} the coordinator can hold.</p>
					{/if}
				{:else if check?.kind === "current"}
					<p class="text-muted-foreground text-xs">No newer firmware in the Zigbee2MQTT index.</p>
				{/if}
				{#if checkError}<p class="text-destructive text-xs">{checkError}</p>{/if}
				<div class="flex flex-wrap gap-2">
					{#if check?.kind === "available" && check.fits}
						{@const entry = check.entry}
						<!-- The entry is read before the check is cleared: a {@const} is derived from
						     `check` and is recomputed on read, so read afterwards it throws and the
						     click silently does nothing. -->
						<Button size="sm" onclick={() => { const picked = entry; check = null; installEntry(device, picked); }}>Install {versionText(entry.fileVersion)}</Button>
					{:else}
						<Button variant="outline" size="sm" disabled={checking} onclick={runCheck}>{checking ? "Checking…" : "Check for updates"}</Button>
					{/if}
					<input type="file" accept=".ota,.zigbee,.bin,.fw,.signed,.sbl-ota,application/octet-stream" hidden bind:this={fileInput} onchange={pickFile} />
					<Button variant="ghost" size="sm" onclick={() => fileInput?.click()}>Install from a file…</Button>
				</div>
				{#if sleepy}
					<p class="text-muted-foreground text-xs">A remote only hears the offer when it is woken: after installing, press one of its buttons. It is offered again each time it wakes for ten minutes.</p>
				{/if}
			{/if}
		{/if}
	</div>
{/if}
