<script lang="ts">
	/* The tab bar: one solid pill, the current section a darker pill in it. At
	 * the foot of a phone's screen, where a thumb is, and at the top of a wide
	 * one. Each item is its label, its path, and the routes that are in its
	 * section. */
	import { route } from "$lib/router.svelte.ts";
	let { items }: { items: [label: string, href: string, routes: string][] } = $props();
</script>

<nav aria-label="Main">
	{#each items as [label, href, routes] (href)}
		<a href="#{href}" aria-current={routes.split(" ").includes(route.name) ? "page" : undefined}>{label}</a>
	{/each}
</nav>

<style>
	nav {
		position: fixed; z-index: 40; left: .75rem; right: .75rem; bottom: calc(.75rem + env(safe-area-inset-bottom));
		display: flex; gap: 2px; padding: 4px; margin: 0 auto; max-width: 26rem;
		border-radius: 9999px; background: var(--popover); box-shadow: var(--elev-3);
	}
	/* `auto`, not equal shares: what is left over goes round the words, so
	 * "Remotes" is not tighter than "Rooms" on a narrow phone. */
	a {
		flex: auto; display: grid; place-items: center; height: 2.5rem; padding: 0 .25rem;
		border-radius: 9999px; font-size: clamp(.75rem, 3.4vw, .8125rem); font-weight: 600; white-space: nowrap;
	}
	a:hover { background: var(--muted); }
	a[aria-current] { background: var(--nav-on); color: var(--nav-on-foreground); }
	a:focus-visible { outline: 2px solid var(--ring); outline-offset: 2px; }
	@media (min-width: 768px) {
		nav { top: .875rem; bottom: auto; left: 50%; right: auto; translate: -50%; max-width: none; }
		a { height: 2.25rem; padding: 0 1rem; }
	}
</style>
