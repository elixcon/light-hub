/* A promise-shaped confirm dialog, so the views read like the window.confirm
 * calls they replace. One ConfirmDialog in App.svelte renders it. */
export interface ConfirmRequest {
  message: string;
  confirmLabel: string;
  destructive: boolean;
  resolve: (ok: boolean) => void;
}

export const confirmState = $state<{ current: ConfirmRequest | null }>({ current: null });

export function confirm(message: string, opts: { confirmLabel?: string; destructive?: boolean } = {}): Promise<boolean> {
  return new Promise((resolve) => {
    confirmState.current = {
      message,
      confirmLabel: opts.confirmLabel ?? "OK",
      destructive: opts.destructive ?? false,
      resolve: (ok) => { confirmState.current = null; resolve(ok); },
    };
  });
}
