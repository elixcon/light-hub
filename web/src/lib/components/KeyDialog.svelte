<script lang="ts">
	/* The popup that explains the whole mechanism, shown whenever a key is
	 * issued - which is the one moment the key can be seen. */
	import * as Dialog from "$lib/components/ui/dialog";
	import { Button } from "$lib/components/ui/button";
	import Copy from "@lucide/svelte/icons/copy";
	import { auth, copyText } from "$lib/auth.svelte.ts";
	const k = $derived(auth.keyShown);
	const isApi = $derived(k?.kind === "api");
	let keyEl: HTMLElement | null = $state(null);
</script>

<Dialog.Root open={!!k} onOpenChange={(o) => { if (!o) auth.keyShown = null; }}>
	<Dialog.Content showCloseButton={false} class="max-h-[90vh] overflow-y-auto">
		{#if k}
			<Dialog.Header>
				<Dialog.Title>{isApi ? "API key created" : "This browser is signed in"}</Dialog.Title>
				<Dialog.Description>
					{isApi ? `For ${k.session.name}.` : `It has its own key now, “${k.session.name}”, and remembers it.`}
				</Dialog.Description>
			</Dialog.Header>
			<div bind:this={keyEl} class="bg-muted rounded-lg px-3 py-3 text-center font-mono text-lg tracking-wider select-all">{k.session.key}</div>
			<div class="flex justify-center">
				<Button variant="ghost" size="sm" onclick={() => copyText(k.session.key ?? "", keyEl)}><Copy /> Copy</Button>
			</div>
			<p class="text-sm"><b>Write this down.</b> It is shown only this once: the coordinator keeps a fingerprint of the key, not the key itself.</p>
			<ul class="text-muted-foreground list-disc space-y-1.5 pl-5 text-sm">
				{#if isApi}
					<li>Send it with every request:<br><code class="bg-muted rounded px-1 text-xs break-all">curl -H "Authorization: Bearer {k.session.key}" http://{location.host}/api/devices</code></li>
				{:else}
					<li>This browser stays signed in until its site data is cleared. The key you wrote down gets it back in.</li>
				{/if}
				<li>On any other device, type the key into the lock screen.</li>
				<li>No key to hand? Press one of the coordinator's buttons <b>5 times, about once a second</b> (the lights it switches will switch too). For two minutes anyone on your network can get in — until the first browser takes a key.</li>
				<li>On the wrong network altogether? Keep pressing, to <b>ten</b>: the coordinator restarts into its setup hotspot, and goes back to the stored Wi-Fi by itself if nobody picks a new one.</li>
				<li>Keys are listed and revoked under <b>Settings → Access</b>. Revoke the last one and the coordinator is open to everyone again.</li>
				<li>The Wi-Fi setup portal never asks for a key.</li>
			</ul>
			<Dialog.Footer>
				<Button onclick={() => (auth.keyShown = null)}>I have written it down</Button>
			</Dialog.Footer>
		{/if}
	</Dialog.Content>
</Dialog.Root>
