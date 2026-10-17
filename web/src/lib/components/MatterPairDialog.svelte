<script lang="ts">
	/* Pairing a Matter lamp from a code another app gives out. The box finds the
	 * device on the network, pairs it and reads which of its endpoints are lamps;
	 * the steps are polled from /api/matter while it runs, which takes a few
	 * seconds for a Wi-Fi lamp and can take longer for one on Thread. */
	import * as Dialog from "$lib/components/ui/dialog";
	import { Button } from "$lib/components/ui/button";
	import { Input } from "$lib/components/ui/input";
	import { api, post } from "$lib/api.ts";
	import { refresh } from "$lib/state.svelte.ts";
	import { toast } from "$lib/toast.svelte.ts";
	import { plural } from "$lib/format.ts";
	import type { MatterStatus } from "$lib/types.ts";

	let { open = $bindable(false) }: { open?: boolean } = $props();

	let code = $state("");
	let busy = $state(false);
	let step = $state("");
	let error = $state("");

	/* What each step is, in words - `step` is the firmware's name for it. */
	const STEPS: Record<string, string> = {
		find: "Looking for the device on the network",
		pase: "Connecting with the code",
		arm: "Connecting with the code",
		csr: "Handing over this box's certificate",
		root: "Handing over this box's certificate",
		noc: "Handing over this box's certificate",
		case: "Connecting on this box's own network",
		complete: "Finishing the pairing",
		read: "Finding the lamps on it",
	};

	let timer: ReturnType<typeof setTimeout> | undefined;
	function stop() { clearTimeout(timer); timer = undefined; }

	async function poll() {
		try {
			const m = await api<MatterStatus>("/api/matter");
			const p = m.controller?.pairing;
			if (!p || p.state === "running") {
				step = p ? STEPS[p.step] ?? "" : "";
				timer = setTimeout(poll, 1000);
				return;
			}
			busy = false;
			if (p.state === "done") {
				toast(`Paired: ${plural(p.lamps, "lamp")}`);
				code = "";
				open = false;
				refresh();
			} else {
				error = p.error || "Pairing failed";
			}
		} catch (err: any) {
			/* One failed poll is not the pairing failing: keep asking. */
			timer = setTimeout(poll, 2000);
		}
	}

	async function pair() {
		const c = code.trim();
		if (!c || busy) return;
		error = "";
		step = STEPS.find;
		busy = true;
		try {
			await post("/api/matter/lamps/pair", { code: c });
			stop();
			timer = setTimeout(poll, 1000);
		} catch (err: any) {
			busy = false;
			error = err.message;
		}
	}

	$effect(() => { if (!open) stop(); });
</script>

<Dialog.Root {open} onOpenChange={(o) => { if (!busy) open = o; }}>
	<Dialog.Content class="max-h-[90vh] overflow-y-auto">
		<Dialog.Header>
			<Dialog.Title>Add a Matter lamp</Dialog.Title>
			<Dialog.Description>A lamp that is already in another app, or a hub such as a Hue Bridge with its lamps.</Dialog.Description>
		</Dialog.Header>
		<ol class="text-muted-foreground list-decimal space-y-1.5 pl-5 text-sm">
			<li>In Apple Home, open the lamp (or the hub), go to its settings and choose <b>Turn On Pairing Mode</b>. Other apps call it sharing or linking to another service.</li>
			<li>Type the code it shows below. It works for a few minutes.</li>
		</ol>
		<form class="flex gap-2" onsubmit={(ev) => { ev.preventDefault(); pair(); }}>
			<Input bind:value={code} placeholder="1234-567-8901 or MT:…" autocomplete="off" disabled={busy} class="font-mono" />
			<Button type="submit" disabled={busy || !code.trim()}>{busy ? "Pairing…" : "Pair"}</Button>
		</form>
		{#if busy}
			<p class="text-sm">{step || "Pairing…"}</p>
		{/if}
		{#if error}
			<p class="text-destructive text-sm">{error}</p>
		{/if}
		<p class="text-muted-foreground text-xs">
			Each light on the device becomes a light here, for rooms, curves and buttons like any other.
			The lamp stays in its other app too. Its certificate is not checked.
		</p>
		<Dialog.Footer>
			<Button variant="ghost" disabled={busy} onclick={() => (open = false)}>Close</Button>
		</Dialog.Footer>
	</Dialog.Content>
</Dialog.Root>
