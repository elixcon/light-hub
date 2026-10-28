/* Host test for the bootloader's ESP-IDF check in main/ota.c.
 *
 * An image the bootloader will not start is only found out after it has been
 * written and the board restarts into it: the updater is then all that runs, or,
 * for a refused updater, the next update has nothing to run in. A check that
 * refuses too much is as bad the other way, since it locks a board out of the
 * builds it can run. Neither shows until somebody installs an older release.
 *
 * idf-ver.inc is extracted from main/ota.c by the Makefile, never copied.
 *
 *   make -C test
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "idf-ver.inc"

static int failures;
static int checks;

static void series(const char *ver, int want)
{
    checks++;
    const int got = idf_series(ver, strlen(ver));
    if (got != want) {
        printf("\n  idf_series(\"%s\") = %d, expected %d", ver, got, want);
        failures++;
    }
}

static void too_old(const char *image, const char *boot, bool want)
{
    checks++;
    if (idf_image_too_old(image, strlen(image), boot, strlen(boot)) != want) {
        printf("\n  an image from \"%s\" under a bootloader from \"%s\" should %sbe refused", image, boot,
               want ? "" : "not ");
        failures++;
    }
}

int main(void)
{
    /* What `git describe` leaves in IDF_VER. */
    series("v6.0.3", 6000);
    series("v6.0", 6000);
    series("v5.5.1", 5005);
    series("v6.0.3-dirty", 6000);
    series("v6.1-dev-1234-gabcdef0", 6001);
    series("v5.10.2", 5010);
    series("6.0.3", 6000);
    /* Not a version: nothing is refused on account of it. */
    series("", -1);
    series("v", -1);
    series("v6", -1);
    series("v6.", -1);
    series("v6-dev", -1);
    series("HEAD-HASH-NOTFOUND", -1);
    series("v1000.0", -1);
    series("v6.1000", -1);

    /* A field that fills its 32 bytes has no NUL, and nothing past it is read. */
    {
        char field[40];
        memset(field, '9', sizeof(field));
        memcpy(field, "v6.0", 4);
        checks++;
        if (idf_series(field, 4) != 6000) {
            printf("\n  idf_series read past the field's size");
            failures++;
        }
    }

    /* The case this exists for: a board with an ESP-IDF 6 bootloader and a
     * release built with ESP-IDF 5.5. */
    too_old("v5.5.1", "v6.0.3", true);
    too_old("v5.5", "v6.0.3", true);
    /* A newer bootloader's series refuses an older one's. */
    too_old("v6.0.3", "v6.1", true);
    too_old("v5.4.2", "v5.5.1", true);
    /* An older bootloader starts newer images. */
    too_old("v6.0.3", "v5.5.1", false);
    too_old("v6.1", "v6.0.3", false);
    /* Bugfix releases do not count, either way. */
    too_old("v6.0.2", "v6.0.3", false);
    too_old("v6.0", "v6.0.3", false);
    too_old("v6.0.3-dirty", "v6.0.3", false);
    /* Major before minor, not text order. */
    too_old("v5.10", "v6.0", true);
    too_old("v6.0", "v5.10", false);
    too_old("v5.10", "v5.9", false);
    /* Unknown on either side refuses nothing: no description is an old bootloader. */
    too_old("v5.5.1", "", false);
    too_old("", "v6.0.3", false);
    too_old("HEAD-HASH-NOTFOUND", "v6.0.3", false);

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("all %d checks passed\n", checks);
    return 0;
}
