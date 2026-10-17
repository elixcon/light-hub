<script lang="ts">
	import type { Snippet } from "svelte";
	import type { HTMLDialogAttributes } from "svelte/elements";
	import XIcon from "@lucide/svelte/icons/x";
	import { Button } from "$lib/components/ui/button/index.js";
	import { cn } from "$lib/utils.js";
	import { getDialog } from "./context.js";

	let {
		class: className,
		children,
		showCloseButton = true,
		...restProps
	}: Omit<HTMLDialogAttributes, "open"> & {
		children?: Snippet;
		showCloseButton?: boolean;
	} = $props();

	const dialog = getDialog();
	let el: HTMLDialogElement;

	$effect(() => {
		if (dialog.open) {
			if (!el.open) el.showModal();
		} else if (el.open) {
			el.close();
		}
	});

	/* A click on the backdrop lands on the dialog element itself, outside its
	 * box. The target test alone is not enough: the dialog's own padding and
	 * scrollbar are the element too. */
	function backdropClick(e: MouseEvent) {
		if (e.target !== el) return;
		const r = el.getBoundingClientRect();
		if (e.clientX < r.left || e.clientX > r.right || e.clientY < r.top || e.clientY > r.bottom) dialog.close();
	}
</script>

<!-- svelte-ignore a11y_click_events_have_key_events, a11y_no_noninteractive_element_interactions -->
<dialog
	bind:this={el}
	data-slot="dialog-content"
	aria-labelledby={dialog.titleId}
	aria-describedby={dialog.descriptionId}
	class={cn(
		"bg-popover text-popover-foreground ring-edge m-auto w-full max-w-[calc(100%_-_2rem)] gap-4 rounded-xl p-4 text-sm shadow-lg ring-1 outline-none open:grid sm:max-w-sm",
		className
	)}
	oncancel={(e) => { e.preventDefault(); dialog.close(); }}
	onclose={() => { if (dialog.open) dialog.close(); }}
	onclick={backdropClick}
	{...restProps}
>
	{@render children?.()}
	{#if showCloseButton}
		<Button variant="ghost" class="absolute top-2 right-2" size="icon-sm" onclick={() => dialog.close()}>
			<XIcon />
			<span class="sr-only">Close</span>
		</Button>
	{/if}
</dialog>

<style>
	/* Escape can be refused only once in a row by Chrome, after which the
	 * dialog closes by itself - onclose above brings the caller along. */
	dialog {
		opacity: 0; scale: .95;
		transition: opacity .1s, scale .1s, overlay .1s allow-discrete, display .1s allow-discrete;
	}
	dialog[open] { opacity: 1; scale: 1; }
	@starting-style { dialog[open] { opacity: 0; scale: .95; } }
	dialog::backdrop {
		background-color: rgb(0 0 0 / 0);
		transition: background-color .1s, overlay .1s allow-discrete, display .1s allow-discrete;
	}
	dialog[open]::backdrop { background-color: rgb(0 0 0 / .1); backdrop-filter: blur(4px); }
	@starting-style { dialog[open]::backdrop { background-color: rgb(0 0 0 / 0); } }
	:global(html:has(dialog[open])) { overflow: hidden; }
</style>
