<script lang="ts">
	import * as Card from "$lib/components/ui/card";
	import { Button } from "$lib/components/ui/button";
	import { Badge } from "$lib/components/ui/badge";
	import Pencil from "@lucide/svelte/icons/pencil";
	import ChevronRight from "@lucide/svelte/icons/chevron-right";
	import PowerSwitch from "$lib/components/PowerSwitch.svelte";
	import CreateByName from "$lib/components/CreateByName.svelte";
	import LevelSlider from "$lib/components/LevelSlider.svelte";
	import MatterPairDialog from "$lib/components/MatterPairDialog.svelte";
	import { app, data, pending, key, value, epPct, isRemoteEp, setOnOff, setLevel, seenIeee, openPairing } from "$lib/state.svelte.ts";
	import { navigate } from "$lib/router.svelte.ts";
	import { stateLabel } from "$lib/format.ts";
	import { lightColour, lampAttrs } from "$lib/glow.ts";

	/* A device that arrived while the page was open is "just joined" until the
	 * page is next loaded; the poll fills the set on its first pass. */
	const isNew = (ieee: string) => !app.firstLoad && !seenIeee.has(ieee);
	let matterOpen = $state(false);

	/* Remotes have a page of their own; here they would only be cards with
	 * nothing to switch. A device still being interrogated has no endpoints yet
	 * and is kept, because nobody knows yet whether it is a remote. */
	const lights = $derived(data.devices
		.map((d) => ({ ...d, endpoints: d.endpoints.filter((e) => !isRemoteEp(e)) }))
		.filter((d, i) => d.endpoints.length || !data.devices[i].endpoints.length));
</script>

<!-- Three ways a light arrives, behind one button: side by side they wrapped
     into two rows beside the heading on a phone, and only the empty page said
     which one is for what. -->
<CreateByName title="Lights" addLabel="Add a light" placeholder="Name this light" path="/api/lights" route="/lights"
	to={(l) => `/lights/${l.ieee}/${l.ep}`}>
	{#snippet choices({ name, close })}
		<button class="choice" disabled={app.pairing} onclick={() => { close(); openPairing(); }}>
			<span><b>Pair a light</b><small>A Zigbee bulb, plug or relay, put into pairing mode.</small></span><ChevronRight />
		</button>
		<button class="choice" onclick={() => { close(); matterOpen = true; }}>
			<span><b>Add a Matter lamp</b><small>A lamp you already have in Apple Home or another app.</small></span><ChevronRight />
		</button>
		<button class="choice" onclick={name}>
			<span><b>Add a wired light</b><small>A lamp or relay wired to a pin of this board.</small></span><ChevronRight />
		</button>
	{/snippet}
</CreateByName>
<MatterPairDialog bind:open={matterOpen} />

{#if !lights.length}
	<div class="text-muted-foreground mt-8 text-center text-sm">
		<p class="text-foreground text-base font-medium">No lights paired yet</p>
		<p class="mt-2">Press <b>Add a light</b> and <b>Pair a light</b>, then put the light into pairing mode — for most bulbs that means a power-cycle sequence, or a reset with the remote that came with it.</p>
		<p class="mt-2">A lamp or relay wired to a pin of the board works too: press <b>Add a wired light</b> and pick the pin.</p>
		<p class="mt-2">A Matter lamp you already have in Apple Home or another app: press <b>Add a Matter lamp</b>.</p>
	</div>
{:else}
	<div class="mt-4 grid gap-3 md:grid-cols-2">
		{#each lights as d (d.ieee)}
			{@const multi = d.endpoints.length > 1}
			{#if !d.endpoints.length}
				<Card.Root><Card.Content>
					<div class="text-base font-semibold">{d.name}</div>
					<div class="text-muted-foreground mt-1 text-xs">Still discovering what this device can do…</div>
				</Card.Content></Card.Root>
			{/if}
			{#each d.endpoints as e (e.ep)}
				{@const on = value(d.ieee, e.ep, "on", e.on)}
				{@const pct = value(d.ieee, e.ep, "pct", epPct(e))}
				{@const dims = e.features.includes("brightness")}
				<Card.Root class="lamp lift data-new:ring-primary/60" data-new={isNew(d.ieee) || undefined} data-lit={on === true || undefined}
					{...lampAttrs(lightColour(e))}>
					<Card.Content class="flex flex-col gap-3">
						<div class="flex items-start gap-2">
							<div class="min-w-0 flex-1">
								<div class="flex flex-wrap items-center gap-2">
									<span class="truncate text-base font-semibold">{d.name}</span>
									{#if multi}<span class="text-muted-foreground text-xs">endpoint {e.ep}</span>{/if}
									{#if isNew(d.ieee)}<Badge>just joined</Badge>{/if}
									{#if d.needs_pairing}<Badge variant="outline">needs pairing again</Badge>
									{:else if !d.online}<Badge variant="outline">offline</Badge>{/if}
								</div>
								<div class="text-muted-foreground mt-0.5 truncate text-xs">{d.source !== "gpio"
								? [d.manufacturer, d.model].filter(Boolean).join(" · ") || d.ieee
								: d.kind === "cwnwww"
									? (d.gpio == null || d.gpio_neutral == null || d.gpio_warm == null ? "three-channel white · pins not chosen yet" : `GPIO ${d.gpio} + ${d.gpio_neutral} + ${d.gpio_warm} · three-channel white · ${d.pwm_hz} Hz${d.pwm_phase ? ` · ${d.pwm_phase}°` : ""}`)
								: d.kind === "cwww"
									? (d.gpio == null || d.gpio_warm == null ? "tunable white · pins not chosen yet" : `GPIO ${d.gpio} + ${d.gpio_warm} · tunable white · ${d.pwm_hz} Hz${d.pwm_phase ? ` · ${d.pwm_phase}°` : ""}`)
									: d.kind === "dim"
										? (d.gpio == null ? "dimmable · no pin yet" : `GPIO ${d.gpio} · dimmable · ${d.pwm_hz} Hz${d.pwm_phase ? ` · ${d.pwm_phase}°` : ""}`)
										: (d.gpio == null ? "wired · no pin yet" : `GPIO ${d.gpio} · wired`)}</div>
							</div>
							<Button variant="ghost" size="icon" aria-label="Settings for {d.name}" onclick={() => navigate(`/lights/${d.ieee}/${e.ep}`)}><Pencil /></Button>
						</div>
						{#if e.features.includes("onoff")}
							<div class="flex items-center gap-3">
								<PowerSwitch {on} pending={pending.has(key(d.ieee, e.ep, "on"))} label="Toggle {d.name}" onchange={(v) => setOnOff(d.ieee, e.ep, v)} />
								{#if dims}
									<LevelSlider {pct} {on} label="Brightness of {d.name}" onsend={(v) => setLevel(d.ieee, e.ep, v)} class="flex-1" />
								{:else}
									<span class="text-muted-foreground text-sm">{stateLabel(on)} · can only switch</span>
								{/if}
							</div>
						{/if}
					</Card.Content>
				</Card.Root>
			{/each}
		{/each}
	</div>
{/if}

<style>
	/* The rows of Settings' Access / Matter / Wi-Fi list, with a line on what
	 * each one is for. */
	.choice {
		display: flex; align-items: center; gap: .75rem; width: 100%; text-align: left;
		padding: .7rem var(--card-spacing); cursor: pointer;
	}
	.choice span { flex: 1; min-width: 0; }
	.choice b { display: block; font-size: .95rem; font-weight: 500; }
	.choice small { display: block; margin-top: .1rem; font-size: .75rem; color: var(--muted-foreground); }
	.choice :global(svg) { width: 1rem; height: 1rem; flex-shrink: 0; color: var(--muted-foreground); }
	.choice:hover:enabled b { color: var(--primary); }
	.choice:focus-visible { outline: 2px solid var(--ring); outline-offset: -2px; border-radius: calc(var(--radius) - 4px); }
	.choice:disabled { opacity: .5; cursor: default; }
</style>
