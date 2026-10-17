<script lang="ts">
	import * as Card from "$lib/components/ui/card";
	import { Button } from "$lib/components/ui/button";
	import { Input } from "$lib/components/ui/input";
	import { Label } from "$lib/components/ui/label";
	import { Progress } from "$lib/components/ui/progress";
	import { Switch } from "$lib/components/ui/switch";
	import Upload from "@lucide/svelte/icons/upload";
	import Download from "@lucide/svelte/icons/download";
	import TriangleAlert from "@lucide/svelte/icons/triangle-alert";
	import ChevronRight from "@lucide/svelte/icons/chevron-right";
	import ZigbeeChannel from "$lib/components/ZigbeeChannel.svelte";
	import RestoreDialog from "$lib/components/RestoreDialog.svelte";
	import AdaptiveCard from "$lib/components/AdaptiveCard.svelte";
	import { app, data } from "$lib/state.svelte.ts";
	import { api, post } from "$lib/api.ts";
	import { toast } from "$lib/toast.svelte.ts";
	import { confirm } from "$lib/confirm.svelte.ts";
	import { ota, uploadFirmware, uploadUpdater, runUpdater } from "$lib/ota.svelte.ts";
	import { zbota, askEveryone, removeImage, cancelTarget, transferText } from "$lib/zbota.svelte.ts";
	import { kb, versionText } from "$lib/zbota.ts";
	import { backup, downloadBackup, readBackup } from "$lib/backup.svelte.ts";
	import { backupWarning, type BackupSummary, type Secret } from "$lib/backup.ts";
	import { auth } from "$lib/auth.svelte.ts";
	import { agoS, hex4 } from "$lib/format.ts";
	import type { Settings, ThermalConfig } from "$lib/types.ts";

	const FADES: [number, string][] = [[0, "Instant"], [200, "0.2 s"], [400, "0.4 s"], [700, "0.7 s"],
		[1000, "1 s"], [1500, "1.5 s"], [2000, "2 s"], [3000, "3 s"], [5000, "5 s"]];

	const s = $derived(app.status);
	let fade = $state(0);
	let warmOff = $state<boolean | null>(null);
	let name = $state("");
	let saved = $state("");
	let hostname = $state("");
	let maxLen = $state(31);
	let fileInput: HTMLInputElement;
	let updaterInput: HTMLInputElement;
	const zo = $derived(s?.zigbee.ota);
	const oldLayout = $derived(s?.ota?.layout === "old");
	const nameOf = (ieee: string | null | undefined) =>
		ieee ? (data.devices.find((d) => d.ieee === ieee)?.name ?? ieee) : "";
	const targetProgress = $derived(zo?.target ? transferText(zo.target) : zo?.transfer ? transferText(zo.transfer.ieee) : null);
	function pickUpdater() {
		const file = updaterInput.files?.[0];
		updaterInput.value = "";
		if (file) uploadUpdater(file);
	}
	async function makeRoom() {
		if (!(await confirm("Restart through the updater so it can make room for Zigbee device firmware? The lights do not respond for about a minute.", { confirmLabel: "Restart" }))) return;
		runUpdater();
	}

	$effect(() => {
		api<Settings>("/api/settings").then((cfg) => {
			/* Snap to the nearest offered value: the API takes any millisecond
			 * count, the menu only offers the useful ones. */
			let best = FADES[0][0], gap = Infinity;
			for (const [ms] of FADES) { const d = Math.abs(ms - cfg.transition_ms); if (d < gap) { gap = d; best = ms; } }
			fade = best;
			warmOff = cfg.warm_off ?? false;
			name = saved = cfg.name;
			hostname = cfg.hostname;
			maxLen = cfg.name_max_len || 31;
			adaptiveCfg = cfg;
			if (cfg.thermal) th = cfg.thermal;
			wledBuilt = !!cfg.wled;
			wledOn = cfg.wled?.enabled ?? false;
			wledRoom = cfg.wled?.room ?? null;
			wledOthersOff = cfg.wled?.others_off ?? false;
			backupBuilt = cfg.backup ?? false;
		}).catch(() => { hostname = s?.wifi.hostname ?? ""; name = saved = s?.name ?? ""; });
	});

	/* Adaptive Lighting's card fetches nothing itself: it starts from this. */
	let adaptiveCfg = $state<Settings | null>(null);

	/* The WLED interface: one room's Hue colour lamps as a WLED strip, a pixel
	 * each in the room's own order - which the coordinator works out, so the
	 * list comes from its status rather than from a rule repeated here. A build
	 * without it (CONFIG_ZBC_WLED off) has no `wled` in its settings, and no card. */
	let wledBuilt = $state(false);
	let wledOn = $state<boolean | null>(null);
	let wledRoom = $state<number | null>(null);
	let wledOthersOff = $state<boolean | null>(null);
	const ws = $derived(s?.wled);
	const wledPixels = $derived(ws && ws.room === wledRoom ? ws.pixels : []);
	const wledOthers = $derived.by(() => {
		const g = data.groups.find((x) => x.id === wledRoom);
		if (!g) return [];
		const taken = new Set(wledPixels.map((p) => `${p.ieee}/${p.ep}`));
		return g.members.filter((m) => !taken.has(`${m.ieee}/${m.ep}`)).map((m) => m.name);
	});
	const wledLine = $derived.by(() => {
		if (!ws?.enabled) return "";
		if (ws.live) return `Streaming from ${ws.source} by ${ws.protocol}: ${ws.fps_in} frames a second in, 20 out to the lamps.`;
		if (ws.override) return `Taken back by hand. ${ws.source ?? "The client"} is ignored until it stops for a moment.`;
		if (!ws.listening) return "Not listening: the UDP ports could not be opened.";
		return `Waiting for a client at ${s?.wifi.ip || s?.wifi.hostname + ".local"} (DDP on UDP 4048, WLED's own on 21324).`;
	});
	async function saveWled(patch: { enabled?: boolean; room?: number | null; others_off?: boolean }) {
		try {
			const cfg = await post<Settings>("/api/settings", { wled: patch });
			wledOn = cfg.wled?.enabled ?? false;
			wledRoom = cfg.wled?.room ?? null;
			wledOthersOff = cfg.wled?.others_off ?? false;
			toast(patch.others_off !== undefined ? (patch.others_off ? "Other lights go off while streaming" : "Other lights left as they are")
				: patch.enabled === undefined ? "Room saved" : patch.enabled ? "WLED interface on" : "WLED interface off");
		} catch (err: any) { toast(err.message); }
	}

	/* Overheat protection. The pin menu offers the chip's analog inputs that no
	 * wired light or button already uses - the coordinator refuses the rest. */
	const SHELLY: Omit<ThermalConfig, "adc_pins"> = { gpio: 3, b: 3350, r0_ohm: 10000, rs_ohm: 10000, ntc_to: "gnd", trip_c: 75 };
	let th = $state<ThermalConfig>({ ...SHELLY, gpio: null, adc_pins: [0, 1, 2, 3, 4, 5, 6] });
	const usedPins = $derived(new Set([
		...data.devices.filter((d) => d.source === "gpio").flatMap((d) => [d.gpio, d.gpio_warm]),
		...data.buttons.filter((b) => b.source === "gpio").map((b) => b.gpio),
	].filter((p): p is number => typeof p === "number")));
	const thPins = $derived((th.adc_pins ?? []).filter((p) => p === th.gpio || !usedPins.has(p)));
	const ts = $derived(s?.thermal);
	const deg = (c: number | null | undefined) => (c == null ? "—" : `${c.toFixed(1)} °C`);
	const thLine = $derived.by(() => {
		if (!ts || ts.state === "off" || th.gpio === null) return "";
		if (ts.state === "fault") return `The sensor reads as broken (${ts.mv} mV at the pin) — wired lights are held off.`;
		const now = `Now ${deg(ts.temp_c)}, highest ${deg(ts.max_c)} since start.`;
		return ts.state === "hot" ? `${now} Too hot — wired lights are held off until below ${ts.release_c} °C.` : now;
	});
	async function saveThermal(patch: Partial<ThermalConfig>) {
		const cur: Partial<ThermalConfig> = { ...th };
		delete cur.adc_pins;
		try {
			const cfg = await post<Settings>("/api/settings", { thermal: { ...cur, ...patch } });
			if (cfg.thermal) th = cfg.thermal;
			toast("Overheat protection saved");
		} catch (err: any) { toast(err.message); }
	}
	const numberField = (v: string) => Number(v.trim().replace(",", "."));

	async function saveWarmOff(on: boolean) {
		const was = warmOff;
		warmOff = on;
		try { await post("/api/settings", { warm_off: on }); toast(on ? "Lights will be left warm while off" : "Lights keep their colour while off"); }
		catch (err: any) { warmOff = was; toast(err.message); }
	}

	async function saveFade(ms: number) {
		fade = ms;
		try { await post("/api/settings", { transition_ms: ms }); toast("Fade time saved"); }
		catch (err: any) { toast(err.message); }
	}
	/* The name is one field and the address is derived from it, so the server
	 * is the authority on the slug. An empty field is how you get back to the
	 * default, which typing the default name would not do. */
	async function saveName() {
		const n = name.trim();
		if (n === saved) { name = saved; return; }
		try {
			const cfg = await post<Settings>("/api/settings", { name: n });
			saved = name = cfg.name; hostname = cfg.hostname;
			toast(`Saved — restarting as http://${cfg.hostname}.local`);
		} catch (err: any) { name = saved; toast(err.message); }
	}
	async function reset() {
		if (!(await confirm("Erase the Zigbee network, all paired devices, the Wi-Fi credentials, every Matter controller and every access key?", { confirmLabel: "Erase everything", destructive: true }))) return;
		try { await post("/api/factory_reset"); toast("Reset — the coordinator is restarting into setup mode"); }
		catch (err: any) { toast(err.message); }
	}
	async function restart() {
		if (!(await confirm("Restart the coordinator? Nothing is erased. Lights and buttons do not respond until it is back.", { confirmLabel: "Restart" }))) return;
		try { await post("/api/restart"); toast("Restarting — the page reconnects by itself"); }
		catch (err: any) { toast(err.message); }
	}
	function pickFile() {
		const file = fileInput.files?.[0];
		fileInput.value = "";   /* picking the same file twice in a row still fires */
		if (file) uploadFirmware(file);
	}

	/* The configuration file. Which secrets go in is this browser's choice,
	 * remembered here; the coordinator puts in whatever it is asked for. */
	const OMIT_KEY = "backup-omit";
	const storedOmit = (): Secret[] => {
		try { return JSON.parse(localStorage.getItem(OMIT_KEY) ?? "[]").filter((x: unknown) => x === "wifi" || x === "auth"); }
		catch { return []; }
	};
	let omit = $state<Secret[]>(storedOmit());
	function setSecret(which: Secret, include: boolean) {
		omit = include ? omit.filter((x) => x !== which) : [...omit.filter((x) => x !== which), which];
		try { localStorage.setItem(OMIT_KEY, JSON.stringify(omit)); } catch { /* private window: not remembered */ }
	}
	const withWifi = $derived(!omit.includes("wifi"));
	const withKeys = $derived(!omit.includes("auth"));
	const keyCount = $derived(auth.state?.keys ?? null);
	const homeSsid = $derived(s?.wifi.mode === "sta" ? s.wifi.ssid : (s?.wifi.home_ssid ?? ""));
	const backupWarn = $derived(backupWarning(withWifi, withKeys, !!homeSsid, keyCount));
	/* Only once the settings say this build has the routes. */
	let backupBuilt = $state(false);
	let restoreInput = $state<HTMLInputElement>();
	let restoring = $state<BackupSummary | null>(null);
	async function download() {
		/* No keys is nothing to leave out: the file says the coordinator had
		 * none, and the restore dialog then asks before removing protection. */
		const left = keyCount === 0 ? omit.filter((x) => x !== "auth") : omit;
		try { await downloadBackup(hostname || s?.wifi.hostname || "", left); }
		catch (err: any) { toast(err.message); }
	}
	async function pickRestore() {
		const file = restoreInput?.files?.[0];
		if (restoreInput) restoreInput.value = "";
		if (!file) return;
		try { restoring = await readBackup(file); }
		catch (err: any) { toast(err.message); }
	}
</script>

<h2 class="mt-5 text-lg font-semibold">Settings</h2>

<Card.Root class="mt-4">
	<Card.Header><Card.Title>Lights</Card.Title></Card.Header>
	<Card.Content class="flex flex-col gap-2">
		<Label for="fade">Fade time</Label>
		<select id="fade" class="sel" value={fade} onchange={(e) => saveFade(Number(e.currentTarget.value))}>
			{#each FADES as [ms, label] (ms)}<option value={ms}>{label}</option>{/each}
		</select>
		<div class="text-muted-foreground text-xs">How long lights take to move between states. Applies to dimming, colour and switching on and off — lamps that ignore it keep their own pace.</div>

		<div class="mt-2 flex items-center justify-between gap-4">
			<Label for="warmoff">Warm while off</Label>
			<Switch id="warmoff" checked={warmOff === true} disabled={warmOff === null} aria-label="Warm while off" onCheckedChange={saveWarmOff} />
		</div>
		<div class="text-muted-foreground text-xs">A light being switched off is dimmed right down, set to its warmest white and then switched off, so it comes back on warm in the evening instead of flashing cold. A light already at its warmest is left alone. Lights without a colour temperature are unaffected, and so is a light switched off at the wall.</div>
	</Card.Content>
</Card.Root>

{#if wledBuilt}
<Card.Root class="mt-4">
	<Card.Header>
		<Card.Title>WLED interface</Card.Title>
		<Card.Description>Lets Hyperion, HyperHDR or LedFx drive one room's Hue colour lamps as if they were a WLED strip — a TV's ambilight, or music. Each lamp is a pixel, up to ten. A switch, a remote or this page takes the room back, and the client then has to stop before it gets it again.</Card.Description>
	</Card.Header>
	<Card.Content class="flex flex-col gap-3">
		<div class="flex items-center justify-between gap-4">
			<Label for="wled">On</Label>
			<Switch id="wled" checked={wledOn === true} disabled={wledOn === null} aria-label="WLED interface" onCheckedChange={(on) => saveWled({ enabled: on })} />
		</div>
		<div class="text-muted-foreground text-xs">WLED has no keys: while this is on, anything on your network can switch this room and stream to it.</div>
		<Label for="wledroom">Room</Label>
		<select id="wledroom" class="sel" value={wledRoom ?? ""} onchange={(e) => saveWled({ room: e.currentTarget.value === "" ? null : Number(e.currentTarget.value) })}>
			<option value="">None</option>
			{#each data.groups as g (g.id)}<option value={g.id}>{g.name}</option>{/each}
		</select>
		{#if wledRoom !== null}
			{#if wledPixels.length}
				<ol class="list-decimal pl-5 text-sm">
					{#each wledPixels as p (p.ieee + "/" + p.ep)}<li>{nameOf(p.ieee)}</li>{/each}
				</ol>
			{:else}
				<div class="text-sm">No light in this room can take part: only Hue colour lamps can.</div>
			{/if}
			{#if wledOthers.length}<div class="text-muted-foreground text-xs">{wledOthersOff ? "Switched off while it streams" : "Left as they are"}: {wledOthers.join(", ")}.</div>{/if}
		{/if}
		<div class="flex items-center justify-between gap-4">
			<Label for="wledothers">Switch the other lights off</Label>
			<Switch id="wledothers" checked={wledOthersOff === true} disabled={wledOthersOff === null} aria-label="Switch the other lights off while streaming" onCheckedChange={(on) => saveWled({ others_off: on })} />
		</div>
		<div class="text-muted-foreground text-xs">The room's lights that cannot take part go off when a stream starts and come back when it ends — a dark room for a film. Switching one on by hand while it streams keeps it on. Takes effect from the next stream.</div>
		{#if wledLine}<div class="text-muted-foreground text-xs">{wledLine}</div>{/if}
	</Card.Content>
</Card.Root>
{/if}

<Card.Root class="mt-4">
	<Card.Header>
		<Card.Title>Overheat protection</Card.Title>
		<Card.Description>For a board with a temperature sensor (NTC) beside its relay, like a converted Shelly. At the shutoff temperature — or when the sensor reads as broken — every wired light is switched off at once. They stay off until the board is 5 °C cooler and somebody switches them on again. Zigbee lamps are not affected.</Card.Description>
	</Card.Header>
	<Card.Content class="flex flex-col gap-4">
		<div>
			<Label for="thpin" class="mb-2 block">Sensor pin</Label>
			<select id="thpin" class="sel w-full" value={th.gpio ?? ""}
				onchange={(e) => saveThermal({ gpio: e.currentTarget.value === "" ? null : Number(e.currentTarget.value) })}>
				<option value="">No sensor</option>
				{#each thPins as p (p)}<option value={p}>GPIO {p}</option>{/each}
			</select>
			{#if thLine}<div class="mt-2 text-xs {ts?.state === 'ok' ? 'text-muted-foreground' : 'text-destructive'}">{thLine}</div>{/if}
		</div>
		{#if th.gpio !== null}
			<div class="grid grid-cols-2 gap-3">
				<div>
					<Label for="thtrip" class="mb-2 block">Shutoff at (°C)</Label>
					<Input id="thtrip" type="number" min="40" max="120" value={th.trip_c} onchange={(e) => saveThermal({ trip_c: numberField(e.currentTarget.value) })} />
				</div>
				<div>
					<Label for="thb" class="mb-2 block">B constant (K)</Label>
					<Input id="thb" type="number" min="1000" max="10000" value={th.b} onchange={(e) => saveThermal({ b: numberField(e.currentTarget.value) })} />
				</div>
				<div>
					<Label for="thr0" class="mb-2 block">NTC at 25 °C (Ω)</Label>
					<Input id="thr0" type="number" min="100" max="1000000" value={th.r0_ohm} onchange={(e) => saveThermal({ r0_ohm: numberField(e.currentTarget.value) })} />
				</div>
				<div>
					<Label for="thrs" class="mb-2 block">Fixed resistor (Ω)</Label>
					<Input id="thrs" type="number" min="100" max="1000000" value={th.rs_ohm} onchange={(e) => saveThermal({ rs_ohm: numberField(e.currentTarget.value) })} />
				</div>
			</div>
			<fieldset class="flex flex-col gap-1.5 text-sm">
				<legend class="mb-2 text-sm font-medium">The NTC connects the pin to</legend>
				<label class="flex items-center gap-2"><input type="radio" name="thto" class="accent-primary" checked={th.ntc_to === "gnd"} onchange={() => saveThermal({ ntc_to: "gnd" })} /> GND, fixed resistor to 3V3 — Shelly, ESPHome "downstream"</label>
				<label class="flex items-center gap-2"><input type="radio" name="thto" class="accent-primary" checked={th.ntc_to === "3v3"} onchange={() => saveThermal({ ntc_to: "3v3" })} /> 3V3, fixed resistor to GND — ESPHome "upstream"</label>
			</fieldset>
		{/if}
		{#if thPins.includes(3)}
			<Button variant="outline" size="sm" class="self-start" onclick={() => saveThermal({ ...SHELLY })}>Use the Shelly Gen3/Gen4 sensor</Button>
		{/if}
	</Card.Content>
</Card.Root>

<AdaptiveCard cfg={adaptiveCfg} />

<Card.Root class="mt-4">
	<Card.Header><Card.Title>Zigbee network</Card.Title></Card.Header>
	<Card.Content class="flex flex-col gap-5">
		<ZigbeeChannel />
	</Card.Content>
</Card.Root>

<Card.Root class="mt-4">
	<Card.Header><Card.Title>Coordinator</Card.Title></Card.Header>
	<Card.Content class="flex flex-col gap-4">
		<div>
			<Label for="cname" class="mb-2 block">Name</Label>
			<Input id="cname" bind:value={name} maxlength={maxLen} autocomplete="off" onchange={saveName}
				onkeydown={(ev) => { if (ev.key === "Enter") (ev.currentTarget as HTMLInputElement).blur(); }} />
			<div class="text-muted-foreground mt-2 text-xs">Reachable at http://{hostname}.local</div>
			<div class="text-muted-foreground mt-1 text-xs">What this coordinator calls itself — on the network, and to Matter controllers. The address follows the name, so changing it restarts the coordinator. Clear the field to go back to the default.</div>
		</div>
		<div class="text-muted-foreground text-xs">
			{#if s}
				Firmware {s.version}{s.ota?.updater ? ` · updater ${s.ota.updater}` : ""} · up {Math.floor(s.uptime_s / 60)} min<br>
				Coordinator {s.zigbee.ieee}
				{#if s.ota?.pending_verify}<br>This firmware is still on trial — it is confirmed once the coordinator is back on Wi-Fi.{/if}
			{/if}
		</div>
		<div class="flex flex-col divide-y">
			<a href="#/settings/access" class="link"><span>Access</span><ChevronRight class="text-muted-foreground size-4" /></a>
			<a href="#/settings/matter" class="link"><span>Matter</span><ChevronRight class="text-muted-foreground size-4" /></a>
			<a href="#/settings/wifi" class="link"><span>Wi-Fi</span><ChevronRight class="text-muted-foreground size-4" /></a>
		</div>
	</Card.Content>
</Card.Root>

<Card.Root class="mt-4">
	<Card.Header><Card.Title>Firmware</Card.Title><Card.Description>Upload a plain ESP-IDF application image, the file ESPHome would call firmware.ota.bin. The coordinator restarts into its updater to install it, so the lights do not respond for a minute or two.</Card.Description></Card.Header>
	<Card.Content class="flex flex-col gap-3">
		<input type="file" accept=".bin,application/octet-stream" hidden bind:this={fileInput} onchange={pickFile} />
		<div><Button variant="outline" disabled={ota.busy || s?.ota?.updater_valid === false} onclick={() => fileInput.click()}><Upload /> Update firmware</Button></div>
		{#if s?.ota?.updater_valid === false}
			<div class="text-destructive text-xs">{s.ota.updater_problem ?? "The updater cannot be trusted"}, so the coordinator will not hand an update to it. Upload the elixcon_light_hub_updater.bin from this build below.</div>
		{/if}
		<input type="file" accept=".bin,application/octet-stream" hidden bind:this={updaterInput} onchange={pickUpdater} />
		<div class="text-muted-foreground text-xs">
			Updater {s?.ota?.updater ?? "unknown"}. The updater is the small second firmware that installs this one. Replace it only with the elixcon_light_hub_updater.bin built alongside the firmware; the coordinator keeps running while it is written.
		</div>
		<div><Button variant="ghost" size="sm" disabled={ota.busy} onclick={() => updaterInput.click()}>Replace updater…</Button></div>
		{#if ota.shown}
			{#if ota.fraction === null}
				<div class="bg-muted h-2 w-full overflow-hidden rounded-full"><div class="bg-primary h-full w-1/3 animate-pulse rounded-full"></div></div>
			{:else}
				<Progress value={ota.fraction * 100} class="h-2" />
			{/if}
			<div class="text-muted-foreground text-xs">{ota.message}</div>
		{/if}
	</Card.Content>
</Card.Root>

<Card.Root class="mt-4">
	<Card.Header>
		<Card.Title>Zigbee device firmware</Card.Title>
		<Card.Description>Lamps, plugs and remotes get their firmware from the Zigbee2MQTT collection: open a device and check for updates. Its page downloads the image and hands it to the coordinator, which sends it over Zigbee — that takes a quarter of an hour or more, and the device keeps working until it restarts at the end.</Card.Description>
	</Card.Header>
	<Card.Content class="flex flex-col gap-3 text-sm">
		{#if oldLayout}
			<p>This coordinator has no room for device firmware yet. The updater makes it, by shortening the coordinator's own partition — nothing stored is lost.</p>
			<ol class="text-muted-foreground list-decimal space-y-1 pl-5 text-xs">
				<li>Install this firmware as usual (done, if you are reading this).</li>
				<li>Replace the updater with the elixcon_light_hub_updater.bin from the same build, under Firmware above.</li>
				<li>Restart through the updater.</li>
			</ol>
			<div><Button variant="outline" disabled={ota.busy || s?.ota?.updater_valid === false} onclick={makeRoom}>Restart through the updater</Button></div>
		{:else if zo}
			{#if zo.image}
				<dl class="text-muted-foreground grid grid-cols-[auto_1fr] gap-x-4 gap-y-1">
					<dt>Image</dt><dd class="text-foreground">{zo.image.header || "no description"} · {kb(zo.image.size)} of {kb(zo.room)}</dd>
					<dt>For</dt><dd class="text-foreground">manufacturer {hex4(zo.image.manufacturer_code)}, image {hex4(zo.image.image_type)}, version {versionText(zo.image.file_version)}</dd>
					<dt>Given to</dt><dd class="text-foreground">{zo.target ? nameOf(zo.target) : "no device — offered to nobody"}</dd>
				</dl>
			{:else}
				<p class="text-muted-foreground">No image stored.{zo.error ? ` The last one was not kept: ${zo.error}.` : ""}</p>
			{/if}
			{#if targetProgress}
				{#if targetProgress.fraction === null}
					<div class="bg-muted h-2 w-full overflow-hidden rounded-full"><div class="bg-primary h-full w-1/3 animate-pulse rounded-full"></div></div>
				{:else if !targetProgress.done}
					<Progress value={targetProgress.fraction * 100} class="h-2" />
				{/if}
				<p class="text-muted-foreground text-xs">{nameOf(zo.transfer?.ieee ?? zo.target)}: {targetProgress.text}</p>
			{/if}
			{#if zo.last_query}
				<p class="text-muted-foreground text-xs">Last asked {agoS(zo.last_query.s_ago)} by {nameOf(zo.last_query.ieee) || hex4(zo.last_query.short_addr)} (running {versionText(zo.last_query.file_version)}): {zo.last_query.answer}.</p>
			{/if}
			<div class="flex flex-wrap gap-2">
				<Button variant="outline" size="sm" disabled={!s?.zigbee.network_up} onclick={askEveryone}>Ask all devices to check</Button>
				{#if zo.target}<Button variant="ghost" size="sm" onclick={cancelTarget}>Offer it to nobody</Button>{/if}
				{#if zo.image}<Button variant="ghost" size="sm" disabled={zbota.busy || zo.state === "serving"} onclick={removeImage}>Remove image</Button>{/if}
			</div>
			<p class="text-muted-foreground text-xs">Asking every device makes each one that listens say what firmware it runs, so its page can check for updates. Nothing is installed by asking.</p>
		{/if}
	</Card.Content>
</Card.Root>

{#if backupBuilt}
<Card.Root class="mt-4">
	<Card.Header>
		<Card.Title>Backup</Card.Title>
		<Card.Description>Everything set up on this page and the others, in one file: the settings, the names of the lights, the rooms, curves, buttons, wired lights and Matter lamps. Not the Zigbee network itself — after a factory reset, lamps and remotes are paired again and find their names, rooms and buttons waiting.</Card.Description>
	</Card.Header>
	<Card.Content class="flex flex-col gap-3">
		<div class="flex items-center justify-between gap-4">
			<Label for="bkwifi">Include the Wi-Fi network and password</Label>
			<Switch id="bkwifi" checked={withWifi} aria-label="Include the Wi-Fi network and password" onCheckedChange={(on) => setSecret("wifi", on)} />
		</div>
		<div class="text-muted-foreground -mt-2 text-xs">{homeSsid ? `“${homeSsid}”. ` : ""}Without it, a restore from the file keeps the coordinator's own Wi-Fi.</div>
		<div class="flex items-center justify-between gap-4">
			<Label for="bkkeys">Include the access keys</Label>
			<Switch id="bkkeys" checked={withKeys && keyCount !== 0} disabled={keyCount === 0} aria-label="Include the access keys" onCheckedChange={(on) => setSecret("auth", on)} />
		</div>
		<div class="text-muted-foreground -mt-2 text-xs">
			{#if keyCount === 0}This coordinator has no access keys.
			{:else}{keyCount === null ? "" : `${keyCount} ${keyCount === 1 ? "key" : "keys"}, stored as fingerprints. `}Without them, a restore from the file keeps the coordinator's own keys.{/if}
		</div>
		{#if backupWarn}
			<div class="warn" role="note"><TriangleAlert class="mt-0.5 size-4 shrink-0" /><span>{backupWarn}</span></div>
		{:else}
			<div class="text-muted-foreground text-xs">No passwords or keys: a restore from this file keeps the coordinator's own Wi-Fi and access keys.</div>
		{/if}
		<input type="file" accept=".jsonl,.json,.txt,application/x-ndjson,application/json,text/plain" hidden bind:this={restoreInput} onchange={pickRestore} />
		<div class="flex flex-wrap gap-2">
			<Button variant="outline" disabled={backup.busy} onclick={download}><Download /> Download configuration</Button>
			<Button variant="outline" disabled={backup.busy || !s?.zigbee.network_up} onclick={() => restoreInput?.click()}><Upload /> Restore…</Button>
		</div>
		{#if s && !s.zigbee.network_up}<div class="text-muted-foreground text-xs">A restore waits for the Zigbee network to be up: it decides whether the devices' addresses still hold.</div>{/if}
	</Card.Content>
</Card.Root>

<RestoreDialog bind:file={restoring} />
{/if}

<Card.Root class="mt-4">
	<Card.Header><Card.Title>Danger</Card.Title></Card.Header>
	<Card.Content class="flex flex-wrap gap-2">
		<Button variant="outline" onclick={restart}>Restart</Button>
		<Button variant="destructive" onclick={reset}>Factory reset</Button>
	</Card.Content>
</Card.Root>

<style>
	.sel { height: 2.5rem; border-radius: var(--radius-md); border: 1px solid var(--input); background: var(--card);
		color: var(--foreground); padding: 0 .6rem; font-size: .875rem; }
	.link { display: flex; align-items: center; justify-content: space-between; padding: .7rem 0; font-size: .95rem; }
	.link:hover { color: var(--primary); }
	.warn { display: flex; gap: .6rem; padding: .7rem .9rem; border-radius: var(--radius); font-size: .8125rem;
		background: color-mix(in srgb, var(--destructive) 14%, var(--card));
		border: 1px solid color-mix(in srgb, var(--destructive) 45%, transparent); }
</style>
