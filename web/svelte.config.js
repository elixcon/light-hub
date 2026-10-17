import { vitePreprocess } from "@sveltejs/vite-plugin-svelte";
/* discloseVersion: no `window.__svelte` version stamp in the page. */
export default { preprocess: vitePreprocess(), compilerOptions: { discloseVersion: false } };
