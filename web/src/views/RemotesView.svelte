<script lang="ts">
	/* One card per remote, listing its buttons - the firmware's one-record-per-key
	 * table put back together (see remotes.ts) - and the buttons wired to a pin in
	 * a section of their own below. A remote's setup, firmware and removal are on
	 * its own page (RemoteView). */
	import * as Card from "$lib/components/ui/card";
	import { Button } from "$lib/components/ui/button";
	import Pencil from "@lucide/svelte/icons/pencil";
	import Plus from "@lucide/svelte/icons/plus";
	import CreateByName from "$lib/components/CreateByName.svelte";
	import ButtonRow from "$lib/components/ButtonRow.svelte";
	import { app, data, openPairing } from "$lib/state.svelte.ts";
	import { navigate } from "$lib/router.svelte.ts";
	import { remoteCards, remoteHeard, remoteKind, remotePath, remoteTitle, strayParent } from "$lib/remotes.ts";
	import { ago } from "$lib/format.ts";
	import type { ButtonDef } from "$lib/types.ts";

	const cards = $derived(remoteCards(data.devices, data.buttons));
	const wired = $derived(data.buttons.filter((b) => b.source !== "zigbee"));
	const unlinked = $derived(data.buttons.filter((b) => b.source === "zigbee" && !b.ieee));

	const wiring = (b: ButtonDef) => b.gpio === null || b.gpio === undefined ? "no pin yet"
		: `GPIO ${b.gpio} · ${b.pull === "none" ? "no pull" : "pull-" + b.pull} · active ${b.inverted ? "low" : "high"}`;
</script>

<div class="mt-5 flex items-center justify-between gap-3">
	<h2 class="text-lg font-semibold">Remotes</h2>
	<Button variant="outline" size="sm" disabled={app.pairing} onclick={openPairing}><Plus /> Pair a remote</Button>
</div>

{#if !cards.length}
	<div class="text-muted-foreground mt-6 text-center text-sm">
		<p class="text-foreground text-base font-medium">No remotes yet</p>
		<p class="mt-2">Press <b>Pair a remote</b> and put the remote into pairing mode. It appears here, and a remote this coordinator knows gets its buttons filled in for you.</p>
	</div>
{:else}
	<div class="mt-4 grid gap-3 md:grid-cols-2">
		{#each cards as c (c.ieee)}
			{@const h = remoteHeard(c)}
			{@const stray = strayParent(c, data.devices)}
			<Card.Root class="lift">
				<Card.Content class="flex flex-col gap-3">
					<div class="flex items-start gap-2">
						<div class="min-w-0 flex-1">
							<div class="flex flex-wrap items-center gap-2">
								<span class="truncate text-base font-semibold">{remoteTitle(c)}</span>
								{#if h !== null}<span class="text-muted-foreground text-xs">heard {ago(h)}</span>{/if}
							</div>
							<div class="text-muted-foreground mt-0.5 truncate text-xs">
								{remoteKind(c)}
							</div>
							{#if stray}
								<div class="text-destructive mt-0.5 text-xs">Hangs off {stray}: presses can get lost</div>
							{/if}
						</div>
						<Button variant="ghost" size="icon" aria-label="Settings for {remoteTitle(c)}" onclick={() => navigate(remotePath(c.ieee))}><Pencil /></Button>
					</div>
					{#if c.buttons.length}
						<div class="divide-border flex flex-col divide-y">
							{#each c.buttons as b (b.id)}<ButtonRow button={b} known={c.known} editable={false} showEp={c.spans} />{/each}
						</div>
					{:else}
						<p class="text-muted-foreground text-sm">Not set up yet — tap the pencil to set up its buttons.</p>
					{/if}
				</Card.Content>
			</Card.Root>
		{/each}
	</div>
{/if}

{#if unlinked.length}
	<h3 class="mt-8 text-base font-semibold">Not linked to a remote yet</h3>
	<Card.Root class="mt-3">
		<Card.Content class="divide-border flex flex-col divide-y">
			{#each unlinked as b (b.id)}<ButtonRow button={b} />{/each}
		</Card.Content>
	</Card.Root>
{/if}

<CreateByName title="Wired buttons" addLabel="Add a wired button" placeholder="Name this button" path="/api/buttons" route="/remotes" />
{#if !wired.length}
	<p class="text-muted-foreground mt-2 text-sm">A push button between a GPIO pin and GND works the same way as a remote button. The board's own BOOT button is on GPIO 9, with nothing to solder.</p>
{:else}
	<div class="mt-3 grid gap-3 md:grid-cols-2">
		{#each wired as b (b.id)}
			<Card.Root class="glow lift" data-lit={b.pressed || undefined}>
				<Card.Content class="flex flex-col gap-1">
					<ButtonRow button={b} />
					<div class="text-muted-foreground flex items-center gap-1.5 text-xs">
						{#if b.pressed !== null && b.pressed !== undefined}
							<span class="inline-block size-2 rounded-full {b.pressed ? 'bg-on' : 'bg-muted-foreground/40'}"></span>
						{/if}
						{wiring(b)}
					</div>
				</Card.Content>
			</Card.Root>
		{/each}
	</div>
{/if}
