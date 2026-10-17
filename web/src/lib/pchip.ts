/* Mirrors zbc_pchip() in curve_db.c: a Fritsch-Carlson monotone cubic. The
 * tangent clamp is the only thing keeping the curve inside the box its points
 * define; do not simplify it away. Agreement with the C is about 1e-4. */

/* Fritsch-Carlson monotone cubic through (xs, ys), evaluated at t. */
export function pchip(xs: number[], ys: number[], t: number): number {
  const n = xs.length;
  if (n < 2) return n === 1 ? ys[0] : 0;
  if (t <= xs[0]) return ys[0];
  if (t >= xs[n - 1]) return ys[n - 1];

  let i = 0;
  while (i < n - 2 && t > xs[i + 1]) i++;

  const h = xs[i + 1] - xs[i];
  const d = (ys[i + 1] - ys[i]) / h;
  const dp = i > 0 ? (ys[i] - ys[i - 1]) / (xs[i] - xs[i - 1]) : d;
  const dn = i < n - 2 ? (ys[i + 2] - ys[i + 1]) / (xs[i + 2] - xs[i + 1]) : d;

  const tangent = (a: number, b: number, atEnd: boolean) => {
    if (atEnd) return d;
    if (a * b <= 0) return 0;
    const m = (a + b) / 2;
    const limit = 3 * Math.min(Math.abs(a), Math.abs(b));
    return Math.abs(m) > limit ? (m > 0 ? limit : -limit) : m;
  };
  const m0 = tangent(dp, d, i === 0);
  const m1 = tangent(d, dn, i === n - 2);

  const s = (t - xs[i]) / h, s2 = s * s, s3 = s2 * s;
  return (2 * s3 - 3 * s2 + 1) * ys[i] + (s3 - 2 * s2 + s) * h * m0
       + (-2 * s3 + 3 * s2) * ys[i + 1] + (s3 - s2) * h * m1;
}

