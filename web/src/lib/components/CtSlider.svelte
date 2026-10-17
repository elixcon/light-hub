<script lang="ts">
	/* A colour-temperature track painted from warm to cool. `idle` dims it and
	 * hides the knob - it is not what the selected thing is set to - but it
	 * still takes a pick, which is how a caller lets it change that. */
	import { drag } from "$lib/drag.ts";
	import { kelvinToCss } from "$lib/colorspace.ts";
	let { kelvin, minK, maxK, idle = false, onpick, onend, label = "Colour temperature" }: {
		kelvin: number; minK: number; maxK: number; idle?: boolean; label?: string;
		onpick: (kelvin: number) => void; onend?: () => void;
	} = $props();
	const at = $derived(Math.max(0, Math.min(1, (kelvin - minK) / Math.max(1, maxK - minK))) * 100);
	function pick(ev: PointerEvent, r: DOMRect) {
		const frac = Math.max(0, Math.min(1, (ev.clientX - r.left) / r.width));
		onpick(Math.round(minK + frac * (maxK - minK)));
	}
</script>

<div class="ct" class:idle role="slider" aria-label={label} aria-valuemin={minK} aria-valuemax={maxK} aria-valuenow={kelvin} tabindex="0"
	use:drag={{ onMove: pick, onEnd: () => onend?.() }}
	onkeydown={(e) => { const d = e.key === "ArrowLeft" ? -100 : e.key === "ArrowRight" ? 100 : 0; if (d) { onpick(Math.max(minK, Math.min(maxK, kelvin + d))); onend?.(); } }}>
	<div class="label" style:left="{at}%">{kelvin} K</div>
	<div class="track" style:background="linear-gradient(to right, {kelvinToCss(minK)}, {kelvinToCss((minK + maxK) / 2)}, {kelvinToCss(maxK)})">
		<div class="knob" style:left="{at}%"></div>
	</div>
	<div class="scale"><span>{Math.round(minK)} K</span><span>{Math.round(maxK)} K</span></div>
</div>

<style>
	.ct { position: relative; padding-top: 1.4rem; touch-action: none; user-select: none; }
	.ct.idle { opacity: .5; cursor: pointer; }
	.ct.idle .label, .ct.idle .knob { visibility: hidden; }
	.ct:focus-visible { outline: 2px solid var(--ring); outline-offset: 4px; border-radius: 6px; }
	.label { position: absolute; top: 0; transform: translateX(-50%); font-size: .75rem; color: var(--muted-foreground); font-variant-numeric: tabular-nums; }
	.track { position: relative; height: 12px; border-radius: 9999px; box-shadow: inset 0 0 0 1px rgb(0 0 0 / .08); }
	.knob { position: absolute; top: 50%; width: 24px; height: 24px; border-radius: 9999px; background: var(--card);
		border: 1px solid var(--border); box-shadow: var(--elev-2); transform: translate(-50%, -50%); }
	.scale { display: flex; justify-content: space-between; font-size: .7rem; color: var(--muted-foreground); margin-top: .35rem; }
</style>
