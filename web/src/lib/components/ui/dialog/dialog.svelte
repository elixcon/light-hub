<script lang="ts">
	/* The dialog replaces bits-ui's with a native <dialog>, which brings the
	 * focus trap, Escape and the inert page behind it for nothing. It is
	 * controlled only: it never closes itself, it asks through onOpenChange and
	 * the caller decides. */
	import type { Snippet } from "svelte";
	import { setDialog } from "./context.js";

	let { open = false, onOpenChange, children }: {
		open?: boolean;
		onOpenChange?: (open: boolean) => void;
		children?: Snippet;
	} = $props();

	const id = $props.id();
	setDialog({
		get open() { return open; },
		titleId: `${id}-title`,
		descriptionId: `${id}-description`,
		close: () => onOpenChange?.(false),
	});
</script>

{@render children?.()}
