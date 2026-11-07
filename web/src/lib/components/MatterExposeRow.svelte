<script lang="ts">
	/* "Show in Matter" for a Zigbee device: a lamp, a remote's buttons or a motion
	 * sensor, as an accessory of its own beside the rooms. Off by default, and
	 * taken at once: the bridge adds or removes the accessory within a second.
	 * `eps` is every endpoint it stands for - a remote's buttons can sit on more
	 * than one - and the switch is on while all of them are shown. */
	import { Label } from "$lib/components/ui/label";
	import { Switch } from "$lib/components/ui/switch";
	import { post } from "$lib/api.ts";
	import { refresh } from "$lib/state.svelte.ts";
	import { toast } from "$lib/toast.svelte.ts";

	let { ieee, eps, on, help }: { ieee: string; eps: number[]; on: boolean; help: string } = $props();

	/* What the switch shows from the click until the poll agrees. */
	let shown = $state<boolean | null>(null);
	$effect(() => { if (shown !== null && shown === on) shown = null; });
	const id = $derived(`matter-${ieee}`);

	async function save(want: boolean) {
		shown = want;
		try {
			for (const ep of eps) await post(`/api/devices/${ieee}/ep/${ep}/matter`, { expose: want });
			toast(want ? "Shown in Matter: your controllers get it in a few seconds" : "No longer shown in Matter");
		} catch (err: any) {
			shown = null;
			toast(err.message);
		}
		refresh();
	}
</script>

<div class="flex flex-col gap-2">
	<div class="flex items-center justify-between gap-4">
		<Label for={id}>Show in Matter on its own</Label>
		<Switch {id} checked={shown ?? on} disabled={!eps.length} aria-label="Show in Matter on its own" onCheckedChange={save} />
	</div>
	<div class="text-muted-foreground text-xs">{help}</div>
</div>
