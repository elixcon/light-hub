<script lang="ts">
	/* How an upload is getting on, for whichever page started it. */
	import { Progress } from "$lib/components/ui/progress";
	import { ota } from "$lib/ota.svelte.ts";
</script>

{#if ota.shown}
	{#if ota.fraction === null}
		<div class="bg-muted h-2 w-full overflow-hidden rounded-full"><div class="bg-primary h-full w-1/3 animate-pulse rounded-full"></div></div>
	{:else}
		<Progress value={ota.fraction * 100} class="h-2" />
	{/if}
	<div class="text-muted-foreground text-xs">{ota.message}</div>
{/if}
