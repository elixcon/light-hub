<script lang="ts" module>
	import { cn, type WithElementRef } from "$lib/utils.js";
	import type { HTMLAnchorAttributes, HTMLButtonAttributes } from "svelte/elements";

	/* shadcn's button with its tailwind-variants call replaced by two plain maps:
	 * tv() and tailwind-merge together cost 10 KB of the gzip budget for a
	 * lookup. Sizes are a touch taller than shadcn's defaults, for thumbs.
	 * Without tailwind-merge nothing drops a class another overrides, so the
	 * base, a variant and a size never set the same property: the border's
	 * colour is each variant's, and text size, radius and icon size each
	 * size's (test/classes.test.ts). */
	const BASE =
		"focus-visible:border-ring focus-visible:ring-ring/50 border font-medium focus-visible:ring-3 active:translate-y-px inline-flex shrink-0 items-center justify-center whitespace-nowrap transition-all outline-none select-none disabled:pointer-events-none disabled:opacity-50 [&_svg]:pointer-events-none [&_svg]:shrink-0";
	const VARIANTS = {
		default: "border-transparent bg-primary text-primary-foreground hover:bg-primary/85 shadow-xs",
		outline: "border-border bg-card hover:bg-muted shadow-xs",
		secondary: "border-transparent bg-secondary text-secondary-foreground hover:bg-secondary/80 shadow-xs",
		ghost: "border-transparent hover:bg-muted",
		destructive: "border-transparent bg-destructive/10 hover:bg-destructive/20 text-destructive",
	};
	const SIZES = {
		default: "h-10 gap-1.5 rounded-lg px-4 text-sm [&_svg:not([class*='size-'])]:size-4",
		sm: "h-8 gap-1 rounded-md px-3 text-[0.8rem] [&_svg:not([class*='size-'])]:size-3.5",
		icon: "size-10 rounded-lg text-sm [&_svg:not([class*='size-'])]:size-4",
		"icon-sm": "size-8 rounded-md text-sm [&_svg:not([class*='size-'])]:size-4",
	};

	export type ButtonVariant = keyof typeof VARIANTS;
	export type ButtonSize = keyof typeof SIZES;
	export const buttonVariants = ({ variant = "default", size = "default" }: { variant?: ButtonVariant; size?: ButtonSize } = {}) =>
		cn(BASE, VARIANTS[variant], SIZES[size]);

	export type ButtonProps = WithElementRef<HTMLButtonAttributes> &
		WithElementRef<HTMLAnchorAttributes> & {
			variant?: ButtonVariant;
			size?: ButtonSize;
		};
</script>

<script lang="ts">
	let {
		class: className,
		variant = "default",
		size = "default",
		ref = $bindable(null),
		href = undefined,
		type = "button",
		disabled,
		children,
		...restProps
	}: ButtonProps = $props();
</script>

{#if href}
	<a
		bind:this={ref}
		data-slot="button"
		class={cn(buttonVariants({ variant, size }), className)}
		href={disabled ? undefined : href}
		aria-disabled={disabled}
		role={disabled ? "link" : undefined}
		tabindex={disabled ? -1 : undefined}
		{...restProps}
	>
		{@render children?.()}
	</a>
{:else}
	<button
		bind:this={ref}
		data-slot="button"
		class={cn(buttonVariants({ variant, size }), className)}
		{type}
		{disabled}
		{...restProps}
	>
		{@render children?.()}
	</button>
{/if}
