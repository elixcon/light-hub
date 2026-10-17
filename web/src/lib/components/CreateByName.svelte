<script lang="ts">
	/* A list view's heading, with an "add" button that asks for a name, creates the
	 * item and opens its editor. */
	import { Button } from "$lib/components/ui/button";
	import { Input } from "$lib/components/ui/input";
	import * as Card from "$lib/components/ui/card";
	import Plus from "@lucide/svelte/icons/plus";
	import { post } from "$lib/api.ts";
	import { toast } from "$lib/toast.svelte.ts";
	import { navigate } from "$lib/router.svelte.ts";
	import { refresh } from "$lib/state.svelte.ts";
	import type { Snippet } from "svelte";

	/* `choices`, for a view with more than one way in: the add button opens them
	 * instead, and the snippet gets `name` for the way that is this name form.
	 * `to` names the editor for an item whose route is not `${route}/${id}`. */
	let { title, addLabel, placeholder, path, route, choices, to }:
		{ title: string; addLabel: string; placeholder: string; path: string; route: string;
		  choices?: Snippet<[{ name: () => void; close: () => void }]>; to?: (item: any) => string } = $props();

	let choosing = $state(false);
	let adding = $state(false);
	let addEl = $state<HTMLElement | null>(null);
	let newName = $state("");

	async function create() {
		const name = newName.trim();
		if (!name) return;
		try {
			const item = await post<{ id: number }>(path, { name });
			newName = ""; adding = false;
			await refresh();
			navigate(to ? to(item) : `${route}/${item.id}`);
		} catch (err: any) { toast(err.message); }
	}
	const close = () => { choosing = false; };
	const name = () => { choosing = false; adding = true; };
</script>

<!-- Escape takes the focused choice away with the list, so focus goes back to
     the button that opened it. -->
<svelte:window onkeydown={(e) => { if (e.key === "Escape" && choosing) { choosing = false; addEl?.focus(); } }} />

<div class="mt-5 flex items-center justify-between gap-3">
	<h2 class="text-lg font-semibold">{title}</h2>
	{#if !adding}
		<Button bind:ref={addEl} variant="outline" size="sm" aria-expanded={choices ? choosing : undefined}
			onclick={() => { if (choices) choosing = !choosing; else adding = true; }}><Plus /> {addLabel}</Button>
	{/if}
</div>

{#if choosing && choices}
	<Card.Root size="sm" class="mt-3">
		<div class="divide-border flex flex-col divide-y">
			{@render choices({ name, close })}
		</div>
	</Card.Root>
{/if}

{#if adding}
	<form class="mt-3 flex gap-2" onsubmit={(e) => { e.preventDefault(); create(); }}>
		<!-- svelte-ignore a11y_autofocus -->
		<Input bind:value={newName} {placeholder} maxlength={31} autofocus />
		<Button type="submit">Create</Button>
		<Button variant="ghost" onclick={() => { adding = false; newName = ""; }}>Cancel</Button>
	</form>
{/if}
