<script lang="ts">
	/* shadcn's switch at a size a thumb can hit, 28 x 48, as a plain button
	 * with role="switch". data-state is kept so callers can style on it. */
	import type { HTMLButtonAttributes } from "svelte/elements";
	import { cn } from "$lib/utils.js";

	let {
		checked = $bindable(false),
		class: className,
		onCheckedChange,
		...restProps
	}: Omit<HTMLButtonAttributes, "type" | "role"> & {
		checked?: boolean;
		onCheckedChange?: (checked: boolean) => void;
	} = $props();

	const dataState = $derived(checked ? "checked" : "unchecked");
</script>

<button
	type="button"
	role="switch"
	aria-checked={checked}
	data-state={dataState}
	data-slot="switch"
	class={cn(
		"data-[state=checked]:bg-on data-[state=unchecked]:bg-muted-foreground/40 focus-visible:border-ring focus-visible:ring-ring/50 relative inline-flex h-7 w-12 shrink-0 items-center rounded-full border border-transparent transition-colors outline-none focus-visible:ring-3 after:absolute after:-inset-2 disabled:cursor-not-allowed disabled:opacity-50",
		className
	)}
	{...restProps}
	onclick={() => { checked = !checked; onCheckedChange?.(checked); }}
>
	<span
		data-slot="switch-thumb"
		data-state={dataState}
		class="pointer-events-none block size-6 rounded-full bg-white shadow-md ring-0 transition-transform data-[state=checked]:translate-x-5 data-[state=unchecked]:translate-x-0"
	></span>
</button>
