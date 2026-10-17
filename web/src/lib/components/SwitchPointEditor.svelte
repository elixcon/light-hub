<script lang="ts">
	/* A light that can only switch reads its level curve as a switch point:
	 * on while its group is at or above this brightness, off below it. */
	import { switchLabel } from "$lib/curves.ts";
	let { at, cursorPct = null, onchange, oncommit }: {
		at: number | null; cursorPct?: number | null; onchange: (at: number) => void; oncommit: () => void;
	} = $props();
	const lit = $derived(at !== null && cursorPct != null ? cursorPct >= at : null);
</script>

<div class="text-sm">{switchLabel(at)}</div>
<input type="range" min="1" max="100" step="1" value={at ?? 50} class="accent-curve mt-2 w-full"
	aria-label="Brightness at which this light turns on"
	oninput={(e) => onchange(Number(e.currentTarget.value))} onchange={oncommit} />
{#if lit !== null}
	<div class="text-muted-foreground mt-1 text-xs">At the test slider's {cursorPct} % this light is {lit ? "on" : "off"}.</div>
{/if}
