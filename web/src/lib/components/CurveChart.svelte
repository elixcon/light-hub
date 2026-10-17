<script lang="ts">
	/* The brightness curve: drag a point, tap the line to insert one, drag a
	 * point off the chart to delete it. The first and last points are pinned to
	 * 1 % and 100 % so the curve always spans the whole slider. The <svg> holds
	 * the pointer capture; the circles under it are repositioned, never
	 * re-created, so a poll cannot end a drag. */
	import { curveAt, cx, cy, invx, invy, CW, CH, CPAD, MAX_POINTS, type Point } from "$lib/curves.ts";
	import { drag } from "$lib/drag.ts";
	let { pts, ensure, peers = [], cursorPct = null, onchange, ondone,
		labels = { top: "lamp 100 %", left: "0 %", right: "room 100 %" },
		ariaLabel = "Brightness response curve", peersLabel = "Also in its rooms:",
		yRange = { lo: 0, hi: 100, step: 1 }, marks = [], zeroLabel = "" }: {
		pts: Point[];                       /* what to draw */
		ensure: () => Point[];              /* the mutable curve, created on demand */
		peers?: { name: string; points: Point[] }[];
		cursorPct?: number | null;          /* where the test slider is */
		onchange: () => void;               /* something moved: save (debounced) */
		ondone?: () => void;
		/* The same chart draws a light's mixing curves, whose axes are other things. */
		labels?: { top: string; left: string; right: string };
		ariaLabel?: string;
		peersLabel?: string;
		/* What the y axis spans and the steps a dragged point lands on: Adaptive
		 * Lighting's colour shift is -3000 to 3000 K in 50s. */
		yRange?: { lo: number; hi: number; step: number };
		/* Vertical guides at x, labelled at the top: "sunrise · sunset". */
		marks?: { x: number; label: string }[];
		/* A range across 0 gets a line there, labelled with this. */
		zeroLabel?: string;
	} = $props();

	const lo = $derived(yRange.lo), hi = $derived(yRange.hi), step = $derived(yRange.step);
	const clamp = (v: number) => Math.max(lo, Math.min(hi, v));
	const snap = (v: number) => Math.round(clamp(v) / step) * step;
	const yAt = (v: number) => cy(clamp(v), lo, hi);
	const line = $derived.by(() => {
		let d = "";
		for (let i = 0; i <= 60; i++) {
			const x = 1 + (99 * i) / 60;
			d += `${i ? "L" : "M"}${cx(x).toFixed(1)},${yAt(curveAt(pts, x)).toFixed(1)}`;
		}
		return d;
	});
	const peerLines = $derived(peers.map((p) => {
		let d = "";
		for (let i = 0; i <= 40; i++) {
			const x = 1 + (99 * i) / 40;
			d += `${i ? "L" : "M"}${cx(x).toFixed(1)},${yAt(curveAt(p.points, x)).toFixed(1)}`;
		}
		return d;
	}));

	/* The viewBox scales with the column, so on a wide desktop layout a radius in
	 * chart units comes out oversized. Radii are sized in screen pixels instead,
	 * and strokes use non-scaling-stroke; the grab distances stay in chart units. */
	let width = $state(0);
	const px = $derived(width ? CW / width : 1);

	/* The other lights' curves carry no marking of their own, so pointing at one -
	 * or at its name in the list below, which also works by tap - lights it up and
	 * names it where the pointer is. */
	let hot = $state<{ i: number; x: number } | null>(null);
	const hotLabel = $derived.by(() => {
		const p = hot && peers[hot.i];
		if (!hot || !p) return null;
		return { name: p.name, x: cx(hot.x), y: yAt(curveAt(p.points, hot.x)), end: hot.x > 60 };
	});
	function peerHover(ev: MouseEvent, i: number) {
		const svg = (ev.currentTarget as SVGElement).ownerSVGElement;
		if (!svg) return;
		const r = svg.getBoundingClientRect();
		hot = { i, x: Math.max(1, Math.min(100, invx(((ev.clientX - r.left) / r.width) * CW))) };
	}
	const togglePeer = (i: number) => (hot = hot?.i === i ? null : { i, x: 50 });

	let dragging = $state<{ idx: number; doomed: boolean } | null>(null);
	let live: Point[] | null = null;

	const local = (ev: PointerEvent, r: DOMRect): [number, number] =>
		[((ev.clientX - r.left) / r.width) * CW, ((ev.clientY - r.top) / r.height) * CH];

	function start(ev: PointerEvent, r: DOMRect) {
		const p = ensure();
		const [px, py] = local(ev, r);
		let idx = -1, best = 16;
		p.forEach((q, i) => {
			const d = Math.hypot(px - cx(q[0]), py - yAt(q[1]));
			if (d < best) { best = d; idx = i; }
		});
		if (idx < 0 && p.length < MAX_POINTS) {
			const x = invx(px);
			const onLine = Math.abs(py - yAt(curveAt(p, x)));
			if (onLine < 18 && x > p[0][0] && x < p[p.length - 1][0] && !p.some((q) => q[0] === x)) {
				p.push([x, snap(curveAt(p, x))]);
				p.sort((a, b) => a[0] - b[0]);
				idx = p.findIndex((q) => q[0] === x);
			}
		}
		if (idx < 0) return false;
		live = p;
		dragging = { idx, doomed: false };
	}
	function move(ev: PointerEvent, r: DOMRect) {
		if (!dragging || !live) return;
		const p = live;
		const [px, py] = local(ev, r);
		const idx = dragging.idx, last = p.length - 1;
		if (idx > 0 && idx < last) p[idx][0] = Math.max(p[idx - 1][0] + 1, Math.min(p[idx + 1][0] - 1, invx(px)));
		p[idx][1] = snap(invy(py, lo, hi));
		dragging.doomed = idx > 0 && idx < last && (py < -6 || py > CH + 6);
	}
	function end() {
		if (!dragging || !live) return;
		const { idx, doomed } = dragging;
		dragging = null;
		if (doomed && idx > 0 && idx < live.length - 1) live.splice(idx, 1);
		live = null;
		onchange();
		ondone?.();
	}
</script>

<div bind:clientWidth={width}>
<svg class="chart" viewBox="0 0 {CW} {CH}" role="application" aria-label={ariaLabel}
	use:drag={{ onStart: start, onMove: move, onEnd: end }}>
	<line class="grid" x1={CPAD} y1={CH - CPAD} x2={CW - CPAD} y2={CH - CPAD} />
	<line class="grid" x1={CPAD} y1={CPAD} x2={CPAD} y2={CH - CPAD} />
	<text class="axis" x={CPAD} y={CPAD - 5}>{labels.top}</text>
	<text class="axis" x={CPAD} y={CH - 3}>{labels.left}</text>
	<text class="axis" x={CW - CPAD} y={CH - 3} text-anchor="end">{labels.right}</text>
	{#if lo < 0 && hi > 0}
		<line class="zero" x1={CPAD} y1={yAt(0)} x2={CW - CPAD} y2={yAt(0)} />
		{#if zeroLabel}<text class="axis" x={CPAD + 3} y={yAt(0) - 3}>{zeroLabel}</text>{/if}
	{/if}
	{#each marks as m (m.x)}
		<line class="mark" x1={cx(m.x)} y1={CPAD} x2={cx(m.x)} y2={CH - CPAD} />
		<text class="axis" x={cx(m.x)} y={CPAD - 5} text-anchor="middle">{m.label}</text>
	{/each}
	{#each peerLines as d, i (i)}
		<path class="peer" class:hot={hot?.i === i} {d} />
		<!-- svelte-ignore a11y_no_static_element_interactions -->
		<path class="peer-hit" {d} onmousemove={(ev) => peerHover(ev, i)} onmouseleave={() => (hot = null)} />
	{/each}
	<path class="line" d={line} />
	{#if cursorPct !== null && cursorPct !== undefined}
		<line class="cursor" x1={cx(cursorPct)} y1={CPAD} x2={cx(cursorPct)} y2={CH - CPAD} />
		<circle class="cursor-dot" cx={cx(cursorPct)} cy={yAt(curveAt(pts, cursorPct))} r={3.5 * px} />
	{/if}
	{#each pts as p, i (i)}
		<circle class="dot" class:doomed={dragging?.idx === i && dragging.doomed} class:active={dragging?.idx === i}
			cx={cx(p[0]).toFixed(1)} cy={yAt(p[1]).toFixed(1)} r={(dragging?.idx === i ? 6 : 5) * px} />
	{/each}
	{#if hotLabel}
		<text class="peer-label" x={hotLabel.x + (hotLabel.end ? -6 : 6)} y={hotLabel.y - 6}
			text-anchor={hotLabel.end ? "end" : "start"}>{hotLabel.name}</text>
	{/if}
</svg>
{#if peers.length}
	<div class="legend">
		<span>{peersLabel}</span>
		{#each peers as p, i (i)}
			<button type="button" class:hot={hot?.i === i} onmouseenter={() => (hot = { i, x: 50 })}
				onmouseleave={() => (hot = null)} onclick={() => togglePeer(i)}><span class="swatch"></span>{p.name}</button>
		{/each}
	</div>
{/if}
</div>

<style>
	.chart { width: 100%; height: auto; display: block; touch-action: none; user-select: none;
		background: var(--muted); border-radius: var(--radius-md); }
	.chart * { vector-effect: non-scaling-stroke; }
	.grid { stroke: var(--border); stroke-width: 1; }
	.zero, .mark { stroke: var(--border); stroke-width: 1; stroke-dasharray: 3 3; }
	.axis { fill: var(--muted-foreground); font-size: 9px; }
	.peer { fill: none; stroke: var(--muted-foreground); stroke-width: 1; opacity: .35; transition: opacity .15s; }
	.peer.hot { stroke: var(--foreground); stroke-width: 1.5; opacity: .8; }
	.peer-hit { fill: none; stroke: transparent; stroke-width: 12; pointer-events: stroke; }
	.peer-label { fill: var(--foreground); font-size: 9px; font-weight: 500; pointer-events: none;
		paint-order: stroke; stroke: var(--muted); stroke-width: 3; stroke-linejoin: round; }
	.legend { display: flex; flex-wrap: wrap; align-items: center; gap: .25rem .75rem; margin-top: .5rem;
		font-size: .75rem; color: var(--muted-foreground); }
	.legend button { display: inline-flex; align-items: center; gap: .375rem; border: 0; padding: 0;
		background: none; font: inherit; color: inherit; cursor: pointer; }
	.legend button.hot { color: var(--foreground); }
	.swatch { width: 14px; border-top: 1.5px solid currentColor; opacity: .6; }
	.line { fill: none; stroke: var(--curve); stroke-width: 1.75; stroke-linecap: round; }
	.dot { fill: var(--card); stroke: var(--curve); stroke-width: 1.75; cursor: grab; }
	.dot.active { stroke-width: 2.25; }
	.dot.doomed { stroke: var(--destructive); fill: var(--destructive); }
	.cursor { stroke: var(--foreground); stroke-width: 1; stroke-dasharray: 2 3; opacity: .5; pointer-events: none; }
	.cursor-dot { fill: var(--on); stroke: var(--card); stroke-width: 1.5; pointer-events: none; }
</style>
