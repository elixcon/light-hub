#include "state_rev.h"

static uint32_t s_rev;

void zbc_state_bump(void)
{
    __atomic_add_fetch(&s_rev, 1, __ATOMIC_RELAXED);
}

uint32_t zbc_state_rev(void)
{
    return __atomic_load_n(&s_rev, __ATOMIC_RELAXED);
}
