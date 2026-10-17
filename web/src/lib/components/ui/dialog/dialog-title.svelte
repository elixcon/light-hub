<script lang="ts">
	import { cn, type WithElementRef } from "$lib/utils.js";
	import type { HTMLAttributes } from "svelte/elements";
	import { getDialog } from "./context.js";

	let {
		ref = $bindable(null),
		class: className,
		children,
		...restProps
	}: WithElementRef<HTMLAttributes<HTMLHeadingElement>> = $props();

	const dialog = getDialog();
</script>

<h2
	bind:this={ref}
	id={dialog.titleId}
	data-slot="dialog-title"
	class={cn("text-base leading-none font-medium", className)}
	{...restProps}
>
	{@render children?.()}
</h2>
