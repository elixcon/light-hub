<script lang="ts">
	/* The row of little status pills under a page's title. They lie on the
	 * room, so they are solid. */
	let { chips }: { chips: { text: string; bad: boolean }[] } = $props();
</script>

<div class="mt-2.5 flex flex-wrap gap-1.5 text-xs font-medium">
	{#each chips as c, i (i)}
		<span class="bg-card rounded-full px-2.5 py-1 {c.bad ? 'text-destructive' : ''}">{c.text}</span>
	{/each}
</div>
