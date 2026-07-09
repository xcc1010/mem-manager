/* Step-2 pool allocator (v1) — shared-memory metadata + lock-free attach.
 *
 * v0 -> v1 changes (see the conversation / docs/STEP2_DESIGN.zh.md §2.2, §2.5):
 *   - Metadata (config, block table, name registry, lock) lives in a shared OS
 *     region "mmpool/meta" (VA-same), so every process/plane resolves the same
 *     name -> slot. CP creates it (OS refcount == 1) and publishes a ready flag;
 *     other mappers attach and spin until ready.
 *   - attach (the hot path: name -> address of an existing slot) is LOCK-FREE:
 *     acquire-load the entry's published `state`, atomic refcount++. Only the
 *     create path takes the cross-process spinlock (TrySpinLock + fallback to
 *     passthrough, never deadlocks). All synchronisation is C11 <stdatomic.h>
 *     (no pthread) so the same code compiles for the C-only DP plane.
 *
 * Still v1-scoped: BUMP strategy only (no per-slot reclaim); passthrough names
 * are not registered (assumes a given name is always mapped with a consistent
 * size, so routing is stable) — a passthrough marker is a later hardening step.
 */
#include "pool/mm_pool.h"

#include "os/os_shm.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MM_MAX_BLOCKS    1024
#define MM_NAME_CAP      8192            /* open-addressing table; power of two   */
#define MM_NAME_MAX      64
#define MM_SLOT_ALIGN    16u
#define MM_META_NAME     "mmpool/meta"
#define MM_META_FLAGS    4              /* VA-same (raw pointers valid cross-proc) */
#define MM_META_MAGIC    0x4D4D504F4F4C3031ULL   /* "MMPOOL01" */
#define MM_LOCK_RETRIES  20000

/* NameEntry.state values */
enum { ST_EMPTY = 0, ST_USED = 1 };

typedef struct {
    _Atomic int  state;     /* ST_EMPTY -> ST_USED, published release / read acquire */
    _Atomic int  refcount;  /* attach count (monotonic under bump)                   */
    int          block_idx;
    UINT32       off;
    UINT32       size;
    INT32        flags;
    char         name[MM_NAME_MAX];
} NameEntry;

typedef struct {
    void*  base;            /* VA-same base -> valid in every process                */
    UINT32 size;
    UINT32 next;            /* bump cursor; only touched under the create lock       */
    INT32  flags;
} PoolBlock;

typedef struct {
    _Atomic unsigned long long ready;   /* MM_META_MAGIC once the creator finished   */
    atomic_flag                lock;    /* create-path spinlock (test-and-set)       */
    /* config snapshot (creator wins; attachers use these, not their own cfg) */
    int          enable;
    UINT32       threshold;
    UINT32       block_size;
    UINT32       poolable_flags_mask;
    int          strategy;
    _Atomic int  nblocks;               /* published release, read acquire           */
    PoolBlock    blocks[MM_MAX_BLOCKS];
    NameEntry    names[MM_NAME_CAP];
} PoolMeta;

static PoolMeta*    g_meta;             /* process-local pointer to the shared meta  */
static _Atomic int  g_init_state;       /* 0 none, 1 in-progress, 2 done             */

/* ---------------- config ---------------- */

static UINT32 env_u32(const char* key, UINT32 dflt) {
    const char* v = getenv(key);
    return (v && *v) ? (UINT32)strtoul(v, NULL, 0) : dflt;
}

void mm_pool_default_cfg(Mm_PoolCfg* cfg) {
    if (!cfg) return;
    cfg->enable              = (int)env_u32("MEM_POOL_ENABLE", 1);
    cfg->threshold           = env_u32("MEM_POOL_THRESHOLD", 0x200000u);
    cfg->block_size          = env_u32("MEM_POOL_BLOCK_SIZE", 0x200000u);
    cfg->poolable_flags_mask = env_u32("MEM_POOL_FLAGS", (1u << 4) | (1u << 5));
    cfg->strategy            = MM_ALLOC_BUMP;
    const char* s = getenv("MEM_POOL_STRATEGY");
    if (s) {
        if      (!strcmp(s, "slab"))     cfg->strategy = MM_ALLOC_SLAB;
        else if (!strcmp(s, "freelist")) cfg->strategy = MM_ALLOC_FREELIST;
        else                             cfg->strategy = MM_ALLOC_BUMP;
    }
}

/* ---------------- init-once (attach or create the shared meta) ---------------- */

static void meta_attach(const Mm_PoolCfg* cfg) {
    void* p = NULL;
    INT32 ret = ShmMap(MM_META_FLAGS, (char*)MM_META_NAME, (UINT32)sizeof(PoolMeta), &p);
    if (ret < 0 || !p) return;          /* no meta -> g_meta stays NULL -> passthrough */
    PoolMeta* m = (PoolMeta*)p;

    if (ret == 1) {                     /* we are the creator */
        memset(m, 0, sizeof *m);        /* OS shm may not be zero-filled; be explicit */
        Mm_PoolCfg c;
        if (cfg) c = *cfg; else mm_pool_default_cfg(&c);
        m->enable              = c.enable;
        m->threshold           = c.threshold;
        m->block_size          = c.block_size;
        m->poolable_flags_mask = c.poolable_flags_mask;
        m->strategy            = (int)c.strategy;
        atomic_store_explicit(&m->nblocks, 0, memory_order_relaxed);
        /* atomic_flag cleared by memset(0); publish everything with a release. */
        atomic_store_explicit(&m->ready, MM_META_MAGIC, memory_order_release);
    } else {                            /* attacher: wait for the creator */
        while (atomic_load_explicit(&m->ready, memory_order_acquire) != MM_META_MAGIC) {
            /* brief spin; creator only holds this open for a memset + a few stores */
        }
    }
    g_meta = m;
}

void mm_pool_init(const Mm_PoolCfg* cfg) {
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&g_init_state, &expected, 1,
            memory_order_acq_rel, memory_order_acquire)) {
        meta_attach(cfg);
        atomic_store_explicit(&g_init_state, 2, memory_order_release);
    } else {
        while (atomic_load_explicit(&g_init_state, memory_order_acquire) != 2) { /* spin */ }
    }
}

static void ensure_init(void) {
    if (atomic_load_explicit(&g_init_state, memory_order_acquire) != 2)
        mm_pool_init(NULL);
}

/* ---------------- cross-process create-path spinlock ---------------- */

static int try_lock(PoolMeta* m) {
    for (int i = 0; i < MM_LOCK_RETRIES; i++)
        if (!atomic_flag_test_and_set_explicit(&m->lock, memory_order_acquire))
            return 1;
    return 0;                           /* contended/stuck -> caller falls back to OS */
}
static void unlock(PoolMeta* m) {
    atomic_flag_clear_explicit(&m->lock, memory_order_release);
}

/* ---------------- routing predicate ---------------- */

static int poolable(PoolMeta* m, INT32 flags, UINT32 size) {
    if (!m->enable)                                   return 0;
    if (size == 0 || size >= m->threshold)            return 0;
    if (flags < 0 || flags >= 32)                     return 0;
    if (!((m->poolable_flags_mask >> flags) & 1u))    return 0;
    return 1;
}

/* ---------------- name registry ---------------- */

static unsigned name_hash(const char* s) {
    unsigned h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

/* Lock-free lookup: acquire-load state; entries are never tombstoned in v1, so a
 * probe can stop at the first ST_EMPTY slot. */
static NameEntry* lookup(PoolMeta* m, const char* name) {
    unsigned h = name_hash(name) & (MM_NAME_CAP - 1);
    for (unsigned i = 0; i < MM_NAME_CAP; i++) {
        NameEntry* e = &m->names[(h + i) & (MM_NAME_CAP - 1)];
        int st = atomic_load_explicit(&e->state, memory_order_acquire);
        if (st == ST_EMPTY) return NULL;
        if (st == ST_USED && !strcmp(e->name, name)) return e;
    }
    return NULL;
}

/* Reserve an empty slot and fill its fields, leaving state ST_EMPTY (unpublished).
 * Caller (holding the lock) sets refcount then publishes state with a release. */
static NameEntry* reserve(PoolMeta* m, const char* name, int blk, UINT32 off,
                          UINT32 size, INT32 flags) {
    unsigned h = name_hash(name) & (MM_NAME_CAP - 1);
    for (unsigned i = 0; i < MM_NAME_CAP; i++) {
        NameEntry* e = &m->names[(h + i) & (MM_NAME_CAP - 1)];
        if (atomic_load_explicit(&e->state, memory_order_relaxed) == ST_EMPTY) {
            snprintf(e->name, sizeof e->name, "%s", name);
            e->block_idx = blk; e->off = off; e->size = size; e->flags = flags;
            return e;
        }
    }
    return NULL;                        /* table full */
}

/* ---------------- strategy: carve / release (holds the lock) ---------------- */

static UINT32 align_up(UINT32 v, UINT32 a) { return (v + (a - 1)) & ~(a - 1); }

static int carve_bump(PoolMeta* m, UINT32 size, INT32 flags, int* blk_out, UINT32* off_out) {
    UINT32 need = align_up(size, MM_SLOT_ALIGN);
    int n = atomic_load_explicit(&m->nblocks, memory_order_acquire);
    for (int i = 0; i < n; i++) {
        PoolBlock* b = &m->blocks[i];
        if (b->flags != flags) continue;
        UINT32 start = align_up(b->next, MM_SLOT_ALIGN);
        if (start <= b->size && need <= b->size - start) {
            b->next = start + need;
            *blk_out = i; *off_out = start;
            return 1;
        }
    }
    if (n >= MM_MAX_BLOCKS)  return 0;
    if (need > m->block_size) return 0;

    void* base = NULL;
    char  nm[MM_NAME_MAX];
    snprintf(nm, sizeof nm, "mmpool/blk-%d-%d", (int)flags, n);
    INT32 r = ShmMap(flags, nm, m->block_size, &base);
    if (r < 0 || !base) return 0;

    PoolBlock* b = &m->blocks[n];
    b->base = base; b->size = m->block_size; b->next = need; b->flags = flags;
    atomic_store_explicit(&m->nblocks, n + 1, memory_order_release);  /* publish block */
    *blk_out = n; *off_out = 0;
    return 1;
}

static int carve(PoolMeta* m, UINT32 size, INT32 flags, int* blk, UINT32* off) {
    switch (m->strategy) {
    case MM_ALLOC_BUMP:     return carve_bump(m, size, flags, blk, off);
    case MM_ALLOC_SLAB:                 /* reserved: fall through */
    case MM_ALLOC_FREELIST:
    default:                return 0;   /* not implemented -> caller passes through */
    }
}

/* ---------------- public entry points ---------------- */

int mm_pool_try_map(INT32 flags, char* name, UINT32 size, void** out, INT32* ret) {
    ensure_init();
    PoolMeta* m = g_meta;
    if (!m || !m->enable)              return 0;
    if (!out || !name)                 return 0;
    if (strlen(name) >= MM_NAME_MAX)   return 0;
    if (!poolable(m, flags, size))     return 0;   /* passthrough (incl. big attach) */

    /* attach hot path: lock-free lookup + atomic refcount */
    NameEntry* e = lookup(m, name);
    if (e) {
        int rc = atomic_fetch_add_explicit(&e->refcount, 1, memory_order_acq_rel) + 1;
        *out = (char*)m->blocks[e->block_idx].base + e->off;
        *ret = rc;
        return 1;
    }

    /* create path: cross-process spinlock, fall back to passthrough if stuck */
    if (!try_lock(m)) return 0;

    e = lookup(m, name);                            /* double-check under lock */
    if (e) {
        int rc = atomic_fetch_add_explicit(&e->refcount, 1, memory_order_acq_rel) + 1;
        unlock(m);
        *out = (char*)m->blocks[e->block_idx].base + e->off;
        *ret = rc;
        return 1;
    }

    int blk; UINT32 off;
    if (!carve(m, size, flags, &blk, &off)) { unlock(m); return 0; }   /* pool full */
    e = reserve(m, name, blk, off, size, flags);
    if (!e)                                 { unlock(m); return 0; }   /* table full */
    atomic_store_explicit(&e->refcount, 1, memory_order_relaxed);
    atomic_store_explicit(&e->state, ST_USED, memory_order_release);   /* publish */
    unlock(m);

    *out = (char*)m->blocks[blk].base + off;
    *ret = 1;
    return 1;
}

int mm_pool_try_unmap(void* addr, INT32* ret) {
    PoolMeta* m = g_meta;
    if (!m || !addr) return 0;
    int n = atomic_load_explicit(&m->nblocks, memory_order_acquire);
    for (int i = 0; i < n; i++) {
        PoolBlock* b = &m->blocks[i];
        char* lo = (char*)b->base;
        if (lo && (char*)addr >= lo && (char*)addr < lo + b->size) {
            /* BUMP: no per-slot reclaim; just claim it so the caller doesn't
             * OS-unmap a sub-slot. Full refcount lifecycle lands with slab/freelist. */
            if (ret) *ret = 0;
            return 1;
        }
    }
    return 0;   /* not a pooled address -> caller OS-unmaps */
}
