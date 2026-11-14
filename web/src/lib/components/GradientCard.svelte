<script lang="ts">
	/* A strip's gradient: two to five colours from its start to its end, each
	 * picked on the wheel. There is no Save - a change goes to the strip, and the
	 * strip is the record of it. What the strip reports comes back in the poll;
	 * while a stop is being edited the page's own copy is the one drawn, so a
	 * poll arriving mid-drag does not snap it back. */
	import * as Card from "$lib/components/ui/card";
	import { Button } from "$lib/components/ui/button";
	import ColourWheel from "./ColourWheel.svelte";
	import { sendThrottled, sendCommand, schedule } from "$lib/state.svelte.ts";
	import { toast } from "$lib/toast.svelte.ts";
	import { addStop, removeStop, flipStops, gradientCss, stopAt, sameStops, DEFAULT_GRADIENT, GRADIENT_MAX, GRADIENT_MIN,
		type GradColour } from "$lib/gradient.ts";
	import type { Endpoint } from "$lib/types.ts";

	let { ieee, ep, e }: { ieee: string; ep: number; e: Endpoint } = $props();

	const g = $state({ loaded: "", stops: [] as GradColour[], sel: 0, mine: false });
	const reported = $derived(e.gradient?.colors ?? null);
	/* Taken from the strip once per light, and again whenever it reports colours
	 * this page did not just send. */
	$effect(() => {
		const id = `${ieee}/${ep}`;
		if (g.loaded !== id) {
			g.loaded = id;
			g.mine = false;
			g.stops = (reported ?? DEFAULT_GRADIENT).map((c) => ({ ...c }));
			g.sel = 0;
		} else if (reported && !g.mine && !sameStops(reported, g.stops)) {
			g.stops = reported.map((c) => ({ ...c }));
			g.sel = Math.min(g.sel, g.stops.length - 1);
		}
	});
	/* Shown as a suggestion until the strip has one. */
	const draft = $derived(!reported && !g.mine);
	const taken = $derived(e.gradient?.state ?? null);
	const path = $derived(`/api/devices/${ieee}/ep/${ep}/gradient`);
	const sel = $derived(g.stops[Math.min(g.sel, g.stops.length - 1)]);

	function send(now = false) {
		g.mine = true;
		const body = { colors: $state.snapshot(g.stops) };
		if (now) sendCommand(path, body).catch((err) => toast(err.message));
		else sendThrottled(path, body, 300);
	}
	/* The drag is over: from the next poll on, the strip's word counts again. */
	function settle() {
		send(true);
		schedule(900);
		setTimeout(() => { g.mine = false; }, 4000);
	}
	function pick(hue: number, sat: number) {
		g.stops[g.sel] = { hue, sat };
		send();
	}
	function change(next: GradColour[], selected = g.sel) {
		if (next === g.stops) return;
		g.stops = next;
		g.sel = Math.max(0, Math.min(selected, next.length - 1));
		settle();
	}
	/* One colour again: the selected stop's, as a plain colour. */
	function single() {
		g.mine = false;
		sendCommand(path, { colors: [$state.snapshot(sel)] }).catch((err) => toast(err.message));
		schedule(900);
	}
</script>

<Card.Root class="mt-4">
	<Card.Header>
		<Card.Title>Gradient</Card.Title>
		<Card.Description>Colours along the strip, from its start on the left to its end on the right. The strip blends between them; its brightness stays where it is.</Card.Description>
	</Card.Header>
	<Card.Content class="flex flex-col gap-4">
		{#if draft}
			<div class="border-border bg-muted/40 flex flex-wrap items-center gap-x-3 gap-y-2 rounded-md border px-3 py-2 text-xs">
				<span class="text-muted-foreground min-w-0 flex-1">Not in use: the strip shows one colour. Pick a stop's colour to start one, or use these.</span>
				<Button variant="secondary" size="sm" onclick={settle}>Use this gradient</Button>
			</div>
		{:else if taken === "not_taken"}
			<p class="text-destructive text-xs">This lamp was sent the gradient and reports none. It lists the cluster a gradient strip does, but is most likely an ordinary colour lamp.</p>
		{:else if taken === "pending"}
			<p class="text-muted-foreground text-xs">Sent - waiting for the lamp to say it shows it.</p>
		{/if}

		<div class="strip" class:draft>
			<div class="bar" style:background={gradientCss(g.stops)}></div>
			<div class="stops">
				{#each g.stops as st, i (i)}
					<button class="stop" class:selected={i === g.sel} style:left="{stopAt(i, g.stops.length)}%"
						style:background="hsl({st.hue} {st.sat}% 55%)" aria-label="Colour {i + 1} of {g.stops.length}"
						aria-pressed={i === g.sel} onclick={() => { g.sel = i; }}></button>
				{/each}
			</div>
		</div>

		<ColourWheel hue={sel?.hue ?? 0} sat={sel?.sat ?? 0} size="compact" onpick={pick} onend={settle} />

		<div class="flex flex-wrap items-center gap-2">
			<Button variant="outline" size="sm" disabled={g.stops.length >= GRADIENT_MAX} onclick={() => change(addStop($state.snapshot(g.stops)))}>Add colour</Button>
			<Button variant="outline" size="sm" disabled={g.stops.length <= GRADIENT_MIN} onclick={() => change(removeStop($state.snapshot(g.stops), g.sel))}>Remove colour</Button>
			<Button variant="outline" size="sm" onclick={() => change(flipStops($state.snapshot(g.stops)), g.stops.length - 1 - g.sel)}>Flip</Button>
			{#if !draft}
				<Button variant="ghost" size="sm" class="ml-auto" onclick={single}>One colour</Button>
			{/if}
		</div>
		<div class="text-muted-foreground text-xs">Tap a colour below the bar to select it, then set it on the wheel. "Flip" swaps the ends, for a strip mounted the other way round. A colour or warmth set any other way ends the gradient; a warmth curve and Adaptive Lighting leave it alone while it shows.</div>
	</Card.Content>
</Card.Root>

<style>
	.strip { padding: 0 12px; }
	.strip.draft { opacity: .5; }
	.bar { height: 28px; border-radius: 8px; box-shadow: inset 0 0 0 1px rgb(0 0 0 / .1); }
	.stops { position: relative; height: 30px; }
	.stop {
		position: absolute; top: 5px; width: 24px; height: 24px; border-radius: 4px 4px 9999px 9999px;
		border: 2px solid var(--card); box-shadow: var(--elev-2); transform: translateX(-50%); cursor: pointer;
	}
	.stop.selected { outline: 2px solid var(--curve); outline-offset: 1px; }
</style>
