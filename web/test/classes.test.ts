import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";

/* `cn` is plain clsx, not tailwind-merge (lib/utils.ts): nothing drops a class
 * that another overrides. When two classes on one element set the same
 * property, the one Tailwind happens to emit later wins - by number, by
 * alphabet - whatever the markup meant. A small button's icon was 16 px for
 * that, not 14. So a component's base, its variant and its size, and a
 * caller's classes, never set a property the others set. A class behind a
 * variant (`hover:`, `data-new:`) is its own thing: Tailwind emits variants
 * after plain utilities, so one of those does override on purpose. */

const src = new URL("../src/", import.meta.url);
const read = (p: string) => readFileSync(new URL(p, src), "utf8");

/* The properties a utility sets, coarse; anything not listed is its own. */
function props(util: string): string[] {
  const u = util.replace(/^-/, "").replace(/!$/, "");
  const sides = (p: string, s: string) =>
    ({ x: ["l", "r"], y: ["t", "b"], "": ["t", "r", "b", "l"] } as Record<string, string[]>)[s]?.map((d) => p + d) ?? [p + s];
  let m: RegExpMatchArray | null;
  if (/^(block|inline|inline-block|inline-flex|flex|grid|hidden|contents)$/.test(u)) return ["display"];
  if (/^(relative|absolute|fixed|sticky|static)$/.test(u)) return ["position"];
  if ((m = u.match(/^inset-/))) return ["top", "right", "bottom", "left"];
  if ((m = u.match(/^(top|right|bottom|left)-/))) return [m[1]];
  if (u.startsWith("size-")) return ["width", "height"];
  if ((m = u.match(/^(min-w|max-w|w|min-h|max-h|h)-/))) return [m[1] === "w" ? "width" : m[1] === "h" ? "height" : m[1]];
  if ((m = u.match(/^([pm])([xytrbl]?)-/))) return sides(m[1], m[2]);
  if ((m = u.match(/^gap-(?:([xy])-)?/))) return m[1] ? ["gap-" + m[1]] : ["gap-x", "gap-y"];
  if (/^rounded(-|$)/.test(u)) return ["radius"];
  if (/^text-(xs|sm|base|lg|[2-9]?xl|\[[\d.]+(rem|px|em)\])$/.test(u)) return ["font-size"];
  if (/^text-(left|center|right|justify)$/.test(u)) return ["text-align"];
  if (u.startsWith("text-")) return ["color"];
  if (/^font-(thin|extralight|light|normal|medium|semibold|bold|extrabold|black)$/.test(u)) return ["font-weight"];
  if (u.startsWith("font-")) return ["font-family"];
  if (u.startsWith("bg-")) return ["background"];
  if (/^border(-[xytrbl])?(-\d+)?$/.test(u)) return ["border-width"];
  if (/^border-(solid|dashed|dotted|double|none)$/.test(u)) return ["border-style"];
  if (u.startsWith("border-")) return ["border-color"];
  if (/^ring(-\d+)?$/.test(u)) return ["ring-width"];
  if (u.startsWith("ring-")) return ["ring-color"];
  if (/^shadow(-(2xs|xs|sm|md|lg|xl|2xl|none))?$/.test(u)) return ["shadow"];
  if ((m = u.match(/^\[(--[\w-]+):/))) return [m[1]];
  for (const p of ["pointer-events", "whitespace", "transition", "translate-x", "translate-y", "overflow", "outline",
    "select", "cursor", "shrink", "grow", "items", "justify", "self", "opacity", "z", "leading", "underline-offset"])
    if (u === p || u.startsWith(p + "-")) return [p];
  return [u];
}

/* A class as its variant prefix and utility: `data-[state=checked]:bg-on`. */
function split(cls: string): [string, string] {
  const parts: string[] = [];
  let depth = 0, from = 0;
  for (let i = 0; i < cls.length; i++) {
    const c = cls[i];
    if (c === "[" || c === "(") depth++;
    else if (c === "]" || c === ")") depth--;
    else if (c === ":" && depth === 0) { parts.push(cls.slice(from, i)); from = i + 1; }
  }
  return [parts.join(":"), cls.slice(from)];
}

/* Two different classes of `classes` that set one property behind one prefix. */
function clashes(classes: string[]): string[] {
  const seen = new Map<string, string>(), out = new Set<string>();
  for (const cls of new Set(classes)) {
    const [prefix, util] = split(cls);
    for (const p of props(util)) {
      const key = `${prefix}|${p}`, other = seen.get(key);
      if (other && other !== cls) out.add(`${other} / ${cls}`);
      else seen.set(key, cls);
    }
  }
  return [...out];
}

const words = (s: string) => s.split(/\s+/).filter(Boolean);
const constant = (text: string, name: string) => text.match(new RegExp(`const ${name} =\\s*"([^"]*)"`))![1];
const map = (text: string, name: string) => {
  const body = text.match(new RegExp(`const ${name} = \\{([\\s\\S]*?)\\};`))![1];
  return Object.fromEntries([...body.matchAll(/("?[\w-]+"?):\s*"([^"]*)"/g)].map((m) => [m[1].replace(/"/g, ""), m[2]]));
};
/* A component whose classes are one string, or strings, passed to cn(). */
const cnStrings = (text: string) => [...text.matchAll(/cn\(\s*"([^"]*)"/g)].flatMap((m) => words(m[1]));

const button = read("lib/components/ui/button/button.svelte"), badge = read("lib/components/ui/badge/badge.svelte");
const B = { base: constant(button, "BASE"), variants: map(button, "VARIANTS"), sizes: map(button, "SIZES") };
const G = { base: constant(badge, "BASE"), variants: map(badge, "VARIANTS") };

/* What the component puts on the element, for one tag's attributes. */
const COMPONENTS: Record<string, (attr: (name: string) => string | undefined) => string[]> = {
  Button: (attr) => {
    const vs = attr("variant") ? [attr("variant")!] : ["default"], ss = attr("size") ? [attr("size")!] : ["default"];
    return vs.flatMap((v) => ss.flatMap((s) => words(`${B.base} ${B.variants[v]} ${B.sizes[s]}`)));
  },
  Badge: (attr) => words(`${G.base} ${G.variants[attr("variant") ?? "default"]}`),
  "Card.Root": () => cnStrings(read("lib/components/ui/card/card.svelte")),
  Input: () => cnStrings(read("lib/components/ui/input/input.svelte")),
  Switch: () => cnStrings(read("lib/components/ui/switch/switch.svelte")),
};

test("a button's base, variant and size never set the same property", () => {
  for (const [v, vc] of Object.entries(B.variants))
    for (const [s, sc] of Object.entries(B.sizes))
      assert.deepEqual(clashes(words(`${B.base} ${vc} ${sc}`)), [], `button ${v} ${s}`);
  for (const [v, vc] of Object.entries(G.variants))
    assert.deepEqual(clashes(words(`${G.base} ${vc}`)), [], `badge ${v}`);
  for (const name of ["Card.Root", "Input", "Switch"])
    assert.deepEqual(clashes(COMPONENTS[name](() => undefined)), [], name);
});

/* Every tag of a component in the page's source, as its attribute text. Braces
 * are followed, so an arrow function's `>` does not end the tag. */
function tags(text: string, name: string): string[] {
  const out: string[] = [];
  const re = new RegExp(`<${name.replace(".", "\\.")}(?=[\\s/>])`, "g");
  for (const m of text.matchAll(re)) {
    let i = m.index! + m[0].length, depth = 0, quote = "";
    for (; i < text.length; i++) {
      const c = text[i];
      if (quote) { if (c === quote) quote = ""; continue; }
      if (c === "{") depth++;
      else if (c === "}") depth--;
      else if ((c === '"' || c === "'") && depth === 0) quote = c;
      else if (c === ">" && depth === 0) break;
    }
    out.push(text.slice(m.index! + m[0].length, i));
  }
  return out;
}

/* The classes a tag passes: the static words of `class="..."`, and the string
 * literals inside any braces there or in `class={...}`. */
function passed(tag: string): string[] {
  const m = tag.match(/\bclass=("((?:[^"{]|\{[^}]*\})*)"|\{([^}]*)\})/);
  if (!m) return [];
  const value = m[2] ?? m[3];
  const literals = [...value.matchAll(/"([^"]*)"|'([^']*)'/g)].flatMap((l) => words(l[1] ?? l[2]));
  const bare = m[2] !== undefined ? words(value.replace(/\{[^}]*\}/g, " ")) : [];
  return [...bare, ...literals];
}

function* svelteFiles(dir: URL): Generator<URL> {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    if (e.isDirectory()) yield* svelteFiles(new URL(`${e.name}/`, dir));
    else if (e.name.endsWith(".svelte")) yield new URL(e.name, dir);
  }
}

test("no caller sets a property the component already sets", () => {
  let checked = 0;
  for (const file of svelteFiles(src)) {
    const text = readFileSync(file, "utf8");
    for (const [name, own] of Object.entries(COMPONENTS))
      for (const tag of tags(text, name)) {
        const mine = passed(tag);
        if (!mine.length) continue;
        const attr = (a: string) => tag.match(new RegExp(`\\b${a}="([^"]*)"`))?.[1];
        const theirs = own(attr);
        /* A caller's own classes may be alternatives (`a ? "x" : "y"`); only a
         * clash between one of them and the component's counts. */
        const bad = clashes([...theirs, ...mine]).filter((c) => {
          const [a, b] = c.split(" / ");
          return mine.includes(a) !== mine.includes(b);
        });
        assert.deepEqual(bad, [], `${file.pathname.split("/src/")[1]}: <${name} class="${mine.join(" ")}">`);
        checked++;
      }
  }
  assert.ok(checked > 10, `found ${checked} tags with classes`);
});

test("the checks know a clash when they see one", () => {
  assert.deepEqual(clashes(["h-8", "h-10"]), ["h-8 / h-10"]);
  assert.deepEqual(clashes(["[&_svg]:size-4", "[&_svg]:size-3.5"]), ["[&_svg]:size-4 / [&_svg]:size-3.5"]);
  assert.deepEqual(clashes(["ring-edge", "data-new:ring-primary/60"]), [], "a variant overrides on purpose");
  assert.deepEqual(clashes(["text-sm", "text-primary"]), [], "size and colour are two properties");
  assert.equal(clashes(["px-4", "p-2"]).length, 1, "p sets px");
  assert.equal(clashes(["border-border", "border-transparent"]).length, 1);
  assert.deepEqual(clashes(["border", "border-transparent"]), [], "width and colour");
  assert.deepEqual(passed(`variant="ghost" class={on ? "text-primary bg-muted" : "text-muted-foreground"} onclick={() => x > 1}`),
    ["text-primary", "bg-muted", "text-muted-foreground"]);
  assert.deepEqual(passed(`class="lamp lift {isNew ? 'ring-primary/60' : ''}"`), ["lamp", "lift", "ring-primary/60"]);
});
