/* Pointer capture on the element the action is on, so a gesture survives the
 * finger leaving the control and a re-render of what is inside it. Nothing
 * inside a dragged container may be the node that captured the pointer. */
import { schedule } from "./state.svelte.ts";

export interface DragHandlers {
  /* Called on pointerdown and every pointermove while captured. Return false
   * from onStart to decline the gesture (the pointer stays uncaptured). */
  onStart?: (ev: PointerEvent, rect: DOMRect) => boolean | void;
  onMove: (ev: PointerEvent, rect: DOMRect) => void;
  onEnd?: (ev: PointerEvent) => void;
}

export function drag(el: HTMLElement | SVGElement, handlers: DragHandlers) {
  let active = false;
  const down = (ev: PointerEvent) => {
    if (ev.button !== 0 && ev.pointerType === "mouse") return;
    const rect = el.getBoundingClientRect();
    if (handlers.onStart && handlers.onStart(ev, rect) === false) return;
    active = true;
    el.setPointerCapture?.(ev.pointerId);
    if (!handlers.onStart) handlers.onMove(ev, rect);
    ev.preventDefault();
  };
  const move = (ev: PointerEvent) => { if (active) handlers.onMove(ev, el.getBoundingClientRect()); };
  const end = (ev: PointerEvent) => {
    if (!active) return;
    active = false;
    handlers.onEnd?.(ev);
    schedule(400);
  };
  el.addEventListener("pointerdown", down as EventListener);
  el.addEventListener("pointermove", move as EventListener);
  el.addEventListener("pointerup", end as EventListener);
  el.addEventListener("pointercancel", end as EventListener);
  return {
    update(h: DragHandlers) { handlers = h; },
    destroy() {
      el.removeEventListener("pointerdown", down as EventListener);
      el.removeEventListener("pointermove", move as EventListener);
      el.removeEventListener("pointerup", end as EventListener);
      el.removeEventListener("pointercancel", end as EventListener);
    },
  };
}
