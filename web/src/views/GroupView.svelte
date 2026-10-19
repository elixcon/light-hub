<script lang="ts">
	/* Rename, membership and delete for one group. Save sends one request per
	 * membership change rather than replacing the list. */
	import * as Card from "$lib/components/ui/card";
	import { Button } from "$lib/components/ui/button";
	import { Input } from "$lib/components/ui/input";
	import { Label } from "$lib/components/ui/label";
	import Trash from "@lucide/svelte/icons/trash-2";
	import type { AdaptiveExpose, TurnOn } from "$lib/types.ts";
	import ViewHeader from "$lib/components/ViewHeader.svelte";
	import AdaptiveDayEditor from "$lib/components/AdaptiveDayEditor.svelte";
	import { data, findGroup, refresh } from "$lib/state.svelte.ts";
	import { lightChoices } from "$lib/buttons.ts";
	import { api, post, patch, del } from "$lib/api.ts";
	import { toast } from "$lib/toast.svelte.ts";
	import { confirm } from "$lib/confirm.svelte.ts";
	import { navigate } from "$lib/router.svelte.ts";

	let { id }: { id: number } = $props();
	const g = $derived(findGroup(id));

	let name = $state("");
	let wanted = $state<Set<string>>(new Set());
	let expose = $state<AdaptiveExpose>("off");
	let turnOn = $state<TurnOn>("last");
	let loaded = $state(-1);
	const mk = (ieee: string, ep: number) => `${ieee}/${ep}`;
	const savedExpose = () => g?.adaptive?.expose ?? "off";
	const savedTurnOn = () => g?.turn_on ?? "last";

	/* Seed the form once per group; a poll must not wipe half-made ticks. */
	$effect(() => {
		if (g && loaded !== g.id) {
			loaded = g.id;
			name = g.name;
			wanted = new Set(g.members.map((m) => mk(m.ieee, m.ep)));
			expose = savedExpose();
			turnOn = savedTurnOn();
		}
	});

	const candidates = $derived(lightChoices(data.devices));

	const dirty = $derived.by(() => {
		if (!g) return false;
		const have = new Set(g.members.map((m) => mk(m.ieee, m.ep)));
		if (name.trim() !== g.name) return true;
		if (expose !== savedExpose()) return true;
		if (turnOn !== savedTurnOn()) return true;
		if (have.size !== wanted.size) return true;
		for (const k of wanted) if (!have.has(k)) return true;
		return false;
	});

	function toggle(k: string, on: boolean) {
		const s = new Set(wanted);
		if (on) s.add(k); else s.delete(k);
		wanted = s;
	}

	async function save() {
		if (!g) return;
		try {
			const body: Record<string, unknown> = {};
			if (name.trim() && name.trim() !== g.name) body.name = name.trim();
			if (expose !== savedExpose()) body.adaptive = expose;
			if (turnOn !== savedTurnOn()) body.turn_on = turnOn;
			let restart = false;
			if (Object.keys(body).length) {
				restart = !!(await patch<{ restart_needed?: boolean }>(`/api/groups/${g.id}`, body)).restart_needed;
			}
			const have = new Set(g.members.map((m) => mk(m.ieee, m.ep)));
			for (const c of candidates) {
				if (wanted.has(c.k) && !have.has(c.k)) await post(`/api/groups/${g.id}/members`, { ieee: c.ieee, ep: c.ep });
			}
			for (const m of g.members) {
				if (!wanted.has(mk(m.ieee, m.ep))) await del(`/api/groups/${g.id}/members/${m.ieee}/${m.ep}`);
			}
			await refresh();
			toast(restart ? "Saved — Matter controllers see the change after the coordinator restarts" : "Saved");
			navigate("/groups");
		} catch (err: any) { toast(err.message); }
	}

	/* What each exposure gives, and - once one is saved - whether the mode is running,
	 * which is the server's to say. */
	const signedK = (k: number) => `${k > 0 ? "+" : k < 0 ? "−" : "±"}${Math.abs(k)} K`;
	const exposeHelp = $derived.by(() => {
		switch (expose) {
			case "mode_select": return "A sun button on the room, the Adaptive Lighting button action, and a Normal / Adaptive mode on this room's Matter light. Home Assistant shows the mode; Apple Home does not.";
			case "endpoint": return `A sun button on the room, the Adaptive Lighting button action, and a second light “${name.trim() || g?.name} Adaptive” in Matter controllers, on while the mode runs. For Apple Home.`;
			default: return "No sun button and no Adaptive Lighting button action for this room.";
		}
	});
	const turnOnHelp = $derived.by(() => {
		if (turnOn === "adaptive" && expose === "off") return "Adaptive Lighting is off for this room, so it comes back as it was.";
		switch (turnOn) {
			case "adaptive": return "The room starts Adaptive Lighting, also when it is already on.";
			case "full": return "The room goes to full brightness, also when it is already on.";
			default: return "A dark room comes back at the brightness it last had; a lit one stays as it is.";
		}
	});
	const alLine = $derived.by(() => {
		const a = g?.adaptive;
		if (!a || a.expose === "off") return "";
		const now = a.pct !== null && a.offset_k !== null ? `${a.pct} %, ${signedK(a.offset_k)}` : "";
		switch (a.reason) {
			case "on": return `Running: ${now} right now.`;
			case "no_clock": return "Running, waiting for the coordinator's clock.";
			case "no_location": return "Running, waiting for a location — enter it under Settings.";
			default: return now ? `Not running. Started now, its day would give ${now}.` : "Not running. Enter a location under Settings before starting it.";
		}
	});

	async function remove() {
		if (!g) return;
		if (!(await confirm(`Delete ${g.name}? The lights themselves stay paired.`, { confirmLabel: "Delete", destructive: true }))) return;
		try { await del(`/api/groups/${g.id}`); navigate("/groups"); refresh(); }
		catch (err: any) { toast(err.message); }
	}
</script>

<ViewHeader title={g ? g.name : "Room"} back="/groups" />

{#if !g}
	<p class="text-muted-foreground mt-6 text-sm">This room is gone.</p>
{:else}
	<Card.Root class="mt-4">
		<Card.Content class="flex flex-col gap-5">
			<div>
				<Label for="gname" class="mb-2 block">Name</Label>
				<Input id="gname" bind:value={name} maxlength={31} />
			</div>
			<div>
				<div class="mb-1 font-medium">Lights</div>
				<div class="text-muted-foreground mb-3 text-xs">One slider moves every light you tick.</div>
				{#if !candidates.length}
					<div class="text-muted-foreground text-sm">No switchable lights are paired yet.</div>
				{:else}
					<div class="divide-border divide-y">
						{#each candidates as c (c.k)}
							<label class="flex cursor-pointer items-center gap-3 py-2.5">
								<input type="checkbox" class="accent-primary size-5" checked={wanted.has(c.k)} onchange={(e) => toggle(c.k, e.currentTarget.checked)} />
								<span class="flex-1 text-sm">{c.name}</span>
								<span class="text-muted-foreground text-xs">{c.kind}</span>
							</label>
						{/each}
					</div>
				{/if}
			</div>
			<div>
				<Label for="gadaptive" class="mb-2 block">Adaptive Lighting</Label>
				<select id="gadaptive" class="sel" bind:value={expose}>
					<option value="off">Off</option>
					<option value="mode_select">Mode Select (Matter standard)</option>
					<option value="endpoint">Second light (for Apple Home)</option>
				</select>
				<div class="text-muted-foreground mt-2 text-xs">{exposeHelp}</div>
				{#if alLine}<div class="text-muted-foreground mt-1 text-xs">{alLine}</div>{/if}
			</div>
			<div>
				<Label for="gturnon" class="mb-2 block">Turned on by Alexa or a Home app</Label>
				<select id="gturnon" class="sel" bind:value={turnOn}>
					<option value="last">As it was</option>
					<option value="adaptive" disabled={expose === "off" && turnOn !== "adaptive"}>Adaptive Lighting</option>
					<option value="full">Full brightness</option>
				</select>
				<div class="text-muted-foreground mt-2 text-xs">{turnOnHelp} The buttons and this page are not affected.</div>
			</div>
			<div class="flex items-center justify-between gap-2">
				<Button variant="destructive" onclick={remove}><Trash /> Delete room</Button>
				<Button disabled={!dirty || !name.trim()} onclick={save}>Save</Button>
			</div>
		</Card.Content>
	</Card.Root>
	{#if savedExpose() !== "off"}
		<AdaptiveDayEditor groupId={g.id} />
	{/if}
{/if}

<style>
	.sel { height: 2.5rem; width: 100%; border-radius: var(--radius-md); border: 1px solid var(--input); background: var(--card);
		color: var(--foreground); padding: 0 .6rem; font-size: .875rem; }
</style>
