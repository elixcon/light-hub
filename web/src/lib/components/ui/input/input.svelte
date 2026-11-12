<script lang="ts">
	/* shadcn's input less what nothing here uses: the file type (the pages'
	 * file inputs are hidden native ones) and the invalid state. Solid, because
	 * a name field can lie on the room as well as on a card. */
	import { cn } from "$lib/utils.js";
	import type { HTMLInputAttributes } from "svelte/elements";

	let {
		value = $bindable(),
		class: className,
		...restProps
	}: HTMLInputAttributes = $props();
</script>

<input
	class={cn(
		"border-input focus-visible:border-ring focus-visible:ring-ring/50 disabled:bg-input/50 h-10 rounded-lg border bg-popover px-3 py-1 text-base transition-colors focus-visible:ring-3 md:text-sm w-full min-w-0 outline-none placeholder:text-muted-foreground disabled:pointer-events-none disabled:cursor-not-allowed disabled:opacity-50",
		className
	)}
	bind:value
	{...restProps}
/>
