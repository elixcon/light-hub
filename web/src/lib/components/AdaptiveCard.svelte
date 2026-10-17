<script lang="ts">
	/* Settings -> Adaptive Lighting: where the coordinator is, and the zone the
	 * rooms' sunrise and sunset limits are read in. Each room's day is its own,
	 * edited on the room's page (AdaptiveDayEditor). */
	import * as Card from "$lib/components/ui/card";
	import { Button } from "$lib/components/ui/button";
	import { Input } from "$lib/components/ui/input";
	import { Label } from "$lib/components/ui/label";
	import { post } from "$lib/api.ts";
	import { toast } from "$lib/toast.svelte.ts";
	import { browserZone } from "$lib/tz.ts";
	import type { Settings } from "$lib/types.ts";

	let { cfg }: { cfg: Settings | null } = $props();

	let lat = $state("");
	let lon = $state("");
	let timeUtc = $state<number | null>(null);
	let sun = $state<Settings["sun"]>(null);
	let zone = $state<Settings["time_zone"]>(null);

	const browser = browserZone();

	function apply(c: Settings) {
		lat = c.location ? String(c.location.lat) : "";
		lon = c.location ? String(c.location.lon) : "";
		timeUtc = c.time_utc ?? null;
		sun = c.sun ?? null;
		zone = c.time_zone ?? null;
	}
	$effect(() => { if (cfg) apply(cfg); });

	const clock = (s: number) => new Date(s * 1000).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
	const sunLine = $derived.by(() => {
		if (timeUtc === null) return "The coordinator's clock is not set yet, so Adaptive Lighting waits.";
		if (!sun) return "Enter a location to follow the sun.";
		if (sun.polar === 1) return "The sun does not set here today.";
		if (sun.polar === -1) return "The sun does not rise here today.";
		return `Today: sunrise ${clock(sun.rise)}, solar noon ${clock(sun.noon)}, sunset ${clock(sun.set)}.`;
	});
	const zoneLine = $derived.by(() => {
		if (!zone) return "No time zone yet: the first sunrise or sunset limit a room is given sends this browser's.";
		const name = zone.name ?? "an unnamed time zone";
		if (zone.posix === browser.posix) return `Sunrise and sunset limits are read in ${name}.`;
		return `Sunrise and sunset limits are read in ${name}${browser.name ? `; this browser is in ${browser.name}` : ", not this browser's"}.`;
	});
	const zoneDiffers = $derived(!!zone && zone.posix !== browser.posix);

	const degrees = (s: string) => Number(s.trim().replace(",", "."));
	async function saveLocation() {
		const a = lat.trim(), o = lon.trim();
		if ((a === "") !== (o === "")) return;   /* wait for the other half */
		const location = a === "" ? null : { lat: degrees(a), lon: degrees(o) };
		if (location && (!Number.isFinite(location.lat) || !Number.isFinite(location.lon))) {
			toast("Enter latitude and longitude in decimal degrees, like 48.137");
			return;
		}
		try {
			apply(await post<Settings>("/api/settings", { location }));
			toast(location ? "Location saved" : "Location cleared");
		} catch (err: any) { toast(err.message); }
	}
	async function useBrowserZone() {
		try {
			apply(await post<Settings>("/api/settings", { time_zone: browser }));
			toast("Time zone saved");
		} catch (err: any) { toast(err.message); }
	}
</script>

<Card.Root class="mt-4">
	<Card.Header>
		<Card.Title>Adaptive Lighting</Card.Title>
		<Card.Description>Start it with a room's sun button, a button set to Adaptive Lighting, or from a Matter controller — each room chooses how, under Rooms. The room then follows its own day, set on the room's page, until it is switched off or changed by hand. Here is where the sun is.</Card.Description>
	</Card.Header>
	<Card.Content class="flex flex-col gap-4">
		<div class="grid grid-cols-2 gap-3">
			<div>
				<Label for="lat" class="mb-2 block">Latitude</Label>
				<Input id="lat" bind:value={lat} inputmode="decimal" placeholder="48.137" autocomplete="off" onchange={saveLocation} />
			</div>
			<div>
				<Label for="lon" class="mb-2 block">Longitude</Label>
				<Input id="lon" bind:value={lon} inputmode="decimal" placeholder="11.575" autocomplete="off" onchange={saveLocation} />
			</div>
		</div>
		<div class="text-muted-foreground -mt-2 text-xs">Decimal degrees, north and east positive. {sunLine}</div>
		<div class="text-muted-foreground flex flex-wrap items-center gap-x-3 gap-y-1 text-xs">
			<span>{zoneLine}</span>
			{#if zoneDiffers}
				<Button variant="outline" size="sm" onclick={useBrowserZone}>Use this browser's time zone</Button>
			{/if}
		</div>
	</Card.Content>
</Card.Root>
