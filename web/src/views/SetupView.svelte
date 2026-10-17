<script lang="ts">
	/* The provisioning portal: the same page, opened by the captive portal on
	 * the coordinator's own hotspot, with nothing to do but pick a network. */
	import * as Card from "$lib/components/ui/card";
	import WifiForm from "$lib/components/WifiForm.svelte";
</script>

<h2 class="mt-5 text-lg font-semibold">Connect to Wi-Fi</h2>
<div class="text-muted-foreground mt-1 text-sm">The coordinator is running its own setup network. Pick your Wi-Fi: it checks the password first, and restarts onto the network only once it has got on.</div>
<Card.Root class="mt-4"><Card.Content><WifiForm /></Card.Content></Card.Root>
