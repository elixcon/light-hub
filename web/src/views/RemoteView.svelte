<script lang="ts">
	/* One remote on a page of its own, as a light has: its name, its firmware,
	 * and its buttons - setting them up, adding one, removing them - and taking
	 * it off the network. The Remotes list only shows what each button does. */
	import * as Card from "$lib/components/ui/card";
	import * as Dialog from "$lib/components/ui/dialog";
	import { Button } from "$lib/components/ui/button";
	import { Label } from "$lib/components/ui/label";
	import { Input } from "$lib/components/ui/input";
	import Plus from "@lucide/svelte/icons/plus";
	import Trash from "@lucide/svelte/icons/trash-2";
	import ViewHeader from "$lib/components/ViewHeader.svelte";
	import ButtonRow from "$lib/components/ButtonRow.svelte";
	import FirmwareRow from "$lib/components/FirmwareRow.svelte";
	import { data, refresh } from "$lib/state.svelte.ts";
	import { navigate } from "$lib/router.svelte.ts";
	import { post, patch, del } from "$lib/api.ts";
	import { toast } from "$lib/toast.svelte.ts";
	import { confirm } from "$lib/confirm.svelte.ts";
	import { ruleText } from "$lib/buttons.ts";
	import { addKnownButtons, gesturesFor, missingKeys, remoteCards, remoteHeard, remoteKind, remoteLink, remoteTitle, rulesFor,
		strayParent, type KnownKey } from "$lib/remotes.ts";
	import { ago, plural } from "$lib/format.ts";

	let { ieee }: { ieee: string } = $props();

	const c = $derived(remoteCards(data.devices, data.buttons).find((x) => x.ieee === ieee) ?? null);
	const known = $derived(c?.known ?? null);
	const missing = $derived(c ? missingKeys(c) : []);
	const heard = $derived(c ? remoteHeard(c) : null);
	const link = $derived(c ? remoteLink(c, data.devices) : null);
	const stray = $derived(c ? strayParent(c, data.devices) : null);

	/* A remote is a device, so its name is the device's name - the same one the
	 * log and the API use. Seeded once; the poll must not wipe a half-typed name. */
	let name = $state("");
	let seeded = $state("");
	$effect(() => {
		if (c?.device && seeded !== c.ieee) { seeded = c.ieee; name = c.device.name; }
	});
	const dirty = $derived(!!c?.device && !!name.trim() && name.trim() !== c.device.name);

	async function save() {
		if (!c?.device || !dirty) return;
		try {
			await patch(`/api/devices/${c.ieee}`, { name: name.trim() });
			toast("Saved");
		} catch (err: any) { toast(err.message); }
		refresh();
	}

	const setup = $state({ open: false, keys: [] as KnownKey[], chosen: [] as number[], group: 0, busy: false });
	const describe = (k: KnownKey) => {
		const rules = rulesFor(k, setup.group);
		return gesturesFor(k.key, known).filter(([g]) => rules[g].length)
			.map(([g, label]) => `${label}: ${ruleText(rules[g][0], data.groups, data.devices)}`).join(" · ");
	};
	function openSetup() {
		if (!c) return;
		setup.keys = missingKeys(c);
		setup.chosen = setup.keys.map((k) => k.key);
		setup.group = data.groups[0]?.id ?? 0;
		setup.open = true;
	}
	async function runSetup() {
		if (!c || !setup.group) return;
		setup.busy = true;
		try {
			const n = await addKnownButtons(c.ieee, c.ep, setup.keys.filter((k) => setup.chosen.includes(k.key)), setup.group);
			toast(`${plural(n, "button")} set up`);
			setup.open = false;
		} catch (err: any) {
			toast(err.message);
		} finally {
			setup.busy = false;
			refresh();
		}
	}

	/* Any remote, known or not: one more button, found by pressing it. */
	async function addButton() {
		try {
			const item = await post<{ id: number }>("/api/buttons", { name: "New button" });
			await patch(`/api/buttons/${item.id}`, { source: "zigbee" });
			await refresh();
			navigate(`/remotes/${item.id}`);
		} catch (err: any) { toast(err.message); }
	}
	async function removeButtons() {
		if (!c) return;
		if (!(await confirm(`Remove ${plural(c.buttons.length, "button")} from ${remoteTitle(c)}? The remote itself stays paired.`,
			{ confirmLabel: "Remove", destructive: true }))) return;
		try {
			for (const b of c.buttons) await del(`/api/buttons/${b.id}`);
		} catch (err: any) { toast(err.message); }
		refresh();
	}
	/* Its buttons go with it. The API would otherwise keep them as buttons with
	 * no remote, each holding one of the sixteen slots with nothing that could
	 * ever press it. */
	async function removeRemote() {
		if (!c) return;
		const title = remoteTitle(c);
		const buttons = c.buttons;
		const also = buttons.length ? ` Its ${plural(buttons.length, "button")} ${buttons.length === 1 ? "is" : "are"} deleted too.` : "";
		/* A sleepy remote is not sent a leave request - it would collect it only
		 * after being paired again, and leave then (see zb_remove_device). */
		if (!(await confirm(`Remove ${title}?${also} To take it off the network as well, reset the remote itself (usually a long press of its setup or pairing button). To use it again, pair it again.`,
			{ confirmLabel: "Remove", destructive: true }))) return;
		try {
			for (const b of buttons) await del(`/api/buttons/${b.id}`);
			await del(`/api/devices/${c.ieee}`);
			toast(`${title} removed`);
			navigate("/remotes");
		} catch (err: any) { toast(err.message); }
		refresh();
	}
</script>

<ViewHeader title={c ? remoteTitle(c) : "Remote"} back="/remotes" />

{#if !c}
	<p class="text-muted-foreground mt-6 text-sm">This remote is gone.</p>
{:else}
	<Card.Root class="mt-4">
		<Card.Content class="flex flex-col gap-5">
			{#if c.device}
				<div>
					<Label for="rname" class="mb-2 block">Name</Label>
					<Input id="rname" bind:value={name} maxlength={31}
						onkeydown={(e) => { if (e.key === "Enter") save(); }} />
				</div>
			{/if}
			<div class="text-muted-foreground flex flex-col gap-0.5 text-xs">
				<div>{remoteKind(c)}</div>
				{#if link}<div>{link}</div>{/if}
				<div>{heard !== null ? `Last press heard ${ago(heard)}` : "No press heard since the coordinator started"}</div>
			</div>
			{#if stray}
				<p class="text-destructive text-sm">
					This remote sends to a group and hangs off {stray}, not the coordinator, so each press reaches the
					coordinator as one unacknowledged broadcast from {stray} and some get lost. Pair it again next to the
					coordinator with {stray} switched off.
				</p>
			{/if}
			<div class="flex items-center justify-between gap-2">
				{#if c.device}
					<Button variant="destructive" onclick={removeRemote}><Trash /> Remove remote</Button>
					<Button disabled={!dirty} onclick={save}>Save</Button>
				{:else}
					<p class="text-muted-foreground text-sm">This remote is no longer paired. Its buttons are kept until you remove them.</p>
				{/if}
			</div>
		</Card.Content>
	</Card.Root>

	<Card.Root class="mt-4">
		<Card.Header>
			<Card.Title>Buttons</Card.Title>
			<Card.Description>
				{#if c.buttons.length}What each button does. Tap the pencil to change it.
				{:else if known}Not set up yet. {known.note ?? ""}
				{:else}Not set up yet. Add a button, then press the one on the remote you want it to be.{/if}
			</Card.Description>
		</Card.Header>
		<Card.Content class="flex flex-col gap-4">
			{#if c.buttons.length}
				<div class="divide-border flex flex-col divide-y">
					{#each c.buttons as b (b.id)}<ButtonRow button={b} known={c.known} showEp={c.spans} />{/each}
				</div>
			{/if}
			<div class="flex flex-wrap gap-2">
				{#if missing.length}
					<Button size="sm" onclick={openSetup}>
						{c.buttons.length ? `Add ${plural(missing.length, "missing button")}` : `Set up ${plural(missing.length, "button")}`}
					</Button>
				{/if}
				{#if c.device && !missing.length}
					<Button variant="outline" size="sm" onclick={addButton}><Plus /> Add a button</Button>
				{/if}
				{#if c.buttons.length}
					<Button variant="ghost" size="sm" onclick={removeButtons}>Remove buttons</Button>
				{/if}
			</div>
		</Card.Content>
	</Card.Root>

	{#if c.device?.ota}
		<Card.Root class="mt-4">
			<Card.Content><FirmwareRow device={c.device} /></Card.Content>
		</Card.Root>
	{/if}
{/if}

<Dialog.Root open={setup.open} onOpenChange={(o) => (setup.open = o)}>
	<Dialog.Content class="max-h-[90vh] overflow-y-auto">
		{#if c}
			<Dialog.Header>
				<Dialog.Title>Set up {remoteTitle(c)}</Dialog.Title>
				<Dialog.Description>{known ? `${known.title}. ${known.note ?? ""}` : ""}</Dialog.Description>
			</Dialog.Header>
			{#if !data.groups.length}
				<p class="text-sm">There is no room for these buttons to control yet. <a href="#/groups" class="text-primary underline underline-offset-4">Create a room</a> first.</p>
			{:else}
				<div>
					<Label for="setup-group" class="mb-2 block">Which room they control</Label>
					<select id="setup-group" class="sel w-full" bind:value={setup.group}>
						{#each data.groups as g (g.id)}<option value={g.id}>{g.name}</option>{/each}
					</select>
				</div>
				<div class="flex flex-col gap-3 text-sm">
					{#each setup.keys as k (k.key)}
						<label class="flex items-start gap-2">
							<input type="checkbox" class="accent-primary mt-1" checked={setup.chosen.includes(k.key)}
								onchange={(e) => (setup.chosen = e.currentTarget.checked
									? [...setup.chosen, k.key] : setup.chosen.filter((x) => x !== k.key))} />
							<span><span class="font-medium">{k.label}</span><br /><span class="text-muted-foreground">{describe(k)}</span></span>
						</label>
					{/each}
				</div>
				<p class="text-muted-foreground text-xs">Each can be changed afterwards.</p>
			{/if}
			<Dialog.Footer>
				<Button variant="ghost" onclick={() => (setup.open = false)}>Cancel</Button>
				<Button disabled={setup.busy || !setup.group || !setup.chosen.length} onclick={runSetup}>
					{setup.busy ? "Setting up…" : "Set up"}
				</Button>
			</Dialog.Footer>
		{/if}
	</Dialog.Content>
</Dialog.Root>

<style>
	.sel { height: 2.5rem; border-radius: var(--radius-md); border: 1px solid var(--input); background: var(--card);
		color: var(--foreground); padding: 0 .6rem; font-size: .875rem; }
</style>
