/* main/state_rev.c, and one thing more: a change the hub should hear about wakes
 * the attribute sync (node_zb.c), so local_lights.c needs no hook of its own. */
#include "node_zb.h"
#include "state_rev.h"

static uint32_t s_rev;

void zbc_state_bump(void)
{
    __atomic_add_fetch(&s_rev, 1, __ATOMIC_RELAXED);
    node_zb_state_changed();
}

uint32_t zbc_state_rev(void)
{
    return __atomic_load_n(&s_rev, __ATOMIC_RELAXED);
}
