<script lang="ts">
	/* A full page, not a dialog: there is nothing behind it to come back to. */
	import { Button } from "$lib/components/ui/button";
	import { Input } from "$lib/components/ui/input";
	import { Label } from "$lib/components/ui/label";
	import { auth, signInWithKey } from "$lib/auth.svelte.ts";
	let key = $state("");
	$effect(() => { if (!auth.locked) key = ""; });
</script>

<div class="bg-background fixed inset-0 z-50 flex items-center justify-center p-4">
	<div class="surface w-full max-w-sm rounded-xl p-6 shadow-lg ring-1 ring-edge">
		<h2 class="text-lg font-semibold">This coordinator is locked</h2>
		<p class="text-muted-foreground mt-2 text-sm">Press one of its buttons <b>5 times, about once a second</b> — the BOOT button on the board, a wired switch or a paired remote. This page unlocks by itself.</p>
		<Label for="lockKey" class="mt-5 mb-2 block">Or enter a key</Label>
		<Input id="lockKey" bind:value={key} autocomplete="off" autocapitalize="characters" spellcheck={false}
			placeholder="XXXXX-XXXXX-XXXXX-XXXXX" class="font-mono tracking-wider"
			onkeydown={(e) => { if (e.key === "Enter") signInWithKey(key); }} />
		{#if auth.message}<div class="text-destructive mt-2 text-sm">{auth.message}</div>{/if}
		<div class="mt-4 flex justify-end">
			<Button disabled={auth.signingIn} onclick={() => signInWithKey(key)}>Sign in</Button>
		</div>
	</div>
</div>
