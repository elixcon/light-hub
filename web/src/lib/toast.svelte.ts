export const toastState = $state({ text: "", n: 0 });

export function toast(message: string) {
  toastState.text = message;
  toastState.n++;
}
