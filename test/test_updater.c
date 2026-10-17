/* The updater's boot decision from updater/main/updater.c.
 *
 * Lifted rather than copied: see test/Makefile. The two ways this can be wrong
 * are both invisible until they cost a trip to the board: a box that sits in the
 * updater with a working coordinator beside it, and a box that starts an image
 * the bootloader has already rolled back - which, because nothing marks it on
 * trial again, would never be rolled back a second time. */
#include <stdbool.h>
#include <stdio.h>

#include "updater-decide.inc"

static int failures;
static int checks;

static const char *decision_name(upd_decision_t d)
{
    switch (d) {
    case UPD_BOOT_APP:       return "boot app";
    case UPD_WAIT_REQUESTED: return "wait (requested)";
    case UPD_STAY_FAILED:    return "stay (failed)";
    case UPD_STAY_EMPTY:     return "stay (empty)";
    }
    return "?";
}

static const char *state_name(upd_app_state_t s)
{
    switch (s) {
    case UPD_APP_NO_STATE: return "no state";
    case UPD_APP_ACCEPTED: return "accepted";
    case UPD_APP_REJECTED: return "rejected";
    }
    return "?";
}

static void expect(bool requested, upd_app_state_t state, bool valid, upd_decision_t want, const char *why)
{
    checks++;
    const upd_decision_t got = updater_decide(requested, state, valid);
    if (got != want) {
        printf("\n  requested %d, %s, image %s: %s, expected %s - %s", requested, state_name(state),
               valid ? "valid" : "invalid", decision_name(got), decision_name(want), why);
        failures++;
    }
}

int main(void)
{
    /* Not asked for: start whatever verifies and has not been rejected. */
    expect(false, UPD_APP_NO_STATE, true, UPD_BOOT_APP,
           "a fresh USB flash, or a power cut after a handover - both must start the coordinator");
    expect(false, UPD_APP_ACCEPTED, true, UPD_BOOT_APP,
           "a confirmed image, e.g. after the setup hotspot restarted from here");
    expect(false, UPD_APP_REJECTED, true, UPD_STAY_FAILED,
           "the bootloader rolled it back; starting it again would put it beyond rollback");

    /* Asked for: wait, whatever otadata says. */
    expect(true, UPD_APP_NO_STATE, true, UPD_WAIT_REQUESTED, "the ordinary handover");
    expect(true, UPD_APP_ACCEPTED, true, UPD_WAIT_REQUESTED, "a handover whose otadata was not erased");
    expect(true, UPD_APP_REJECTED, true, UPD_WAIT_REQUESTED, "an update asked for from a rolled-back state");

    /* No bootable image: stay, flag or no flag. An upload cut short leaves this. */
    for (int r = 0; r <= 1; r++) {
        for (int s = UPD_APP_NO_STATE; s <= UPD_APP_REJECTED; s++) {
            expect(r, (upd_app_state_t)s, false, UPD_STAY_EMPTY, "there is nothing to go back to");
        }
    }

    /* The property the cases above exist for, over every input: the coordinator
     * is started only from a valid image nobody asked to replace and the
     * bootloader has not rejected. */
    for (int r = 0; r <= 1; r++) {
        for (int s = UPD_APP_NO_STATE; s <= UPD_APP_REJECTED; s++) {
            for (int v = 0; v <= 1; v++) {
                checks++;
                if (updater_decide(r, (upd_app_state_t)s, v) == UPD_BOOT_APP &&
                    !(v && !r && s != UPD_APP_REJECTED)) {
                    printf("\n  requested %d, %s, image %s starts the coordinator", r,
                           state_name((upd_app_state_t)s), v ? "valid" : "invalid");
                    failures++;
                }
            }
        }
    }

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("all %d checks passed\n", checks);
    return 0;
}
