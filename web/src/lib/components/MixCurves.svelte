<script lang="ts">
	/* A tunable white light's mixing curves: one per LED channel, how much of it
	 * lights at each colour temperature, warmest on the left. The chart is the
	 * brightness curve's, one channel at a time, with the others drawn behind it
	 * - their overlap is what the light does. */
	import CurveChart from "$lib/components/CurveChart.svelte";
	import { Button } from "$lib/components/ui/button";
	import { CHANNEL_NAMES, channelAt, channelsOf, mixPosition, presetsFor, type Channel, type MixCurves } from "$lib/mix.ts";
	import type { Point } from "$lib/curves.ts";

	let { curves, three, coldK, neutralK, warmK, kelvin = null, onchange }: {
		curves: MixCurves;                  /* edited in place */
		three: boolean;
		coldK: number;
		neutralK: number;
		warmK: number;
		kelvin?: number | null;             /* where the light is now */
		onchange: () => void;               /* something moved: save (debounced) */
	} = $props();

	const channels = $derived(channelsOf(three));
	let sel = $state<Channel>("cold");
	$effect(() => { if (!channels.includes(sel)) sel = channels[0]; });

	const presets = $derived(presetsFor(three, coldK, neutralK, warmK));
	const ensure = (): Point[] => (curves[sel] ??= structuredClone(presets["Constant total"][sel] ?? [[1, 0], [100, 0]]));
	const peers = $derived(channels.filter((ch) => ch !== sel)
		.map((ch) => ({ name: CHANNEL_NAMES[ch], points: curves[ch] ?? [[1, 0], [100, 0]] })));
	const cursor = $derived(kelvin ? mixPosition(kelvin, coldK, warmK) : null);
	/* The light's total at the cursor, against one channel at full: a constant
	 * mix keeps it at 100 %, a full one goes above it in the middle. */
	const total = $derived(cursor === null ? null
		: Math.round(channels.reduce((s, ch) => s + channelAt(curves[ch], cursor), 0)));

	function apply(name: string) {
		const p = presets[name];
		for (const ch of channels) curves[ch] = structuredClone(p[ch]);
		onchange();
	}
</script>

<div class="flex flex-col gap-3">
	<div class="bg-muted inline-flex self-start rounded-lg p-0.5" role="tablist">
		{#each channels as ch (ch)}
			<button class="tab" role="tab" aria-selected={sel === ch} onclick={() => (sel = ch)}>{CHANNEL_NAMES[ch]}</button>
		{/each}
	</div>
	<CurveChart pts={curves[sel] ?? [[1, 0], [100, 0]]} {ensure} {peers} cursorPct={cursor} {onchange}
		labels={{ top: "100 %", left: `${warmK} K`, right: `${coldK} K` }}
		ariaLabel="{CHANNEL_NAMES[sel]} across the colour temperatures" peersLabel="The other channels:" />
	<div class="text-muted-foreground text-xs">
		How much of the {CHANNEL_NAMES[sel].toLowerCase()} LEDs' full output lights at each colour temperature,
		from {warmK} K on the left to {coldK} K on the right, at full brightness; the brightness scales all of them.
		{#if total !== null}At {kelvin} K the channels add up to <b class="text-foreground">{total} %</b> of one at full.{/if}
	</div>
	<div class="flex flex-wrap gap-2">
		{#each Object.keys(presets) as p (p)}
			<Button variant="outline" size="sm" onclick={() => apply(p)}>{p}</Button>
		{/each}
	</div>
	{#if three}
		<div class="text-muted-foreground text-xs">"Philips Hue Being" is that lamp's own mix as measured at
			<a href="https://wejn.org/2024/12/hue-engine-reversing-the-functions/" class="text-primary underline underline-offset-4" target="_blank" rel="noreferrer">wejn.org</a>:
			each white at full on its own side, all three together a little cold of the middle. It is drawn over
			the Hue's 2203–6536 K and stretched to this light's range.</div>
	{/if}
</div>

<style>
	.tab { padding: .4rem .9rem; border-radius: 6px; font-size: .85rem; font-weight: 500; color: var(--muted-foreground); }
	.tab[aria-selected="true"] { background: var(--card); color: var(--foreground); box-shadow: var(--elev-1); }
</style>
