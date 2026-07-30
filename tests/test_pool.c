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

    /* two small poolable allocs (flags=1) pack adjacently in one block */
    r = Platform_ShmMap(1, "obj/a", 4000, &a); assert(r == 1 && a);
    r = Platform_ShmMap(1, "obj/b", 4000, &b); assert(r == 1 && b);
    assert((char*)b - (char*)a == 4000);          /* 16-aligned, packed */

    /* same name -> same slot; refcount is attach-monotonic under bump */
    r = Platform_ShmMap(1, "obj/a", 4000, &c); assert(c == a && r == 2);

    /* size >= threshold -> passthrough OS (separate region) */
    r = Platform_ShmMap(1, "obj/big", 0x200000, &big); assert(r >= 0 && big);
    assert((char*)big < (char*)a || (char*)big >= (char*)a + 0x200000);

    /* flags not in the mask (0) -> passthrough OS */
    r = Platform_ShmMap(0, "obj/np", 4000, &np); assert(r >= 0 && np);

    /* unmap: pooled detected (not OS-freed); bump does not reclaim */
    assert(Platform_ShmUnmap(a) == 0);
    assert(Platform_ShmUnmap(c) == 0);
    Platform_ShmUnmap(big);
    Platform_ShmUnmap(np);

    /* re-attach: same slot; bump keeps it, refcount keeps climbing (3rd attach) */
    void* a2 = NULL;
    r = Platform_ShmMap(1, "obj/a", 4000, &a2); assert(a2 == a && r == 3);

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
        r = Platform_ShmMap(1, (char*)SHARED[k], 128, &p);
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
        r = Platform_ShmMap(1, nm, 64, &q); assert(r >= 1 && q);
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
    INT32 r = Platform_ShmMap(1, "cross/x", 200, &a);   /* process 1 creates the slot */
    assert(r == 1 && a);

    mm_pool_reset_for_test();                            /* drop our attachment + VA cache */

    mm_pool_init(NULL);                                  /* process 2: re-attach (ret>1) */

    void* a2 = NULL;
    r = Platform_ShmMap(1, "cross/x", 200, &a2);         /* must find the shared entry */
    assert(a2 == a);                                     /* same slot -> shared registry */
    assert(r == 2);                                      /* attach -> refcount incremented */
    printf("cross-process attach: ok (shared registry survived re-attach, addr=%p)\n", a2);
}

/* ---- JSON config file parsing ---- */
static void test_json_config(void) {
    const char* path = "test_pool_cfg.json";
    FILE* f = fopen(path, "w");
    assert(f);
    fputs("{\n"
          "  \"enable\": false,\n"
          "  \"threshold\": \"0x10000\",\n"
          "  \"block_size\": 4194304,\n"
          "  \"poolable_flags\": [1, 2]\n"
          "}\n", f);
    fclose(f);

    Mm_PoolCfg c;
    mm_pool_default_cfg(&c);                 /* sane baseline (env/file-free here) */
    assert(mm_pool_load_config(path, &c) == 0);
    assert(c.enable == 0);
    assert(c.threshold == 0x10000u);         /* hex string form */
    assert(c.block_size == 4194304u);        /* decimal number form */
    assert(c.poolable_flags_mask == ((1u << 1) | (1u << 2)));
    remove(path);

    assert(mm_pool_load_config("no/such/file.json", &c) == -1);
    printf("json config: ok\n");
}

/* ---- consistency: conflicting re-map of a pooled name must fail loudly ---- */
static void test_conflict(void) {
    void* p = (void*)1;
    INT32 r;

    /* "obj/a" was created by test_functional: flags=1, size=4000 */
    r = Platform_ShmMap(1, "obj/a", 8000, &p);   /* same name, larger size */
    assert(r < 0 && p == NULL);                  /* rejected, not silently truncated */

    p = (void*)1;
    r = Platform_ShmMap(2, "obj/a", 4000, &p);   /* same name, different flags */
    assert(r < 0 && p == NULL);                  /* rejected, scope must not mix */

    p = NULL;
    r = Platform_ShmMap(1, "obj/a", 2000, &p);   /* smaller attach: fits, allowed */
    assert(p != NULL && r >= 2);

    printf("conflict: ok\n");
}

/* ---- concurrency: simultaneous mm_pool_init from many threads ----
 * Regression test for the init race: previously every concurrent caller
 * entered meta_attach together, relying on the OS to serialise same-name
 * creation (unique refcount==1 creator); a platform ShmMap that does not
 * serialise that case lets two threads both take the creator branch, and the
 * loser's memset()/lock-reinit hits metadata already in use -> coredump. The
 * CAS init gate (0 -> 1 -> 2) must let exactly one thread attach the meta. */
static void* init_worker(void* arg) {
    (void)arg;
    mm_pool_init(NULL);                      /* all threads race the init gate */
    void* p = NULL; INT32 r;
    r = Platform_ShmMap(1, "init/race", 256, &p);
    assert(r >= 1 && p);                     /* pool usable right after init   */
    return NULL;
}

static void test_concurrent_init(void) {
    mm_pool_reset_for_test();                /* forget our attachment -> re-init */
    pthread_t th[NT];
    for (intptr_t i = 0; i < NT; i++) assert(pthread_create(&th[i], NULL, init_worker, (void*)i) == 0);
    for (int i = 0; i < NT; i++) pthread_join(th[i], NULL);

    /* after the storm: init done, shared name resolves to one slot */
    void* p = NULL; INT32 r;
    r = Platform_ShmMap(1, "init/race", 256, &p);
    assert(r >= 2 && p);                     /* attach to the existing entry   */
    printf("concurrent init: ok (%d threads raced mm_pool_init)\n", NT);
}

/* ---- lifecycle: uninit fully detaches; the next init starts FRESH ----
 * Must run FIRST: single process, so uninit drops the stub refcounts to zero
 * and the segments are really destroyed. The business "ret==1 => creator
 * initialises" protocol must survive a full detach + re-init. */
static void test_lifecycle(const Mm_PoolCfg* cfg) {
    void* a = NULL;
    INT32 r = Platform_ShmMap(1, "life/a", 256, &a);   /* creator: ret == 1 */
    assert(r == 1 && a);

    mm_pool_uninit();                                   /* detach meta + blocks */
    mm_pool_init(cfg);                                  /* fresh meta */

    void* b = NULL;
    r = Platform_ShmMap(1, "life/a", 256, &b);
    assert(r == 1 && b);    /* fresh registry -> we are the creator AGAIN (ret==1) */
    printf("lifecycle: ok\n");
}

int main(void) {
    Mm_PoolCfg cfg;
    mm_pool_default_cfg(&cfg);
    cfg.enable = 1;
    cfg.threshold = 0x200000;
    cfg.block_size = 0x200000;
    cfg.poolable_flags_mask = (1u << 1) | (1u << 2);
    mm_pool_init(&cfg);           /* explicit init: creator's config wins */

    test_lifecycle(&cfg);
    test_functional();
    test_concurrency();
    test_cross_process_attach();
    test_concurrent_init();
    test_json_config();
    test_conflict();

    printf("pool v1 (bump, shared-meta, lock-free attach) tests passed\n");
    return 0;
}
