<script lang="ts" module>
	/* tailwind-variants replaced by a plain map; see button.svelte, also for
	 * why the border's colour is each variant's. */
	const BASE =
		"h-5 gap-1 rounded-full border px-2 py-0.5 text-xs font-medium inline-flex w-fit shrink-0 items-center justify-center overflow-hidden whitespace-nowrap [&>svg]:size-3 [&>svg]:pointer-events-none";
	const VARIANTS = {
		default: "border-transparent bg-primary text-primary-foreground",
		secondary: "border-transparent bg-secondary text-secondary-foreground",
		destructive: "border-transparent bg-destructive/10 text-destructive",
		outline: "border-border text-muted-foreground",
	};
	export type BadgeVariant = keyof typeof VARIANTS;
	export const badgeVariants = ({ variant = "default" }: { variant?: BadgeVariant } = {}) =>
		cn(BASE, VARIANTS[variant]);
</script>

<script lang="ts">
	import { cn, type WithElementRef } from "$lib/utils.js";
	import type { HTMLAnchorAttributes } from "svelte/elements";

	let {
		ref = $bindable(null),
		href,
		class: className,
		variant = "default",
		children,
		...restProps
	}: WithElementRef<HTMLAnchorAttributes> & { variant?: BadgeVariant } = $props();
</script>

<svelte:element
	this={href ? "a" : "span"}
	bind:this={ref}
	data-slot="badge"
	{href}
	class={cn(badgeVariants({ variant }), className)}
	{...restProps}
>
	{@render children?.()}
</svelte:element>
