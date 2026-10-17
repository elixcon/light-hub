/* Host test for the key format in main/auth.c.
 *
 * A key is meant to be written on a piece of paper and typed back in, possibly
 * years later, on a phone that capitalises the first letter. A fault in the
 * normalisation is somebody locked out with the right key in their hand; a fault
 * in the encoding is a key with less randomness than it looks. Neither shows on
 * hardware until it matters.
 *
 * authkey.inc is extracted from main/auth.c by the Makefile, never copied.
 *
 *   make -C test
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* From auth.h. */
#define ZBC_AUTH_KEY_CHARS     20
#define ZBC_AUTH_KEY_CANON_LEN (ZBC_AUTH_KEY_CHARS + 1)
#define ZBC_AUTH_KEY_SHOWN_LEN (ZBC_AUTH_KEY_CHARS + 3 + 1)

#include "authkey.inc"

static int failures;
static int checks;

static void fail(const char *fmt, const char *a, const char *b)
{
    printf("\n  ");
    printf(fmt, a, b);
    failures++;
}

/* `in` must normalise to `want`, or be rejected when want is NULL. */
static void expect(const char *in, const char *want)
{
    char out[64] = "";
    checks++;
    bool ok = auth_normalize(in, out);
    if (!want) {
        if (ok) {
            fail("\"%s\" was accepted as \"%s\", expected a rejection", in ? in : "(null)", out);
        }
        return;
    }
    if (!ok || strcmp(out, want) != 0) {
        fail("\"%s\" -> \"%s\"", in, ok ? out : "(rejected)");
        printf(", expected \"%s\"", want);
    }
}

int main(void)
{
    static const char canon[] = "K7M2Q9XH4TR3WZP6NBVA";

    /* Every way a person might hand it back. */
    expect("K7M2Q-9XH4T-R3WZP-6NBVA", canon);
    expect("K7M2Q9XH4TR3WZP6NBVA", canon);
    expect("k7m2q-9xh4t-r3wzp-6nbva", canon);
    expect("K7m2Q 9xH4T R3wzP 6NbvA", canon);
    expect("  K7M2Q--9XH4T-R3WZP-6NBVA  ", canon);

    /* The substitutions the alphabet was chosen to make safe. */
    expect("OOOOO-OOOOO-OOOOO-OOOOO", "00000000000000000000");
    expect("iiiii-lllll-IIIII-LLLLL", "11111111111111111111");
    expect("o1i0L-00000-00000-00000", "01101000000000000000");

    /* Not keys. */
    expect(NULL, NULL);
    expect("", NULL);
    expect("-----", NULL);
    expect("K7M2Q-9XH4T-R3WZP-6NBV", NULL);    /* 19 */
    expect("K7M2Q-9XH4T-R3WZP-6NBVAA", NULL);  /* 21 */
    expect("K7M2Q-9XH4T-R3WZP-6NBUA", NULL);   /* right length, but U is not in the alphabet */
    expect("K7M2Q_9XH4T_R3WZP_6NBVA", NULL);
    expect("Bearer K7M2Q9XH4TR3WZP6NBVA", NULL);
    expect("K7M2Q-9XH4T-R3WZP-6NBV\303\204", NULL);

    /* The output buffer is exactly ZBC_AUTH_KEY_CANON_LEN on the device, and a
     * long enough input must be refused before it writes past that - not only
     * rejected afterwards, which the cases above cannot tell apart. */
    {
        static const char *longs[] = {
            "K7M2Q9XH4TR3WZP6NBVAK7M2Q9XH4TR3WZP6NBVA",
            "000000000000000000000",
            "K7M2Q-9XH4T-R3WZP-6NBVA-K7M2Q-9XH4T-R3WZP-6NBVA-K7M2Q",
        };
        for (size_t i = 0; i < sizeof(longs) / sizeof(longs[0]); i++) {
            char buf[ZBC_AUTH_KEY_CANON_LEN + 8];
            memset(buf, '#', sizeof(buf));
            bool ok = auth_normalize(longs[i], buf);
            checks++;
            if (ok) {
                fail("\"%s\" was accepted%s", longs[i], "");
            }
            for (size_t j = ZBC_AUTH_KEY_CANON_LEN; j < sizeof(buf); j++) {
                if (buf[j] != '#') {
                    fail("\"%s\" wrote past the end of the key buffer%s", longs[i], "");
                    break;
                }
            }
        }
    }

    /* Round trips over random keys: what is shown normalises back to exactly
     * what was generated, and looks the way the UI promises. */
    srand(1234);
    for (int n = 0; n < 5000; n++) {
        uint8_t random[ZBC_AUTH_KEY_CHARS];
        for (int i = 0; i < ZBC_AUTH_KEY_CHARS; i++) {
            random[i] = (uint8_t)rand();
        }
        char key[ZBC_AUTH_KEY_CANON_LEN];
        char shown[ZBC_AUTH_KEY_SHOWN_LEN];
        char back[ZBC_AUTH_KEY_CANON_LEN];
        auth_encode(random, key);
        auth_format(key, shown);

        checks++;
        if (strlen(key) != ZBC_AUTH_KEY_CHARS || strlen(shown) != 23 ||
            shown[5] != '-' || shown[11] != '-' || shown[17] != '-') {
            fail("bad shape: \"%s\" shown as \"%s\"", key, shown);
            break;
        }
        checks++;
        if (!auth_normalize(shown, back) || strcmp(back, key) != 0) {
            fail("\"%s\" did not come back as \"%s\"", shown, key);
            break;
        }
        /* Nothing generated may be a character the normaliser would rewrite, or
         * the key someone is shown is not the key that is stored. */
        for (int i = 0; i < ZBC_AUTH_KEY_CHARS; i++) {
            if (strchr("ILOU", key[i])) {
                checks++;
                fail("\"%s\" contains an ambiguous character%s", key, "");
                break;
            }
        }
    }

    /* Every byte value picks a symbol, and each symbol exactly 8 times over the
     * 256 - so a key's 100 bits are really 100 bits. */
    {
        int counts[256] = { 0 };
        for (int b = 0; b < 256; b++) {
            uint8_t random[ZBC_AUTH_KEY_CHARS];
            memset(random, b, sizeof(random));
            char key[ZBC_AUTH_KEY_CANON_LEN];
            auth_encode(random, key);
            counts[(unsigned char)key[0]]++;
        }
        int symbols = 0;
        checks++;
        for (int c = 0; c < 256; c++) {
            if (counts[c] && counts[c] != 8) {
                char s[2] = { (char)c, 0 };
                fail("symbol '%s' picked an uneven number of times%s", s, "");
            }
            symbols += counts[c] ? 1 : 0;
        }
        if (symbols != 32) {
            printf("\n  %d distinct symbols, expected 32", symbols);
            failures++;
        }
    }

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("all %d checks passed\n", checks);
    return 0;
}
