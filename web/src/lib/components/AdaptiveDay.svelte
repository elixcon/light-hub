<script lang="ts">
	/* Today under Adaptive Lighting: the brightness a running room is sent from
	 * midnight to midnight in this browser's time, the colour shift as a band
	 * under it, the sun's own sunrise and sunset dotted, and the ones the mode
	 * uses as handles. Dragging a handle moves that end of the day: the parent
	 * turns the drop into a limit (adaptive.ts dropOn) and redraws from the
	 * mirror while the finger is still down. The look is the curve editor's
	 * (CurveChart); the geometry is in screen pixels instead, a fixed height and
	 * the card's width, so a timeline gets longer on a wide screen rather than
	 * bigger, and its labels stay the size of the text around them. */
	import { CPAD } from "$lib/curves.ts";
	import { drag } from "$lib/drag.ts";
	import { kelvinToCss } from "$lib/colorspace.ts";
	import { localMinutes, hhmm, type Sample } from "$lib/adaptive.ts";

	type Which = "rise" | "set";
	let { samples, from, to, now = null, boxDot = null, real = null, used = null, kBase, ondrag }: {
		samples: Sample[];
		from: number;                       /* Unix seconds of this local midnight */
		to: number;                         /* and the next */
		now?: number | null;
		boxDot?: { t: number; pct: number } | null;   /* what the coordinator said, and when */
		real?: { rise: number; set: number } | null;  /* the sun's own, today */
		used?: { rise: number; set: number } | null;  /* the mode's, today */
		kBase: number;
		ondrag: (which: Which, minutes: number, done: boolean) => void;
	} = $props();

	let width = $state(0);
	const CW = $derived(Math.max(280, Math.round(width) || 300));
	const CH = 190;
	const W = $derived(CW - 2 * CPAD);
	const x = (t: number) => CPAD + ((t - from) / (to - from)) * W;
	const cy = (v: number) => CH - CPAD - (v / 100) * (CH - 2 * CPAD);
	const inDay = (t: number | null | undefined): t is number => t !== null && t !== undefined && t >= from && t <= to;

	const line = $derived(samples.map((s, i) => `${i ? "L" : "M"}${x(s.t).toFixed(1)},${cy(s.pctLine).toFixed(1)}`).join(""));
	const fill = $derived(samples.length
		? `${line}L${x(samples[samples.length - 1].t).toFixed(1)},${CH - CPAD}L${x(samples[0].t).toFixed(1)},${CH - CPAD}Z` : "");
	const band = $derived.by(() => {
		if (!samples.length) return "none";
		const stops = samples.filter((_, i) => i % 3 === 0 || i === samples.length - 1)
			.map((s) => `${kelvinToCss(kBase + s.offsetK)} ${(((s.t - from) / (to - from)) * 100).toFixed(1)}%`);
		return `linear-gradient(to right, ${stops.join(", ")})`;
	});

	/* 00, 06, 12, 18 and 24 o'clock as this browser counts them - an hour off
	 * the even spacing on the night the clocks change. */
	const ticks = $derived.by(() => {
		const d = new Date(from * 1000);
		return [0, 6, 12, 18, 24].map((h) => ({
			h, t: new Date(d.getFullYear(), d.getMonth(), d.getDate(), h).getTime() / 1000,
		}));
	});

	let dragging = $state<Which | null>(null);
	let last = 0;
	const minutesAt = (ev: PointerEvent, r: DOMRect) => {
		const ux = ((ev.clientX - r.left) / r.width) * CW;
		const t = from + ((ux - CPAD) / W) * (to - from);
		const m = Math.round(localMinutes(Math.max(from, Math.min(to - 60, t))) / 5) * 5;
		return Math.min(23 * 60 + 55, m);
	};
	function start(ev: PointerEvent, r: DOMRect) {
		if (!used) return false;
		const ux = ((ev.clientX - r.left) / r.width) * CW;
		const dr = Math.abs(ux - x(used.rise)), ds = Math.abs(ux - x(used.set));
		const best = Math.min(dr, ds);
		if (best > 14) return false;
		dragging = dr <= ds ? "rise" : "set";
		last = minutesAt(ev, r);
	}
	function move(ev: PointerEvent, r: DOMRect) {
		if (!dragging) return;
		last = minutesAt(ev, r);
		ondrag(dragging, last, false);
	}
	function end() {
		if (!dragging) return;
		const w = dragging;
		dragging = null;
		ondrag(w, last, true);
	}
	const label = (t: number) => hhmm(localMinutes(t));
</script>

<div bind:clientWidth={width}>
	<svg class="chart" viewBox="0 0 {CW} {CH}" role="application"
		aria-label="Adaptive Lighting today: brightness over the day, with the sunrise and sunset it uses as handles"
		use:drag={{ onStart: start, onMove: move, onEnd: end }}>
		<line class="grid" x1={CPAD} y1={CH - CPAD} x2={CW - CPAD} y2={CH - CPAD} />
		<line class="grid" x1={CPAD} y1={CPAD} x2={CPAD} y2={CH - CPAD} />
		<text class="axis" x={CPAD} y={CPAD - 5}>100 %</text>
		{#each ticks as tk (tk.h)}
			{#if tk.h > 0 && tk.h < 24}<line class="tick" x1={x(tk.t)} y1={CPAD} x2={x(tk.t)} y2={CH - CPAD} />{/if}
			<text class="axis" x={x(tk.t)} y={CH - 3}
				text-anchor={tk.h === 0 ? "start" : tk.h === 24 ? "end" : "middle"}>{String(tk.h).padStart(2, "0")}:00</text>
		{/each}

		{#if real && used}
			{#each [["rise", real.rise, used.rise], ["set", real.set, used.set]] as [k, r, u] (k)}
				{#if inDay(r as number) && inDay(u as number) && r !== u}
					<rect class="stretch" x={Math.min(x(r as number), x(u as number))} y={CPAD}
						width={Math.abs(x(u as number) - x(r as number))} height={CH - 2 * CPAD} />
				{/if}
			{/each}
		{/if}
		{#if real}
			{#each [real.rise, real.set] as r, i (i)}
				{#if inDay(r)}
					<line class="sun" x1={x(r)} y1={CPAD} x2={x(r)} y2={CH - CPAD} />
					{#if !used || (i === 0 ? used.rise : used.set) !== r}
						<text class="axis faint" x={x(r)} y={CH - CPAD - 4} text-anchor="middle">☀ {label(r)}</text>
					{/if}
				{/if}
			{/each}
		{/if}

		<path class="area" d={fill} />
		<path class="line" d={line} />

		{#if inDay(now)}
			<line class="cursor" x1={x(now)} y1={CPAD} x2={x(now)} y2={CH - CPAD} />
		{/if}
		{#if boxDot && inDay(boxDot.t)}
			<circle class="cursor-dot" cx={x(boxDot.t)} cy={cy(boxDot.pct)} r={3.5} />
		{/if}

		{#if used}
			{#each [["rise", used.rise], ["set", used.set]] as [k, u] (k)}
				{#if inDay(u as number)}
					<line class="handle" class:active={dragging === k} x1={x(u as number)} y1={CPAD} x2={x(u as number)} y2={CH - CPAD} />
					<circle class="knob" class:active={dragging === k} cx={x(u as number)} cy={CPAD + 10} r={dragging === k ? 7 : 6} />
					<text class="axis strong" x={x(u as number) + (k === "rise" ? -9 : 9)} y={CPAD + 13}
						text-anchor={k === "rise" ? "end" : "start"}>{label(u as number)}</text>
				{/if}
			{/each}
		{/if}
	</svg>
	<div class="band" style="background: {band}; margin: 0 {(CPAD / CW) * 100}%;"></div>
</div>

<style>
	.chart { width: 100%; height: auto; display: block; touch-action: none; user-select: none;
		background: var(--muted); border-radius: var(--radius-md); }
	.chart * { vector-effect: non-scaling-stroke; }
	.grid { stroke: var(--border); stroke-width: 1; }
	.tick { stroke: var(--border); stroke-width: 1; opacity: .6; }
	.axis { fill: var(--muted-foreground); font-size: 10px; }
	.axis.faint { opacity: .8; }
	.axis.strong { fill: var(--foreground); font-weight: 500; paint-order: stroke; stroke: var(--muted);
		stroke-width: 3; stroke-linejoin: round; }
	.stretch { fill: var(--curve); opacity: .1; }
	.sun { stroke: var(--muted-foreground); stroke-width: 1; stroke-dasharray: 1 3; opacity: .8; }
	.area { fill: var(--curve); opacity: .08; }
	.line { fill: none; stroke: var(--curve); stroke-width: 1.75; stroke-linecap: round; }
	.handle { stroke: var(--curve); stroke-width: 1.25; opacity: .7; }
	.handle.active { opacity: 1; stroke-width: 2; }
	.knob { fill: var(--card); stroke: var(--curve); stroke-width: 1.75; cursor: grab; }
	.knob.active { stroke-width: 2.25; }
	.cursor { stroke: var(--foreground); stroke-width: 1; stroke-dasharray: 2 3; opacity: .5; pointer-events: none; }
	.cursor-dot { fill: var(--on); stroke: var(--card); stroke-width: 1.5; pointer-events: none; }
	.band { height: 10px; border-radius: 3px; margin-top: 6px; }
</style>
