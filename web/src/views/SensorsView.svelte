<script lang="ts">
	/* One card per motion sensor: what its rules see now and what they do. Its
	 * setup, rules and removal are on its own page (SensorView). */
	import * as Card from "$lib/components/ui/card";
	import { Button } from "$lib/components/ui/button";
	import Pencil from "@lucide/svelte/icons/pencil";
	import Plus from "@lucide/svelte/icons/plus";
	import SensorState from "$lib/components/SensorState.svelte";
	import { app, data, openPairing, refresh } from "$lib/state.svelte.ts";
	import { del } from "$lib/api.ts";
	import { toast } from "$lib/toast.svelte.ts";
	import { navigate } from "$lib/router.svelte.ts";
	import { delayLabel, eventText } from "$lib/buttons.ts";
	import { SENSOR_EVENTS, sensorCards, sensorPath } from "$lib/sensors.ts";

	const cards = $derived(sensorCards(data.devices, data.buttons));
	/* Rules whose sensor was removed some other way: they would hold a button
	 * slot nobody can see. */
	const unlinked = $derived(data.buttons.filter((b) => b.source === "sensor" && !b.ieee));
	async function forget(id: number) {
		try { await del(`/api/buttons/${id}`); } catch (err: any) { toast(err.message); }
		refresh();
	}
	const title = (c: (typeof cards)[number]) => c.device?.name ?? c.button?.name ?? c.ieee;
	const kind = (c: (typeof cards)[number]) => c.device ? [c.device.manufacturer, c.device.model].filter(Boolean).join(" · ") || c.ieee
		: "no longer paired";
</script>

<div class="mt-5 flex items-center justify-between gap-3">
	<h2 class="text-lg font-semibold">Sensors</h2>
	<Button variant="outline" size="sm" disabled={app.pairing} onclick={openPairing}><Plus /> Pair a sensor</Button>
</div>

{#if !cards.length}
	<div class="text-muted-foreground mt-6 text-center text-sm">
		<p class="text-foreground text-base font-medium">No motion sensors yet</p>
		<p class="mt-2">Press <b>Pair a sensor</b> and put the sensor into pairing mode — on most, a long press of its reset button. It appears here once the coordinator has heard it; a Xiaomi or Aqara sensor may need a wave in front of it first.</p>
	</div>
{:else}
	<div class="mt-4 grid gap-3 md:grid-cols-2">
		{#each cards as c (`${c.ieee}/${c.ep}`)}
			<Card.Root class="lift">
				<Card.Content class="flex flex-col gap-3">
					<div class="flex items-start gap-2">
						<div class="min-w-0 flex-1">
							<div class="truncate text-base font-semibold">{title(c)}</div>
							<div class="text-muted-foreground mt-0.5 truncate text-xs">{kind(c)}</div>
						</div>
						<Button variant="ghost" size="icon" aria-label="Settings for {title(c)}" onclick={() => navigate(sensorPath(c.ieee))}><Pencil /></Button>
					</div>
					{#if c.button}
						<SensorState button={c.button} />
						<div class="grid grid-cols-[auto_1fr] gap-x-3 gap-y-0.5 text-xs">
							{#each SENSOR_EVENTS as [ev] (ev)}
								<span class="text-muted-foreground">{ev === "clear" ? `No motion for ${delayLabel(c.button.clear_s ?? 0)}` : "Motion"}</span>
								<span>{eventText(c.button.events[ev], data.groups, data.devices) ?? "does nothing"}</span>
							{/each}
						</div>
					{:else}
						<p class="text-muted-foreground text-sm">Not set up yet — tap the pencil to pick the room it switches.</p>
					{/if}
				</Card.Content>
			</Card.Root>
		{/each}
	</div>
{/if}

{#if unlinked.length}
	<h3 class="mt-8 text-base font-semibold">Rules of sensors that are gone</h3>
	<Card.Root class="mt-3">
		<Card.Content class="divide-border flex flex-col divide-y">
			{#each unlinked as b (b.id)}
				<div class="flex items-center justify-between gap-2 py-2 first:pt-0 last:pb-0">
					<span class="text-sm">{b.name}</span>
					<Button variant="ghost" size="sm" onclick={() => forget(b.id)}>Remove</Button>
				</div>
			{/each}
		</Card.Content>
	</Card.Root>
{/if}
