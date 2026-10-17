import { test } from "node:test";
import assert from "node:assert/strict";
import { qrMatrix } from "../src/lib/qr.ts";

/* Generated once with segno 1.6 (Python), never by the code under test:
 *   segno.make(text, error="m", mode="alphanumeric", mask=mask, boost_error=False, micro=False)
 * Each row is the modules left to right as a hex number, quiet zone left out.
 * With no mask, segno's own choice by the standard's penalty rules - so these
 * also pin the mask selection. The first two are pairing payloads; the last two
 * are versions 1 and 3. */
const VECTORS: { text: string; mask: number | undefined; rows: string }[] = [
  { text: "MT:Y.K90AFN00KA0648G00", mask: undefined, rows: "1fd1e7f 104c241 175585d 174e25d 174c35d 105fc41 1fd557f 0004800 146db25 113a0af 01c2ff6 1b1240f 1bf6f24 0008c6f 1cca3d6 040c887 1973bfc 001131f 1fddd5f 104f717 1748dfa 174cd04 175a367 104c92d 1fd3b77" },
  { text: "MT:Y.K9042C00KA0648G00", mask: undefined, rows: "1fc747f 104a141 174b65d 174285d 174505d 1054241 1fd557f 0002b00 12d25a0 08bda05 1dc0c78 1487a37 09d059e 0d22fd1 17e2dfe 0e2e22d 19519f2 001dd17 1fc7755 105d519 17483f2 175a7ae 17480d1 104472d 1fd11f9" },
  { text: "MT:0123456789ABCDE", mask: undefined, rows: "1fc57f 104b41 174b5d 17445d 17455d 105341 1fd57f 000300 12d8a0 01aaf5 09c5ca 0cb458 1be13a 001daf 1fcd6c 105428 174316 175107 174879 1040f5 1fdcfc" },
  { text: "MT:0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-.0123", mask: undefined, rows: "1fd9e17f 10589241 174a175d 175c6a5d 1748565d 104cba41 1fd5557f 001e3e00 16e4a94b 0da5e776 00489688 0b8316e5 1ac76903 1492567f 0967baeb 123059e9 1e5d65df 0628567e 16cdef93 0290c2db 0ccad3fe 001d2519 1fd77559 105bd518 174e2bf4 17573ad2 175d2be9 1042b6cb 1fde0bc0" },
  { text: "MT:Y.K90AFN00KA0648G00", mask: 0, rows: "1fcb47f 1056841 174f25d 174485d 175695d 1045641 1fd557f 000e200 1547112 1b90a05 0b6855c 11b8ea5 115c58e 0aa26c5 166097c 0ea622d 13d91f6 001b915 1fc7755 1045d1d 17527f0 17467ae 17509cd 1046387 1fd91dd" },
  { text: "MT:Y.K90AFN00KA0648G00", mask: 1, rows: "1fd1e7f 104c241 175585d 174e25d 174c35d 105fc41 1fd557f 0004800 146db25 113a0af 01c2ff6 1b1240f 1bf6f24 0008c6f 1cca3d6 040c887 1973bfc 001131f 1fddd5f 104f717 1748dfa 174cd04 175a367 104c92d 1fd3b77" },
  { text: "MT:Y.K90AFN00KA0648G00", mask: 2, rows: "1fc737f 1045041 175355d 175705d 175ae5d 1056e41 1fd557f 001da00 17cb67c 03332e6 0c74240 091b646 1640292 1201e26 117ce60 1605ace 14c57fa 0018116 1fcb159 105651e 175e1fc 1755f4d 175ced1 1045b64 1fd56c1" },
  { text: "MT:Y.K90AFN00KA0648G00", mask: 3, rows: "1fd737f 105e641 174ee5d 175705d 174185d 104b541 1fd557f 0016c00 16e6d4b 03332e6 1aef49b 1236d2b 1640292 049a8fd 0a5150d 1605ace 025e1f1 0015b1b 1fdb159 105d315 1743bf1 1755f4d 175780a 1048009 1fd56c1" },
  { text: "MT:Y.K90AFN00KA0648G00", mask: 4, rows: "1fdfd7f 104de41 174445d 175015d 175205d 105e041 1fd557f 001ab00 11738f9 0d0bcde 1df3387 189c781 1878caa 1c3901e 00fbfa7 0782b09 1afd9f2 0010f1e 1fdc15e 1041519 1756ff4 174d175 174bf16 1042aa3 1fdd8f9" },
  { text: "MT:Y.K90AFN00KA0648G00", mask: 5, rows: "1fc1e7f 105d241 175355d 175b75d 174ae5d 104ec41 1fd557f 0015800 105b6ce 042f5fa 0c74240 0b1344e 1bf6f24 1009c2e 117ce60 1119dd2 14c57fa 001031e 1fcdd5f 104e716 174e1fc 1749851 174ced1 104d96c 1fd3b77" },
  { text: "MT:Y.K90AFN00KA0648G00", mask: 6, rows: "1fd1e7f 105de41 1757c5d 174b75d 1758a5d 1048d41 1fd557f 0005400 13fff97 042f5fa 08e66d2 0a955c8 1bf6f24 1c3901e 1858744 1119dd2 10573f8 0016318 1fddd5f 105eb16 175a9f8 1759851 174ea43 104b8ea 1fd3b77" },
  { text: "MT:Y.K90AFN00KA0648G00", mask: 7, rows: "1fcb47f 1042141 174d65d 174485d 174205d 1057241 1fd557f 000ab00 12d55a0 1b90a05 024cc78 152aa37 115c58e 0386fe1 12f2dee 0ea622d 1afd9f2 0019d17 1fc7755 1051519 17403f2 17567ae 17440e9 1044715 1fd91dd" },
];

const hex = (m: boolean[][]) =>
  m.map((row) => BigInt("0b" + row.map((d) => (d ? "1" : "0")).join("")).toString(16).padStart(Math.ceil(row.length / 4), "0")).join(" ");

test("qrMatrix matches segno module for module", () => {
  for (const { text, mask, rows } of VECTORS) {
    const m = qrMatrix(text, mask);
    assert.ok(m, text);
    assert.equal(hex(m), rows, `${text} mask ${mask ?? "auto"}`);
  }
});

test("qrMatrix refuses what it cannot encode", () => {
  assert.equal(qrMatrix("mt:lowercase"), null);
  assert.equal(qrMatrix("MT:" + "0".repeat(59)), null);   /* 62 characters: past version 3-M */
  assert.ok(qrMatrix("MT:" + "0".repeat(58)));
});
