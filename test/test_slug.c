/* The name -> DNS label rule from main/settings.c.
 *
 * Lifted rather than copied: see test/Makefile. Nothing here needs ESP-IDF,
 * because slugify() deliberately uses nothing but plain indexing - the one
 * place a fault would show on hardware is a box that comes back at an address
 * nobody can guess, which is the worst possible thing to debug remotely. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

#include "slug.inc"

static int failures;
static int checks;

static void expect(const char *in, const char *want, size_t cap)
{
    char out[64];
    memset(out, '?', sizeof(out));
    checks++;
    bool ok = slugify(in, out, cap);
    if (!want) {
        if (ok) {
            printf("\n  \"%s\" was accepted as \"%s\", expected a rejection", in, out);
            failures++;
        }
        return;
    }
    if (!ok || strcmp(out, want) != 0) {
        printf("\n  \"%s\" -> \"%s\" (ok %d), expected \"%s\"", in, out, ok, want);
        failures++;
    }
}

/* Whatever else it does, the output has to be something a DNS label can be. */
static void expect_label(const char *in, size_t cap)
{
    char out[64];
    checks++;
    if (!slugify(in, out, cap)) {
        return;
    }
    size_t n = strlen(out);
    if (n == 0 || n + 1 > cap) {
        printf("\n  \"%s\" -> length %zu does not fit a %zu-byte buffer", in, n, cap);
        failures++;
    }
    if (out[0] == '-' || out[n - 1] == '-') {
        printf("\n  \"%s\" -> \"%s\" has a hyphen on an end", in, out);
        failures++;
    }
    for (size_t i = 0; i < n; i++) {
        char c = out[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            printf("\n  \"%s\" -> \"%s\" contains 0x%02x", in, out, (unsigned char)c);
            failures++;
        }
        if (c == '-' && i + 1 < n && out[i + 1] == '-') {
            printf("\n  \"%s\" -> \"%s\" has a run of hyphens", in, out);
            failures++;
        }
    }
}

int main(void)
{
    const size_t cap = 32;   /* ZBC_COORD_NAME_LEN */

    /* The ordinary cases. */
    expect("zigbee", "zigbee", cap);
    expect("Living Room Hub", "living-room-hub", cap);
    expect("ZIGBEE", "zigbee", cap);
    expect("hub2", "hub2", cap);

    /* Punctuation becomes a hyphen, and runs of it collapse to one. */
    expect("Attic / Loft", "attic-loft", cap);
    expect("a___b", "a-b", cap);
    expect("a - - b", "a-b", cap);
    expect("a.b.c", "a-b-c", cap);

    /* Ends are trimmed, whichever kind of junk made them. */
    expect("  spaced  ", "spaced", cap);
    expect("---hub---", "hub", cap);
    expect("!hub!", "hub", cap);

    /* A hyphen the name already had is kept, not doubled. */
    expect("living-room", "living-room", cap);

    /* Non-ASCII is bytes, not characters: every byte of a UTF-8 sequence is one
     * hyphen, and the run collapses, so "Kuche" with an umlaut is "k-che". */
    expect("K\303\274che", "k-che", cap);

    /* Nothing usable in it at all is a rejection, not a fallback. */
    expect("", NULL, cap);
    expect("!!!", NULL, cap);
    expect("   ", NULL, cap);
    expect("\303\274", NULL, cap);
    expect(NULL, NULL, cap);
    expect("hub", NULL, 1);   /* no room for even one character plus its NUL */

    /* Truncation. The buffer is the limit, and the cut must not leave a hyphen
     * on the end - which is the case a fixed-length test would miss, because it
     * only happens when the character the cut lands on is a separator. */
    expect("abcdefghij", "abcde", 6);
    expect("abcde fghij", "abcde", 6);      /* cut exactly at the space */
    expect("abcd efghij", "abcd", 6);       /* cut just after it */
    expect("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", cap);

    /* And the invariants, over everything above plus some noise. */
    static const char *corpus[] = {
        "zigbee", "Living Room Hub", "---", "a-b", "  x  ", "K\303\274che", "9",
        "-9-", "A B C D E F G H I J K L M N O P Q R S T U V W X Y Z",
        "................................................", "hub--", "--hub",
    };
    for (size_t i = 0; i < sizeof(corpus) / sizeof(corpus[0]); i++) {
        for (size_t c = 2; c <= cap; c++) {
            expect_label(corpus[i], c);
        }
    }

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("all %d checks passed\n", checks);
    return 0;
}
