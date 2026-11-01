/* Host test for the check in main/ota.c that keeps a hub's image off a node and a
 * node's off a hub. Each would start on the other's stored Zigbee network, and nothing
 * on the page says why that goes wrong. A check that refuses too much is as bad: an
 * ordinary update, or a board from before the names existed, must go through.
 *
 * variant.inc is extracted from main/ota.c by the Makefile, never copied.
 *
 *   make -C test
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "variant.inc"

static int failures, checks;

static void other(const char *incoming, const char *installed, bool want)
{
    char a[32] = { 0 }, b[32] = { 0 };   /* as an esp_app_desc_t holds them */
    strncpy(a, incoming, sizeof(a) - 1);
    strncpy(b, installed, sizeof(b) - 1);
    checks++;
    if (image_is_other_variant(a, sizeof(a), b, sizeof(b)) != want) {
        printf("\n  %s over %s: expected %s", incoming, installed, want ? "refused" : "accepted");
        failures++;
    }
}

int main(void)
{
    /* The ordinary update, on either. */
    other("elixcon_light_hub", "elixcon_light_hub", false);
    other("elixcon_light_node", "elixcon_light_node", false);
    /* What this is for. */
    other("elixcon_light_hub", "elixcon_light_node", true);
    other("elixcon_light_node", "elixcon_light_hub", true);
    /* Nothing installed: a first install, or an upload cut short. */
    other("elixcon_light_hub", "", false);
    other("elixcon_light_node", "", false);
    /* A build from before these names, or something else entirely: not ours to judge. */
    other("elixcon_light_hub", "zigbee_coordinator", false);
    other("elixcon_light_node", "esphome", false);
    other("some_other_firmware", "elixcon_light_hub", false);

    if (failures) {
        printf("\n%d of %d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("all %d checks passed\n", checks);
    return 0;
}
