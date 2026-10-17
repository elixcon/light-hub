<script lang="ts">
	import { toastState } from "$lib/toast.svelte.ts";
	let shown = $state(false);
	$effect(() => {
		if (!toastState.n) return;
		shown = true;
		const t = setTimeout(() => (shown = false), 2600);
		return () => clearTimeout(t);
	});
</script>

<div
	class="bg-foreground text-background pointer-events-none fixed bottom-6 left-1/2 z-[60] max-w-[calc(100%-2rem)] -translate-x-1/2 rounded-full px-4 py-2 text-sm shadow-lg transition-[opacity,transform] duration-200"
	class:opacity-0={!shown}
	class:translate-y-2={!shown}
	role="status"
	aria-live="polite"
>
	{toastState.text}
</div>
