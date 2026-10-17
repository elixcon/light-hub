<script lang="ts">
	import ChevronLeft from "@lucide/svelte/icons/chevron-left";
	import type { Snippet } from "svelte";
	let { title, back, children }: { title: string; back: string; children?: Snippet } = $props();
</script>

<div class="mt-4 flex items-center gap-1">
	<a href="#{back}" class="hover:bg-muted -ml-2 inline-flex size-9 items-center justify-center rounded-full" aria-label="Back"><ChevronLeft class="size-5" /></a>
	<h2 class="min-w-0 flex-1 truncate text-lg font-semibold">{title}</h2>
	{@render children?.()}
</div>
