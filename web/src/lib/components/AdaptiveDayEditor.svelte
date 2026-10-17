<script lang="ts">
	/* A room's Adaptive Lighting day: what it looks like, and when it starts and
	 * ends. Each room has its own; the location and the time zone are the
	 * coordinator's, under Settings. Today's day is drawn from the mirror in
	 * adaptive.ts as it is edited; the coordinator's own figure for now is the
	 * dot on it. Times here are this browser's, but for the sunrise and sunset
	 * limits, which are read in the zone this page sends with the first one. */
	import * as Card from "$lib/components/ui/card";
	import { Button } from "$lib/components/ui/button";
	import { Input } from "$lib/components/ui/input";
	import { Label } from "$lib/components/ui/label";
	import X from "@lucide/svelte/icons/x";
	import CurveChart from "$lib/components/CurveChart.svelte";
	import AdaptiveDay from "$lib/components/AdaptiveDay.svelte";
	import { api, post } from "$lib/api.ts";
	import { toast } from "$lib/toast.svelte.ts";
	import { browserZone, offsetEast } from "$lib/tz.ts";
	import { daySeries, dayAround, dropOn, fitShape, hhmm, localMinutes, parseHhmm, sampleAt, shapeX, SHAPES,
	         usedDay, type Profile, type ShapePoint, type SunDay, type Window } from "$lib/adaptive.ts";
	import type { Point } from "$lib/curves.ts";
	import type { RoomDay, Settings } from "$lib/types.ts";

	let { groupId }: { groupId: number } = $props();

	/* The shape curves hold the four end values and the points between; the
	 * windows and the base are the rest of the day. */
	let pctCurve = $state<Point[]>([[1, 20], [100, 100]]);
	let kCurve = $state<Point[]>([[1, -1500], [100, 500]]);
	let rise = $state<Window>([null, null]);
	let set = $state<Window>([null, null]);
	let kBase = $state(4000);
	let kLimit = $state(3000);
	let timeUtc = $state<number | null>(null);
	let loadedAt = $state(0);
	let sun = $state<RoomDay["sun"]>(null);
	let zone = $state<RoomDay["time_zone"]>(null);
	let boxNow = $state<RoomDay["now"]>(null);
	let tab = $state<"pct" | "k">("pct");
	let loaded = $state(false);

	const browser = browserZone();
	const curveOf = (mid: number, noon: number, pts: [number, number][] | undefined): Point[] =>
		[[1, mid], ...(pts ?? []).map(([x, y]) => [x, y] as Point), [100, noon]];
	const windowOf = (w: { earliest: string | null; latest: string | null } | undefined): Window =>
		[parseHhmm(w?.earliest), parseHhmm(w?.latest)];

	/* Edits since the last answer: an answer to an older save must not undo a
	 * point dragged while it was on its way. */
	let edits = 0;
	function apply(r: RoomDay, day = true) {
		const a = r.day;
		if (day) {
			pctCurve = curveOf(a.pct_midnight, a.pct_noon, a.pct_points);
			kCurve = curveOf(a.k_midnight, a.k_noon, a.k_points);
			rise = windowOf(a.sunrise);
			set = windowOf(a.sunset);
			kBase = a.k_base;
		}
		if (a.offset_limit_k) kLimit = a.offset_limit_k;
		timeUtc = r.time_utc ?? null;
		loadedAt = Date.now();
		sun = r.sun ?? null;
		zone = r.time_zone ?? null;
		boxNow = r.now ?? null;
		loaded = true;
	}
	$effect(() => {
		const id = groupId;
		loaded = false;
		api<RoomDay>(`/api/groups/${id}/day`).then((r) => { if (id === groupId) apply(r); })
			.catch((err: any) => toast(err.message));
	});

	/* The clock moves on between answers. */
	let tick = $state(Date.now());
	$effect(() => {
		const id = setInterval(() => (tick = Date.now()), 30_000);
		return () => clearInterval(id);
	});
	const nowT = $derived(timeUtc === null ? null : timeUtc + (tick - loadedAt) / 1000);

	const ends = (c: Point[]) => ({ mid: c[0][1], noon: c[c.length - 1][1], pts: c.slice(1, -1).map(([x, y]) => [x, y] as ShapePoint) });
	const profile = $derived.by((): Profile => {
		const p = ends(pctCurve), k = ends(kCurve);
		return { pct_midnight: p.mid, pct_noon: p.noon, pct_points: p.pts, k_midnight: k.mid, k_noon: k.noon, k_points: k.pts,
			k_base: kBase, sunrise: rise, sunset: set };
	});
	const anyLimit = $derived([...rise, ...set].some((v) => v !== null));
	/* The offset the limits are read in: the coordinator's zone, or this
	 * browser's, which is sent with the first limit. */
	const offsetS = $derived(zone ? zone.offset_s : offsetEast(Date.now()));

	const sunDay = $derived<SunDay | null>(sun ? { rise: sun.rise, noon: sun.noon, set: sun.set, polar: sun.polar } : null);
	const today = $derived.by(() => {
		const d = new Date(nowT !== null ? nowT * 1000 : tick);
		const from = new Date(d.getFullYear(), d.getMonth(), d.getDate()).getTime() / 1000;
		const to = new Date(d.getFullYear(), d.getMonth(), d.getDate() + 1).getTime() / 1000;
		const noon = new Date(d.getFullYear(), d.getMonth(), d.getDate(), 12).getTime() / 1000;
		return { from, to, noon };
	});
	const samples = $derived(sunDay ? daySeries(sunDay, today.from, today.to, profile, offsetS) : []);
	const realToday = $derived(sunDay && !sunDay.polar ? dayAround(sunDay, today.noon) : null);
	const usedToday = $derived(sunDay && !sunDay.polar ? usedDay(sunDay, today.noon, profile, offsetS) : null);
	const nowPhase = $derived(sunDay && nowT !== null ? sampleAt(sunDay, nowT, profile, offsetS).phase : null);

	const clock = (s: number) => new Date(s * 1000).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
	const zoneLine = $derived.by(() => {
		if (!zone) return browser.name ? `Times are this browser's (${browser.name}).` : "Times are this browser's.";
		const name = zone.name ?? "the coordinator's time zone";
		if (zone.posix === browser.posix) return `Times are in ${name}.`;
		return `Times are in ${name}${browser.name ? `; this browser is in ${browser.name}` : ", not this browser's"}.`;
	});

	function body() {
		const p = profile;
		const clampPct = (v: number) => Math.max(1, Math.min(100, Math.round(v)));
		const win = (w: Window) => ({ earliest: w[0] === null ? null : hhmm(w[0]), latest: w[1] === null ? null : hhmm(w[1]) });
		return {
			pct_midnight: clampPct(p.pct_midnight), pct_noon: clampPct(p.pct_noon),
			k_midnight: Math.round(p.k_midnight), k_noon: Math.round(p.k_noon), k_base: Math.round(kBase),
			pct_points: p.pct_points.map(([x, y]) => [x, clampPct(y)]),
			k_points: p.k_points.map(([x, y]) => [x, Math.round(y)]),
			sunrise: win(rise), sunset: win(set),
		};
	}
	let saveTimer: ReturnType<typeof setTimeout> | undefined;
	function save(delay = 400) {
		edits++;
		clearTimeout(saveTimer);
		saveTimer = setTimeout(async () => {
			const at = edits;
			try {
				/* The limits are clock times in the coordinator's zone, which is
				 * this browser's until it has one. */
				if (!zone && anyLimit) await post<Settings>("/api/settings", { time_zone: browser });
				const r = await post<RoomDay>(`/api/groups/${groupId}/day`, body());
				apply(r, at === edits);
			} catch (err: any) { toast(err.message); }
		}, delay);
	}

	function onDrag(which: "rise" | "set", minutes: number, done: boolean) {
		if (!realToday) return;
		const sunAt = localMinutes(which === "rise" ? realToday.rise : realToday.set);
		if (which === "rise") rise = dropOn(rise, minutes, sunAt);
		else set = dropOn(set, minutes, sunAt);
		if (done) save(0);
	}
	function setBound(which: "rise" | "set", i: 0 | 1, value: string) {
		const v = parseHhmm(value);
		const w: Window = [...(which === "rise" ? rise : set)];
		w[i] = v;
		/* An earliest after the latest: the one just typed wins. */
		const o = i === 0 ? 1 : 0;
		if (v !== null && w[o] !== null && (i === 0 ? v > w[o]! : v < w[o]!)) w[o] = null;
		if (which === "rise") rise = w; else set = w;
		save(0);
	}

	/* An end value typed in, or a point dragged on a shape. */
	function shapeChanged() {
		const fix = (c: Point[], lo: number, hi: number) => { for (const p of c) p[1] = Math.max(lo, Math.min(hi, p[1])); };
		fix(pctCurve, 1, 100);
		fix(kCurve, -kLimit, kLimit);
		save();
	}
	function applyShape(kind: "pct" | "k", name: string) {
		const c = kind === "pct" ? pctCurve : kCurve;
		const mid = c[0][1], noon = c[c.length - 1][1];
		const pts = fitShape(SHAPES[kind][name], mid, noon, kind === "k" ? 50 : 1);
		const next: Point[] = [[1, mid], ...pts.map(([x, y]) => [x, y] as Point), [100, noon]];
		if (kind === "pct") pctCurve = next; else kCurve = next;
		shapeChanged();
	}
	const MARKS = [{ x: 50.5, label: "sunrise · sunset" }];
	const cursor = $derived(nowPhase === null ? null : shapeX(nowPhase));
	const usedLine = $derived.by(() => {
		if (!realToday || !usedToday) return "";
		const moved = [realToday.rise !== usedToday.rise ? `morning from ${clock(usedToday.rise)}` : "",
			realToday.set !== usedToday.set ? `evening until ${clock(usedToday.set)}` : ""].filter(Boolean);
		return moved.length ? `This room counts the ${moved.join(" and the ")}.` : "";
	});
	const noSunLine = $derived(timeUtc === null ? "The coordinator's clock is not set yet, so Adaptive Lighting waits."
		: !sun ? "Enter a location under Settings to see the day drawn and to follow the sun." : "");
</script>

<Card.Root class="mt-4">
	<Card.Header>
		<Card.Title>Adaptive Lighting day</Card.Title>
		<Card.Description>What this room follows while Adaptive Lighting runs: dimmer and warmer towards midnight, brighter and cooler towards noon, with the colour shifted from each light's own warmth curve. Every room has its own day. Changes are saved as you make them.</Card.Description>
	</Card.Header>
	<Card.Content class="flex flex-col gap-4">
		{#if !loaded}
			<div class="text-muted-foreground text-sm">Loading…</div>
		{:else}
			{#if samples.length}
				<div class="flex flex-col gap-2">
					<h3 class="text-sm font-semibold">Today</h3>
					<AdaptiveDay {samples} from={today.from} to={today.to} now={nowT}
						boxDot={boxNow && timeUtc !== null ? { t: timeUtc, pct: boxNow.pct } : null}
						real={realToday} used={usedToday} {kBase} ondrag={onDrag} />
					<div class="text-muted-foreground text-xs">
						The brightness this room gets while it follows the sun, with its colour shift underneath{boxNow ? `; it is at ${boxNow.pct} % now` : ""}.
						Drag the handles to start the day earlier or end it later than the sun. {usedLine}
					</div>
				</div>
			{:else if noSunLine}
				<div class="text-muted-foreground text-xs">{noSunLine}</div>
			{/if}

			<div class="flex flex-col gap-3">
				{#each [["rise", "Sunrise", rise], ["set", "Sunset", set]] as [k, name, w] (k)}
					{@const win = w as Window}
					<div class="flex flex-wrap items-center gap-2 text-sm">
						<span class="w-16 shrink-0 font-medium">{name}</span>
						<span class="text-muted-foreground">not before</span>
						<span class="time">
							<span class="w-28"><Input type="time" step="300" value={hhmm(win[0])} aria-label="{name} not before"
								onchange={(e) => setBound(k as "rise" | "set", 0, e.currentTarget.value)} /></span>
							{#if win[0] !== null}<button class="clear" aria-label="Clear" onclick={() => setBound(k as "rise" | "set", 0, "")}><X class="size-3.5" /></button>{/if}
						</span>
						<span class="text-muted-foreground">not after</span>
						<span class="time">
							<span class="w-28"><Input type="time" step="300" value={hhmm(win[1])} aria-label="{name} not after"
								onchange={(e) => setBound(k as "rise" | "set", 1, e.currentTarget.value)} /></span>
							{#if win[1] !== null}<button class="clear" aria-label="Clear" onclick={() => setBound(k as "rise" | "set", 1, "")}><X class="size-3.5" /></button>{/if}
						</span>
					</div>
				{/each}
				<div class="text-muted-foreground text-xs">
					Empty follows the sun. A sunrise not after 06:30 starts winter mornings at 06:30; a sunset not before 21:00 keeps winter evenings going until then. {zoneLine}
				</div>
			</div>

			<div class="flex flex-col gap-3">
				<div class="bg-muted inline-flex self-start rounded-lg p-0.5" role="tablist">
					<button class="tab" role="tab" aria-selected={tab === "pct"} onclick={() => (tab = "pct")}>Brightness</button>
					<button class="tab" role="tab" aria-selected={tab === "k"} onclick={() => (tab = "k")}>Colour shift</button>
				</div>
				{#if tab === "pct"}
					<div class="max-w-md">
						<CurveChart pts={pctCurve} ensure={() => pctCurve} cursorPct={cursor} onchange={shapeChanged} marks={MARKS}
							labels={{ top: "100 %", left: "midnight", right: "noon" }} ariaLabel="Brightness from solar midnight to noon" />
					</div>
					<div class="grid max-w-md grid-cols-2 gap-3">
						<div>
							<Label for="pmid" class="mb-2 block">At midnight, %</Label>
							<Input id="pmid" type="number" min="1" max="100" bind:value={pctCurve[0][1]} onchange={shapeChanged} />
						</div>
						<div>
							<Label for="pnoon" class="mb-2 block">At noon, %</Label>
							<Input id="pnoon" type="number" min="1" max="100" bind:value={pctCurve[pctCurve.length - 1][1]} onchange={shapeChanged} />
						</div>
					</div>
				{:else}
					<div class="max-w-md">
						<CurveChart pts={kCurve} ensure={() => kCurve} cursorPct={cursor} onchange={shapeChanged} marks={MARKS}
							yRange={{ lo: -kLimit, hi: kLimit, step: 50 }} zeroLabel="0 K"
							labels={{ top: `+${kLimit} K cooler`, left: "midnight", right: "noon" }} ariaLabel="Colour shift from solar midnight to noon" />
					</div>
					<div class="grid max-w-md grid-cols-2 gap-3">
						<div>
							<Label for="kmid" class="mb-2 block">At midnight, K</Label>
							<Input id="kmid" type="number" min={-kLimit} max={kLimit} step="50" bind:value={kCurve[0][1]} onchange={shapeChanged} />
						</div>
						<div>
							<Label for="knoon" class="mb-2 block">At noon, K</Label>
							<Input id="knoon" type="number" min={-kLimit} max={kLimit} step="50" bind:value={kCurve[kCurve.length - 1][1]} onchange={shapeChanged} />
						</div>
					</div>
				{/if}
				<div class="text-muted-foreground text-xs">
					{tab === "pct" ? "How bright" : "How much warmer (below 0) or cooler"} from solar midnight on the left to noon on the right; the
					morning and the evening are the same curve, both crossing the middle at sunrise and sunset. Drag a point to bend it, tap the line to
					add one, drag a point off the chart to remove it.
				</div>
				<div class="flex flex-wrap gap-2">
					{#each Object.keys(SHAPES[tab]) as name (name)}
						<Button variant="outline" size="sm" onclick={() => applyShape(tab, name)}>{name}</Button>
					{/each}
				</div>
			</div>

			<div>
				<Label for="kbase" class="mb-2 block">Lights without a warmth curve start from, K</Label>
				<Input id="kbase" type="number" min="1500" max="8000" step="100" bind:value={kBase} onchange={() => save()} />
			</div>
		{/if}
	</Card.Content>
</Card.Root>

<style>
	.tab { padding: .4rem .9rem; border-radius: 6px; font-size: .85rem; font-weight: 500; color: var(--muted-foreground); }
	.tab[aria-selected="true"] { background: var(--card); color: var(--foreground); box-shadow: var(--elev-1); }
	.time { display: inline-flex; align-items: center; gap: 2px; }
	.clear { display: inline-flex; padding: 4px; border-radius: 4px; color: var(--muted-foreground); }
	.clear:hover { color: var(--foreground); }
</style>
