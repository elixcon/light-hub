<script lang="ts">
	/* The row of little status pills under a page's title. */
	let { chips }: { chips: { text: string; bad: boolean }[] } = $props();
</script>

<div class="mt-1.5 flex flex-wrap gap-1.5 text-xs">
	{#each chips as c, i (i)}
		<span class="bg-card dark:bg-background/80 rounded-full px-2 py-0.5 {c.bad ? 'text-destructive font-medium' : 'text-primary'}">{c.text}</span>
	{/each}
</div>
