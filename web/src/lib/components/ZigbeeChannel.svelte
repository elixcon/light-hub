<script lang="ts">
	/* The Zigbee channel: which one the network is on, moving it, and a survey
	 * to choose by.
	 *
	 * A move keeps every pairing: the coordinator tells the lamps, waits for the
	 * word to spread, then follows (zb_move_channel). The page only asks and
	 * shows the coordinator's own account of it from the status.
	 *
	 * The survey is two scans from the coordinator: Wi-Fi (/api/wifi/scan), which
	 * says what other networks there are, and its Zigbee radio
	 * (/api/zigbee/scan), which says what is actually on each channel. The
	 * ranking is channels.ts's; this draws it. The Zigbee half takes the antenna
	 * from Wi-Fi a second at a time, so polls may come back slowly or not at all
	 * meanwhile - they are retried, not reported. */
	import { Button } from "$lib/components/ui/button";
	import { Label } from "$lib/components/ui/label";
	import { app } from "$lib/state.svelte.ts";
	import { api, post } from "$lib/api.ts";
	import { toast } from "$lib/toast.svelte.ts";
	import { confirm } from "$lib/confirm.svelte.ts";
	import { rankChannels, verdict, whyNotFree, zigbeeMhz, overlaps, PREFERRED, ZIGBEE_CHANNELS,
		type WifiAp, type ChannelRating } from "$lib/channels.ts";
	import type { ChannelSurveyAnswer } from "$lib/types.ts";

	const s = $derived(app.status);
	const current = $derived(s?.zigbee.network_up ? s.zigbee.channel : null);
	const move = $derived(s?.zigbee.channel_move);
	const moving = $derived(move?.state === "announced" || move?.state === "moving");
	const station = $derived(s?.wifi.mode === "sta" && s.wifi.connected ? s.wifi.channel : null);

	function optionLabel(n: number): string {
		let t = `${n} · ${zigbeeMhz(n)} MHz`;
		if (PREFERRED.includes(n)) t += " · preferred";
		if (station && overlaps(n, { channel: station })) t += ` · overlaps your Wi-Fi (${station})`;
		return t;
	}

	async function moveTo(n: number, select?: HTMLSelectElement) {
		if (current === null || n === current) return;
		const ok = await confirm(`Move the Zigbee network from channel ${current} to ${n}? Lamps follow within about ten seconds. Remotes find the network again on their next press; one that does not — Aqara remotes often — has to be paired again. A lamp without power now stays on channel ${current} and has to be paired again too.`,
			{ confirmLabel: `Move to ${n}`, destructive: true });
		if (!ok) { if (select) select.value = String(current); return; }
		try { await post("/api/settings", { zigbee_channel: n }); toast(`Moving the network to channel ${n}`); }
		catch (err: any) { toast(err.message); if (select) select.value = String(current); }
	}

	const moveLine = $derived.by(() => {
		if (!move || move.state === "idle" || move.to === undefined) return "";
		if (move.state === "announced") return `Telling the lamps to move to channel ${move.to}…`;
		if (move.state === "moving") return `Following them to channel ${move.to}…`;
		if (move.state === "failed") return `The coordinator did not follow the lamps to channel ${move.to}. A restart puts it there.`;
		if (move.boot || (move.s_ago ?? 0) > 600) return "";
		return `Moved from channel ${move.from} to ${move.to}. Remotes find the network again on their next press.`;
	});

	/* The survey. */
	let phase = $state<"" | "wifi" | "zigbee">("");
	let ranked = $state<ChannelRating[] | null>(null);
	let measured = $state(false);
	let note = $state("");
	let picked = $state<number | null>(null);
	const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

	async function survey() {
		phase = "wifi"; note = ""; picked = null;
		const notes: string[] = [];
		let aps: WifiAp[] = [];
		try { aps = (await api<{ networks: WifiAp[] }>("/api/wifi/scan")).networks; }
		catch { notes.push("the Wi-Fi scan failed, so other networks are not counted"); }
		phase = "zigbee";
		let sv: ChannelSurveyAnswer | null = null;
		try {
			await post("/api/zigbee/scan");
			for (const until = Date.now() + 60000; Date.now() < until;) {
				await sleep(2000);
				try { sv = await api<ChannelSurveyAnswer>("/api/zigbee/scan"); } catch { continue; }
				if (sv.state !== "running") break;
			}
			if (sv?.state === "failed") notes.push(`the Zigbee radio could not measure: ${sv.error}`);
			else if (sv?.state !== "done") notes.push("the Zigbee measurement did not finish");
		} catch (err: any) { notes.push(`no Zigbee measurement: ${err.message}`); }
		const done = sv?.state === "done" ? sv : null;
		measured = !!done;
		ranked = rankChannels({
			aps, ownSsid: s?.wifi.mode === "sta" ? s.wifi.ssid : null, ownChannel: station,
			energy: done?.channels ?? null, networks: done?.networks ?? null, current,
		});
		note = notes.length ? `Only partly measured: ${notes.join("; ")}.` : "";
		phase = "";
	}

	const byChannel = $derived(new Map((ranked ?? []).map((r) => [r.channel, r])));
	const best = $derived.by(() => {
		if (!ranked) return [] as number[];
		const free = ranked.filter((r) => r.free).slice(0, 3).map((r) => r.channel);
		return free.length ? free : [ranked[0].channel];
	});
	/* The chart's one scale, in dBm: what it shows is the measurement where there
	 * is one, the Wi-Fi expected otherwise. The floor sits just under the radio's
	 * own noise (about -92 dBm), the top where a network right on the channel lands. */
	const FLOOR = -96, TOP = -50;
	const shown = (r: ChannelRating | undefined) => (r ? (measured ? r.medianDbm : r.predictedDbm) : null);
	const height = (r: ChannelRating | undefined) => {
		const v = shown(r);
		return v === null ? 2 : Math.max(2, Math.min(100, ((v - FLOOR) / (TOP - FLOOR)) * 100));
	};
	const dbm = (v: number | null) => (v === null ? "—" : `${Math.round(v)} dBm`);
	const pick = $derived(picked === null ? null : byChannel.get(picked) ?? null);
	function details(r: ChannelRating): string[] {
		const out: string[] = [];
		if (measured) out.push(`Measured ${dbm(r.medianDbm)} typically, ${dbm(r.maxDbm)} at its busiest.`);
		if (r.ownWifi.length) out.push(`Inside your own Wi-Fi (channel ${r.ownWifi.join(" and ")}): the coordinator's two radios would fight over its one antenna.`);
		const aps = r.aps.filter((a) => a.dbm >= -100).slice(0, 3);
		if (aps.length) out.push(`Other Wi-Fi reaching it: ${aps.map((a) => `“${a.ssid || "hidden"}” on ${a.channel} (${a.dbm} dBm here)`).join(", ")}.`);
		if (r.networks) out.push(`${r.networks} other Zigbee network${r.networks > 1 ? "s" : ""} on it.`);
		out.push(r.free ? "Free." : `Not free: ${whyNotFree(r, ranked ?? [])}.`);
		return out;
	}
</script>

<div class="flex flex-col gap-2">
	<Label for="zbch">Channel</Label>
	<select id="zbch" class="sel" value={current ?? ""} disabled={current === null || moving || phase !== ""}
		onchange={(e) => moveTo(Number(e.currentTarget.value), e.currentTarget)}>
		{#if current === null}<option value="">Zigbee is not running</option>{/if}
		{#each ZIGBEE_CHANNELS as n (n)}<option value={n}>{optionLabel(n)}</option>{/each}
	</select>
	{#if moveLine}<div class="text-sm" class:text-destructive={move?.state === "failed"} aria-live="polite">{moveLine}</div>{/if}
	<div class="text-muted-foreground text-xs">Keep it clear of your Wi-Fi: the coordinator shares one antenna between the two. 11, 15, 20 and 25 are the preferred channels — every device works on any channel, but some find a network faster on these. Changing it moves the network and keeps every pairing.</div>
</div>

<div class="flex flex-col gap-3">
	<div class="flex flex-wrap items-center gap-3">
		<Button variant="outline" onclick={survey} disabled={phase !== "" || current === null || moving}>
			{phase === "wifi" ? "Listening for Wi-Fi…" : phase === "zigbee" ? "Measuring the channels…" : "Find the best channel"}
		</Button>
		<span class="text-muted-foreground text-xs">About 15 seconds. Remotes are not heard while the radio measures, and Wi-Fi is slow for a second at a time.</span>
	</div>

	{#if ranked}
		<p class="text-sm">{verdict(ranked, current)}</p>
		{#if note}<p class="text-muted-foreground text-xs">{note}</p>{/if}
		{#if best.some((n) => n !== current)}
			<div class="flex flex-wrap gap-2">
				{#each best.filter((n) => n !== current) as n (n)}
					<Button size="sm" onclick={() => moveTo(n)} disabled={moving}>Move to {n}</Button>
				{/each}
			</div>
		{/if}

		<figure class="m-0">
			<figcaption class="text-muted-foreground mb-1 text-xs">{measured ? "Energy on each channel as the coordinator measured it" : "Wi-Fi expected on each channel"}, in dBm — taller is busier. The best are in colour.</figcaption>
			<div class="plot">
				<span class="tick top">{TOP}</span>
				<span class="tick bottom">{FLOOR}</span>
				{#each ZIGBEE_CHANNELS as n (n)}
					{@const r = byChannel.get(n)}
					<button type="button" class="col" class:on={picked === n}
						aria-label={`Channel ${n}: ${r ? (r.free ? "free" : "not free") : ""}${shown(r) !== null ? `, ${dbm(shown(r))}` : ""}`}
						onclick={() => (picked = n)} onpointerenter={() => (picked = n)} onfocus={() => (picked = n)}>
						<span class="bar" class:best={best.includes(n)} style:height="{height(r)}%"></span>
					</button>
				{/each}
			</div>
			<div class="axis">
				{#each ZIGBEE_CHANNELS as n (n)}
					{@const r = byChannel.get(n)}
					<div class="lab">
						<span class:font-semibold={PREFERRED.includes(n)} class:text-foreground={PREFERRED.includes(n)}>{n}</span>
						{#if n === current}<span aria-label="in use">▲</span>{:else if r?.ownWifi.length}<span aria-label="your Wi-Fi">W</span>{/if}
					</div>
				{/each}
			</div>
			<div class="text-muted-foreground mt-1 text-xs">Bold: the preferred channels. ▲ the one in use. W inside your own Wi-Fi.</div>
		</figure>

		<div class="min-h-[4.5rem] text-sm" aria-live="polite">
			{#if pick}
				<div class="font-medium">Channel {pick.channel} · {pick.mhz} MHz{pick.preferred ? " · preferred" : ""}{pick.channel === current ? " · in use" : ""}</div>
				{#each details(pick) as line (line)}<div class="text-muted-foreground">{line}</div>{/each}
				{#if pick.channel !== current}<Button size="sm" variant="outline" class="mt-2" disabled={moving} onclick={() => moveTo(pick.channel)}>Move to {pick.channel}</Button>{/if}
			{:else}
				<div class="text-muted-foreground">Point at a channel, or tab to it, for what was found there.</div>
			{/if}
		</div>

		<details class="text-sm">
			<summary class="text-muted-foreground cursor-pointer text-xs">All channels as a table</summary>
			<table class="mt-2 w-full text-xs">
				<thead><tr class="text-muted-foreground text-left"><th>Ch</th><th>MHz</th>{#if measured}<th>Typical</th><th>Busiest</th>{/if}<th>Wi-Fi</th><th>Zigbee</th><th></th></tr></thead>
				<tbody>
					{#each ZIGBEE_CHANNELS as n (n)}
						{@const r = byChannel.get(n)}
						{#if r}
							<tr>
								<td class:font-semibold={r.preferred}>{n}</td><td>{r.mhz}</td>
								{#if measured}<td>{dbm(r.medianDbm)}</td><td>{dbm(r.maxDbm)}</td>{/if}
								<td>{r.ownWifi.length ? "yours" : r.predictedDbm === null ? "—" : dbm(r.predictedDbm)}</td>
								<td>{r.networks || "—"}</td><td>{r.free ? "free" : ""}</td>
							</tr>
						{/if}
					{/each}
				</tbody>
			</table>
		</details>
	{/if}
</div>

<style>
	.sel { height: 2.5rem; border-radius: var(--radius-md); border: 1px solid var(--input); background: var(--card);
		color: var(--foreground); padding: 0 .6rem; font-size: .875rem; }
	/* Sixteen columns on one baseline. The gray is the theme's own muted ink
	 * mixed into the card, so dark mode gets its own step rather than a flip. */
	.plot { position: relative; display: flex; align-items: flex-end; gap: 2px; height: 8rem;
		border-bottom: 1px solid var(--border); padding-left: 2.2rem; }
	.plot::before { content: ""; position: absolute; left: 2.2rem; right: 0; top: 0; border-top: 1px solid var(--border); }
	.tick { position: absolute; left: 0; font-size: .7rem; color: var(--muted-foreground); font-variant-numeric: tabular-nums; }
	.tick.top { top: -.45rem; }
	.tick.bottom { bottom: -.1rem; }
	.col { flex: 1; height: 100%; display: flex; align-items: flex-end; justify-content: center; padding: 0;
		background: none; border: 0; border-radius: 4px 4px 0 0; cursor: pointer; }
	.col.on, .col:hover { background: var(--muted); }
	.col:focus-visible { outline: 2px solid var(--ring); outline-offset: 1px; }
	.bar { width: 100%; max-width: 24px; border-radius: 4px 4px 0 0;
		background: color-mix(in srgb, var(--muted-foreground) 55%, var(--card)); }
	.bar.best { background: var(--primary); }
	.axis { display: flex; gap: 2px; padding-left: 2.2rem; margin-top: .25rem; }
	.lab { flex: 1; display: flex; flex-direction: column; align-items: center; font-size: .7rem; line-height: 1.15;
		color: var(--muted-foreground); font-variant-numeric: tabular-nums; min-width: 0; }
	.lab span:nth-child(2) { font-size: .65rem; }
	td, th { padding: .15rem .4rem .15rem 0; font-variant-numeric: tabular-nums; }
</style>
