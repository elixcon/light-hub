import { clsx } from "clsx";
import type { ClassValue } from "clsx";
import type { Device } from "$lib/types.ts";

/* shadcn-svelte's cn() is clsx + tailwind-merge. The merge is only needed when
 * two classes on one element set the same property - a caller's over the
 * component's, a size's over the base - where without it the one Tailwind
 * emits later wins, whatever was meant. Nothing here does, test/classes.test.ts
 * keeps it that way, and tailwind-merge is 7 KB of the gzip budget. */
export const cn = (...inputs: ClassValue[]) => clsx(inputs);

/* A Light Node's clusters say so, on its control endpoint. */
export const isNode = (d: Device) => d.endpoints.some((e) => e.features.includes("node"));

export type WithElementRef<T, U extends HTMLElement = HTMLElement> = T & { ref?: U | null };
