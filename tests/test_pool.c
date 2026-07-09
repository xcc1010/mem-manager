/* Step-2 pool allocator (v0, BUMP) unit test. Built/run only when
 * MEM_MANAGER_POOL is enabled. No framework: asserts + exit code. */
#include <assert.h>
#include <stdio.h>

#include "platform/platform_shm.h"
#include "pool/mm_pool.h"

int main(void) {
    Mm_PoolCfg cfg;
    mm_pool_default_cfg(&cfg);
    cfg.enable = 1;
    cfg.threshold = 0x200000;   /* 2MB */
    cfg.block_size = 0x200000;  /* 2MB */
    cfg.poolable_flags_mask = (1u << 4) | (1u << 5);
    cfg.strategy = MM_ALLOC_BUMP;
    mm_pool_init(&cfg);

    void *a = NULL, *b = NULL, *c = NULL, *big = NULL, *np = NULL;
    INT32 r;

    /* 1) two small poolable allocs (flags=4) pack adjacently in one block */
    r = Platform_ShmMap(4, "obj/a", 4000, &a); assert(r == 1 && a);
    r = Platform_ShmMap(4, "obj/b", 4000, &b); assert(r == 1 && b);
    assert((char*)b - (char*)a == 4000);      /* 16-aligned, packed */

    /* 2) same name -> same slot, refcount increments (name-based sharing) */
    r = Platform_ShmMap(4, "obj/a", 4000, &c); assert(c == a && r == 2);

    /* 3) size >= threshold -> passthrough OS (separate region) */
    r = Platform_ShmMap(4, "obj/big", 0x200000, &big); assert(r >= 0 && big);
    assert((char*)big < (char*)a || (char*)big >= (char*)a + 0x200000);

    /* 4) non-poolable flags (0) -> passthrough OS */
    r = Platform_ShmMap(0, "obj/np", 4000, &np); assert(r >= 0 && np);

    /* unmap: pooled handled (no OS free of a sub-slot); passthrough -> OS */
    assert(Platform_ShmUnmap(a) == 0);        /* obj/a refcount 2 -> 1 */
    assert(Platform_ShmUnmap(c) == 0);        /* obj/a refcount 1 -> 0 */
    Platform_ShmUnmap(big);
    Platform_ShmUnmap(np);

    /* re-attach after release: bump keeps the slot -> same address */
    void* a2 = NULL;
    r = Platform_ShmMap(4, "obj/a", 4000, &a2); assert(a2 == a && r == 1);

    printf("pool v0 (bump) tests passed\n");
    return 0;
}
