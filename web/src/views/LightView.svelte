<script lang="ts">
	/* Everything about one lamp on one page, with its response - the reason
	 * this device exists - at the top. The test slider under the editor drives
	 * the lamp and marks where on the curve and the path it is. */
	import * as Card from "$lib/components/ui/card";
	import { Button } from "$lib/components/ui/button";
	import { Input } from "$lib/components/ui/input";
	import { Label } from "$lib/components/ui/label";
	import Trash from "@lucide/svelte/icons/trash-2";
	import ViewHeader from "$lib/components/ViewHeader.svelte";
	import PowerSwitch from "$lib/components/PowerSwitch.svelte";
	import LevelSlider from "$lib/components/LevelSlider.svelte";
	import CurveChart from "$lib/components/CurveChart.svelte";
	import ChromaPath from "$lib/components/ChromaPath.svelte";
	import SwitchPointEditor from "$lib/components/SwitchPointEditor.svelte";
	import MixCurves from "$lib/components/MixCurves.svelte";
	import FirmwareRow from "$lib/components/FirmwareRow.svelte";
	import { data, pending, key, value, epPct, isRemoteEp, findEp, findGroup, refresh, schedule, seenIeee,
		setOnOff, setLevel, sendCommand, getOffEpoch } from "$lib/state.svelte.ts";
	import { api, post, patch, del } from "$lib/api.ts";
	import { toast } from "$lib/toast.svelte.ts";
	import { confirm } from "$lib/confirm.svelte.ts";
	import { navigate } from "$lib/router.svelte.ts";
	import { agoS, hex4 } from "$lib/format.ts";
	/* The neighbour scan's lqa and, from the frames the device itself sent, the
	 * receiving side: RSSI and how many of its frames never arrived. */
	const linkText = (d: Device) => {
		const parts: string[] = [];
		if (d.lqa !== null) parts.push(`lqa ${d.lqa}/255`);
		if (d.rssi != null) parts.push(`${d.rssi} dBm`);
		if (d.frames_heard) parts.push(`${d.frames_missed ?? 0} of ${(d.frames_heard ?? 0) + (d.frames_missed ?? 0)} frames lost`);
		if (d.child) parts.push("child of the coordinator");
		return parts.length ? parts.join(" · ") : "not measured yet";
	};
	import { PINS, PIN_NOTE } from "$lib/buttons.ts";
	import type { Device, LightKind, LightMix, PowerOn } from "$lib/types.ts";
	import { curveAt, PRESETS, chromaPresetsFor, clonePoints, clonePath, defaultLevel, defaultPath,
		switchPoints, switchPointOf, DEFAULT_MIN_K, DEFAULT_MAX_K, type Point, type Stop } from "$lib/curves.ts";
	import { pathAt, colourOf, rgbCss, isKelvinStop } from "$lib/colorspace.ts";
	import { channelsOf, curvesFromJson, curvesFromMix, curvesToJson, type MixCurves as Curves,
		type MixCurvesJson } from "$lib/mix.ts";

	let { ieee, ep }: { ieee: string; ep: number } = $props();

	const found = $derived(findEp(ieee, ep));
	const d = $derived(found?.device);
	const e = $derived(found?.endpoint);
	const f = $derived(e?.features ?? []);
	const remote = $derived(!!e && isRemoteEp(e));
	const dims = $derived(f.includes("brightness"));
	const canCt = $derived(f.includes("color_temp"));
	const canColor = $derived(f.includes("color"));
	const switchOnly = $derived(!remote && !dims && f.includes("onoff"));
	const minK = $derived(e?.min_kelvin || DEFAULT_MIN_K);
	const maxK = $derived(e?.max_kelvin || DEFAULT_MAX_K);

	const on = $derived(e ? value(ieee, ep, "on", e.on) : null);
	/* The "Try it" slider of a light that can only switch sends nothing, so its
	 * value stays here rather than in `pending`, which only a command clears. */
	let tryPct = $state<number | null>(null);
	const pct = $derived(tryPct ?? (e ? value(ieee, ep, "pct", epPct(e)) : null));

	/* ---- the curves ---- */
	const c = $state({
		loaded: "", level: null as Point[] | null, path: null as Stop[] | null,
		peers: [] as { name: string; points: Point[] }[], sel: 0, tab: "level" as "level" | "chroma",
	});
	const tabs = $derived.by(() => {
		const t: ["level" | "chroma", string][] = [];
		if (dims) t.push(["level", "Brightness"]);
		if (canCt || canColor) t.push(["chroma", canCt && canColor ? "Temperature / Colour" : canCt ? "Warmth" : "Colour"]);
		return t;
	});
	$effect(() => { if (tabs.length && !tabs.some(([id]) => id === c.tab)) c.tab = tabs[0][0]; });

	async function load() {
		const id = `${ieee}/${ep}`;
		if (c.loaded === id || !e) return;
		c.loaded = id;
		try {
			const cs = await api(`/api/devices/${ieee}/ep/${ep}/curves`);
			c.level = cs.level ? cs.level.points : null;
			c.path = cs.chroma ? cs.chroma.stops : null;
		} catch (err: any) { toast(err.message); return; }
		/* The other lamps in this light's groups, so their ramps can be drawn behind
		 * the one being edited - a room is designed by how the curves overlap. */
		const peers: { ieee: string; ep: number; name: string }[] = [];
		for (const g of data.groups) {
			if (!g.members.some((m) => m.ieee === ieee && m.ep === ep)) continue;
			for (const m of g.members) {
				if ((m.ieee === ieee && m.ep === ep) || peers.some((p) => p.ieee === m.ieee && p.ep === m.ep)) continue;
				peers.push(m);
			}
		}
		const pc: { name: string; points: Point[] }[] = [];
		for (const m of peers.slice(0, 6)) {
			try {
				const r = await api(`/api/devices/${m.ieee}/ep/${m.ep}/curves`);
				pc.push({ name: m.name, points: r.level ? r.level.points : [[1, 1], [100, 100]] });
			} catch { /* a peer we cannot read simply is not drawn */ }
		}
		c.peers = pc;
	}
	$effect(() => { load(); });

	const ensureLevel = () => (c.level ??= defaultLevel());
	const ensurePath = () => (c.path ??= defaultPath(canCt, minK, maxK));
	const shownLevel = $derived(c.level ?? defaultLevel());
	const shownPath = $derived(c.path ?? defaultPath(canCt, minK, maxK));

	/* Saves and then re-applies the current brightness, so the lamp follows the
	 * curve as it is edited. There is no Cancel: the change goes to the light and
	 * the light is the record of it. */
	let saveTimer: ReturnType<typeof setTimeout> | undefined;
	function saveCurves() {
		const body = {
			level: c.level ? { points: $state.snapshot(c.level) } : null,
			chroma: c.path ? { stops: $state.snapshot(c.path) } : null,
		};
		clearTimeout(saveTimer);
		const epoch = getOffEpoch();
		saveTimer = setTimeout(async () => {
			try {
				await post(`/api/devices/${ieee}/ep/${ep}/curves`, body);
				const now = findEp(ieee, ep);
				const at = now && now.endpoint.input_pct;
				/* Only while the light is on, or editing a curve would switch it
				 * back on; offEpoch catches a group being switched off, which sets
				 * no per-endpoint state for the check to notice. */
				const lit = now && value(ieee, ep, "on", now.endpoint.on) !== false;
				if (at && lit && getOffEpoch() === epoch) {
					await sendCommand(`/api/devices/${ieee}/ep/${ep}/level`, { pct: at });
				}
				schedule(600);
			} catch (err: any) { toast(err.message); }
		}, 400);
	}

	const levelPresets = Object.keys(PRESETS.level);
	const chromaPresets = $derived(chromaPresetsFor(canCt, canColor, minK, maxK));
	function applyPreset(name: string) {
		if (c.tab === "level") c.level = clonePoints(PRESETS.level[name]);
		else { c.path = clonePath(chromaPresets[name]); c.sel = 0; }
		saveCurves();
	}
	/* The path drawn while the lamp has none, stored as it is. */
	function usePath() {
		ensurePath();
		saveCurves();
	}
	function removeCurve() {
		if (c.tab === "level" || switchOnly) c.level = null; else { c.path = null; c.sel = 0; }
		saveCurves();
	}
	const hasCurrent = $derived(switchOnly ? !!c.level : c.tab === "level" ? !!c.level : !!c.path);

	/* The test slider's readout: what the lamp is told at this room percentage. */
	const lampPct = $derived(pct == null ? null : c.level ? Math.max(0, Math.min(100, Math.round(curveAt(c.level, pct)))) : pct);
	const lampColour = $derived(pct == null || !c.path ? null : pathAt(c.path, pct));
	const lampColourText = $derived.by(() => {
		const v = lampColour;
		if (!v) return "";
		if (isKelvinStop(v)) return `${v.kelvin} K`;
		if ("hue" in v) return `hue ${v.hue}° · sat ${v.sat} %`;
		return "blended";
	});

	/* ---- name and removal ---- */
	let name = $state("");
	let named = $state("");
	$effect(() => { if (d && d.name !== named) { named = d.name; name = d.name; } });
	async function rename() {
		const n = name.trim();
		if (!d || !n || n === d.name) { name = d?.name ?? ""; return; }
		try { await patch(`/api/devices/${ieee}`, { name: n }); refresh(); }
		catch (err: any) { toast(err.message); name = d.name; }
	}
	/* ---- wiring, for a light on a pin ---- */
	const wired = $derived(d?.source === "gpio");
	/* The failsafe pin is refused by the coordinator: it is always read as an
	 * input, and driving it would lock people out of a locked box. */
	const LIGHT_PINS = PINS.filter((p) => p !== 9);
	const POWER_ON: [PowerOn, string][] = [["off", "Off"], ["on", "On"], ["last", "As it was"]];
	/* ESPHome's recommended frequencies: each the fastest at its resolution
	 * (80 MHz / 2^bits), so anything just below one dims no finer. */
	const HZ_PICKS: [number, number][] = [[152, 19], [305, 18], [610, 17], [1220, 16], [2441, 15], [4882, 14],
		[9765, 13], [19531, 12]];
	const three = $derived(d?.kind === "cwnwww");
	const tunable = $derived(d?.kind === "cwww" || three);
	/* Driven by PWM, so it dims: tunable white, or one colour on one pin. */
	const pwm = $derived(tunable || d?.kind === "dim");
	type Wiring = Partial<{ kind: LightKind; gpio: number | null; gpio_warm: number | null; gpio_neutral: number | null;
		active_low: boolean; power_on: PowerOn; pwm_hz: number; cold_kelvin: number; warm_kelvin: number;
		neutral_kelvin: number; mix: LightMix; mix_curves: MixCurvesJson }>;
	async function saveWiring(body: Wiring, revert?: () => void) {
		if (!d || d.id == null) return;
		try { await patch(`/api/lights/${d.id}`, body); }
		catch (err: any) { toast(err.message); revert?.(); }
		refresh();
	}

	/* ---- a tunable white light's mixing curves ---- */
	/* Taken from the poll once per light; after that the page's copy is the one
	 * being drawn, and a poll arriving mid-drag must not snap it back. */
	const mix = $state({ loaded: "", curves: { cold: null, neutral: null, warm: null } as Curves });
	$effect(() => {
		if (!d || d.source !== "gpio" || mix.loaded === ieee) return;
		mix.loaded = ieee;
		mix.curves = curvesFromJson(d.mix_curves);
	});
	const coldK = $derived(d?.cold_kelvin ?? 6500);
	const warmK = $derived(d?.warm_kelvin ?? 2700);
	const neutralK = $derived(d?.neutral_kelvin ?? 4000);
	let mixTimer: ReturnType<typeof setTimeout> | undefined;
	function saveMix() {
		const body: Wiring = { mix: "custom", mix_curves: curvesToJson($state.snapshot(mix.curves)) };
		clearTimeout(mixTimer);
		mixTimer = setTimeout(() => saveWiring(body), 400);
	}
	/* Drawn curves start where the formula was, so choosing them changes nothing
	 * until a point is moved. */
	function chooseMix(m: LightMix) {
		if (m !== "custom") return saveWiring({ mix: m });
		const from = curvesFromMix(d?.mix ?? "constant", three, coldK, neutralK, warmK);
		for (const ch of channelsOf(three)) mix.curves[ch] ??= from[ch];
		saveWiring({ mix: "custom", mix_curves: curvesToJson($state.snapshot(mix.curves)) });
	}

	/* ---- a Matter lamp ---- */
	const matter = $derived(d?.source === "matter");
	/* The other lamps of the same device: a hub brings many, and removing the
	 * last one is what tells the device to forget this box. */
	const siblings = $derived(matter ? data.devices.filter((x) => x.node_id === d?.node_id && x.ieee !== ieee).length : 0);
	async function rescan() {
		if (!d?.node_id) return;
		try { await post(`/api/matter/nodes/${d.node_id}/rescan`); toast("Reading the device's lamps again"); schedule(4000); }
		catch (err: any) { toast(err.message); }
	}

	async function remove() {
		if (!d) return;
		const question = wired ? `Remove ${d.name}? Its pin is handed back.`
			: matter ? (siblings ? `Remove ${d.name}? The other ${siblings === 1 ? "lamp" : `${siblings} lamps`} of its device stay.`
				: `Remove ${d.name}? The device is told to forget this box; if it is unplugged, remove this box from it in its own app too.`)
			: `Remove ${d.name} from the network?`;
		if (!(await confirm(question, { confirmLabel: "Remove", destructive: true }))) return;
		try { await del(`/api/devices/${ieee}`); seenIeee.delete(ieee); navigate("/lights"); refresh(); }
		catch (err: any) { toast(err.message); }
	}
	const groupsOf = $derived(data.groups.filter((g) => g.members.some((m) => m.ieee === ieee && m.ep === ep)).map((g) => g.name));
</script>

<ViewHeader title={d ? d.name : "Light"} back="/lights">
	{#if e && !remote && f.includes("onoff")}
		<PowerSwitch {on} pending={pending.has(key(ieee, ep, "on"))} label="Power" onchange={(v) => setOnOff(ieee, ep, v)} />
	{/if}
</ViewHeader>

{#if !d || !e}
	<p class="text-muted-foreground mt-6 text-sm">This light is gone.</p>
{:else}
	{#if d.held}
		<p class="text-destructive mt-3 text-sm">Held off by the overheat protection — it can be switched on again once the board has cooled.</p>
	{/if}
	{#if remote}
		<Card.Root class="mt-4"><Card.Content class="text-muted-foreground text-sm">
			This is a remote: it sends presses rather than answering commands. Set up its buttons under <a href="#/remotes" class="text-primary underline underline-offset-4">Remotes</a>.
		</Card.Content></Card.Root>
	{:else if f.includes("onoff")}
		<!-- Response -->
		<Card.Root class="mt-4">
			<Card.Header>
				<Card.Title>Response</Card.Title>
				<Card.Description>How this light answers the brightness the room is set to.</Card.Description>
			</Card.Header>
			<Card.Content class="flex flex-col gap-4">
				{#if switchOnly}
					<SwitchPointEditor at={switchPointOf(c.level)} cursorPct={pct}
						onchange={(at) => { c.level = switchPoints(at); }} oncommit={saveCurves} />
					<div class="text-muted-foreground text-xs">This light cannot dim. It is on while its room is at or above this brightness and off below it. Remove it to have the light follow only the room's on/off.</div>
				{:else}
					{#if tabs.length > 1}
						<div class="bg-muted inline-flex self-start rounded-lg p-0.5" role="tablist">
							{#each tabs as [id, label] (id)}
								<button class="tab" role="tab" aria-selected={c.tab === id} onclick={() => { c.tab = id; c.sel = 0; }}>{label}</button>
							{/each}
						</div>
					{/if}
					<div>
						<h3 class="text-sm font-semibold">{tabs.find(([id]) => id === c.tab)?.[1]}</h3>
						<p class="text-muted-foreground mt-0.5 text-xs">
							{#if c.tab === "level"}
								How bright this light is at each room brightness, from 1 % on the left to 100 % on the right.
							{:else}
								{canCt && canColor ? "Its warmth or colour" : canCt ? "Its warmth" : "Its colour"} at each room brightness, from 1 % on the left to 100 % on the right.{#if canCt}{" "}This light does {minK}–{maxK} K.{/if}
							{/if}
						</p>
					</div>
					{#if c.tab === "level"}
						<CurveChart pts={shownLevel} ensure={ensureLevel} peers={c.peers} cursorPct={pct} onchange={saveCurves} />
						<div class="text-muted-foreground text-xs">Drag a point to bend the ramp. Tap the line to add one, drag a point off the chart to remove it.</div>
					{:else}
						{#if !c.path}
							<!-- Without a path the lamp's colour is left alone, so the one drawn
							     is where editing starts, not what the lamp does. -->
							<div class="border-border bg-muted/40 flex flex-wrap items-center gap-x-3 gap-y-2 rounded-md border px-3 py-2 text-xs">
								<span class="text-muted-foreground min-w-0 flex-1">
									Not in use: the room's brightness leaves this light's {canCt ? "warmth" : "colour"} as it is.
									The path below is where one would start - change a stop or pick a preset to use one, or use it as drawn.
								</span>
								<Button variant="secondary" size="sm" onclick={usePath}>Use this path</Button>
							</div>
						{/if}
						<ChromaPath path={shownPath} ensure={ensurePath} bind:sel={c.sel} {canCt} {canColor} {minK} {maxK} cursorPct={pct} onchange={saveCurves} draft={!c.path} />
						<div class="text-muted-foreground text-xs">
							{canCt && canColor
								? "Warmth stops sit above the bar, colour stops below it. Tap one to select it, then set it on the slider or the wheel; tap the bar above or below to add one. Drag a stop away from the bar to remove it."
								: "Tap a stop to select it, then set it above. Drag a stop along the bar to move it, away from the bar to remove it; tap the bar to add one."}
						</div>
					{/if}
				{/if}
				<div class="flex flex-wrap gap-2">
					{#if !switchOnly}
						{#each c.tab === "level" ? levelPresets : Object.keys(chromaPresets) as p (p)}
							<Button variant="outline" size="sm" onclick={() => applyPreset(p)}>{p}</Button>
						{/each}
					{/if}
					{#if hasCurrent}
						<Button variant="destructive" size="sm" onclick={removeCurve}>Remove</Button>
					{/if}
				</div>

				<!-- Test slider -->
				<div class="border-border border-t pt-4">
					<Label class="mb-2 block">Try it</Label>
					{#if dims}
						<LevelSlider {pct} {on} label="Test brightness" onsend={(v) => setLevel(ieee, ep, v)} />
					{:else}
						<input type="range" min="1" max="100" step="1" value={pct ?? 50} class="accent-curve w-full" aria-label="Room brightness to try"
							oninput={(ev) => { tryPct = Number(ev.currentTarget.value); }} />
					{/if}
					<div class="text-muted-foreground mt-2 flex flex-wrap items-center gap-x-3 gap-y-1 text-xs">
						{#if pct == null}
							<span>Move the slider to see what the lamp does.</span>
						{:else}
							<span>room <b class="text-foreground">{pct} %</b></span>
							<span>→</span>
							{#if switchOnly}
								<span>lamp <b class="text-foreground">{curveAt(c.level ?? [[1, 100], [100, 100]], pct) > 0 ? "on" : "off"}</b></span>
							{:else}
								<span>lamp <b class="text-foreground">{lampPct} %</b></span>
							{/if}
							{#if lampColour}
								<span class="inline-flex items-center gap-1.5"><span class="inline-block size-3.5 rounded-full ring-1 ring-black/10" style:background={rgbCss(colourOf(lampColour))}></span>{lampColourText}</span>
							{/if}
						{/if}
					</div>
				</div>
			</Card.Content>
		</Card.Root>
	{/if}

	{#if wired}
		{#snippet pinSelect(id: string, label: string, current: number | null | undefined, save: (pin: number | null) => Wiring)}
			<div>
				<Label for={id} class="mb-2 block">{label}</Label>
				<select {id} class="sel w-full" value={current == null ? "" : String(current)}
					onchange={(ev) => {
						const el = ev.currentTarget, was = current == null ? "" : String(current);
						saveWiring(save(el.value === "" ? null : Number(el.value)), () => { el.value = was; });
					}}>
					<option value="">— no pin yet —</option>
					{#each LIGHT_PINS as p (p)}<option value={String(p)}>GPIO {p}{PIN_NOTE[p] ? ` — ${PIN_NOTE[p]}` : ""}</option>{/each}
				</select>
				{#if current != null && PIN_NOTE[current] === "strapping pin"}
					<div class="text-muted-foreground mt-1 text-xs">A strapping pin is read while the board starts, so what is wired to it can stop the board booting. Prefer another pin if there is one free.</div>
				{/if}
			</div>
		{/snippet}
		{#snippet kelvinInput(id: string, label: string, current: number, save: (k: number) => Wiring)}
			<div>
				<Label for={id} class="mb-2 block">{label}</Label>
				<Input {id} type="number" min={1000} max={10000} step={100} value={current}
					onchange={(ev) => {
						const el = ev.currentTarget as HTMLInputElement, was = String(current);
						saveWiring(save(Number(el.value)), () => { el.value = was; });
					}} />
			</div>
		{/snippet}

		<!-- Wiring -->
		<Card.Root class="mt-4">
			<Card.Header>
				<Card.Title>Wiring</Card.Title>
				<Card.Description>{three ? "The three pins of the board the cold, neutral and warm white LEDs are driven from." : tunable ? "The two pins of the board the cold and warm white LEDs are driven from." : pwm ? "The pin of the board the LEDs are dimmed from." : "The pin of the board this light is switched from."}</Card.Description>
			</Card.Header>
			<Card.Content class="flex flex-col gap-4">
				<div>
					<Label for="lkind" class="mb-2 block">Type</Label>
					<select id="lkind" class="sel w-full" value={d.kind ?? "onoff"}
						onchange={(ev) => {
							const el = ev.currentTarget, was = d.kind ?? "onoff";
							saveWiring({ kind: el.value as LightKind }, () => { el.value = was; });
						}}>
						<option value="onoff">On/off — a relay, or LEDs on one pin</option>
						<option value="dim">Dimmable — single-colour LEDs on one PWM pin</option>
						<option value="cwww">Tunable white — cold and warm white LEDs on two PWM pins</option>
						<option value="cwnwww">Tunable white, three channels — cold, neutral and warm white LEDs on three PWM pins</option>
					</select>
				</div>

				{#if tunable}
					<div class="grid gap-4 {three ? 'sm:grid-cols-3' : 'sm:grid-cols-2'}">
						{@render pinSelect("lpin", "Cold white pin", d.gpio, (pin) => ({ gpio: pin }))}
						{#if three}
							{@render pinSelect("lpinn", "Neutral white pin", d.gpio_neutral, (pin) => ({ gpio_neutral: pin }))}
						{/if}
						{@render pinSelect("lpinw", "Warm white pin", d.gpio_warm, (pin) => ({ gpio_warm: pin }))}
					</div>
					<div class="grid gap-4 {three ? 'sm:grid-cols-3' : 'sm:grid-cols-2'}">
						{@render kelvinInput("lcold", "Cold white, kelvin", d.cold_kelvin ?? 6500, (k) => ({ cold_kelvin: k }))}
						{#if three}
							{@render kelvinInput("lneutral", "Neutral white, kelvin", d.neutral_kelvin ?? 4000, (k) => ({ neutral_kelvin: k }))}
						{/if}
						{@render kelvinInput("lwarm", "Warm white, kelvin", d.warm_kelvin ?? 2700, (k) => ({ warm_kelvin: k }))}
					</div>
				{:else}
					{@render pinSelect("lpin", "Pin", d.gpio, (pin) => ({ gpio: pin }))}
				{/if}

				{#if pwm}
					<div>
						<Label for="lhz" class="mb-2 block">PWM frequency, Hz</Label>
						<div class="flex flex-wrap items-center gap-2">
							<div class="w-28"><Input id="lhz" type="number" min={100} max={19531} step={1} value={d.pwm_hz ?? 4882}
								onchange={(ev) => {
									const el = ev.currentTarget as HTMLInputElement, was = String(d.pwm_hz ?? 4882);
									saveWiring({ pwm_hz: Number(el.value) }, () => { el.value = was; });
								}} /></div>
							{#each HZ_PICKS as [hz, bits] (hz)}
								<Button variant={d.pwm_hz === hz ? "secondary" : "outline"} size="sm" title="{bits} bits" onclick={() => saveWiring({ pwm_hz: hz })}>{hz}</Button>
							{/each}
						</div>
						<div class="text-muted-foreground mt-1 text-xs">100–19531 Hz. The presets are the fastest frequency at each resolution, from 152 Hz at 19 bits to 19531 Hz at 12; hover over one for its bits. More bits make smoother fades at the very bottom; higher frequencies are kinder to cameras and flicker-sensitive eyes, so use the lowest three only for a driver that needs them.{tunable ? " The two channels run half a period apart either way, so their ripple cancels." : ""}</div>
						{#if d.pwm_phase != null}
							<div class="text-muted-foreground mt-1 text-xs">Runs at {d.pwm_phase}° of the period. Dimming lights at the same frequency are spread across it, so where several light one room, giving them the same frequency makes their ripple partly cancel.</div>
						{/if}
					</div>
				{/if}

				{#if tunable}
					<div class="flex flex-col gap-2 text-sm">
						<span class="font-medium">Mixing</span>
						<label class="flex items-start gap-2"><input type="radio" name="lmix" class="accent-primary mt-1" checked={(d.mix ?? "constant") === "constant"} onchange={() => chooseMix("constant")} />
							<span>{three
								? "Constant total — the two whites either side of the colour temperature share the brightness, so every temperature is exactly as bright"
								: "Constant total — the two channels share the brightness, so a mid temperature is exactly as bright as either end"}</span></label>
						<label class="flex items-start gap-2"><input type="radio" name="lmix" class="accent-primary mt-1" checked={d.mix === "full"} onchange={() => chooseMix("full")} />
							<span>{three
								? "Full output in the middle — two neighbouring whites both reach full half way between them, about twice as bright there; the supply has to carry both at once"
								: "Full output in the middle — both channels reach full at a mid temperature, about twice as bright there; the supply has to carry both at once"}</span></label>
						<label class="flex items-start gap-2"><input type="radio" name="lmix" class="accent-primary mt-1" checked={d.mix === "custom"} onchange={() => chooseMix("custom")} />
							<span>Your own curves — draw how much of each channel lights at each colour temperature</span></label>
					</div>
					{#if d.mix === "custom"}
						<MixCurves curves={mix.curves} {three} {coldK} {neutralK} {warmK} kelvin={e?.kelvin ?? null} onchange={saveMix} />
					{/if}
				{/if}

				<div class="flex flex-col gap-2 text-sm">
					<span class="font-medium">{pwm ? "The LEDs are lit when their pin is" : "The light is on when the pin is"}</span>
					<label class="flex items-center gap-2"><input type="radio" name="lpolarity" class="accent-primary" checked={!d.active_low} onchange={() => saveWiring({ active_low: false })} /> HIGH (3V3) — {pwm ? "most MOSFET and LED driver boards" : "most LEDs and relay modules"}</label>
					<label class="flex items-center gap-2"><input type="radio" name="lpolarity" class="accent-primary" checked={!!d.active_low} onchange={() => saveWiring({ active_low: true })} /> LOW (GND) — {pwm ? "drivers with an inverting input" : "\"low-level trigger\" relay modules"}</label>
				</div>
				<div class="flex flex-col gap-2 text-sm">
					<span class="font-medium">When the coordinator starts</span>
					<div class="flex flex-wrap gap-x-4 gap-y-2">
						{#each POWER_ON as [v, label] (v)}
							<label class="flex items-center gap-2"><input type="radio" name="lpoweron" class="accent-primary" checked={(d.power_on ?? "off") === v} onchange={() => saveWiring({ power_on: v })} /> {label}</label>
						{/each}
					</div>
					<div class="text-muted-foreground text-xs">
						{pwm
							? `That includes after a power cut and after a firmware update. "On" comes back at the last ${tunable ? "brightness and temperature" : "brightness"}, and "As it was" also remembers whether it was on. Changes are stored 10 s after the last one.`
							: "That includes after a power cut and after a firmware update. \"As it was\" remembers every switch."}
					</div>
				</div>
			</Card.Content>
		</Card.Root>
	{/if}

	<!-- Device -->
	<Card.Root class="mt-4">
		<Card.Header><Card.Title>Device</Card.Title></Card.Header>
		<Card.Content class="flex flex-col gap-4">
			<div>
				<Label for="lname" class="mb-2 block">Name</Label>
				<Input id="lname" bind:value={name} maxlength={31} onchange={rename} onkeydown={(ev) => { if (ev.key === "Enter") (ev.currentTarget as HTMLInputElement).blur(); }} />
			</div>
			<dl class="text-muted-foreground grid grid-cols-[auto_1fr] gap-x-4 gap-y-1 text-sm">
				{#if wired}
					<dt>Wiring</dt><dd class="text-foreground">{d.gpio == null ? "no pin yet" : `GPIO ${d.gpio}`}</dd>
				{:else}
					<dt>Model</dt><dd class="text-foreground">{[d.manufacturer, d.model].filter(Boolean).join(" · ") || "unknown"}</dd>
				{/if}
				<dt>Address</dt><dd class="text-foreground font-mono text-xs">{d.ieee} · endpoint {e.ep}</dd>
				{#if matter}
					<dt>Matter</dt><dd class="text-foreground">node <span class="font-mono text-xs">{d.node_id}</span> · endpoint {d.remote_ep}{d.bridged ? " · behind a hub" : ""}</dd>
					<dt>Vendor</dt><dd class="text-foreground font-mono text-xs">{hex4(d.vendor_id ?? 0)} · product {hex4(d.product_id ?? 0)}</dd>
					<dt>Certificate</dt><dd class="text-foreground">{d.verified ? "verified" : "not verified"}</dd>
					<dt>Connection</dt><dd class="text-foreground">{d.needs_pairing ? "needs pairing again: Matter was reset" : d.online ? "subscribed" : "offline"}</dd>
					<dt>Last report</dt><dd class="text-foreground">{agoS(d.last_seen_s_ago)}</dd>
				{:else if !wired}
					<dt>Link</dt><dd class="text-foreground">{linkText(d)}{d.online ? "" : " · offline"}</dd>
					<dt>Last seen</dt><dd class="text-foreground">{agoS(d.last_seen_s_ago)}</dd>
				{/if}
				<dt>Rooms</dt><dd class="text-foreground">{groupsOf.length ? groupsOf.join(", ") : "none"}</dd>
			</dl>
			{#if matter && d.bridged}
				<div class="text-muted-foreground text-xs">A lamp added to the hub later appears here after <b>Rescan device</b>.</div>
			{/if}
			{#if !wired && !matter}<FirmwareRow device={d} />{/if}
			<div class="flex flex-wrap gap-2">
				{#if matter && !d.needs_pairing}<Button variant="outline" onclick={rescan}>Rescan device</Button>{/if}
				<Button variant="destructive" onclick={remove}><Trash /> Remove device</Button>
			</div>
		</Card.Content>
	</Card.Root>
{/if}

<style>
	.sel { height: 2.5rem; border-radius: var(--radius-md); border: 1px solid var(--input); background: var(--card);
		color: var(--foreground); padding: 0 .6rem; font-size: .875rem; }
	.tab { padding: .4rem .9rem; border-radius: 6px; font-size: .85rem; font-weight: 500; color: var(--muted-foreground); }
	.tab[aria-selected="true"] { background: var(--card); color: var(--foreground); box-shadow: var(--elev-1); }
</style>
