<script lang="ts">
	/* A full-height panel from the right, two coloured layers running ahead of
	 * it, and the items rising in one after another. The timing is all CSS: the
	 * opening delays live in the [data-open="true"] rules and the closing ones
	 * in the base rules, because a transition takes its duration and delay from
	 * the state it is going to. */
	import { route, navigate } from "$lib/router.svelte.ts";
	import { openPairing } from "$lib/state.svelte.ts";

	let open = $state(false);
	let scrollY = $state(0);
	let toggleEl: HTMLButtonElement;
	let firstItem = $state<HTMLAnchorElement | null>(null);

	const items: [string, string, string][] = [
		["Rooms", "/groups", "groups"], ["Lights", "/lights", "lights"],
		["Remotes", "/remotes", "remotes"], ["Sensors", "/sensors", "sensors"], ["Settings", "/settings", "settings"],
	];

	/* Any navigation closes it. */
	$effect(() => { void route.name; open = false; });
	$effect(() => { document.body.style.overflow = open ? "hidden" : ""; });
	$effect(() => {
		if (open) setTimeout(() => firstItem?.focus(), 350);
		else if (document.activeElement && document.activeElement !== document.body && !document.activeElement.closest("main")) toggleEl?.focus();
	});

	function pair() {
		open = false;
		openPairing();
	}
</script>

<svelte:window bind:scrollY onkeydown={(e) => { if (e.key === "Escape") open = false; }} />

<!-- Raised once the page has scrolled: it then sits over the cards, where a
     bare icon reads as part of whichever card is under it - and the cards'
     own edit buttons pass under the same corner. -->
<button bind:this={toggleEl} class="sm-toggle" data-open={open} data-raised={scrollY > 8 && !open} aria-expanded={open} aria-controls="sm-panel"
	aria-label={open ? "Close menu" : "Open menu"} onclick={() => (open = !open)}>
	<span class="sm-icon" data-open={open} aria-hidden="true">
		<span class="sm-line t"></span><span class="sm-line m"></span><span class="sm-line b"></span>
	</span>
</button>

<div class="sm" data-open={open} aria-hidden={!open}>
	<button class="sm-backdrop" tabindex="-1" aria-label="Close menu" onclick={() => (open = false)}></button>
	<div class="sm-prelayer" style:--i={0}></div>
	<div class="sm-prelayer" style:--i={1}></div>
	<nav class="sm-panel" id="sm-panel" aria-label="Main">
		<ul>
			{#each items as [label, href, name], i (name)}
				<li style:--i={i}>
					{#if i === 0}
						<a bind:this={firstItem} href="#{href}" class="sm-item" aria-current={route.name.startsWith(name.slice(0, -1)) ? "page" : undefined}
							onclick={() => (open = false)}><span class="sm-label">{label}</span></a>
					{:else}
						<a href="#{href}" class="sm-item" aria-current={route.name.startsWith(name.slice(0, -1)) ? "page" : undefined}
							onclick={() => (open = false)}><span class="sm-label">{label}</span></a>
					{/if}
				</li>
			{/each}
		</ul>
		<div class="sm-foot" style:--i={4}>
			<button class="sm-pair" onclick={pair}>Pair a device</button>
		</div>
	</nav>
</div>

<style>
	.sm-toggle {
		position: fixed; top: 10px; right: 10px; z-index: 51;
		width: 44px; height: 44px; border: 0; border-radius: 9999px;
		background: transparent; color: var(--foreground); cursor: pointer;
		display: grid; place-items: center; transition: color .3s .18s, background-color .2s, box-shadow .2s;
	}
	.sm-toggle:hover { background: color-mix(in srgb, var(--foreground) 6%, transparent); }
	.sm-toggle[data-raised="true"] { background: var(--card); box-shadow: var(--elev-2); }
	.sm-toggle[data-raised="true"]:hover { background: color-mix(in srgb, var(--foreground) 6%, var(--card)); }
	.sm-toggle[data-open="true"] { color: var(--foreground); }
	.sm-toggle:focus-visible { outline: 2px solid var(--ring); outline-offset: 2px; }
	/* Three bars; opening folds the outer two into an X and fades the middle one. */
	.sm-icon { position: relative; display: block; width: 18px; height: 14px; }
	.sm-line { position: absolute; left: 0; right: 0; height: 2px; background: currentColor; border-radius: 2px;
		transition: transform .35s cubic-bezier(.65,.05,.36,1), opacity .2s; }
	.sm-line.t { top: 0; }
	.sm-line.m { top: 6px; }
	.sm-line.b { top: 12px; }
	.sm-icon[data-open="true"] .sm-line { transition: transform .6s cubic-bezier(.19,1,.22,1), opacity .2s; }
	.sm-icon[data-open="true"] .sm-line.t { transform: translateY(6px) rotate(45deg); }
	.sm-icon[data-open="true"] .sm-line.m { opacity: 0; }
	.sm-icon[data-open="true"] .sm-line.b { transform: translateY(-6px) rotate(-45deg); }

	.sm { position: fixed; inset: 0; z-index: 50; pointer-events: none; }
	.sm[data-open="true"] { pointer-events: auto; }
	.sm-backdrop { position: absolute; inset: 0; border: 0; padding: 0;
		background: rgb(0 0 0 / .35); opacity: 0; transition: opacity .3s; cursor: default; }
	.sm[data-open="true"] .sm-backdrop { opacity: 1; }

	.sm-prelayer, .sm-panel {
		position: absolute; top: 0; right: 0; height: 100%;
		width: clamp(280px, 38vw, 420px);
		transform: translateX(100%);
		transition: transform .32s cubic-bezier(.55,.06,.68,.19);
	}
	.sm-prelayer { background: var(--menu-c1); }
	.sm-prelayer:nth-of-type(2) { background: var(--menu-c2); }
	.sm-panel {
		background: var(--menu-panel); color: var(--foreground);
		border-left: 1px solid var(--border); box-shadow: var(--elev-3);
		display: flex; flex-direction: column;
		padding: calc(5rem + env(safe-area-inset-top)) 2rem calc(2rem + env(safe-area-inset-bottom));
		overflow-y: auto; visibility: hidden;
		transition: transform .32s cubic-bezier(.55,.06,.68,.19), visibility 0s .32s;
	}
	.sm[data-open="true"] .sm-prelayer { transform: none;
		transition: transform .5s cubic-bezier(.19,1,.22,1) calc(var(--i) * .07s); }
	.sm[data-open="true"] .sm-panel { transform: none; visibility: visible;
		transition: transform .65s cubic-bezier(.19,1,.22,1) .22s, visibility 0s; }

	ul { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: .35rem; counter-reset: smi; }
	li { position: relative; overflow: hidden; line-height: 1; padding-right: 3.2rem; }
	.sm-item {
		display: block; text-decoration: none; color: inherit;
		font-size: clamp(2.4rem, 9vw, 3.6rem); font-weight: 650; letter-spacing: -.03em;
		text-transform: uppercase; line-height: 1.05; padding: .05em 0;
		transition: color .25s;
	}
	.sm-item[aria-current="page"] { color: var(--primary); }
	.sm-item:hover { color: var(--hover); }
	.sm-item:focus-visible { outline: 2px solid var(--ring); outline-offset: 4px; border-radius: 4px; }
	.sm-label { display: inline-block; transform: translateY(140%) rotate(10deg); transform-origin: 0 100%;
		transition: transform 0s .32s; }
	.sm[data-open="true"] .sm-label { transform: none;
		transition: transform 1s cubic-bezier(.19,1,.22,1) calc(.3s + var(--i) * .1s); }
	li::after {
		counter-increment: smi; content: "0" counter(smi);
		position: absolute; right: .2rem; top: .35em;
		font-size: .95rem; font-weight: 500; color: var(--primary);
		opacity: 0; transition: opacity 0s .32s;
	}
	.sm[data-open="true"] li::after { opacity: 1; transition: opacity .6s ease-out calc(.35s + var(--i) * .08s); }

	.sm-foot { margin-top: auto; padding-top: 2rem; display: flex; flex-wrap: wrap; align-items: center; gap: 1rem;
		opacity: 0; transform: translateY(20px); transition: opacity 0s .32s, transform 0s .32s; }
	.sm[data-open="true"] .sm-foot { opacity: 1; transform: none;
		transition: opacity .5s ease-out .55s, transform .55s cubic-bezier(.19,1,.22,1) .55s; }
	.sm-pair { border: 0; border-radius: 9999px; padding: .8rem 1.4rem; font: inherit; font-weight: 600;
		background: var(--primary); color: var(--primary-foreground); cursor: pointer; box-shadow: var(--elev-2); }
	.sm-pair:hover { filter: brightness(1.05); }

	@media (max-width: 640px) { .sm-prelayer, .sm-panel { width: 100%; } }
	@media (prefers-reduced-motion: reduce) {
		.sm-prelayer, .sm-panel, .sm-label, .sm-line, .sm-foot, .sm-backdrop, li::after {
			transition-duration: .01ms !important; transition-delay: 0s !important;
		}
	}
</style>
