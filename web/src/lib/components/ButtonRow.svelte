<script lang="ts">
	/* One button and what each of its gestures does, with the way into its
	 * editor. The same row on the Remotes list and on a remote's own page. */
	import { Button } from "$lib/components/ui/button";
	import Pencil from "@lucide/svelte/icons/pencil";
	import { data } from "$lib/state.svelte.ts";
	import { navigate } from "$lib/router.svelte.ts";
	import { eventText } from "$lib/buttons.ts";
	import { gesturesFor, keyName, type KnownRemote } from "$lib/remotes.ts";
	import type { ButtonDef } from "$lib/types.ts";

	/* `editable` off where the card around it already has the pencil. */
	/* `showEp` on a remote with buttons on more than one endpoint, where a key
	 * number alone does not say which button it is. */
	let { button: b, known = null, editable = true, showEp = false }:
		{ button: ButtonDef; known?: KnownRemote | null; editable?: boolean; showEp?: boolean } = $props();

	const kname = $derived(keyName(b.key, known));
	const texts = $derived(gesturesFor(b.key, known, b.events).map(([g, label]) => [label, eventText(b.events?.[g], data.groups, data.devices)] as const)
		.filter(([, text]) => text));
</script>

<div class="flex items-start gap-2 py-2.5 first:pt-0 last:pb-0">
	<div class="min-w-0 flex-1">
		<div class="flex flex-wrap items-baseline gap-x-2">
			<span class="text-sm font-medium">{b.name}</span>
			{#if kname && kname !== b.name}<span class="text-muted-foreground text-xs">{kname}</span>{/if}
			{#if showEp && b.ep !== null}<span class="text-muted-foreground text-xs">endpoint {b.ep}</span>{/if}
		</div>
		{#if texts.length}
			<div class="mt-1 grid grid-cols-[auto_1fr] gap-x-3 gap-y-0.5 text-xs">
				{#each texts as [label, text] (label)}<span class="text-muted-foreground">{label}</span><span>{text}</span>{/each}
			</div>
		{:else}
			<div class="text-muted-foreground mt-0.5 text-xs">Does nothing yet</div>
		{/if}
	</div>
	{#if editable}<Button variant="ghost" size="icon" aria-label="Edit {b.name}" onclick={() => navigate(`/remotes/${b.id}`)}><Pencil /></Button>{/if}
</div>
