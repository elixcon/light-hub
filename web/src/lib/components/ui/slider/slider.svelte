<script lang="ts">
	/* A native range input sized for a finger: 8 px track, 24 px thumb with a
	 * 48 px hit area, the range painted by the caller through --range. The input
	 * draws only the thumb; the track and the fill are spans behind it, because
	 * an input's own track cannot take a gradient that stops at the thumb.
	 *
	 * Touch is handled here rather than by the input: iOS moves a range input
	 * only when the thumb itself is dragged, so a tap on the track would do
	 * nothing. Mouse and keyboard stay native. */
	import type { HTMLInputAttributes } from "svelte/elements";
	import { cn } from "$lib/utils.js";

	let {
		value = $bindable(0),
		min = 0,
		max = 100,
		step = 1,
		disabled = false,
		class: className,
		style,
		onValueCommit,
		...restProps
	}: Omit<HTMLInputAttributes, "value" | "type" | "min" | "max" | "step"> & {
		value?: number; min?: number; max?: number; step?: number;
		onValueCommit?: (value: number) => void;
	} = $props();

	const THUMB_PX = 24;
	const frac = $derived(max > min ? Math.min(1, Math.max(0, (value - min) / (max - min))) : 0);

	/* The same geometry the input uses: the thumb's centre travels from half a
	 * thumb in from one end to half a thumb in from the other. */
	function valueAt(input: HTMLInputElement, x: number) {
		const r = input.getBoundingClientRect();
		const f = Math.min(1, Math.max(0, (x - r.left - THUMB_PX / 2) / (r.width - THUMB_PX)));
		return min + Math.round((f * (max - min)) / step) * step;
	}

	/* touchstart has to be non-passive to be cancelled, and Svelte registers it
	 * passive, hence the attachment. */
	function noNativeTouch(input: HTMLInputElement) {
		const cancel = (e: TouchEvent) => e.preventDefault();
		input.addEventListener("touchstart", cancel, { passive: false });
		return () => input.removeEventListener("touchstart", cancel);
	}

	let dragging = false;
	function down(e: PointerEvent & { currentTarget: HTMLInputElement }) {
		if (e.pointerType === "mouse" || disabled) return;
		dragging = true;
		e.currentTarget.setPointerCapture(e.pointerId);
		move(e);
	}
	function move(e: PointerEvent & { currentTarget: HTMLInputElement }) {
		if (!dragging) return;
		const v = valueAt(e.currentTarget, e.clientX);
		if (v !== value) value = v;
	}
	function up() {
		if (!dragging) return;
		dragging = false;
		onValueCommit?.(value);
	}
</script>

<div data-slot="slider" class={cn("relative flex h-6 w-full items-center select-none", disabled && "opacity-50", className)} {style}>
	<span data-slot="slider-track" class="bg-muted relative h-2 w-full overflow-hidden rounded-full">
		<span data-slot="slider-range" class="absolute inset-y-0 left-0"
			style="width: calc({THUMB_PX / 2}px + (100% - {THUMB_PX}px) * {frac}); background: var(--range, var(--color-primary))"></span>
	</span>
	<input type="range" {min} {max} {step} {disabled} bind:value {@attach noNativeTouch}
		class="absolute inset-x-0 -top-3 h-12 w-full touch-none"
		onchange={() => onValueCommit?.(value)}
		onpointerdown={down} onpointermove={move} onpointerup={up} onpointercancel={up}
		{...restProps} />
</div>

<style>
	input { appearance: none; -webkit-appearance: none; background: transparent; margin: 0; cursor: pointer; }
	input:disabled { cursor: default; }
	input:focus { outline: none; }
	input:is(:hover, :focus-visible, :active):not(:disabled) {
		--thumb-ring: 0 0 0 4px color-mix(in oklab, var(--ring) 40%, transparent);
	}
	input::-webkit-slider-runnable-track { height: 100%; background: transparent; }
	input::-moz-range-track { background: transparent; }
	/* WebKit puts the thumb at the top of its track; Firefox centres it. */
	input::-webkit-slider-thumb {
		-webkit-appearance: none; margin-top: 12px;
		box-sizing: border-box; width: 24px; height: 24px; border-radius: 9999px;
		background: #fff; border: 1px solid var(--border);
		box-shadow: var(--thumb-ring, 0 0 #0000), var(--elev-2);
		transition: box-shadow .15s;
	}
	input::-moz-range-thumb {
		box-sizing: border-box; width: 24px; height: 24px; border-radius: 9999px;
		background: #fff; border: 1px solid var(--border);
		box-shadow: var(--thumb-ring, 0 0 #0000), var(--elev-2);
		transition: box-shadow .15s;
	}
</style>
