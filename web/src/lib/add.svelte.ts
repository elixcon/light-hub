/* The "+" beside the page's title. It belongs to whichever list is showing: a
 * ListHeading with something to add counts itself in while it is on the page
 * (a view has one at most; counted, because the next view's can arrive before
 * the last one's has gone), the button is there while one is, and pressing it
 * opens that list's ways of adding something. `el` is the button, for giving
 * the focus back to. */
export const adder = $state({ lists: 0, open: false, el: null as HTMLElement | null });
