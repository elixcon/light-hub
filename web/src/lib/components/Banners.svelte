<script lang="ts">
	import { Button } from "$lib/components/ui/button";
	import { app, refresh } from "$lib/state.svelte.ts";
	import { auth, claim, closeFailsafe } from "$lib/auth.svelte.ts";
	import { api, post } from "$lib/api.ts";
	import { toast } from "$lib/toast.svelte.ts";
	import { mmss } from "$lib/format.ts";

	const a = $derived(auth.state);
	const w = $derived(app.status?.wifi);
	const pairLeft = $derived(app.status?.zigbee.permit_join_remaining_s ?? 0);
	const th = $derived(app.status?.thermal);

	async function leaveHotspot() {
		try { await post("/api/wifi/resume"); toast("Restarting onto your Wi-Fi — this hotspot is going away"); }
		catch (err: any) { toast(err.message); }
	}
	async function stopPair() {
		try { await post("/api/permit_join", { duration: 0 }); refresh(); }
		catch (err: any) { toast(err.message); }
	}
</script>

{#if a && !auth.locked && a.mode === "open"}
	<div class="banner">
		<span class="grow"><b>Anyone on your network can control this coordinator.</b> Secure it, and this browser gets a key.</span>
		<Button size="sm" onclick={claim}>Secure it</Button>
	</div>
{:else if a && !auth.locked && a.mode === "failsafe" && a.current}
	<div class="banner">
		<span class="grow"><b>Failsafe open</b> — anyone on your network can get in for {mmss(a.failsafe_left_s)}.</span>
		<Button size="sm" variant="ghost" onclick={async () => { await closeFailsafe(); refresh(); }}>Close now</Button>
	</div>
{/if}

{#if w && w.temporary_hotspot}
	<div class="banner">
		<span class="grow"><b>Setup hotspot, started from the button.</b> Pick a network under Settings → Wi-Fi, or the coordinator goes back to “{w.home_ssid}” in {mmss(w.returns_in_s ?? 0)}.</span>
		<Button size="sm" variant="ghost" onclick={leaveHotspot}>Back to “{w.home_ssid}” now</Button>
	</div>
{/if}

{#if th && (th.state === "hot" || th.state === "fault")}
	<div class="banner hot" role="alert">
		<span class="grow"><b>Wired lights held off</b> — {th.state === "fault"
			? "the overheat sensor reads as broken. Check it under Settings → Overheat protection."
			: `the board is at ${th.temp_c?.toFixed(1) ?? "?"} °C, shutoff ${th.trip_c} °C. They can be switched on again below ${th.release_c} °C.`}</span>
	</div>
{/if}

{#if pairLeft > 0}
	<div class="banner pairing">
		<span class="pulse" aria-hidden="true"></span>
		<span class="grow">Pairing open — put your light or remote into pairing mode now. <b>{pairLeft}s left</b></span>
		<Button size="sm" variant="ghost" onclick={stopPair}>Stop</Button>
	</div>
{/if}

<style>
	.banner {
		display: flex; flex-wrap: wrap; align-items: center; gap: .6rem .9rem;
		margin: .9rem 0 0; padding: .7rem .9rem; border-radius: var(--radius);
		background: color-mix(in srgb, var(--primary) 14%, var(--card));
		border: 1px solid color-mix(in srgb, var(--primary) 35%, transparent);
		font-size: .875rem;
	}
	.banner.hot {
		background: color-mix(in srgb, var(--destructive) 14%, var(--card));
		border-color: color-mix(in srgb, var(--destructive) 45%, transparent);
	}
	.grow { flex: 1 1 14rem; }
	.pulse { width: 10px; height: 10px; border-radius: 9999px; background: var(--primary);
		animation: pulse 1.2s ease-in-out infinite; }
	@keyframes pulse { 0%, 100% { transform: scale(.7); opacity: .5 } 50% { transform: scale(1); opacity: 1 } }
	@media (prefers-reduced-motion: reduce) { .pulse { animation: none } }
</style>
