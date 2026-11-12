<script lang="ts">
	import ChevronLeft from "@lucide/svelte/icons/chevron-left";
	import type { Snippet } from "svelte";
	let { title, back, children }: { title: string; back: string; children?: Snippet } = $props();
</script>

<div class="mt-6 flex items-center gap-2.5">
	<a href="#{back}" class="bg-card hover:bg-secondary inline-flex size-9 shrink-0 items-center justify-center rounded-full" aria-label="Back"><ChevronLeft class="size-5" /></a>
	<h2 class="min-w-0 flex-1 truncate text-xl font-semibold tracking-tight">{title}</h2>
	{@render children?.()}
</div>
