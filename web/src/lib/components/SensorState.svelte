<script lang="ts">
	/* A sensor's state and what it has waiting, counted down between polls: the
	 * poll brings new numbers only when something changes. */
	import { sensorState, waitingText } from "$lib/sensors.ts";
	import { ago } from "$lib/format.ts";
	import type { ButtonDef } from "$lib/types.ts";
	let { button: b }: { button: ButtonDef | null } = $props();

	let now = $state(Date.now());
	let stamp = $state(Date.now());
	$effect(() => { void b?.clear_in_ms; void b?.delayed?.next_ms; void b?.last_event_ms; stamp = Date.now(); });
	$effect(() => { const t = setInterval(() => (now = Date.now()), 1000); return () => clearInterval(t); });
	const elapsed = $derived(Math.max(0, now - stamp));
	const waiting = $derived(waitingText(b, elapsed));
</script>

<div class="flex flex-col gap-0.5 text-xs">
	<div class="flex items-center gap-2">
		{#if b?.occupied}<span class="bg-on inline-block size-2 rounded-full"></span>{/if}
		<span>{sensorState(b, elapsed)}</span>
	</div>
	{#if b?.last_event_ms != null}<div class="text-muted-foreground">Last heard {ago(b.last_event_ms + elapsed)}</div>{/if}
	{#if waiting}<div class="text-muted-foreground">{waiting}</div>{/if}
</div>
