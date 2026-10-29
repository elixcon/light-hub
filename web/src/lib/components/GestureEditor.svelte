<script lang="ts">
	import { Button } from "$lib/components/ui/button";
	import RuleRow from "./RuleRow.svelte";
	import { blankRule, hasTarget } from "$lib/buttons.ts";
	import type { Rule, Gesture, SensorEvent } from "$lib/types.ts";
	/* `left` is how many more rows the button's pool takes, all its gestures
	 * together. `sensor` for a motion sensor's events, which never dim. */
	let { gesture, label, rules, left, ontest, sensor = false }:
		{ gesture: Gesture | SensorEvent; label: string; rules: Rule[]; left: number; ontest: () => void; sensor?: boolean } = $props();
	const bare = $derived(rules.length === 1 && rules[0].when === "always" && rules[0].do === "none" && !hasTarget(rules[0]));
	/* More than one case: the conditions decide between them. */
	const cases = $derived(rules.filter((r, i) => i === 0 || !r.and).length);
	const note = $derived(gesture === "long"
		? "Hold to sweep. A reversing dim turns round at full and at minimum, and each new hold goes the other way. One hold can dim up to four rooms or lights."
		: cases > 1 ? "Tried in order — the first one that fits wins, with the rules joined to it by “and”." : "");
	function onlyWhen() {
		const r = rules[0];
		r.when = "below";
		/* "under 0 %" is never true, so revealing the condition would hand back a
		 * rule that has quietly stopped working. */
		if (!(r.arg >= 1)) r.arg = 30;
	}
	function addOtherwise() {
		if (left <= 0) return;
		/* Carried over from the case above, because another case is nearly
		 * always the same light doing something else. */
		let head = rules.length - 1;
		while (head > 0 && rules[head].and) head--;
		const prev = rules[head];
		rules.push({ ...blankRule(), group: prev.group, ieee: prev.ieee ?? null, ep: prev.ep ?? null, do: prev.do, value: prev.value, delay: prev.delay ?? 0 });
	}
	function addAnd() {
		if (left <= 0) return;
		/* A second action in the same case is nearly always another light
		 * doing the same thing, so the action is carried over and the target
		 * is left to pick. */
		const prev = rules[rules.length - 1];
		rules.push({ ...blankRule(), and: true, do: prev.do, value: prev.value });
	}
	function remove(i: number) {
		/* A head taken out of a case that goes on hands its condition to the first
		 * rule joined to it, which heads the case from then on. Otherwise the
		 * joined rules would fall into the case above - or, in the first case, lose
		 * the condition that decided them. */
		const r = rules[i];
		const next = rules[i + 1];
		if ((i === 0 || !r.and) && next?.and) {
			next.and = false;
			next.when = r.when;
			next.arg = r.arg;
		}
		rules.splice(i, 1);
	}
</script>

<div class="border-border border-t pt-4">
	<div class="mb-2 flex items-center justify-between">
		<h3 class="font-medium">{label}</h3>
		<Button variant="ghost" size="sm" onclick={ontest}>Test</Button>
	</div>
	<div class="flex flex-col gap-2">
		{#each rules as rule, i (i)}
			<RuleRow {rule} {sensor} first={i === 0} showWhen={cases > 1 || rule.when !== "always"} removable={rules.length > 1} onremove={() => remove(i)} />
		{/each}
	</div>
	{#if !bare}
		<div class="mt-2 flex flex-wrap gap-4 text-sm">
			{#if cases === 1 && rules[0].when === "always"}
				<button class="text-primary hover:underline" onclick={onlyWhen}>+ only when…</button>
			{/if}
			{#if left > 0}
				<button class="text-primary hover:underline" onclick={addAnd}>+ and also…</button>
				<button class="text-primary hover:underline" onclick={addOtherwise}>+ otherwise…</button>
			{:else}
				<span class="text-muted-foreground">This button has all the rules it can hold.</span>
			{/if}
		</div>
	{/if}
	{#if note}<div class="text-muted-foreground mt-2 text-xs">{note}</div>{/if}
</div>
