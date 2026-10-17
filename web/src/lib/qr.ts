/* The Matter pairing QR code, drawn here rather than by a library: the page
 * lands in the firmware image, and a Matter payload ("MT:" + base38) needs a
 * sliver of QR - alphanumeric mode, error correction M, versions 1-3. All
 * three have a single Reed-Solomon block, so there is no interleaving either.
 * Anything else returns null and the page shows the payload as text only.
 *
 * Tested bit for bit against segno in test/qr.test.ts. */

const ALNUM = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";
const DATA = [16, 28, 44];   /* data codewords, versions 1-3 at M */
const ECC = [10, 16, 26];
const CAP = [20, 38, 61];    /* alphanumeric characters that fit */

const EXP = new Uint8Array(510);
const LOG = new Uint8Array(256);
for (let i = 0, x = 1; i < 255; i++) {
  EXP[i] = EXP[i + 255] = x;
  LOG[x] = i;
  x = (x << 1) ^ (x & 0x80 ? 0x11d : 0);
}
const mul = (a: number, b: number) => (a && b ? EXP[LOG[a] + LOG[b]] : 0);

/* Reed-Solomon remainder of `data` for `n` check codewords. */
function rs(data: number[], n: number): number[] {
  let g = [1];
  for (let i = 0; i < n; i++) {
    const h = [...g, 0];
    for (let j = 0; j < g.length; j++) h[j + 1] ^= mul(g[j], EXP[i]);
    g = h;
  }
  const rem = new Array(n).fill(0);
  for (const d of data) {
    const f = d ^ rem.shift();
    rem.push(0);
    for (let j = 0; j < n; j++) rem[j] ^= mul(g[j + 1], f);
  }
  return rem;
}

const MASKS: ((y: number, x: number) => boolean)[] = [
  (y, x) => (y + x) % 2 == 0,
  (y) => y % 2 == 0,
  (y, x) => x % 3 == 0,
  (y, x) => (y + x) % 3 == 0,
  (y, x) => ((y >> 1) + ((x / 3) | 0)) % 2 == 0,
  (y, x) => ((y * x) % 2) + ((y * x) % 3) == 0,
  (y, x) => (((y * x) % 2) + ((y * x) % 3)) % 2 == 0,
  (y, x) => (((y + x) % 2) + ((y * x) % 3)) % 2 == 0,
];

/* Rows of modules, true for dark, without the quiet zone. `mask` forces one of
 * the eight masks; left out, the one with the lowest penalty is used. */
export function qrMatrix(text: string, mask?: number): boolean[][] | null {
  const v = CAP.findIndex((c) => text.length <= c);
  if (v < 0 || [...text].some((ch) => !ALNUM.includes(ch))) return null;

  const bits: number[] = [];
  const put = (val: number, len: number) => { for (let i = len - 1; i >= 0; i--) bits.push((val >> i) & 1); };
  put(2, 4);
  put(text.length, 9);
  for (let i = 0; i < text.length; i += 2) {
    const a = ALNUM.indexOf(text[i]);
    if (i + 1 < text.length) put(a * 45 + ALNUM.indexOf(text[i + 1]), 11);
    else put(a, 6);
  }
  const cap = DATA[v] * 8;
  put(0, Math.min(4, cap - bits.length));
  put(0, (8 - (bits.length % 8)) % 8);
  for (let p = 0xec; bits.length < cap; p ^= 0xec ^ 0x11) put(p, 8);
  const data: number[] = [];
  for (let i = 0; i < cap; i += 8) data.push(parseInt(bits.slice(i, i + 8).join(""), 2));
  const words = [...data, ...rs(data, ECC[v])];

  const n = 21 + 4 * v;
  const m = Array.from({ length: n }, () => new Array<boolean>(n).fill(false));
  const fn = Array.from({ length: n }, () => new Array<boolean>(n).fill(false));
  const set = (y: number, x: number, dark: boolean) => { m[y][x] = dark; fn[y][x] = true; };

  for (const [fy, fx] of [[0, 0], [0, n - 7], [n - 7, 0]]) {
    for (let dy = -1; dy <= 7; dy++) {
      for (let dx = -1; dx <= 7; dx++) {
        const y = fy + dy, x = fx + dx;
        if (y < 0 || x < 0 || y >= n || x >= n) continue;
        const r = Math.max(Math.abs(dy - 3), Math.abs(dx - 3));
        set(y, x, r != 2 && r != 4);
      }
    }
  }
  for (let i = 8; i < n - 8; i++) { set(6, i, i % 2 == 0); set(i, 6, i % 2 == 0); }
  if (v) {
    for (let dy = -2; dy <= 2; dy++)
      for (let dx = -2; dx <= 2; dx++) set(n - 7 + dy, n - 7 + dx, Math.max(Math.abs(dy), Math.abs(dx)) != 1);
  }
  /* Reserve the format areas (filled once the mask is chosen) and set the dark module. */
  for (let i = 0; i < 9; i++) if (i != 6) { set(8, i, false); set(i, 8, false); }
  for (let i = 0; i < 8; i++) { set(8, n - 1 - i, false); set(n - 1 - i, 8, false); }
  set(n - 8, 8, true);

  let i = 0;
  for (let right = n - 1, up = true; right >= 1; right -= 2, up = !up) {
    if (right == 6) right = 5;
    for (let r = 0; r < n; r++) {
      const y = up ? n - 1 - r : r;
      for (const x of [right, right - 1]) {
        if (fn[y][x]) continue;
        m[y][x] = i < words.length * 8 && ((words[i >> 3] >> (7 - (i & 7))) & 1) == 1;
        i++;
      }
    }
  }

  const masked = (k: number) =>
    m.map((row, y) => row.map((d, x) => (!fn[y][x] && MASKS[k](y, x) ? !d : d)));
  /* Scored before the format bits go in, as the standard says. */
  let k = mask ?? 0;
  if (mask === undefined) {
    for (let j = 1, low = penalty(masked(0)); j < 8; j++) {
      const p = penalty(masked(j));
      if (p < low) { low = p; k = j; }
    }
  }
  const out = masked(k);
  /* Level M is 00, so the format data is the mask alone. */
  let rem = k;
  for (let j = 0; j < 10; j++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
  const f = ((k << 10) | rem) ^ 0x5412;
  const b = (j: number) => ((f >> j) & 1) == 1;
  for (let j = 0; j < 6; j++) out[j][8] = b(j);
  out[7][8] = b(6);
  out[8][8] = b(7);
  out[8][7] = b(8);
  for (let j = 9; j < 15; j++) out[8][14 - j] = b(j);
  for (let j = 0; j < 8; j++) out[8][n - 1 - j] = b(j);
  for (let j = 8; j < 15; j++) out[n - 15 + j][8] = b(j);
  return out;
}

/* The standard's four penalty rules, N1-N4. */
function penalty(m: boolean[][]): number {
  const n = m.length;
  let score = 0, dark = 0;
  const lines = [...m, ...m.map((_, x) => m.map((row) => row[x]))];
  for (const line of lines) {
    const s = line.map((d) => (d ? "1" : "0")).join("");
    for (const run of s.match(/0{5,}|1{5,}/g) ?? []) score += run.length - 2;
    /* 1:1:3:1:1 with four light modules on either side; past the edge is
     * the quiet zone, so light. */
    const p = `0000${s}0000`;
    for (let x = p.indexOf("1011101"); x >= 0; ) {
      const hit = p.slice(x - 4, x) == "0000" || p.slice(x + 7, x + 11) == "0000";
      if (hit) score += 40;
      x = p.indexOf("1011101", x + (hit ? 7 : 4));
    }
  }
  for (let y = 0; y < n; y++) {
    for (let x = 0; x < n; x++) {
      if (m[y][x]) dark++;
      if (y && x && m[y][x] == m[y - 1][x] && m[y][x] == m[y][x - 1] && m[y][x] == m[y - 1][x - 1]) score += 3;
    }
  }
  return score + Math.floor(Math.abs((dark * 100) / (n * n) - 50) / 5) * 10;
}
