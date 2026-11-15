<script lang="ts">
	/* The top of either page, the hub's and the node's: its name, the status
	 * pills, and the "+" where the view below has a list to add to (its
	 * ListHeading opens what the button offers). */
	import StatusChips from "$lib/components/StatusChips.svelte";
	import { Button } from "$lib/components/ui/button";
	import Plus from "@lucide/svelte/icons/plus";
	import { app } from "$lib/state.svelte.ts";
	import { adder } from "$lib/add.svelte.ts";
	let { chips }: { chips: { text: string; bad: boolean }[] } = $props();
</script>

<header class="flex items-start gap-3 pt-[env(safe-area-inset-top)]">
	<div class="min-w-0 flex-1">
		<h1 class="text-3xl font-semibold tracking-[-.035em] md:text-4xl">{app.name}</h1>
		<StatusChips {chips} />
	</div>
	{#if adder.lists}
		<Button bind:ref={adder.el} variant="outline" size="icon" aria-label="Add" aria-expanded={adder.open} onclick={() => (adder.open = !adder.open)}><Plus /></Button>
	{/if}
</header>
