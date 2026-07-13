/* Step-2 pool allocator (v1, BUMP) unit test. Built/run only when
 * MEM_MANAGER_POOL is enabled. No framework: asserts + exit code. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "platform/platform_shm.h"
#include "pool/mm_pool.h"

/* ---- single-threaded functional checks ---- */
static void test_functional(void) {
    void *a = NULL, *b = NULL, *c = NULL, *big = NULL, *np = NULL;
    INT32 r;

    /* two small poolable allocs (flags=4) pack adjacently in one block */
    r = Platform_ShmMap(4, "obj/a", 4000, &a); assert(r == 1 && a);
    r = Platform_ShmMap(4, "obj/b", 4000, &b); assert(r == 1 && b);
    assert((char*)b - (char*)a == 4000);          /* 16-aligned, packed */

    /* same name -> same slot; refcount is attach-monotonic under bump */
    r = Platform_ShmMap(4, "obj/a", 4000, &c); assert(c == a && r == 2);

    /* size >= threshold -> passthrough OS (separate region) */
    r = Platform_ShmMap(4, "obj/big", 0x200000, &big); assert(r >= 0 && big);
    assert((char*)big < (char*)a || (char*)big >= (char*)a + 0x200000);

    /* non-poolable flags (0) -> passthrough OS */
    r = Platform_ShmMap(0, "obj/np", 4000, &np); assert(r >= 0 && np);

    /* unmap: pooled detected (not OS-freed); bump does not reclaim */
    assert(Platform_ShmUnmap(a) == 0);
    assert(Platform_ShmUnmap(c) == 0);
    Platform_ShmUnmap(big);
    Platform_ShmUnmap(np);

    /* re-attach: same slot; bump keeps it, refcount keeps climbing (3rd attach) */
    void* a2 = NULL;
    r = Platform_ShmMap(4, "obj/a", 4000, &a2); assert(a2 == a && r == 3);

    printf("functional: ok\n");
}

/* ---- concurrency: lock-free attach + create race under contention ---- */
#define NT     8
#define ITERS  500
static const char* SHARED[4] = { "s/0", "s/1", "s/2", "s/3" };
static _Atomic(void*) g_shared[4];

static void* worker(void* arg) {
    intptr_t id = (intptr_t)arg;
    for (int it = 0; it < ITERS; it++) {
        int k = it & 3;
        void* p = NULL; INT32 r;
        r = Platform_ShmMap(4, (char*)SHARED[k], 128, &p);
        assert(r >= 1 && p);
        /* every thread must resolve a shared name to the SAME slot */
        void* seen = NULL;
        atomic_compare_exchange_strong(&g_shared[k], &seen, p);
        assert(p == atomic_load(&g_shared[k]));
        Platform_ShmUnmap(p);

        /* a per-thread unique name exercises the locked create path */
        char nm[32];
        snprintf(nm, sizeof nm, "u/%ld/%d", (long)id, it);
        void* q = NULL;
        r = Platform_ShmMap(4, nm, 64, &q); assert(r >= 1 && q);
    }
    return NULL;
}

static void test_concurrency(void) {
    pthread_t th[NT];
    for (intptr_t i = 0; i < NT; i++) assert(pthread_create(&th[i], NULL, worker, (void*)i) == 0);
    for (int i = 0; i < NT; i++) pthread_join(th[i], NULL);
    printf("concurrency: ok (%d threads x %d iters)\n", NT, ITERS);
}

/* Simulate a second process attaching the existing shared meta (OS refcount > 1,
 * ready-flag barrier) and resolving a name the first "process" created. If the
 * re-init had instead created a fresh meta, the name would be gone. */
static void test_cross_process_attach(void) {
    void* a = NULL;
    INT32 r = Platform_ShmMap(4, "cross/x", 200, &a);   /* process 1 creates the slot */
    assert(r == 1 && a);

    mm_pool_reset_for_test();                            /* drop our attachment */

    mm_pool_init(NULL);                                  /* process 2: re-attach (ret>1) */

    void* a2 = NULL;
    r = Platform_ShmMap(4, "cross/x", 200, &a2);         /* must find the shared entry */
    assert(a2 == a);                                     /* same slot -> shared registry */
    assert(r == 2);                                      /* attach -> refcount incremented */
    printf("cross-process attach: ok (shared registry survived re-attach, addr=%p)\n", a2);
}

int main(void) {
    Mm_PoolCfg cfg;
    mm_pool_default_cfg(&cfg);
    cfg.enable = 1;
    cfg.threshold = 0x200000;
    cfg.block_size = 0x200000;
    cfg.poolable_flags_mask = (1u << 4) | (1u << 5);
    cfg.strategy = MM_ALLOC_BUMP;
    mm_pool_init(&cfg);           /* explicit init before threads (see note below) */

    test_functional();
    test_concurrency();
    test_cross_process_attach();

    printf("pool v1 (bump, shared-meta, lock-free attach) tests passed\n");
    return 0;
}
