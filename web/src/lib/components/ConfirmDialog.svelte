<script lang="ts">
	import * as Dialog from "$lib/components/ui/dialog";
	import { Button } from "$lib/components/ui/button";
	import { confirmState } from "$lib/confirm.svelte.ts";
	const c = $derived(confirmState.current);
</script>

<Dialog.Root open={!!c} onOpenChange={(o) => { if (!o) c?.resolve(false); }}>
	<Dialog.Content showCloseButton={false}>
		<Dialog.Header>
			<Dialog.Title>Are you sure?</Dialog.Title>
			<Dialog.Description>{c?.message}</Dialog.Description>
		</Dialog.Header>
		<Dialog.Footer class="flex-row justify-end gap-2">
			<Button variant="ghost" onclick={() => c?.resolve(false)}>Cancel</Button>
			<Button variant={c?.destructive ? "destructive" : "default"} onclick={() => c?.resolve(true)}>{c?.confirmLabel}</Button>
		</Dialog.Footer>
	</Dialog.Content>
</Dialog.Root>
