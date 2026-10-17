<script lang="ts">
	/* The button editor: a working copy, saved as a whole. The one live thing on
	 * it - what the pin reads, or whether a remote press has arrived - is derived
	 * from the poll and never touches the half-filled form. */
	import * as Card from "$lib/components/ui/card";
	import { Button } from "$lib/components/ui/button";
	import { Input } from "$lib/components/ui/input";
	import { Label } from "$lib/components/ui/label";
	import Trash from "@lucide/svelte/icons/trash-2";
	import ViewHeader from "$lib/components/ViewHeader.svelte";
	import GestureEditor from "$lib/components/GestureEditor.svelte";
	import { app, data, refresh, schedule } from "$lib/state.svelte.ts";
	import { api, post, patch, del } from "$lib/api.ts";
	import { toast } from "$lib/toast.svelte.ts";
	import { confirm } from "$lib/confirm.svelte.ts";
	import { navigate } from "$lib/router.svelte.ts";
	import { ALL_GESTURES, GESTURES, MAX_RULES, PINS, PIN_NOTE, PULLS, blankRule, cleanRules, remoteName, rowsUsed } from "$lib/buttons.ts";
	import { gesturesFor, keyName, knownRemote, remotePath } from "$lib/remotes.ts";
	import { ago } from "$lib/format.ts";
	import type { Gesture, InputKind, Pull, Rule } from "$lib/types.ts";

	let { id }: { id: number } = $props();
	const live = $derived(data.buttons.find((x) => x.id === id));
	/* A remote's button belongs to that remote's page; a wired one to the list. */
	const home = $derived(live?.source === "zigbee" && live.ieee ? remotePath(live.ieee) : "/remotes");

	const b = $state({
		loaded: -1, name: "", source: "gpio" as "gpio" | "zigbee",
		gpio: null as number | null, inverted: true, pull: "up" as Pull, input: "button" as InputKind,
		ieee: null as string | null, ep: null as number | null, key: null as number | null,
		capturing: false, captureFrom: 0,
		events: { short: [] as Rule[], long: [] as Rule[], double: [] as Rule[], triple: [] as Rule[] },
	});

	/* A known remote's key lists only the gestures it can send, by their names. */
	const known = $derived(knownRemote(data.devices.find((d) => d.ieee === b.ieee)));
	const gestures = $derived(b.source === "zigbee" && live ? gesturesFor(b.key, known, live.events) : GESTURES);

	/* Seeded once per button; the poll must not wipe half-made edits. */
	$effect(() => {
		if (live && b.loaded !== live.id) {
			b.loaded = live.id;
			b.name = live.name; b.source = live.source || "gpio";
			b.gpio = live.gpio; b.inverted = live.inverted; b.pull = live.pull; b.input = live.input || "button";
			b.ieee = live.ieee; b.ep = live.ep; b.key = live.key;
			for (const [ev] of ALL_GESTURES) {
				const rules = ((live.events && live.events[ev]) || []).map((r) => ({ ...r }));
				/* Always at least one row: a gesture that does nothing is still a
				 * sentence waiting to be filled in. Blank rows are dropped on save. */
				b.events[ev] = rules.length ? rules : [blankRule()];
			}
		}
	});
	$effect(() => { app.capturing = b.capturing; return () => { app.capturing = false; }; });

	/* While capturing, the first press with a sequence number newer than the
	 * moment Listening began is the one the user meant - the oldest of the new
	 * ones, since the list is newest first. */
	$effect(() => {
		if (!b.capturing) return;
		const seen = data.remotes.filter((r) => r.seq > b.captureFrom);
		const hit = seen.length ? seen[seen.length - 1] : null;
		if (hit) { b.ieee = hit.ieee; b.ep = hit.ep; b.key = hit.key; b.capturing = false; }
	});

	const pinLine = $derived.by(() => {
		if (b.source === "zigbee") {
			if (b.capturing) return { dot: "on", text: "Listening — press the button you want to use." };
			if (!b.ieee) return { dot: "", text: "No remote yet. Press the button below, then press the remote." };
			const heard = live && live.source === "zigbee" && live.ieee === b.ieee && live.last_event_ms != null
				? ` — last heard ${ago(live.last_event_ms)}` : "";
			return { dot: "", text: `${remoteName(b.ieee, data.devices)} · ${keyName(b.key, known)}${b.ep !== 1 ? ` · endpoint ${b.ep}` : ""}${heard}` };
		}
		if (b.gpio === null) return { dot: "", text: "Pick a pin and this line will show what it is reading." };
		if (!live || live.gpio !== b.gpio || live.level == null) return { dot: "off", text: `Save, and this will show what GPIO ${b.gpio} reads.` };
		return { dot: live.pressed ? "on" : "off", text: `GPIO ${live.gpio} reads ${live.level ? "HIGH" : "LOW"} — ${live.pressed ? "pressed" : "not pressed"}` };
	});

	/* The rules left in the button's pool, every gesture's rows counted. */
	const left = $derived(MAX_RULES - rowsUsed(b.events));

	const toGnd = $derived(b.pull === "up" && b.inverted);
	const to3v3 = $derived(b.pull === "down" && !b.inverted);
	function setSource(v: "gpio" | "zigbee") {
		b.source = v; b.capturing = false;
		/* The two halves are not interchangeable, so switching drops the other
		 * one's identity rather than leaving a stale pin or a stale key behind. */
		if (v === "zigbee") b.gpio = null;
		else { b.ieee = null; b.ep = null; b.key = null; if (b.pull !== "up" && b.pull !== "down") { b.pull = "up"; b.inverted = true; } }
	}
	function capture() {
		const newest = data.remotes[0];
		b.captureFrom = newest ? newest.seq : 0;
		b.capturing = !b.capturing;
		if (b.capturing) schedule(400);
	}

	async function save(keepOpen: boolean): Promise<boolean> {
		if (!b.name.trim()) { toast("Give the button a name"); return false; }
		const events: Record<string, Rule[]> = {};
		for (const [ev] of ALL_GESTURES) events[ev] = cleanRules(b.events[ev]);
		try {
			await patch(`/api/buttons/${id}`, b.source === "zigbee"
				? { name: b.name.trim(), source: "zigbee", ieee: b.ieee || null, ep: b.ep || null, key: b.key || 0, events }
				: { name: b.name.trim(), source: "gpio", gpio: b.gpio, inverted: b.inverted, pull: b.pull, input: b.input, events });
			if (keepOpen) toast("Saved"); else navigate(home);
			refresh();
			return true;
		} catch (err: any) { toast(err.message); return false; }
	}
	/* Saved first: testing what is on the screen rather than what is stored is
	 * the only thing anybody means by pressing this. A long press is held for a
	 * couple of seconds, because the sweep and the stop are the whole of it. */
	async function test(ev: Gesture) {
		if (!(await save(true))) return;
		try {
			await post(`/api/buttons/${id}/test`, ev === "long" ? { event: ev, hold_ms: 2500 } : { event: ev });
			schedule(600);
		} catch (err: any) { toast(err.message); }
	}
	async function remove() {
		if (!(await confirm(`Delete ${b.name || "this button"}? The pin is handed back.`, { confirmLabel: "Delete", destructive: true }))) return;
		try { await del(`/api/buttons/${id}`); navigate(home); refresh(); }
		catch (err: any) { toast(err.message); }
	}
</script>

<ViewHeader title={live ? live.name : "Button"} back={home} />

{#if !live}
	<p class="text-muted-foreground mt-6 text-sm">This button is gone.</p>
{:else}
	<Card.Root class="mt-4">
		<Card.Content class="flex flex-col gap-5">
			<div>
				<Label for="bname" class="mb-2 block">Name</Label>
				<Input id="bname" bind:value={b.name} maxlength={31} />
			</div>
			<div>
				<Label for="bsource" class="mb-2 block">Where its presses come from</Label>
				<select id="bsource" class="sel w-full" value={b.source} onchange={(e) => setSource(e.currentTarget.value as "gpio" | "zigbee")}>
					<option value="gpio">A button wired to a pin</option>
					<option value="zigbee">A paired Zigbee remote</option>
				</select>
			</div>

			{#if b.source === "zigbee"}
				<div class="flex items-center gap-2 text-sm">
					{#if pinLine.dot}<span class="inline-block size-2.5 rounded-full {pinLine.dot === 'on' ? 'bg-on animate-pulse' : 'bg-muted-foreground/40'}"></span>{/if}
					<span>{pinLine.text}</span>
				</div>
				<div><Button variant="outline" onclick={capture}>{b.capturing ? "Listening… press it now" : "Press the button to learn it"}</Button></div>
				<div class="text-muted-foreground text-xs">A remote does not say how many buttons it has, so which one you mean is settled by pressing it. Battery remotes sleep: press once to wake it if nothing arrives.</div>
			{:else}
				<div>
					<Label for="bpin" class="mb-2 block">Pin</Label>
					<select id="bpin" class="sel w-full" value={b.gpio === null ? "" : String(b.gpio)} onchange={(e) => (b.gpio = e.currentTarget.value === "" ? null : Number(e.currentTarget.value))}>
						<option value="">— no pin yet —</option>
						{#each PINS as p (p)}<option value={String(p)}>GPIO {p}{PIN_NOTE[p] ? ` — ${PIN_NOTE[p]}` : ""}</option>{/each}
					</select>
				</div>
				<div class="flex flex-col gap-2 text-sm">
					<div class="font-medium">What is wired to it</div>
					<label class="flex items-center gap-2"><input type="radio" name="input" class="accent-primary" checked={b.input === "button"} onchange={() => (b.input = "button")} /> A push button — springs back; short, long and double presses</label>
					<label class="flex items-center gap-2"><input type="radio" name="input" class="accent-primary" checked={b.input === "switch"} onchange={() => (b.input = "switch")} /> A switch — stays where it is put; every flip, on or off, is one press</label>
				</div>
				<div class="flex flex-col gap-2 text-sm">
					<label class="flex items-center gap-2"><input type="radio" name="wire" class="accent-primary" checked={toGnd} onchange={() => { b.pull = "up"; b.inverted = true; }} /> It connects the pin to GND</label>
					<label class="flex items-center gap-2"><input type="radio" name="wire" class="accent-primary" checked={to3v3} onchange={() => { b.pull = "down"; b.inverted = false; }} /> It connects the pin to 3V3</label>
				</div>
				<details open={!toGnd && !to3v3} class="text-sm">
					<summary class="text-muted-foreground cursor-pointer">Advanced wiring</summary>
					<div class="mt-2 flex flex-wrap items-center gap-3">
						<select class="sel" value={b.pull} onchange={(e) => (b.pull = e.currentTarget.value as Pull)}>
							{#each PULLS as [v, l] (v)}<option value={v}>{l}</option>{/each}
						</select>
						<label class="flex items-center gap-2"><input type="checkbox" class="accent-primary" checked={b.inverted} onchange={(e) => (b.inverted = e.currentTarget.checked)} /> Inverted</label>
					</div>
				</details>
				<div class="flex items-center gap-2 text-sm">
					{#if pinLine.dot}<span class="inline-block size-2.5 rounded-full {pinLine.dot === 'on' ? 'bg-on' : 'bg-muted-foreground/40'}"></span>{/if}
					<span>{pinLine.text}</span>
				</div>
			{/if}

			{#each gestures as [ev, label] (ev)}
				<!-- A switch has no hold and no double press: only its flips do anything. -->
				{#if b.source === "zigbee" || b.input === "button" || ev === "short"}
					<GestureEditor gesture={ev} label={b.source === "gpio" && b.input === "switch" ? "Each flip" : label} rules={b.events[ev]} {left} ontest={() => test(ev)} />
				{/if}
			{/each}

			<div class="border-border flex items-center justify-between gap-2 border-t pt-4">
				<Button variant="destructive" onclick={remove}><Trash /> Delete</Button>
				<Button onclick={() => save(false)}>Save</Button>
			</div>
		</Card.Content>
	</Card.Root>
{/if}

<style>
	.sel { height: 2.5rem; border-radius: var(--radius-md); border: 1px solid var(--input); background: var(--card);
		color: var(--foreground); padding: 0 .6rem; font-size: .875rem; }
</style>
