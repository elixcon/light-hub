<script lang="ts">
	import * as Card from "$lib/components/ui/card";
	import { Button } from "$lib/components/ui/button";
	import { Badge } from "$lib/components/ui/badge";
	import Pencil from "@lucide/svelte/icons/pencil";
	import SunMedium from "@lucide/svelte/icons/sun-medium";
	import PowerSwitch from "$lib/components/PowerSwitch.svelte";
	import LevelSlider from "$lib/components/LevelSlider.svelte";
	import CreateByName from "$lib/components/CreateByName.svelte";
	import { data, pending, gkey, gvalue, findEp, setGroupOnOff, setGroupLevel, startAdaptive } from "$lib/state.svelte.ts";
	import { navigate } from "$lib/router.svelte.ts";
	import { plural, stateLabel } from "$lib/format.ts";
	import { roomColour, lampAttrs } from "$lib/glow.ts";
</script>

<CreateByName title="Rooms" addLabel="New room" placeholder="Name this room" path="/api/groups" route="/groups" />

{#if !data.groups.length}
	<div class="text-muted-foreground mt-8 text-center text-sm">
		<p class="text-foreground text-base font-medium">No rooms yet</p>
		<p class="mt-2">A room is a set of lights with one brightness slider — a whole room, or just a shelf or a desk. Press <b>New room</b>, name it, then tick the lights it should control.</p>
	</div>
{:else}
	<div class="mt-4 grid gap-3 md:grid-cols-2">
		{#each data.groups as g (g.id)}
			{@const on = gvalue(g.id, "on", g.on)}
			{@const pct = gvalue(g.id, "pct", g.brightness_pct)}
			{@const dimmable = g.members.some((m) => m.brightness)}
			{@const adaptive = gvalue(g.id, "adaptive", g.adaptive?.active ?? false)}
			{@const offline = g.members.filter((m) => !m.online).length}
			<Card.Root class="lamp lift" data-lit={on === true || undefined}
				{...lampAttrs(roomColour(g.members, (m) => findEp(m.ieee, m.ep)?.endpoint))}>
				<Card.Content class="flex flex-col gap-3">
					<div class="flex items-start gap-2">
						<div class="min-w-0 flex-1">
							<div class="flex flex-wrap items-center gap-2">
								<span class="truncate text-base font-semibold">{g.name}</span>
								{#if offline}<Badge variant="outline">{offline} offline</Badge>{/if}
							</div>
							<div class="text-muted-foreground mt-0.5 text-xs">
								{g.members.length ? g.members.map((m) => (m.online ? m.name : `${m.name} (offline)`)).join(", ") : "no lights yet — tap the pencil to pick some"}
							</div>
						</div>
						<Button variant="ghost" size="icon" aria-label="Edit {g.name}" onclick={() => navigate(`/groups/${g.id}`)}><Pencil /></Button>
					</div>
					<div class="flex items-center gap-3">
						<PowerSwitch {on} pending={pending.has(gkey(g.id, "on"))} label="Switch {g.name}" onchange={(v) => setGroupOnOff(g.id, v)} />
						{#if dimmable}
							<LevelSlider {pct} {on} label="Brightness of {g.name}" onsend={(v) => setGroupLevel(g, v)} class="flex-1" />
						{:else}
							<span class="text-muted-foreground text-sm">
								{stateLabel(on)} · {plural(g.members.length, "light")}
							</span>
						{/if}
						{#if g.adaptive && g.adaptive.expose !== "off"}
							<Button variant="ghost" size="icon" class={adaptive ? "text-primary bg-muted" : "text-muted-foreground"}
								aria-label="Adaptive Lighting for {g.name}" aria-pressed={adaptive}
								onclick={() => startAdaptive(g)}><SunMedium /></Button>
						{/if}
					</div>
				</Card.Content>
			</Card.Root>
		{/each}
	</div>
{/if}
