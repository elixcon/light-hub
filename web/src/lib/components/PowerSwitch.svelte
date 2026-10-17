<script lang="ts">
	/* A switch that can also say "unknown": null is not off, and the UI must
	 * not assert something the coordinator has not seen. */
	import { Switch } from "$lib/components/ui/switch";
	let { on, pending = false, label, onchange }: {
		on: boolean | null | undefined; pending?: boolean; label: string; onchange: (next: boolean) => void;
	} = $props();
</script>

<Switch checked={on === true} aria-label={label} data-unknown={on === null || on === undefined || undefined}
	class="{pending ? 'opacity-60' : ''} data-[unknown]:border-dashed data-[unknown]:border-muted-foreground/60"
	onCheckedChange={(v) => onchange(v)} />
