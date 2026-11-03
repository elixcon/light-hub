<script lang="ts">
	import * as Card from "$lib/components/ui/card";
	import ChevronRight from "@lucide/svelte/icons/chevron-right";
	import CreateByName from "$lib/components/CreateByName.svelte";
	import PowerSwitch from "$lib/components/PowerSwitch.svelte";
	import { app, data, pending, key, value, setOnOff } from "$lib/state.svelte.ts";

	const MAX_LIGHTS = 3;   /* CONFIG_ZBC_MAX_LIGHTS in node/sdkconfig.defaults */
	const paired = $derived(app.status?.node?.paired ?? false);
</script>

{#if !paired}
	<Card.Root class="mt-4"><Card.Content class="text-sm">
		<b>Not paired yet.</b> Open your hub, press <b>Add a light</b> and then <b>Pair a light</b>: this node joins by itself within half a minute. It joins any Zigbee network that is open for pairing, so open only yours. Wi-Fi closes two minutes after it has joined and nobody is using this page.
	</Card.Content></Card.Root>
{/if}

<CreateByName title="Lights" addLabel="Add a light" placeholder="Name this light" path="/api/lights" route="/lights"
	to={(l) => `/lights/${l.ieee}/${l.ep}`} />
{#if !data.devices.length}
	<p class="text-muted-foreground mt-3 text-sm">Nothing wired yet. Each light you add here appears on the hub as an ordinary lamp, after this node has restarted onto Zigbee.</p>
{:else}
	<div class="mt-3 grid gap-3">
		{#each data.devices as d (d.ieee)}
			{@const e = d.endpoints[0]}
			<Card.Root><Card.Content class="flex items-center justify-between gap-3">
				<a class="min-w-0 flex-1" href="#/lights/{d.ieee}/{e.ep}">
					<div class="truncate font-medium">{d.name}</div>
					<div class="text-muted-foreground text-xs">{d.gpio == null ? "no pin yet" : `GPIO ${d.gpio}${d.gpio_warm != null ? ` + ${d.gpio_warm}` : ""}`}</div>
				</a>
				<PowerSwitch on={value(d.ieee, e.ep, "on", e.on)} pending={pending.has(key(d.ieee, e.ep, "on"))} label="Power"
					onchange={(v) => setOnOff(d.ieee, e.ep, v)} />
			</Card.Content></Card.Root>
		{/each}
	</div>
	{#if data.devices.length >= MAX_LIGHTS}
		<p class="text-muted-foreground mt-2 text-xs">A node has room for {MAX_LIGHTS} lights: the hub keeps four endpoints per device, and buttons take one.</p>
	{/if}
{/if}

<div class="mt-6"></div>
<CreateByName title="Buttons" addLabel="Add a button" placeholder="Name this button" path="/api/buttons" route="/remotes" />
{#if !data.buttons.length}
	<p class="text-muted-foreground mt-3 text-sm">A button wired here is set up on the hub, under Remotes, like any remote's key.</p>
{:else}
	<div class="mt-3 grid gap-3">
		{#each data.buttons as b (b.id)}
			<a href="#/remotes/{b.id}"><Card.Root><Card.Content class="flex items-center justify-between gap-3">
				<div class="min-w-0">
					<div class="truncate font-medium">{b.name}</div>
					<div class="text-muted-foreground text-xs">{b.gpio == null ? "no pin yet" : `GPIO ${b.gpio}${b.pressed ? " · pressed" : ""}`}</div>
				</div>
				<ChevronRight class="text-muted-foreground size-4 shrink-0" />
			</Card.Content></Card.Root></a>
		{/each}
	</div>
{/if}
