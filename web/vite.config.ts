import { defineConfig } from "vite";
import { svelte } from "@sveltejs/vite-plugin-svelte";
import tailwindcss from "@tailwindcss/vite";
import { viteSingleFile } from "vite-plugin-singlefile";
import path from "node:path";

/* One file, inlined, written straight to main/www/index.html: the coordinator
 * serves exactly one HTML route and gzips whatever is there at build time. */
export default defineConfig(({ mode }) => ({
  plugins: [tailwindcss(), svelte(), viteSingleFile({ removeViteModuleLoader: true })],
  resolve: { alias: { $lib: path.resolve("./src/lib") } },
  publicDir: false,
  base: "./",
  build: {
    /* `--mode node`: the Light Node's own page, from node.html into its firmware. It
     * lands as node.html, which package.json's build:node renames. */
    outDir: mode === "node" ? "../node/main/www" : "../main/www",
    rollupOptions: mode === "node" ? { input: path.resolve("node.html") } : undefined,
    emptyOutDir: false,
    /* Not older: below es2022 Svelte's private class fields are rewritten
     * into WeakMaps and helpers (2.2 KB of the page, CSS included, which
     * follows this target), and the rewritten classes keep SvelteSet and
     * SvelteDate from being tree-shaken. It asks for Safari 15, and
     * Tailwind 4's CSS already needs 16.4. */
    target: "es2022",
    sourcemap: false,
    assetsInlineLimit: () => true,
    cssCodeSplit: false,
    modulePreload: { polyfill: false },
  },
  server: {
    proxy: {
      "/api": process.env.ZBC_TARGET || "http://zigbee.local",
      "/update": process.env.ZBC_TARGET || "http://zigbee.local",
    },
  },
}));
