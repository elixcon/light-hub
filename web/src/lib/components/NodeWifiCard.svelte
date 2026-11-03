<script lang="ts">
	/* A Light Node, on the hub's page: its Wi-Fi is off unless somebody asks. */
	import * as Card from "$lib/components/ui/card";
	import { Button } from "$lib/components/ui/button";
	import { post } from "$lib/api.ts";
	import { toast } from "$lib/toast.svelte.ts";
	import type { Device } from "$lib/types.ts";

	let { device }: { device: Device } = $props();

	const control = $derived(device.endpoints.find((e) => e.features.includes("node")));
	const wifi = $derived(control?.node_wifi ?? "off");
	const ip = $derived(control?.node_ip ?? null);
	let asked = $state(false);
	$effect(() => { if (wifi !== "off") asked = false; });

	async function open() {
		try {
			await post(`/api/devices/${device.ieee}/wifi`, {});
			asked = true;
			toast("Asked. It restarts to open Wi-Fi, so its lights blink once.");
		} catch (err: any) { toast(err.message); }
	}
</script>

{#if control}
	<Card.Root class="mt-4">
		<Card.Header>
			<Card.Title>Light Node</Card.Title>
			<Card.Description>What is wired to it is set up on its own page. Its Wi-Fi is off unless asked, and closes again after 15 idle minutes.</Card.Description>
		</Card.Header>
		<Card.Content class="flex flex-col gap-2 text-sm">
			{#if wifi === "station" && ip}
				<div>On Wi-Fi: <a class="text-primary underline underline-offset-4" href="http://{ip}/" target="_blank" rel="noopener">open its page at {ip}</a></div>
			{:else if wifi === "joining"}
				<div class="text-muted-foreground">Joining its Wi-Fi network…</div>
			{:else if wifi === "hotspot"}
				<div>It opened its setup hotspot. Connect to it from a phone. It is off Zigbee until the hotspot closes.</div>
			{:else if asked}
				<div class="text-muted-foreground">Restarting to open Wi-Fi…</div>
			{:else}
				<div><Button variant="outline" onclick={open}>Open Wi-Fi to configure it</Button></div>
			{/if}
		</Card.Content>
	</Card.Root>
{/if}
