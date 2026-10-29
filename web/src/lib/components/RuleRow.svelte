<script lang="ts">
	/* One rule, read as a sentence: "When under 30 %  ·  Kitchen  ·  Set to 80 %".
	 * A rule after the first opens with how it relates to the one above: a new
	 * case ("otherwise"), or more of the same case ("and"), which has no
	 * condition of its own. */
	import X from "@lucide/svelte/icons/x";
	import { WHENS, ACTIONS, DELAYS, MAX_DELAY_S, takesValue, isDimAction, canWait, delayLabel, lightChoices, lightKey, isLightRule } from "$lib/buttons.ts";
	import { data } from "$lib/state.svelte.ts";
	import type { Rule, Action, When } from "$lib/types.ts";
	/* `sensor` for a motion sensor's rules, which are never held and so never dim. */
	let { rule, first, showWhen, removable, onremove, sensor = false }:
		{ rule: Rule; first: boolean; showWhen: boolean; removable: boolean; onremove: () => void; sensor?: boolean } = $props();
	const lights = $derived(lightChoices(data.devices));
	const light = $derived(isLightRule(rule) ? lights.find((l) => l.k === lightKey(rule.ieee!, rule.ep!)) : undefined);
	/* A light has no Adaptive Lighting - that is a room's mode - and one that
	 * only switches has no brightness to set or dim. */
	const actions = $derived(ACTIONS.filter(([v]) => (!sensor || !isDimAction(v)) && (!isLightRule(rule) ||
		(v !== "adaptive" && (!light || light.dims || (v !== "set" && !isDimAction(v)))))));
	const joined = $derived(!first && !!rule.and);
	const needsArg = $derived(showWhen && !joined && (rule.when === "below" || rule.when === "above"));
	const target = $derived(isLightRule(rule) ? `l:${lightKey(rule.ieee!, rule.ep!)}` : rule.group ? `g:${rule.group}` : "");
	function setDo(v: Action) {
		/* A dim step and a brightness are both percentages and wildly different
		 * numbers, so crossing between them picks a fresh default. */
		const wasDim = isDimAction(rule.do);
		rule.do = v;
		if (isDimAction(v) && !wasDim) rule.value = 6;
		if (v === "set" && (wasDim || !(rule.value >= 1))) rule.value = 60;
		if (!canWait(v)) rule.delay = 0;
	}
	/* The offered delays, and a number of minutes for any other. A stored delay
	 * that is not one of them opens as that number. */
	let custom = $state(false);
	const delay = $derived(rule.delay ?? 0);
	const delayChoice = $derived(custom || !DELAYS.includes(delay) ? "custom" : String(delay));
	function setDelay(v: string) {
		custom = v === "custom";
		if (!custom) rule.delay = Number(v);
		else if (!rule.delay) rule.delay = 1200;
	}
	const setMinutes = (v: string) => (rule.delay = Math.max(1, Math.min(MAX_DELAY_S, Math.round((Number(v) || 0) * 60))));
	function setTarget(v: string) {
		if (v.startsWith("l:")) {
			const l = lights.find((x) => `l:${x.k}` === v);
			if (!l) return;
			rule.group = 0; rule.ieee = l.ieee; rule.ep = l.ep;
			/* What the light cannot do is not left selected. */
			if (rule.do === "adaptive" || (!l.dims && (rule.do === "set" || isDimAction(rule.do)))) rule.do = "toggle";
		} else {
			rule.group = v.startsWith("g:") ? Number(v.slice(2)) : 0; rule.ieee = null; rule.ep = null;
		}
	}
	const clamp = (v: string) => Math.max(0, Math.min(100, Number(v) || 0));
</script>

<div class="flex flex-wrap items-center gap-2">
	{#if !first}
		<select class="sel" aria-label="How this rule follows the one above" value={rule.and ? "and" : "otherwise"} onchange={(e) => (rule.and = e.currentTarget.value === "and")}>
			<option value="otherwise">Otherwise</option>
			<option value="and">And</option>
		</select>
	{/if}
	{#if showWhen && !joined}
		<select class="sel" aria-label="Condition" value={rule.when} onchange={(e) => (rule.when = e.currentTarget.value as When)}>
			{#each WHENS as [v, l] (v)}<option value={v}>{l}</option>{/each}
		</select>
	{/if}
	{#if needsArg}
		<input class="num" type="number" min="0" max="100" value={rule.arg} aria-label="Threshold" oninput={(e) => (rule.arg = clamp(e.currentTarget.value))} /><span class="text-muted-foreground text-xs">%</span>
	{/if}
	<select class="sel" aria-label="Room or light" value={target} onchange={(e) => setTarget(e.currentTarget.value)}>
		<option value="">— pick a room or a light —</option>
		{#if isLightRule(rule) && !light}<option value={target}>a light that is gone</option>{/if}
		{#if data.groups.length}
			<optgroup label="Rooms">
				{#each data.groups as g (g.id)}<option value={`g:${g.id}`}>{g.name}</option>{/each}
			</optgroup>
		{/if}
		{#if lights.length}
			<optgroup label="Lights">
				{#each lights as l (l.k)}<option value={`l:${l.k}`}>{l.name}</option>{/each}
			</optgroup>
		{/if}
	</select>
	<select class="sel" aria-label="Action" value={rule.do} onchange={(e) => setDo(e.currentTarget.value as Action)}>
		{#each actions as [v, l] (v)}<option value={v}>{l}</option>{/each}
	</select>
	{#if takesValue(rule.do)}
		<input class="num" type="number" min="1" max="100" value={rule.value} aria-label="Amount" oninput={(e) => (rule.value = clamp(e.currentTarget.value))} />
		<span class="text-muted-foreground text-xs">{rule.do === "set" ? "%" : "% steps"}</span>
	{/if}
	{#if canWait(rule.do)}
		<select class="sel" aria-label="When it runs" value={delayChoice} onchange={(e) => setDelay(e.currentTarget.value)}>
			{#each DELAYS as d (d)}<option value={String(d)}>{d ? `after ${delayLabel(d)}` : "at once"}</option>{/each}
			<option value="custom">after…</option>
		</select>
		{#if delayChoice === "custom"}
			<input class="num" type="number" min="1" max="1092" step="1" value={Math.round(delay / 60) || 1} aria-label="Minutes to wait"
				oninput={(e) => setMinutes(e.currentTarget.value)} /><span class="text-muted-foreground text-xs">min</span>
		{/if}
	{/if}
	{#if removable}
		<button class="hover:bg-muted text-muted-foreground inline-flex size-8 items-center justify-center rounded-md" aria-label="Remove this rule" onclick={onremove}><X class="size-4" /></button>
	{/if}
</div>

<style>
	.sel, .num { height: 2.25rem; border-radius: var(--radius-md); border: 1px solid var(--input); background: var(--card);
		color: var(--foreground); padding: 0 .6rem; font-size: .875rem; }
	.num { width: 4.5rem; }
</style>
