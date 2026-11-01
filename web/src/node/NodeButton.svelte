<script lang="ts">
	/* A button on a node is a pin and how it is wired. What it does is set up on the
	 * hub; the one thing here is what it does while the hub cannot be reached. */
	import * as Card from "$lib/components/ui/card";
	import { Button } from "$lib/components/ui/button";
	import { Input } from "$lib/components/ui/input";
	import { Label } from "$lib/components/ui/label";
	import Trash from "@lucide/svelte/icons/trash-2";
	import ViewHeader from "$lib/components/ViewHeader.svelte";
	import { data, refresh } from "$lib/state.svelte.ts";
	import { patch, del } from "$lib/api.ts";
	import { toast } from "$lib/toast.svelte.ts";
	import { confirm } from "$lib/confirm.svelte.ts";
	import { navigate } from "$lib/router.svelte.ts";
	import { PINS, PIN_NOTE } from "$lib/buttons.ts";

	let { id }: { id: number } = $props();

	const b = $derived(data.buttons.find((x) => x.id === id));
	let name = $state("");
	$effect(() => { if (b) name = b.name; });

	async function save(fields: Record<string, unknown>, undo?: () => void) {
		try { await patch(`/api/buttons/${id}`, fields); await refresh(); }
		catch (err: any) { toast(err.message); undo?.(); }
	}
	async function remove() {
		if (!(await confirm(`Remove ${b?.name}? Hub rules on this key stop firing.`, { confirmLabel: "Remove" }))) return;
		try { await del(`/api/buttons/${id}`); await refresh(); navigate("/lights"); }
		catch (err: any) { toast(err.message); }
	}
</script>

<ViewHeader title={b ? b.name : "Button"} back="/lights" />
{#if !b}
	<p class="text-muted-foreground mt-6 text-sm">This button is gone.</p>
{:else}
	<Card.Root class="mt-4">
		<Card.Header><Card.Title>Wiring</Card.Title></Card.Header>
		<Card.Content class="flex flex-col gap-4">
			<div>
				<Label for="bname" class="mb-2 block">Name</Label>
				<Input id="bname" bind:value={name} maxlength={31} onchange={() => save({ name })} />
			</div>
			<div>
				<Label for="bpin" class="mb-2 block">Pin</Label>
				<select id="bpin" class="sel w-full" value={b.gpio == null ? "" : String(b.gpio)}
					onchange={(ev) => {
						const el = ev.currentTarget, was = b.gpio == null ? "" : String(b.gpio);
						save({ gpio: el.value === "" ? null : Number(el.value) }, () => { el.value = was; });
					}}>
					<option value="">— no pin yet —</option>
					{#each PINS as p (p)}<option value={String(p)}>GPIO {p}{PIN_NOTE[p] ? ` — ${PIN_NOTE[p]}` : ""}</option>{/each}
				</select>
			</div>
			<div class="grid grid-cols-2 gap-3">
				<div>
					<Label for="binput" class="mb-2 block">What is wired</Label>
					<select id="binput" class="sel w-full" value={b.input ?? "button"} onchange={(ev) => save({ input: ev.currentTarget.value })}>
						<option value="button">A push button</option>
						<option value="switch">A switch that stays put</option>
					</select>
				</div>
				<div>
					<Label for="bpull" class="mb-2 block">Pull resistor</Label>
					<select id="bpull" class="sel w-full" value={b.pull} onchange={(ev) => save({ pull: ev.currentTarget.value })}>
						<option value="up">Up, wired to GND</option>
						<option value="down">Down, wired to 3V3</option>
						<option value="none">None, an external one</option>
					</select>
				</div>
			</div>
			<label class="flex items-center gap-2 text-sm">
				<input type="checkbox" checked={b.inverted} onchange={(ev) => save({ inverted: ev.currentTarget.checked })} />
				The pin reads low when pressed
			</label>
			<div class="text-muted-foreground text-xs">
				{#if b.gpio == null}Pick a pin to see it respond.
				{:else}Pin reads {b.level ?? "?"}, {b.pressed ? "pressed" : "not pressed"}. Press it now to check the wiring.{/if}
			</div>
		</Card.Content>
	</Card.Root>

	<Card.Root class="mt-4">
		<Card.Header>
			<Card.Title>When the hub cannot be reached</Card.Title>
			<Card.Description>Normally a press goes to the hub, and its rules decide. Set up what it does there, under Remotes{b.key != null && b.key >= 0 ? `, where this is key ${b.key}` : ""}.</Card.Description>
		</Card.Header>
		<Card.Content class="flex flex-col gap-2">
			<Label for="bfallback">Toggle this light instead</Label>
			<select id="bfallback" class="sel w-full" value={b.fallback == null ? "" : String(b.fallback)}
				onchange={(ev) => save({ fallback: ev.currentTarget.value === "" ? null : Number(ev.currentTarget.value) })}>
				<option value="">— nothing —</option>
				{#each data.devices as d (d.ieee)}<option value={String(d.id)}>{d.name}</option>{/each}
			</select>
			<div class="text-muted-foreground text-xs">The first press of an outage takes a few seconds, while the node keeps trying the hub; the ones after it are immediate.</div>
		</Card.Content>
	</Card.Root>

	<div class="mt-4"><Button variant="destructive" onclick={remove}><Trash /> Remove button</Button></div>
{/if}

<style>
	.sel { height: 2.5rem; border-radius: var(--radius-md); border: 1px solid var(--input); background: var(--card);
		color: var(--foreground); padding: 0 .6rem; font-size: .875rem; }
</style>
