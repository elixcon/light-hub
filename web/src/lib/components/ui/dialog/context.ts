import { getContext, setContext } from "svelte";

/* What dialog.svelte hands the parts inside it. `open` is a getter over the
 * caller's prop, so it stays live. */
export type DialogContext = {
	readonly open: boolean;
	readonly titleId: string;
	readonly descriptionId: string;
	close(): void;
};

const KEY = Symbol("dialog");
export const setDialog = (ctx: DialogContext) => setContext(KEY, ctx);
export const getDialog = () => getContext<DialogContext>(KEY);
