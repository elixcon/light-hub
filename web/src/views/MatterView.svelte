<script lang="ts">
	/* Every group is offered to Matter controllers as a dimmable light; this is
	 * where the code to add them is. Looked up on its own rather than folded
	 * into /api/state, because nothing about it changes while nobody is here. */
	import * as Card from "$lib/components/ui/card";
	import { Button } from "$lib/components/ui/button";
	import ViewHeader from "$lib/components/ViewHeader.svelte";
	import { api, post } from "$lib/api.ts";
	import { toast } from "$lib/toast.svelte.ts";
	import { confirm } from "$lib/confirm.svelte.ts";
	import { matterCode, plural } from "$lib/format.ts";
	import { qrMatrix } from "$lib/qr.ts";
	import type { MatterStatus } from "$lib/types.ts";

	let m = $state<MatterStatus | null>(null);
	let error = $state("");
	async function load() {
		try { m = await api<MatterStatus>("/api/matter"); error = ""; }
		catch (err: any) { error = err.message; }
	}
	$effect(() => { load(); });

	/* One path of unit squares, offset by the 4-module quiet zone. */
	const qr = $derived.by(() => {
		const q = m?.qr_code ? qrMatrix(m.qr_code) : null;
		if (!q) return null;
		let d = "";
		q.forEach((row, y) => row.forEach((dark, x) => { if (dark) d += `M${x + 4} ${y + 4}h1v1h-1z`; }));
		return { size: q.length + 8, d };
	});

	async function openWindow() {
		try { await post("/api/matter/window"); load(); }
		catch (err: any) { toast(err.message); }
	}
	/* This box's own fabric, for its Matter lamps, is in the same table as the
	 * controllers' and takes one of the same slots - but is not a controller. */
	const controllers = $derived(m?.fabric_list?.filter((f) => !f.ours) ?? []);
	const ours = $derived(m?.fabric_list?.find((f) => f.ours) ?? null);
	const lampDevices = $derived(m?.controller?.nodes.length ?? 0);

	async function reset() {
		const lamps = lampDevices
			? ` Its Matter lamps are told to forget it and then have to be paired again; they stay in rooms, curves and buttons.`
			: "";
		if (!(await confirm(`Remove the coordinator from every Matter controller? Lights, rooms and buttons stay as they are.${lamps}`, { confirmLabel: "Reset Matter", destructive: true }))) return;
		try { await post("/api/matter/reset"); toast("Matter reset — the coordinator is restarting"); }
		catch (err: any) { toast(err.message); }
	}
</script>

<ViewHeader title="Matter" back="/settings" />
<div class="text-muted-foreground mt-1 text-sm">Each room is a dimmable light to Apple Home, Home Assistant or Alexa.</div>

<Card.Root class="mt-4">
	<Card.Content class="flex flex-col gap-4 text-sm">
		{#if error}
			<div class="text-destructive">{error}</div>
		{:else if !m}
			<div class="text-muted-foreground">Loading…</div>
		{:else if !m.running}
			<div class="text-muted-foreground">Matter is not running.</div>
		{:else}
			<div>{controllers.length ? `Added to ${plural(controllers.length, "controller")}` : "Not added to a controller yet"} · {plural(m.groups, "room")} shared as lights</div>
			{#if controllers.length}
				<div class="text-muted-foreground">{controllers.map((f) => f.label || `vendor ${f.vendor_id}`).join(" · ")}</div>
			{/if}
			{#if ours}
				<div class="text-muted-foreground">Controls {plural(lampDevices, "Matter device")} itself, on a fabric of its own. That takes one of the fabric slots the controllers share.</div>
			{/if}
			{#if m.window_open}
				<div class="bg-muted rounded-lg p-4 text-center">
					<div class="text-muted-foreground text-xs">Pairing code</div>
					<div class="mt-1 text-2xl font-semibold tabular-nums tracking-wider">{matterCode(m.manual_code)}</div>
					{#if qr}
						<!-- Black on white in either theme: scanners do not read an inverted code. -->
						<svg class="mx-auto mt-3 size-44 rounded" viewBox="0 0 {qr.size} {qr.size}" shape-rendering="crispEdges" role="img" aria-label="Matter QR code">
							<rect width={qr.size} height={qr.size} fill="#fff" /><path d={qr.d} fill="#000" />
						</svg>
					{/if}
					{#if m.qr_code}<div class="text-muted-foreground mt-2 font-mono text-xs break-all">{m.qr_code}</div>{/if}
					<div class="text-muted-foreground mt-2 text-xs">Scan the code, or enter the number, in Apple Home, Home Assistant or Alexa to add the rooms as lights.</div>
				</div>
			{/if}
			<div class="flex flex-wrap gap-2">
				{#if !m.window_open}<Button onclick={openWindow}>Allow pairing</Button>{/if}
				{#if m.fabrics}<Button variant="destructive" onclick={reset}>Reset Matter</Button>{/if}
			</div>
		{/if}
	</Card.Content>
</Card.Root>
