<script lang="ts" module>
	/* One way of adding something to a list: an action of its own (pairing), or
	 * a thing made by naming it - posted to `path`, and its editor opened, at
	 * `to(item)` or `${route}/${id}`. */
	export type AddItem = { title: string; text?: string; disabled?: boolean } & (
		| { run: () => void }
		| { path: string; placeholder: string; route?: string; to?: (item: any) => string });
</script>

<script lang="ts">
	/* A view's heading, and for a list what the "+" beside the page's title adds
	 * to it. The button opens the list's ways of adding something, each with a
	 * line on what it is for; a list with one way, which is naming a new thing,
	 * asks for the name at once. Without `items` it is only the heading. */
	import { onMount } from "svelte";
	import { Button } from "$lib/components/ui/button";
	import { Input } from "$lib/components/ui/input";
	import * as Card from "$lib/components/ui/card";
	import ChevronRight from "@lucide/svelte/icons/chevron-right";
	import { post } from "$lib/api.ts";
	import { toast } from "$lib/toast.svelte.ts";
	import { navigate } from "$lib/router.svelte.ts";
	import { refresh } from "$lib/state.svelte.ts";
	import { adder } from "$lib/add.svelte.ts";

	let { title, items = [] }: { title: string; items?: AddItem[] } = $props();

	type Named = Extract<AddItem, { path: string }>;
	let picked = $state<Named | null>(null);
	let newName = $state("");
	const naming = $derived(picked ?? (items.length === 1 && "path" in items[0] ? items[0] : null));

	onMount(() => {
		if (!items.length) return;
		adder.lists++;
		return () => { adder.lists--; adder.open = false; };
	});
	$effect(() => { if (!adder.open) { picked = null; newName = ""; } });

	function pick(item: AddItem) {
		if ("path" in item) picked = item;
		else { adder.open = false; item.run(); }
	}
	async function create(item: Named) {
		const name = newName.trim();
		if (!name) return;
		try {
			const made = await post<{ id: number }>(item.path, { name });
			adder.open = false;
			await refresh();
			navigate(item.to ? item.to(made) : `${item.route}/${made.id}`);
		} catch (err: any) { toast(err.message); }
	}
</script>

<!-- Escape takes the focused choice away with the list, so focus goes back to
     the button that opened it. -->
<svelte:window onkeydown={(e) => { if (e.key === "Escape" && adder.open) { adder.open = false; adder.el?.focus(); } }} />

<h2 class="mt-6 text-xl font-semibold tracking-tight">{title}</h2>

{#if adder.open}
	{#if naming}
		<form class="mt-3 flex gap-2" onsubmit={(e) => { e.preventDefault(); create(naming); }}>
			<!-- svelte-ignore a11y_autofocus -->
			<Input bind:value={newName} placeholder={naming.placeholder} aria-label={naming.title} maxlength={31} autofocus />
			<Button type="submit">Create</Button>
			<Button variant="ghost" onclick={() => (adder.open = false)}>Cancel</Button>
		</form>
	{:else}
		<Card.Root size="sm" class="mt-3">
			<div class="divide-border flex flex-col divide-y">
				{#each items as item (item.title)}
					<button class="hover:bg-muted flex w-full items-center gap-3 px-(--card-spacing) py-2.5 text-left disabled:opacity-50"
						disabled={item.disabled} onclick={() => pick(item)}>
						<span class="min-w-0 flex-1">
							<b class="block text-base font-medium">{item.title}</b>
							<span class="text-muted-foreground block text-xs">{item.text}</span>
						</span>
						<ChevronRight class="text-muted-foreground size-4 shrink-0" />
					</button>
				{/each}
			</div>
		</Card.Root>
	{/if}
{/if}
