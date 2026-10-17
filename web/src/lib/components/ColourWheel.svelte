<script lang="ts">
	/* A hue/saturation wheel: red at east, hue rising anticlockwise, saturation
	 * outwards. The cursor is a child of the element that captures the pointer,
	 * so it may be repositioned freely mid-drag. `idle` dims it and hides the
	 * cursor, but it still takes a pick, which is how a caller lets it change
	 * what the selected thing is. */
	import { drag } from "$lib/drag.ts";
	import { wheelCursor, wheelPick } from "$lib/colorspace.ts";
	let { hue, sat, idle = false, size = "big", onpick, onend }: {
		hue: number; sat: number; idle?: boolean; size?: "big" | "compact";
		onpick: (hue: number, sat: number) => void; onend?: () => void;
	} = $props();
	const pos = $derived(wheelCursor(hue, sat));
	function pick(ev: PointerEvent, r: DOMRect) {
		const p = wheelPick(ev.clientX - (r.left + r.width / 2), ev.clientY - (r.top + r.height / 2), r.width / 2);
		onpick(p.hue, p.sat);
	}
</script>

<div class="wheel {size}" class:idle role="application" aria-label="Colour"
	use:drag={{ onMove: pick, onEnd: () => onend?.() }}>
	<div class="cursor" style:left="{pos[0]}%" style:top="{pos[1]}%" style:background="hsl({hue} {sat}% 55%)"></div>
</div>

<style>
	.wheel {
		position: relative; border-radius: 9999px; touch-action: none; user-select: none;
		background: radial-gradient(circle, #fff 0%, rgb(255 255 255 / 0) 70%),
			conic-gradient(from 90deg, red, magenta, blue, cyan, lime, yellow, red);
		box-shadow: inset 0 0 0 1px rgb(0 0 0 / .08);
	}
	.wheel.big { width: min(240px, 70vw); aspect-ratio: 1; margin: 0 auto; }
	.wheel.compact { width: 150px; aspect-ratio: 1; margin: 0 auto; }
	/* The page widens to 900 px at md; a 150 px wheel is then too fine to aim with a mouse. */
	@media (min-width: 48rem) { .wheel.compact { width: 260px; } }
	.wheel.idle { opacity: .5; cursor: pointer; }
	.wheel.idle .cursor { visibility: hidden; }
	.cursor {
		position: absolute; width: 22px; height: 22px; border-radius: 9999px;
		border: 3px solid #fff; box-shadow: var(--elev-2), 0 0 0 1px rgb(0 0 0 / .15);
		transform: translate(-50%, -50%); pointer-events: none;
	}
</style>
