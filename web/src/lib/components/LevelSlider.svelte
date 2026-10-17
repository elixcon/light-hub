<script lang="ts">
	/* The one brightness control, inline on every card. Controlled: what it
	 * shows is the optimistic value while a drag is in flight and the reported
	 * one once the coordinator agrees, and every move goes out throttled. The
	 * fill is `--level-fill`: amber, or what a lit lamp's card sets. */
	import { Slider } from "$lib/components/ui/slider";
	import { schedule } from "$lib/state.svelte.ts";
	let { pct, on, disabled = false, label, onsend, class: className = "" }: {
		pct: number | null | undefined; on: boolean | null | undefined; disabled?: boolean;
		label: string; onsend: (pct: number) => void; class?: string;
	} = $props();
	const shown = $derived(pct === null || pct === undefined ? 1 : pct);
	const off = $derived(on !== true);
</script>

<div class="flex items-center gap-3 {className}">
	<Slider min={1} max={100} step={1} {disabled} aria-label={label}
		class="flex-1 {off ? 'opacity-60' : ''}" style="--range: {off ? 'var(--color-muted-foreground)' : 'var(--level-fill)'}"
		bind:value={() => shown, (v) => onsend(v)}
		onValueCommit={() => schedule(400)} />
	<span class="text-muted-foreground min-w-11 shrink-0 text-right text-sm whitespace-nowrap tabular-nums">
		{pct === null || pct === undefined ? "—" : `${pct} %`}
	</span>
</div>
