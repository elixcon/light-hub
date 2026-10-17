<script lang="ts">
	/* Warmth and colour as one path: warmth stops above the bar, colour stops
	 * below it, the bar painted with what the lamp shows across the range. The
	 * rows and the bar capture the pointer, never a marker, so a re-render of
	 * the markers cannot end a drag. A stop dragged away from the bar - up for
	 * warmth, down for colour - is removed when the finger lifts, like a point
	 * dragged off the brightness chart; the end stops cannot be.
	 *
	 * The selected stop's kind is whichever control was last picked on: the
	 * warmth slider makes it a warmth stop, the wheel a colour stop. The other
	 * control stays live, only dimmed, so there is no separate kind switch. */
	import { Button } from "$lib/components/ui/button";
	import CtSlider from "./CtSlider.svelte";
	import ColourWheel from "./ColourWheel.svelte";
	import { drag } from "$lib/drag.ts";
	import { chromaGradient, MAX_STOPS, type Stop } from "$lib/curves.ts";
	import { colourOf, rgbCss, pathAt, isKelvinStop, xyToCct, xyToHs, xyOf } from "$lib/colorspace.ts";

	let { path, ensure, sel = $bindable(0), canCt, canColor, minK, maxK, cursorPct = null, onchange, draft = false }: {
		path: Stop[]; ensure: () => Stop[]; sel?: number; canCt: boolean; canColor: boolean;
		minK: number; maxK: number; cursorPct?: number | null; onchange: () => void;
		/* The lamp has no path: what is drawn is a starting point, not what it does. */
		draft?: boolean;
	} = $props();

	const i = $derived(Math.max(0, Math.min(path.length - 1, sel)));
	const s = $derived(path[i]);
	const kelvin = $derived(isKelvinStop(s));
	const css = $derived(rgbCss(colourOf(s)));
	const cursorColour = $derived(cursorPct == null ? "" : rgbCss(colourOf(pathAt(path, cursorPct))));

	/* 1 % and 100 % sit half a marker in from the ends of the bar, not on them,
	 * so an end stop covers the colour it sets rather than hanging off the
	 * bar's rounded corner. The gradient is drawn over that same inset span,
	 * with the end colours filling the caps, and the pointer maths is the exact
	 * inverse - so what is under a marker is what the lamp shows there. */
	const INSET = 10;
	const pos = (at: number) => `calc(${INSET}px + (100% - ${2 * INSET}px) * ${(at - 1) / 99})`;
	const barBackground = $derived.by(() => {
		const first = rgbCss(colourOf(pathAt(path, 1)));
		const last = rgbCss(colourOf(pathAt(path, 100)));
		return `${chromaGradient(path)} ${INSET}px 0 / calc(100% - ${2 * INSET}px) 100% no-repeat, `
			+ `linear-gradient(to right, ${first} 50%, ${last} 50%)`;
	});

	let dragIdx = -1;
	/* Whether the stop being dragged has moved: a tap only selects it, and must
	 * not save - which on a lamp with no path would start one. */
	let moved = false;
	/* The stop that will be removed if the finger lifts now, or -1. */
	let doomed = $state(-1);
	/* How far past its row's outer edge a stop has to be dragged to go. */
	const DOOM_PX = 24;
	const frac = (ev: PointerEvent, r: DOMRect) =>
		Math.max(1, Math.min(100, Math.round(1 + ((ev.clientX - r.left - INSET) / (r.width - 2 * INSET)) * 99)));

	/* `kind` says what a stop added on this element is: true on the warmth row,
	 * false on the colour row, null on the bar (whatever the path has there). */
	function handlers(kind: boolean | null) {
		return {
			onStart(ev: PointerEvent, r: DOMRect) {
				const marker = (ev.target as Element).closest?.("[data-si]") as HTMLElement | null;
				if (marker) {
					dragIdx = Number(marker.dataset.si);
					sel = dragIdx;
					moved = false;
					return;
				}
				const stops = ensure();
				dragIdx = -1;
				if (stops.length >= MAX_STOPS) return false;
				const x = Math.max(2, Math.min(99, frac(ev, r)));
				if (stops.some((q) => q.at === x)) return false;
				/* A new stop starts on the path, so adding one never moves the light. */
				const here = pathAt(stops, x)!;
				let want = kind;
				if (want === null) want = isKelvinStop(here);
				if (want && !canCt) want = false;
				if (!want && !canColor) want = true;
				if (want) {
					const k = isKelvinStop(here) ? here.kelvin : xyToCct(...xyOf(here));
					stops.push({ at: x, kelvin: Math.round(Math.max(minK, Math.min(maxK, k))) });
				} else {
					const hs = "hue" in here ? here : xyToHs(...xyOf(here));
					stops.push({ at: x, hue: hs.hue, sat: hs.sat });
				}
				stops.sort((a, b) => a.at - b.at);
				sel = stops.findIndex((q) => q.at === x);
				onchange();
				return false;
			},
			onMove(ev: PointerEvent, r: DOMRect) {
				/* Clamped against its true neighbours in the merged list, so a warmth
				 * marker cannot be dragged past a colour marker on the other row. */
				if (dragIdx <= 0 || dragIdx >= path.length - 1) return;
				const stops = ensure();
				const at = Math.max(stops[dragIdx - 1].at + 1, Math.min(stops[dragIdx + 1].at - 1, frac(ev, r)));
				moved ||= at !== stops[dragIdx].at;
				stops[dragIdx].at = at;
				/* Only away from the bar: a warmth stop pulled down across it is
				 * still over the path, not off it. */
				const off = kind === true ? ev.clientY < r.top - DOOM_PX
				          : kind === false ? ev.clientY > r.bottom + DOOM_PX : false;
				doomed = off ? dragIdx : -1;
			},
			onEnd() {
				if (dragIdx < 0) return;
				if (!moved && doomed !== dragIdx) {
					dragIdx = -1;               /* a tap: selected, nothing changed */
					return;
				}
				const stops = ensure();
				if (doomed === dragIdx && dragIdx > 0 && dragIdx < stops.length - 1) {
					stops.splice(dragIdx, 1);
					sel = Math.max(0, dragIdx - 1);
				}
				dragIdx = -1;
				doomed = -1;
				onchange();
			},
		};
	}

	/* Either one turns a stop of the other kind into its own, keeping where it
	 * sits - picking on the dimmed control is how a stop changes kind. The
	 * save is the control's onend, once, when the finger lifts. */
	function setKelvin(k: number) {
		const stops = ensure();
		stops[i] = { at: stops[i].at, kelvin: k };
	}
	function setColour(hue: number, sat: number) {
		const stops = ensure();
		stops[i] = { at: stops[i].at, hue, sat };
	}
	function removeStop() {
		const stops = ensure();
		if (i <= 0 || i >= stops.length - 1) return;
		stops.splice(i, 1);
		sel = Math.max(0, i - 1);
		onchange();
	}
</script>

<div class="flex flex-col gap-3">
	{#if canCt}
		<CtSlider kelvin={kelvin ? (s as { kelvin: number }).kelvin : Math.round((minK + maxK) / 2)} {minK} {maxK}
			idle={!kelvin} label="Warmth of this stop" onpick={setKelvin} onend={onchange} />
	{/if}

	<div class="path" class:draft>
		<div class="row above" use:drag={handlers(true)} role="presentation">
			{#each path as st, si (si)}
				{#if isKelvinStop(st)}
					<span class="stop" class:selected={si === i} class:doomed={si === doomed} data-si={si} style:left={pos(st.at)} style:background={rgbCss(colourOf(st))}></span>
				{/if}
			{/each}
		</div>
		<div class="bar" style:background={barBackground} use:drag={handlers(null)} role="presentation">
			{#if cursorPct != null}
				<span class="cursor" style:left={pos(cursorPct)}></span>
			{/if}
		</div>
		<div class="row below" use:drag={handlers(false)} role="presentation">
			{#each path as st, si (si)}
				{#if !isKelvinStop(st)}
					<span class="stop" class:selected={si === i} class:doomed={si === doomed} data-si={si} style:left={pos(st.at)} style:background={rgbCss(colourOf(st))}></span>
				{/if}
			{/each}
		</div>
		<div class="text-muted-foreground flex justify-between text-[0.7rem]"><span>room 1 %</span><span>100 %</span></div>
	</div>

	{#if canColor}
		<ColourWheel hue={kelvin ? 0 : (s as { hue: number }).hue} sat={kelvin ? 0 : (s as { sat: number }).sat}
			idle={kelvin} size="compact" onpick={setColour} onend={onchange} />
	{/if}

	<div class="flex flex-wrap items-center gap-3 text-sm">
		<span class="swatch" style:background={css}></span>
		<span class="text-muted-foreground">
			{#if kelvin}At {s.at} % · {(s as { kelvin: number }).kelvin} K{:else}At {s.at} % · hue {(s as { hue: number }).hue}° · saturation {(s as { sat: number }).sat} %{/if}
		</span>
		{#if cursorPct != null}
			<span class="text-muted-foreground ml-auto flex items-center gap-2">now <span class="swatch small" style:background={cursorColour}></span></span>
		{/if}
	</div>

	<div class="flex flex-wrap items-center gap-2">
		<Button variant="ghost" size="sm" class="ml-auto" disabled={i <= 0 || i >= path.length - 1} onclick={removeStop}>Remove stop</Button>
	</div>
</div>

<style>
	.path { touch-action: none; user-select: none; padding: 0 10px; }
	.path.draft { opacity: .45; }
	.row { position: relative; height: 22px; }
	.bar { position: relative; height: 26px; border-radius: 8px; box-shadow: inset 0 0 0 1px rgb(0 0 0 / .1); }
	.stop {
		position: absolute; width: 20px; height: 20px; border-radius: 9999px;
		border: 2px solid var(--card); box-shadow: var(--elev-2);
		transform: translateX(-50%); top: 1px; cursor: grab;
	}
	/* The rounded end points at the bar. */
	.row.above .stop { border-radius: 4px 4px 9999px 9999px; }
	.row.below .stop { border-radius: 9999px 9999px 4px 4px; }
	.stop.selected { outline: 2px solid var(--curve); outline-offset: 1px; }
	.stop.doomed { outline: 2px solid var(--destructive); opacity: .5; }
	.cursor { position: absolute; top: -3px; bottom: -3px; width: 2px; background: var(--foreground); opacity: .6;
		transform: translateX(-50%); pointer-events: none; border-radius: 2px; }
	.swatch { display: inline-block; width: 22px; height: 22px; border-radius: 9999px; box-shadow: inset 0 0 0 1px rgb(0 0 0 / .1); }
	.swatch.small { width: 16px; height: 16px; }
</style>
