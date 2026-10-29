<script lang="ts">
	/* One motion sensor: its name, what its rules see now, the rules for motion
	 * and for no motion, and taking it off the network. Its rules are a button
	 * record with the sensor as its source (button_db.h); a sensor that has none
	 * yet is set up here with one room. */
	import * as Card from "$lib/components/ui/card";
	import { Button } from "$lib/components/ui/button";
	import { Label } from "$lib/components/ui/label";
	import { Input } from "$lib/components/ui/input";
	import Trash from "@lucide/svelte/icons/trash-2";
	import ViewHeader from "$lib/components/ViewHeader.svelte";
	import GestureEditor from "$lib/components/GestureEditor.svelte";
	import SensorState from "$lib/components/SensorState.svelte";
	import { data, refresh, schedule } from "$lib/state.svelte.ts";
	import { navigate } from "$lib/router.svelte.ts";
	import { api, post, patch, del } from "$lib/api.ts";
	import { toast } from "$lib/toast.svelte.ts";
	import { confirm } from "$lib/confirm.svelte.ts";
	import { MAX_RULES, blankRule, cleanRules, rowsUsed } from "$lib/buttons.ts";
	import { CLEAR_DEFAULT_S, CLEAR_MAX_S, SENSOR_EVENTS, sensorCards, setupRules } from "$lib/sensors.ts";
	import type { Rule, SensorEvent } from "$lib/types.ts";

	let { ieee }: { ieee: string } = $props();

	const c = $derived(sensorCards(data.devices, data.buttons).find((x) => x.ieee === ieee) ?? null);
	const rec = $derived(c?.button ?? null);

	/* The device's name is the sensor's name, as a remote's is. Seeded once; the
	 * poll must not wipe a half-typed name. */
	let name = $state("");
	let seeded = $state("");
	$effect(() => {
		if (c?.device && seeded !== c.ieee) { seeded = c.ieee; name = c.device.name; }
	});

	/* The rules' working copy, seeded once per record, saved as a whole. */
	const w = $state({ loaded: -1, minutes: CLEAR_DEFAULT_S / 60, events: { motion: [] as Rule[], clear: [] as Rule[] } });
	$effect(() => {
		if (rec && w.loaded !== rec.id) {
			w.loaded = rec.id;
			w.minutes = Math.max(1, Math.round((rec.clear_s ?? CLEAR_DEFAULT_S) / 60));
			for (const [ev] of SENSOR_EVENTS) {
				const rules = (rec.events[ev] || []).map((r) => ({ ...r }));
				w.events[ev] = rules.length ? rules : [blankRule()];
			}
		}
	});
	const left = $derived(MAX_RULES - rowsUsed(w.events));
	const usesSun = $derived(Object.values(w.events).some((rules) => rules.some((r) => r.when === "sun_down")));

	/* Whether "when the sun is down" can be answered: it needs the location the
	 * Adaptive Lighting settings hold. Asked once per sensor shown: going from one
	 * sensor to the next keeps this page, and the location may have changed. */
	let located = $state(true);
	$effect(() => {
		void ieee;
		api<{ location: unknown }>("/api/settings").then((s) => (located = !!s.location)).catch(() => {});
	});
	/* A sensor that says "unoccupied" itself has none of the Xiaomi's minute. */
	const reportsClear = $derived(!!c?.device?.endpoints.find((e) => e.ep === c.ep)?.reports_clear);

	const setup = $state({ group: 0, busy: false });
	$effect(() => { if (!setup.group && data.groups.length) setup.group = data.groups[0].id; });

	async function runSetup() {
		if (!c || !setup.group) return;
		setup.busy = true;
		let id = 0;
		try {
			id = (await post<{ id: number }>("/api/buttons", { name: (c.device?.name || "Motion sensor").slice(0, 31) })).id;
			await patch(`/api/buttons/${id}`, { source: "sensor", ieee: c.ieee, ep: c.ep, clear_s: CLEAR_DEFAULT_S, events: setupRules(setup.group) });
			toast("Set up");
		} catch (err: any) {
			/* A record that did not become this sensor's would be a stray button. */
			if (id) await del(`/api/buttons/${id}`).catch(() => {});
			toast(err.message);
		} finally {
			setup.busy = false;
			refresh();
		}
	}

	async function save(quiet = false): Promise<boolean> {
		if (!c) return false;
		try {
			const n = name.trim();
			if (c.device && n && n !== c.device.name) await patch(`/api/devices/${c.ieee}`, { name: n });
			if (rec) {
				const events: Record<SensorEvent, Rule[]> = { motion: cleanRules(w.events.motion), clear: cleanRules(w.events.clear) };
				const clear_s = Math.max(60, Math.min(CLEAR_MAX_S, Math.round(w.minutes) * 60));
				await patch(`/api/buttons/${rec.id}`, { ...(n ? { name: n.slice(0, 31) } : {}), clear_s, events });
			}
			if (!quiet) toast("Saved");
			refresh();
			return true;
		} catch (err: any) { toast(err.message); return false; }
	}

	/* Saved first, as a button's test is: what is on the screen is what is meant. */
	async function test(ev: SensorEvent) {
		if (!rec || !(await save(true))) return;
		try {
			await post(`/api/buttons/${rec.id}/test`, { event: ev });
			schedule(600);
		} catch (err: any) { toast(err.message); }
	}

	/* Its rules go with it, as a remote's buttons do. A sleepy sensor is not sent
	 * a leave request (see zb_remove_device), so resetting it is what takes it
	 * off the network. */
	async function remove() {
		if (!c) return;
		const title = c.device?.name ?? "this sensor";
		if (!(await confirm(`Remove ${title}? Its rules are deleted too. To take it off the network as well, reset the sensor itself (usually a long press of its reset button).`,
			{ confirmLabel: "Remove", destructive: true }))) return;
		try {
			if (rec) await del(`/api/buttons/${rec.id}`);
			if (c.device) await del(`/api/devices/${c.ieee}`);
			toast(`${title} removed`);
			navigate("/sensors");
		} catch (err: any) { toast(err.message); }
		refresh();
	}
</script>

<ViewHeader title={c?.device?.name ?? rec?.name ?? "Sensor"} back="/sensors" />

{#if !c}
	<p class="text-muted-foreground mt-6 text-sm">This sensor is gone.</p>
{:else}
	<Card.Root class="mt-4">
		<Card.Content class="flex flex-col gap-5">
			{#if c.device}
				<div>
					<Label for="sname" class="mb-2 block">Name</Label>
					<Input id="sname" bind:value={name} maxlength={31} onkeydown={(e) => { if (e.key === "Enter") save(); }} />
				</div>
				<div class="text-muted-foreground text-xs">{[c.device.manufacturer, c.device.model].filter(Boolean).join(" · ") || c.ieee}</div>
			{:else}
				<p class="text-muted-foreground text-sm">This sensor is no longer paired. Its rules are kept until you remove them.</p>
			{/if}
			{#if rec}<SensorState button={rec} />{/if}

			{#if !rec}
				{#if !data.groups.length}
					<p class="text-sm">There is no room for this sensor to switch yet. <a href="#/groups" class="text-primary underline underline-offset-4">Create a room</a> first.</p>
				{:else}
					<div>
						<Label for="sgroup" class="mb-2 block">Which room it switches</Label>
						<select id="sgroup" class="sel w-full" bind:value={setup.group}>
							{#each data.groups as g (g.id)}<option value={g.id}>{g.name}</option>{/each}
						</select>
						<p class="text-muted-foreground mt-2 text-xs">On with motion, off after two minutes without. Both can be changed afterwards.</p>
					</div>
					<div><Button disabled={setup.busy || !setup.group} onclick={runSetup}>{setup.busy ? "Setting up…" : "Set up"}</Button></div>
				{/if}
			{:else}
				<div>
					<Label for="sclear" class="mb-2 block">No motion means none for</Label>
					<div class="flex items-center gap-2">
						<input id="sclear" class="sel w-24" type="number" min="1" max={CLEAR_MAX_S / 60} bind:value={w.minutes} />
						<span class="text-muted-foreground text-sm">min</span>
					</div>
					{#if !reportsClear}
						<p class="text-muted-foreground mt-2 text-xs">A Xiaomi or Aqara sensor reports motion at most once a minute, so under two minutes the room can go dark with somebody still in it.</p>
					{/if}
				</div>
				{#each SENSOR_EVENTS as [ev, label] (ev)}
					<GestureEditor gesture={ev} {label} sensor rules={w.events[ev]} {left} ontest={() => test(ev)} />
				{/each}
				{#if usesSun && !located}
					<p class="text-destructive text-sm">“When the sun is down” needs a location, and there is none: it counts as night until one is set under <a href="#/settings" class="underline underline-offset-4">Settings</a>.</p>
				{/if}
			{/if}

			<div class="border-border flex items-center justify-between gap-2 border-t pt-4">
				<Button variant="destructive" onclick={remove}><Trash /> Remove sensor</Button>
				<Button onclick={() => save()}>Save</Button>
			</div>
		</Card.Content>
	</Card.Root>
{/if}

<style>
	.sel { height: 2.5rem; border-radius: var(--radius-md); border: 1px solid var(--input); background: var(--card);
		color: var(--foreground); padding: 0 .6rem; font-size: .875rem; }
</style>
