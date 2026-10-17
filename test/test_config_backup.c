/* The configuration backup's decisions, from main/config_backup.c: which keys a
 * download carries, and what the boot after a restore does with each one.
 *
 * Lifted rather than copied: see test/Makefile. A fault here shows on hardware
 * only after a restore, and then as something that should not be: the Wi-Fi
 * password in a file somebody asked to have none, a box whose access keys a
 * file "without keys" erased, a box restored with no Wi-Fi at all, or the
 * Zigbee channel of an old file moving the coordinator away from its lamps. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfg-plan.inc"

static int failures;
static int checks;

#define CHECK(cond, ...)                                                                                      \
    do {                                                                                                      \
        checks++;                                                                                             \
        if (!(cond)) {                                                                                        \
            printf("\n  ");                                                                                   \
            printf(__VA_ARGS__);                                                                              \
            failures++;                                                                                       \
        }                                                                                                     \
    } while (0)

static const char *action_name(cb_action_t a)
{
    return a == CB_COPY ? "copy" : a == CB_ERASE ? "erase" : "leave";
}

static void expect_secrets(const char *list, bool ok, uint8_t want)
{
    uint8_t got = 0xEE;
    const bool parsed = cb_parse_secrets(list, &got);
    CHECK(parsed == ok, "\"%s\" was %s, expected %s", list ? list : "(null)", parsed ? "accepted" : "refused",
          ok ? "accepted" : "refused");
    if (ok && parsed) {
        CHECK(got == want, "\"%s\" -> 0x%x, expected 0x%x", list ? list : "(null)", got, want);
    }
}

static void test_list(void)
{
    /* Every entry is found by its own key, once, and fits an NVS key. */
    for (size_t i = 0; i < CB_ENTRY_COUNT; i++) {
        const cb_entry_t *e = &CB_ENTRIES[i];
        CHECK(cb_find(e->key) == e, "\"%s\" is not found as itself", e->key);
        CHECK(strlen(e->key) <= 15, "\"%s\" is longer than an NVS key", e->key);
        CHECK(e->sect && !(e->sect & (e->sect - 1)) && (e->sect & ~CB_SECT_ALL) == 0,
              "\"%s\" is not in exactly one section", e->key);
        CHECK(e->type <= CB_BLOB, "\"%s\" has no type", e->key);
        /* Settings are scalars; the blobs, each with its own blob_ok(), are the
         * tables and the keys. */
        CHECK((e->type == CB_BLOB) == (e->sect == CB_SECT_TABLES || e->sect == CB_SECT_AUTH),
              "\"%s\": blobs are the tables and the keys, and only they are", e->key);
    }
    /* Every section has something in it. */
    for (uint8_t s = 1; s & CB_SECT_ALL; s <<= 1) {
        bool any = false;
        for (size_t i = 0; i < CB_ENTRY_COUNT; i++) {
            any |= CB_ENTRIES[i].sect == s;
        }
        CHECK(any, "section 0x%x has no key", s);
    }
    /* What must never travel: the network's channel (restored onto a live
     * network it moves the coordinator alone), runtime state, one-shots. */
    static const char *const NEVER[] = {
        "zb_chan", "zb_upd_id", "al_running", "lstate1", "light1", "crash",
        "zota_len", "zota_crc", "upd_wait", "wifi_setup1", "_apply", "", NULL,
    };
    for (size_t i = 0; i < sizeof(NEVER) / sizeof(NEVER[0]); i++) {
        CHECK(cb_find(NEVER[i]) == NULL, "\"%s\" is in the backup", NEVER[i] ? NEVER[i] : "(null)");
    }
    /* And what must: the secrets where the switches say they are. */
    CHECK(cb_find("wifi_pass") && cb_find("wifi_pass")->sect == CB_SECT_WIFI, "the password is not a Wi-Fi secret");
    CHECK(cb_find("wifi_ssid") && cb_find("wifi_ssid")->sect == CB_SECT_WIFI, "the SSID is not with its password");
    CHECK(cb_find("auth") && cb_find("auth")->sect == CB_SECT_AUTH, "the keys are not the auth secret");
    CHECK(cb_find("devices") && cb_find("devices")->sect == CB_SECT_TABLES, "the registry is not a table");
}

static void test_parse(void)
{
    expect_secrets(NULL, true, 0);
    expect_secrets("", true, 0);
    expect_secrets("wifi", true, CB_SECT_WIFI);
    expect_secrets("auth", true, CB_SECT_AUTH);
    expect_secrets("wifi,auth", true, CB_SECT_SECRETS);
    expect_secrets("auth,wifi", true, CB_SECT_SECRETS);
    expect_secrets("wifi,,auth,", true, CB_SECT_SECRETS);
    expect_secrets(",", true, 0);
    /* A typo is refused, never read as "leave nothing out". */
    expect_secrets("wfi", false, 0);
    expect_secrets("wif", false, 0);
    expect_secrets("wifiauth", false, 0);
    expect_secrets("wifi,aut", false, 0);
    expect_secrets("WIFI", false, 0);
    expect_secrets("wifi ", false, 0);
    /* Only the secrets can be named: the rest always travels. */
    expect_secrets("settings", false, 0);
    expect_secrets("tables", false, 0);
}

static void test_export(void)
{
    for (uint8_t omit = 0; omit <= CB_SECT_ALL; omit++) {
        for (size_t i = 0; i < CB_ENTRY_COUNT; i++) {
            const cb_entry_t *e    = &CB_ENTRIES[i];
            const bool        want = !(e->sect & omit & CB_SECT_SECRETS);
            CHECK(cb_exported(e, omit) == want, "omit 0x%x: \"%s\" %s", omit, e->key,
                  want ? "is missing" : "is in the file");
        }
    }
    CHECK(!cb_exported(cb_find("wifi_pass"), CB_SECT_WIFI), "the password survives omit=wifi");
    CHECK(cb_exported(cb_find("devices"), CB_SECT_ALL), "the tables can be left out");
}

static void test_replaced(void)
{
    CHECK(cb_replaced(0, 0, true) == CB_SECT_ALL, "a plain restore does not replace everything");
    CHECK(cb_replaced(CB_SECT_WIFI, 0, true) == (CB_SECT_ALL & ~CB_SECT_WIFI), "keep=wifi replaces the Wi-Fi");
    CHECK(cb_replaced(CB_SECT_AUTH, 0, true) == (CB_SECT_ALL & ~CB_SECT_AUTH), "keep=auth replaces the keys");
    /* Left out at download: kept, whatever the request says. */
    CHECK(cb_replaced(0, CB_SECT_AUTH, true) == (CB_SECT_ALL & ~CB_SECT_AUTH), "omitted keys are replaced");
    CHECK(cb_replaced(0, CB_SECT_WIFI, true) == (CB_SECT_ALL & ~CB_SECT_WIFI), "an omitted Wi-Fi is replaced");
    /* No network in the file: the box keeps its own. */
    CHECK(cb_replaced(0, 0, false) == (CB_SECT_ALL & ~CB_SECT_WIFI), "a file with no SSID replaces the Wi-Fi");
    /* Only secrets can be kept. */
    CHECK(cb_replaced(CB_SECT_TABLES | CB_SECT_SETTINGS, 0, true) == CB_SECT_ALL, "keep can hold back tables");
    CHECK(cb_replaced(0, CB_SECT_SETTINGS, true) == CB_SECT_ALL, "omitted can hold back settings");
}

static void test_actions(void)
{
    const cb_entry_t *auth = cb_find("auth");
    const cb_entry_t *ssid = cb_find("wifi_ssid");
    const cb_entry_t *name = cb_find("name");

    /* The two the plan names. Keys left out of the file are kept, with nothing
     * asked for: a file made "without keys" must not erase this box's keys. */
    CHECK(cb_action(auth, cb_replaced(0, CB_SECT_AUTH, true), false) == CB_LEAVE,
          "keys omitted at download were touched by a restore");
    /* Keys the file could have had and does not: the box had none. */
    CHECK(cb_action(auth, cb_replaced(0, 0, true), false) == CB_ERASE,
          "a file with no keys left this box's keys in place");
    CHECK(cb_action(auth, cb_replaced(0, 0, true), true) == CB_COPY, "the file's keys were not restored");
    CHECK(cb_action(auth, cb_replaced(CB_SECT_AUTH, 0, true), true) == CB_LEAVE, "keep=auth replaced the keys");

    /* No SSID in the file: the box's Wi-Fi stays, even if a password came. */
    CHECK(cb_action(ssid, cb_replaced(0, 0, false), false) == CB_LEAVE, "a restore left the box with no Wi-Fi");
    CHECK(cb_action(cb_find("wifi_pass"), cb_replaced(0, 0, false), true) == CB_LEAVE,
          "a password without its SSID was restored");

    /* A setting the file does not have is the default again. */
    CHECK(cb_action(name, CB_SECT_ALL, false) == CB_ERASE, "a missing setting kept its old value");
    CHECK(cb_action(name, CB_SECT_ALL, true) == CB_COPY, "a setting was not restored");

    /* And every combination, stated independently of cb_replaced. */
    for (uint8_t keep = 0; keep <= CB_SECT_SECRETS; keep++) {
        for (uint8_t omitted = 0; omitted <= CB_SECT_SECRETS; omitted++) {
            for (int has_ssid = 0; has_ssid <= 1; has_ssid++) {
                const uint8_t replaced = cb_replaced(keep, omitted, has_ssid);
                for (size_t i = 0; i < CB_ENTRY_COUNT; i++) {
                    const cb_entry_t *e = &CB_ENTRIES[i];
                    for (int staged = 0; staged <= 1; staged++) {
                        cb_action_t want = staged ? CB_COPY : CB_ERASE;
                        if (e->sect & (keep | omitted) & CB_SECT_SECRETS) {
                            want = CB_LEAVE;
                        }
                        if (e->sect == CB_SECT_WIFI && !has_ssid) {
                            want = CB_LEAVE;
                        }
                        const cb_action_t got = cb_action(e, replaced, staged);
                        CHECK(got == want, "keep 0x%x omitted 0x%x ssid %d staged %d: \"%s\" %s, expected %s", keep,
                              omitted, has_ssid, staged, e->key, action_name(got), action_name(want));
                    }
                }
            }
        }
    }
}

int main(void)
{
    test_list();
    test_parse();
    test_export();
    test_replaced();
    test_actions();
    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("all %d checks passed\n", checks);
    return 0;
}
