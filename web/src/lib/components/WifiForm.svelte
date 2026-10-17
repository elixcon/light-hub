<script lang="ts">
	/* Scan for networks, take an SSID and a password, restart onto them. The
	 * list is a convenience: typing still works if the scan fails.
	 *
	 * From the setup hotspot the network is checked first (/api/wifi/test): the
	 * coordinator tries it without storing anything, and stores and restarts onto
	 * it only once it has got on - by itself, so that happens even if this page
	 * never hears the answer. A wrong password or a name it cannot find is said
	 * here instead, and the hotspot stays. On a network already there is nothing
	 * to check from: trying another would take this page's own connection away. */
	import { Button } from "$lib/components/ui/button";
	import { Input } from "$lib/components/ui/input";
	import { Label } from "$lib/components/ui/label";
	import { api, post } from "$lib/api.ts";
	import { app } from "$lib/state.svelte.ts";
	import { toast } from "$lib/toast.svelte.ts";

	type Check = { state: string; ssid: string; reason: number; ip: string | null };
	type Outcome = { text: string; bad: boolean; anyway: boolean };

	let ssid = $state("");
	let pass = $state("");
	let networks = $state<{ ssid: string; rssi: number; channel: number; open: boolean }[]>([]);
	let scanning = $state(true);
	let saving = $state(false);
	let joined = $state(false);
	let outcome = $state<Outcome | null>(null);
	const hotspot = $derived(app.status?.wifi.mode === "ap");

	$effect(() => {
		api<{ networks: typeof networks }>("/api/wifi/scan")
			.then((r) => { networks = r.networks.sort((a, b) => b.rssi - a.rssi); })
			.catch(() => {})
			.finally(() => { scanning = false; });
	});
	/* A verdict is about what was typed; typing something else retires it. */
	$effect(() => { void ssid; void pass; outcome = null; });

	async function save() {
		if (!ssid.trim()) { toast("Pick a network first"); return; }
		saving = true;
		try { await post("/api/wifi", { ssid: ssid.trim(), password: pass }); toast(`Saved — restarting and joining ${ssid.trim()}`); }
		catch (err: any) { toast(err.message); }
		finally { saving = false; }
	}

	const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

	function describe(r: Check | null, name: string): Outcome {
		const host = app.status?.wifi.hostname || "zigbee";
		switch (r?.state) {
		case "joined":
			return { text: `${name} took the password. The coordinator got ${r.ip} and is restarting onto it: rejoin your own Wi-Fi and open http://${host}.local${r.ip ? ` or http://${r.ip}` : ""}.`, bad: false, anyway: false };
		case "wrong_password":
			return { text: `Wrong password for ${name}. Nothing was saved; the hotspot stays.`, bad: true, anyway: false };
		case "not_found":
			return { text: `${name} was not found. Check the name, and that the network is 2.4 GHz — the coordinator cannot see 5 GHz.`, bad: true, anyway: true };
		case "no_address":
			return { text: `The password is right, but ${name} gave the coordinator no address.`, bad: true, anyway: true };
		case "not_saved":
			return { text: `The coordinator got onto ${name} but could not store it. Try again.`, bad: true, anyway: false };
		case "failed":
			return { text: `Could not join ${name} (reason ${r!.reason}).`, bad: true, anyway: true };
		default:
			/* Never heard back. The coordinator decides by itself, so either it got on
			 * and is restarting - and this hotspot goes away - or it did not. */
			return { text: `No answer while checking ${name}. If this hotspot goes away, the coordinator got on and is restarting onto your Wi-Fi; if it is still here, try again.`, bad: true, anyway: false };
		}
	}

	async function check() {
		const name = ssid.trim();
		if (!name) { toast("Pick a network first"); return; }
		saving = true;
		try { await post("/api/wifi/test", { ssid: name, password: pass }); }
		catch (err: any) { saving = false; toast(err.message); return; }
		outcome = { text: `Checking ${name}…`, bad: false, anyway: false };
		/* The hotspot moves to the network's channel as the check associates, so a
		 * phone can miss an answer or two: keep asking, each question short, until
		 * well after the coordinator must have decided (20 s for DHCP alone). */
		const until = Date.now() + 45000;
		let r: Check | null = null;
		while (Date.now() < until) {
			await sleep(1000);
			try { r = await api<Check>("/api/wifi/test", { signal: AbortSignal.timeout(3000) }); }
			catch { continue; }
			if (r.state !== "checking") break;
		}
		saving = false;
		joined = r?.state === "joined";
		outcome = describe(r, name);
	}
</script>

<form class="flex flex-col gap-4" onsubmit={(e) => { e.preventDefault(); if (hotspot) check(); else save(); }}>
	<div>
		<Label for="ssid" class="mb-2 block">Network</Label>
		<Input id="ssid" bind:value={ssid} list="ssids" autocomplete="off" placeholder={scanning ? "Scanning…" : "Your Wi-Fi name"} />
		<datalist id="ssids">{#each networks as n (n.ssid)}<option value={n.ssid}>ch {n.channel} · {n.rssi} dBm{n.open ? " · open" : ""}</option>{/each}</datalist>
	</div>
	<div>
		<Label for="pass" class="mb-2 block">Password</Label>
		<Input id="pass" type="password" bind:value={pass} autocomplete="off" placeholder="Leave empty for open networks" />
	</div>
	{#if outcome}
		<p role="status" class="text-sm {outcome.bad ? 'text-destructive' : 'text-muted-foreground'}">{outcome.text}</p>
	{/if}
	<div class="flex flex-wrap gap-2">
		<Button type="submit" disabled={saving || joined}>{hotspot ? "Check & join" : "Save & restart"}</Button>
		{#if outcome?.anyway}
			<Button type="button" variant="outline" disabled={saving} onclick={save}>Save anyway</Button>
		{/if}
	</div>
</form>
