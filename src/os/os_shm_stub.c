/* Local-only fake implementation of the OS shared-memory primitives, backed by
 * the heap. Lets mem-manager build, run, and be tested without the real OS
 * library. NOT compiled when MEM_MANAGER_USE_API_H is set.
 *
 * It models the real OS's NAME-BASED SHARING + REFERENCE COUNT within one
 * process: mapping the same shmname again returns the SAME region and an
 * incremented refcount (ShmMap's return value), matching the real convention
 * (success = ret > 0 = the refcount). This lets local tests exercise the pool's
 * cross-process attach path (creator ret==1 vs attacher ret>1) — see the pool's
 * mm_pool_reset_for_test() which simulates a second process re-attaching. */
#include "os/os_shm.h"
#include "os/aaa_atomic.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* ---- AAA_* sync primitives: standalone equivalents via C11 <stdatomic.h> ----
 * Same semantics as the internal api.h versions (see aaa_atomic.h); only the
 * `lock` word of AAASpinLock is used here. */

VOID AAA_InitSpinLock(AAASpinLock *l) {
    l->magic = 0;
    atomic_store_explicit((_Atomic UINT32*)&l->lock, 0, memory_order_relaxed);
    l->pidmask = 0; l->lr = 0; l->tsc = 0; l->nextoffset = 0; l->usingNum = 0;
}
INT32 AAA_TrySpinLock(AAASpinLock *l) {
    UINT32 expected = 0;
    return atomic_compare_exchange_strong_explicit((_Atomic UINT32*)&l->lock,
            &expected, 1, memory_order_acquire, memory_order_relaxed) ? 0 : -1;
}
INT32 AAA_SpinLock(AAASpinLock *l) {
    while (AAA_TrySpinLock(l) != 0) { /* spin */ }
    return 0;
}
INT32 AAA_SpinUnlock(AAASpinLock *l) {
    atomic_store_explicit((_Atomic UINT32*)&l->lock, 0, memory_order_release);
    return 0;
}

INT32 AAA_Atomic32ReadAcquire(INT32 *p) {
    return atomic_load_explicit((_Atomic INT32*)p, memory_order_acquire);
}
INT32 AAA_Atomic32Read(INT32 *p) {
    /* api.h documents this as acq_rel; a pure load lowers to acquire */
    return atomic_load_explicit((_Atomic INT32*)p, memory_order_acquire);
}
VOID AAA_Atomic32SetRelaxed(INT32 *p, INT32 v) {
    atomic_store_explicit((_Atomic INT32*)p, v, memory_order_relaxed);
}
VOID AAA_Atomic32SetRelease(INT32 *p, INT32 v) {
    atomic_store_explicit((_Atomic INT32*)p, v, memory_order_release);
}
INT32 AAA_Atomic32IncReturn(INT32 *p) {
    return atomic_fetch_add_explicit((_Atomic INT32*)p, 1, memory_order_acq_rel) + 1;
}
UINT64 AAA_Atomic64ReadAcquire(UINT64 *p) {
    return atomic_load_explicit((_Atomic UINT64*)p, memory_order_acquire);
}
VOID AAA_Atomic64SetRelease(UINT64 *p, UINT64 v) {
    atomic_store_explicit((_Atomic UINT64*)p, v, memory_order_release);
}

#define STUB_MAX       8192
#define STUB_NAME_MAX  128

typedef struct {
    char   name[STUB_NAME_MAX];
    void*  ptr;
    UINT32 size;
    int    refcount;
    int    used;
} StubReg;

static StubReg         g_reg[STUB_MAX];
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

INT32 ShmMap(INT32 flags, char* shmname, UINT32 size, void** shm) {
    (void)flags;
    if (!shm) return -1;

    pthread_mutex_lock(&g_mtx);

    StubReg* freeslot = NULL;
    if (shmname) {
        for (int i = 0; i < STUB_MAX; i++) {
            StubReg* r = &g_reg[i];
            if (r->used) {
                if (!strcmp(r->name, shmname)) {         /* name-based sharing: attach */
                    r->refcount++;
                    *shm = r->ptr;
                    int rc = r->refcount;
                    pthread_mutex_unlock(&g_mtx);
                    return rc;                           /* success = refcount (>0) */
                }
            } else if (!freeslot) {
                freeslot = r;
            }
        }
    } else {
        for (int i = 0; i < STUB_MAX && !freeslot; i++)
            if (!g_reg[i].used) freeslot = &g_reg[i];
    }

    if (!freeslot) { pthread_mutex_unlock(&g_mtx); return -1; }   /* table full */

    void* p = malloc(size ? size : 1);
    if (!p) { pthread_mutex_unlock(&g_mtx); return -1; }

    freeslot->used = 1;
    freeslot->ptr = p;
    freeslot->size = size;
    freeslot->refcount = 1;
    freeslot->name[0] = '\0';
    if (shmname) {
        strncpy(freeslot->name, shmname, STUB_NAME_MAX - 1);
        freeslot->name[STUB_NAME_MAX - 1] = '\0';
    }
    *shm = p;
    pthread_mutex_unlock(&g_mtx);
    return 1;                                            /* first map -> refcount 1 (creator) */
}

INT32 ShmMapOnPg(INT32 flags, char* pgname, char* shmname, UINT32 size, void** shm) {
    (void)pgname; /* the stub ignores numa placement */
    return ShmMap(flags, shmname, size, shm);
}

INT32 ShmUnmap(void* shm) {
    if (!shm) return -1;
    pthread_mutex_lock(&g_mtx);
    for (int i = 0; i < STUB_MAX; i++) {
        StubReg* r = &g_reg[i];
        if (r->used && r->ptr == shm) {
            if (--r->refcount <= 0) {
                free(r->ptr);
                r->used = 0;
                r->ptr = NULL;
            }
            pthread_mutex_unlock(&g_mtx);
            return 0;                                    /* success */
        }
    }
    pthread_mutex_unlock(&g_mtx);
    return 0;   /* unknown pointer: treat as a no-op success (lenient like free) */
}
