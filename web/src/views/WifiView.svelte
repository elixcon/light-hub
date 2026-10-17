<script lang="ts">
	import * as Card from "$lib/components/ui/card";
	import { Switch } from "$lib/components/ui/switch";
	import { Label } from "$lib/components/ui/label";
	import ViewHeader from "$lib/components/ViewHeader.svelte";
	import WifiForm from "$lib/components/WifiForm.svelte";
	import { app } from "$lib/state.svelte.ts";
	import { api, post } from "$lib/api.ts";
	import { toast } from "$lib/toast.svelte.ts";
	import type { Settings } from "$lib/types.ts";
	const w = $derived(app.status?.wifi);

	/* One antenna for both radios, and Zigbee only listens while Wi-Fi is not.
	 * Read once on open; the switch saves straight away and takes effect at once. */
	let sleep = $state<boolean | null>(null);
	$effect(() => {
		api<Settings>("/api/settings").then((cfg) => { sleep = cfg.wifi_sleep ?? true; }).catch(() => {});
	});
	async function saveSleep(on: boolean) {
		const was = sleep;
		sleep = on;
		try { await post("/api/settings", { wifi_sleep: on }); toast(on ? "Wi-Fi will doze between beacons" : "Wi-Fi will stay awake"); }
		catch (err: any) { sleep = was; toast(err.message); }
	}
</script>

<ViewHeader title="Wi-Fi" back="/settings" />
{#if w && w.mode === "sta"}
	<div class="text-muted-foreground mt-1 text-sm">Currently on {w.ssid} ({w.ip}, channel {w.channel}, {w.rssi} dBm). Saving a network restarts the coordinator onto it.</div>
{/if}
<Card.Root class="mt-4"><Card.Content><WifiForm /></Card.Content></Card.Root>

<Card.Root class="mt-4">
	<Card.Header><Card.Title>Sharing the antenna</Card.Title></Card.Header>
	<Card.Content class="flex flex-col gap-2">
		<div class="flex items-center justify-between gap-4">
			<Label for="wsleep">Let Wi-Fi doze between beacons</Label>
			<Switch id="wsleep" checked={sleep === true} disabled={sleep === null} aria-label="Let Wi-Fi doze between beacons" onCheckedChange={saveSleep} />
		</div>
		<div class="text-muted-foreground text-xs">Wi-Fi and Zigbee share one radio, and Zigbee can only listen while Wi-Fi is not. Dozing hands the radio to Zigbee between the access point's beacons, which is what lets a remote's press be heard — a press is one frame, and a remote does not say it again. Turning this off makes the page answer a few tens of milliseconds sooner and remotes miss presses. Devices show what arrived and what did not under Link.</div>
	</Card.Content>
</Card.Root>
