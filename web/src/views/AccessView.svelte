<script lang="ts">
	import * as Card from "$lib/components/ui/card";
	import { Button } from "$lib/components/ui/button";
	import { Input } from "$lib/components/ui/input";
	import { Label } from "$lib/components/ui/label";
	import { Badge } from "$lib/components/ui/badge";
	import ViewHeader from "$lib/components/ViewHeader.svelte";
	import { api, post, del } from "$lib/api.ts";
	import { toast } from "$lib/toast.svelte.ts";
	import { confirm } from "$lib/confirm.svelte.ts";
	import { auth, claim, closeFailsafe, showLock } from "$lib/auth.svelte.ts";
	import { usedAgo } from "$lib/format.ts";
	import type { AuthKey, AuthState, Session } from "$lib/types.ts";

	let keys = $state<AuthKey[]>([]);
	let max = $state(0);
	let a = $state<AuthState | null>(null);
	let error = $state("");
	let newName = $state("");
	let creating = $state(false);

	/* One request after the other, not Promise.all: the coordinator answers one
	 * at a time anyway, and the second would only queue. */
	async function load() {
		try {
			const list = await api<{ keys: AuthKey[]; max: number }>("/api/auth/keys");
			a = await api<AuthState>("/api/auth");
			keys = list.keys; max = list.max; error = "";
		} catch (err: any) { error = err.message; }
	}
	$effect(() => { load(); });

	const modeLine = $derived(!a ? "" : ({
		open: "Open — anyone on your network can use this coordinator, because it has no keys yet.",
		failsafe: `Failsafe open for another ${a.failsafe_left_s} s — anyone on your network can get in.`,
		setup: "Setup portal — no key is needed while the coordinator runs its own Wi-Fi.",
		locked: "Locked — every device needs a key.",
	})[a.mode] || "");

	async function revoke(k: AuthKey) {
		const msg = keys.length === 1
			? `Revoke “${k.name}”? It is the last key, so the coordinator will be open to everyone on your network.`
			: k.current ? `Revoke “${k.name}”? It is this browser's key, so this browser will be locked out.`
			: `Revoke “${k.name}”? Anything using it is locked out at once.`;
		if (!(await confirm(msg, { confirmLabel: "Revoke", destructive: true }))) return;
		try {
			const r = await del<{ mode: string }>(`/api/auth/keys/${k.id}`);
			toast(r.mode === "open" ? "No keys left — the coordinator is open to everyone" : "Key revoked");
			if (k.current && r.mode === "locked") { showLock(); return; }
			load();
		} catch (err: any) { toast(err.message); }
	}
	async function create() {
		creating = true;
		try {
			const r = await post<Session>("/api/auth/keys", { name: newName.trim() });
			newName = "";
			load();
			auth.keyShown = { session: r, kind: "api" };
		} catch (err: any) { toast(err.message); }
		finally { creating = false; }
	}
</script>

<ViewHeader title="Access" back="/settings" />
<div class="text-muted-foreground mt-1 text-sm">Who can use this page and the API.</div>

<Card.Root class="mt-4">
	<Card.Content class="flex flex-col gap-4">
		{#if error}
			<div class="text-destructive text-sm">{error}</div>
		{:else}
			<div class="text-muted-foreground text-sm">{modeLine}</div>
			{#if keys.length}
				<div class="divide-y">
					{#each keys as k (k.id)}
						<div class="flex items-center gap-3 py-2.5">
							<div class="min-w-0 flex-1">
								<div class="flex flex-wrap items-center gap-2 text-sm">
									<span class="font-medium">{k.name}</span>
									<Badge variant="outline">{k.kind === "api" ? "API" : "browser"}</Badge>
									{#if k.current}<Badge>this browser</Badge>{/if}
								</div>
								<div class="text-muted-foreground text-xs">{usedAgo(k.last_used_s_ago)}</div>
							</div>
							<Button variant="destructive" size="sm" onclick={() => revoke(k)}>Revoke</Button>
						</div>
					{/each}
				</div>
			{:else}
				<div class="text-muted-foreground text-sm">No keys yet.</div>
			{/if}
			{#if a && (a.mode === "open" || (a.mode === "setup" && !a.current))}
				<div><Button onclick={async () => { await claim(); load(); }}>Secure it — give this browser a key</Button></div>
			{/if}
			{#if a && a.mode === "failsafe"}
				<div><Button variant="outline" onclick={async () => { await closeFailsafe(); load(); }}>Close the failsafe now</Button></div>
			{/if}
			<div class="border-border border-t pt-4">
				<Label for="newkey" class="mb-2 block">New API key</Label>
				{#if a?.current}
					<form class="flex gap-2" onsubmit={(e) => { e.preventDefault(); create(); }}>
						<Input id="newkey" bind:value={newName} maxlength={31} autocomplete="off" placeholder="What it is for, e.g. Home Assistant" />
						<Button type="submit" variant="outline" disabled={creating || keys.length >= max}>Create</Button>
					</form>
				{:else}
					<div class="text-muted-foreground text-sm">Secure the coordinator first, so this browser keeps its own way in.</div>
				{/if}
				<div class="text-muted-foreground mt-2 text-xs">{keys.length} of {max} keys in use. Lost every key? Press one of the coordinator's buttons 5 times, about once a second.</div>
			</div>
		{/if}
	</Card.Content>
</Card.Root>
