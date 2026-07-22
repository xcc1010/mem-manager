/* Step-2 pool allocator (v1) — shared-memory metadata + lock-free attach.
 *
 * v1 scope decisions (see docs/STEP2_DESIGN.zh.md §2.2, §2.5 and the 2026-07
 * scope updates):
 *   - BUMP strategy only (no per-slot reclaim; slab/freelist dropped for v1).
 *   - Poolable flags default = {1}: the business workload is ~all VA-DIFFERS
 *     (flags 0/1/2; flags 1 = CP+DP inter-PG shared). VA-same (4/5) is rare.
 *   - VA-differs addressing: the shared meta ("mmpool/meta", flags 1) stores
 *     ONLY offsets/indices — never pointers. Each process attaches every pool
 *     block's OS region by name and keeps its own local VA (g_blk_base);
 *     address = local_base[block_idx] + off. The same code path would also
 *     serve VA-same flags (same-name attach yields one VA everywhere).
 *   - Metadata (config, block table, name registry, lock) lives in the shared
 *     meta so every process/plane resolves the same name -> slot. The creator
 *     (OS refcount == 1) publishes a ready flag; other mappers attach and wait
 *     (bounded) until ready.
 *   - attach (the hot path: name -> address of an existing slot) is LOCK-FREE:
 *     acquire-load the entry's published `state`, atomic refcount++, cached
 *     local base. Only the create path takes the cross-process spinlock
 *     (TrySpinLock + bounded spin + registry re-check, never deadlocks).
 *     All synchronisation is C11 <stdatomic.h> (no pthread) so the same code
 *     compiles for the C-only DP plane.
 *   - Config comes from a flat JSON file (MEM_POOL_CONFIG) read by the meta
 *     creator; attachers use the snapshot stored in shared meta.
 *
 * Correctness rules enforced here:
 *   - A poolable name may NEVER fall back to the OS while a concurrent create
 *     of the same name could still be in flight — that would leave the name
 *     existing both as a pool slot and as a private OS region (split sharing,
 *     silent data divergence). The create path therefore spins bounded rounds
 *     of {try the lock, re-check the registry}; passthrough is allowed only
 *     when the entry is still missing after the full budget (lock frozen by a
 *     dead holder, or pool/table full — in both cases everyone else fails the
 *     same way, so the name is consistently OS-shared).
 *   - Attach validates the request against the registered entry: same flags,
 *     request size must fit the slot. A mismatch is a caller bug and fails
 *     loudly (ret < 0) instead of silently truncating or splitting.
 *   - The OS ShmMap for a new pool block is deliberately kept INSIDE the
 *     create lock: taking it out would open a window where the same name is
 *     registered twice (the slow create would no longer be serialised), which
 *     needs a CREATING-state machine to fix. The bounded-spin budget dwarfs
 *     one OS ShmMap, and a holder crashing mid-call degrades to the
 *     consistent "pool frozen" state above.
 *   - Different flags values NEVER share a pool block (sharing-scope
 *     isolation); blocks are segregated per flags.
 *
 * Still v1-scoped: bump never reclaims slots; pooled unmap does not decrement
 * the refcount (profilers will report pooled regions as never-freed). */
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
#define MM_META_FLAGS    1              /* CP+DP inter-PG shared; meta stores no   */
                                        /* cross-process pointers, VA may differ   */
#define MM_META_MAGIC    0x4D4D504F4F4C3033ULL   /* "MMPOOL03" (layout id) */
#define MM_LOCK_RETRIES  20000      /* one try_lock round (~ms)            */
#define MM_CREATE_ROUNDS 128        /* rounds of {try lock, re-lookup} before passthrough */
#define MM_READY_SPINS   50000000L  /* bounded wait for the meta ready flag (~0.1-0.5s) */

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
    UINT32 size;
    UINT32 next;            /* bump cursor; only touched under the create lock       */
    INT32  flags;
} PoolBlock;                /* NOTE: no base pointer — VA-differs blocks have a      */
                            /* different VA in every process; bases are per-process  */

typedef struct {
    _Atomic unsigned long long ready;   /* MM_META_MAGIC once the creator finished   */
    atomic_flag                lock;    /* create-path spinlock (test-and-set)       */
    /* config snapshot (creator wins; attachers use these, not their own cfg) */
    int          enable;
    UINT32       threshold;
    UINT32       block_size;
    UINT32       poolable_flags_mask;
    _Atomic int  nblocks;               /* published release, read acquire           */
    PoolBlock    blocks[MM_MAX_BLOCKS];
    NameEntry    names[MM_NAME_CAP];
} PoolMeta;

/* The meta must fit one OS region (2MB granularity). Catch cap changes that
 * would silently overflow at compile time. */
_Static_assert(sizeof(PoolMeta) <= 0x200000u, "PoolMeta must fit one 2MB OS region");

static PoolMeta*    g_meta;             /* process-local pointer to the shared meta  */
static _Atomic int  g_init_state;       /* 0 none, 1 in-progress, 2 done             */

/* Process-local VA of each pool block. Business flags are VA-differs, so the
 * same pool block lives at a DIFFERENT address in every process: each process
 * attaches the block's OS region by name (once, lazily) and keeps its own
 * base here. The shared meta only ever stores offsets/indices, never pointers. */
static _Atomic(void*) g_blk_base[MM_MAX_BLOCKS];

/* Resolve this process's VA for block i. Same-name attach is idempotent within
 * a process (OS refcount++), so a racy double resolution is harmless. Returns
 * NULL on OS failure. */
static void* local_base(PoolMeta* m, int i) {
    void* p = atomic_load_explicit(&g_blk_base[i], memory_order_acquire);
    if (p) return p;
    char nm[MM_NAME_MAX];
    snprintf(nm, sizeof nm, "mmpool/blk-%d-%d", (int)m->blocks[i].flags, i);
    INT32 r = ShmMap(m->blocks[i].flags, nm, m->block_size, &p);
    if (r < 0 || !p) return NULL;
    atomic_store_explicit(&g_blk_base[i], p, memory_order_release);
    return p;
}

/* ---------------- config ---------------- */

static UINT32 env_u32(const char* key, UINT32 dflt) {
    const char* v = getenv(key);
    return (v && *v) ? (UINT32)strtoul(v, NULL, 0) : dflt;
}

/* ---------------- flat-JSON config file ---------------- */
/* Minimal hand-written parser for a FLAT JSON object (plus a flat int array).
 * Deliberately tiny: no third-party deps, and only the meta creator (CP /
 * platform init) ever reads the file — attachers use the snapshot in shared
 * meta. Lenient on whitespace; unknown keys ignored. */

#define MM_CFG_MAX_BYTES (64u * 1024u)

static const char* json_key(const char* buf, const char* key) {
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char* p = strstr(buf, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static int json_bool(const char* buf, const char* key, int dflt) {
    const char* p = json_key(buf, key);
    if (!p)                        return dflt;
    if (!strncmp(p, "true", 4))    return 1;
    if (!strncmp(p, "false", 5))   return 0;
    return dflt;
}

static UINT32 json_u32(const char* buf, const char* key, UINT32 dflt) {
    const char* p = json_key(buf, key);
    if (!p) return dflt;
    if (*p == '"') p++;                      /* allow the "0x200000" string form */
    char* end = NULL;
    unsigned long v = strtoul(p, &end, 0);   /* 0x.. hex or decimal */
    return (end && end != p) ? (UINT32)v : dflt;
}

static UINT32 json_flags_mask(const char* buf, const char* key, UINT32 dflt) {
    const char* p = json_key(buf, key);
    if (!p || *p != '[') return dflt;
    p++;
    UINT32 mask = 0;
    while (*p && *p != ']') {
        if (*p >= '0' && *p <= '9') {
            char* end = NULL;
            unsigned long v = strtoul(p, &end, 0);
            if (v < 32) mask |= (1u << v);
            p = end ? end : p + 1;
        } else {
            p++;
        }
    }
    return mask;
}

int mm_pool_load_config(const char* path, Mm_PoolCfg* cfg) {
    if (!path || !cfg) return -1;
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    char* buf = (char*)malloc(MM_CFG_MAX_BYTES + 1);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, MM_CFG_MAX_BYTES, f);
    fclose(f);
    buf[n] = '\0';
    cfg->enable              = json_bool(buf, "enable", cfg->enable);
    cfg->threshold           = json_u32(buf, "threshold", cfg->threshold);
    cfg->block_size          = json_u32(buf, "block_size", cfg->block_size);
    cfg->poolable_flags_mask = json_flags_mask(buf, "poolable_flags",
                                             cfg->poolable_flags_mask);
    free(buf);
    return 0;
}

void mm_pool_default_cfg(Mm_PoolCfg* cfg) {
    if (!cfg) return;
    cfg->enable              = 1;
    cfg->threshold           = 0x200000u;
    cfg->block_size          = 0x200000u;
    cfg->poolable_flags_mask = (1u << 1);           /* v1: flags=1 only (VA-differs, CP+DP inter-PG) */
    const char* path = getenv("MEM_POOL_CONFIG");
    if (path && *path) mm_pool_load_config(path, cfg);   /* file overrides defaults */
    /* env vars override the file: MEM_POOL_ENABLE=0 stays the one-key rollback */
    cfg->enable              = (int)env_u32("MEM_POOL_ENABLE", (UINT32)cfg->enable);
    cfg->threshold           = env_u32("MEM_POOL_THRESHOLD", cfg->threshold);
    cfg->block_size          = env_u32("MEM_POOL_BLOCK_SIZE", cfg->block_size);
    cfg->poolable_flags_mask = env_u32("MEM_POOL_FLAGS", cfg->poolable_flags_mask);
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
        if (c.enable && (c.block_size == 0 || c.threshold == 0)) {
            fprintf(stderr, "mem-manager: bad pool config (block_size/threshold = 0);"
                            " pool disabled\n");
            c.enable = 0;
        }
        m->enable              = c.enable;
        m->threshold           = c.threshold;
        m->block_size          = c.block_size;
        m->poolable_flags_mask = c.poolable_flags_mask;
        atomic_store_explicit(&m->nblocks, 0, memory_order_relaxed);
        /* atomic_flag cleared by memset(0); publish everything with a release. */
        atomic_store_explicit(&m->ready, MM_META_MAGIC, memory_order_release);
    } else {                            /* attacher: wait for the creator */
        long spins = 0;
        while (atomic_load_explicit(&m->ready, memory_order_acquire) != MM_META_MAGIC) {
            if (++spins > MM_READY_SPINS) return;   /* creator died mid-init -> passthrough */
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

/* TEST ONLY: forget our attachment so the next init re-runs meta_attach. The
 * shared region itself is left mapped (not OS-unmapped), so re-attaching sees
 * OS refcount > 1 and takes the attacher (ready-barrier) branch. The local
 * block-VA cache is also dropped, simulating a fresh process that must
 * re-resolve every pool block's VA via local_base(). */
void mm_pool_reset_for_test(void) {
    g_meta = NULL;
    for (int i = 0; i < MM_MAX_BLOCKS; i++)
        atomic_store_explicit(&g_blk_base[i], NULL, memory_order_release);
    atomic_store_explicit(&g_init_state, 0, memory_order_release);
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
    if (size > m->block_size)                         return 0;   /* can never fit (misconfig guard) */
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

/* ---------------- bump carve (holds the lock) ---------------- */

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

    atomic_store_explicit(&g_blk_base[n], base, memory_order_release);  /* our VA */
    PoolBlock* b = &m->blocks[n];
    b->size = m->block_size; b->next = need; b->flags = flags;
    atomic_store_explicit(&m->nblocks, n + 1, memory_order_release);  /* publish block */
    *blk_out = n; *off_out = 0;
    return 1;
}

/* ---------------- public entry points ---------------- */

/* Bounded length: a non-NUL-terminated business string must not run the scan
 * past the cap (plain strlen would read out of bounds). */
static size_t name_len(const char* s, size_t cap) {
    size_t n = 0;
    while (n < cap && s[n]) n++;
    return n;
}

/* Same name must mean the same object: identical flags (sharing scope) and a
 * request that fits the registered slot. A mismatch is a caller bug — fail
 * loudly instead of silently handing out a too-small or wrong-scope region. */
static int entry_ok(const NameEntry* e, INT32 flags, UINT32 size) {
    return e->flags == flags && size <= e->size;
}

static int attach(PoolMeta* m, NameEntry* e, void** out, INT32* ret) {
    void* base = local_base(m, e->block_idx);
    if (!base) {                    /* OS attach failed: visible error, never split */
        *out = NULL; *ret = -1;
        return -1;
    }
    int rc = atomic_fetch_add_explicit(&e->refcount, 1, memory_order_acq_rel) + 1;
    *out = (char*)base + e->off;
    *ret = rc;
    return 1;
}

int mm_pool_try_map(INT32 flags, char* name, UINT32 size, void** out, INT32* ret) {
    ensure_init();
    PoolMeta* m = g_meta;
    if (!m || !m->enable)                    return 0;
    if (!out || !name)                       return 0;
    if (name_len(name, MM_NAME_MAX) >= MM_NAME_MAX) return 0;
    if (!poolable(m, flags, size))           return 0;   /* passthrough (incl. big attach) */

    /* attach hot path: lock-free lookup + atomic refcount */
    NameEntry* e = lookup(m, name);
    if (e) {
        if (!entry_ok(e, flags, size)) goto conflict;
        return attach(m, e, out, ret);
    }

    /* Create path. A concurrent process may be publishing this same name
     * right now; NEVER pass through to the OS while that is possible, or the
     * name would exist both as a pool slot and as a private OS region (split
     * sharing). Spin bounded rounds of {try the lock, re-check the registry};
     * passthrough is allowed only when the entry is still missing after the
     * full budget (lock frozen by a dead holder, or pool/table full — then
     * everyone else fails identically, so the name is consistently OS-shared). */
    for (int round = 0; round < MM_CREATE_ROUNDS; round++) {
        if (try_lock(m)) {
            e = lookup(m, name);                    /* double-check under lock */
            if (!e) {
                int blk; UINT32 off;
                if (!carve_bump(m, size, flags, &blk, &off)) { unlock(m); return 0; }
                e = reserve(m, name, blk, off, size, flags);
                if (!e)                                    { unlock(m); return 0; }
                atomic_store_explicit(&e->refcount, 0, memory_order_relaxed);
                atomic_store_explicit(&e->state, ST_USED, memory_order_release);
            } else if (!entry_ok(e, flags, size)) {
                unlock(m);
                goto conflict;
            }
            int rc = attach(m, e, out, ret);        /* refcount 0->1 for a fresh entry */
            unlock(m);
            return rc;
        }
        e = lookup(m, name);                        /* creator may have published meanwhile */
        if (e) {
            if (!entry_ok(e, flags, size)) goto conflict;
            return attach(m, e, out, ret);
        }
    }
    return 0;   /* lock frozen / budget exhausted -> consistent OS passthrough */

conflict:
    *out = NULL;
    *ret = -1;              /* OS-style failure: visible to the caller, not silent */
    return -1;
}

int mm_pool_try_unmap(void* addr, INT32* ret) {
    ensure_init();
    PoolMeta* m = g_meta;
    if (!m || !addr) return 0;
    int n = atomic_load_explicit(&m->nblocks, memory_order_acquire);
    for (int i = 0; i < n; i++) {
        char* lo = (char*)atomic_load_explicit(&g_blk_base[i], memory_order_acquire);
        if (lo && (char*)addr >= lo && (char*)addr < lo + m->blocks[i].size) {
            /* BUMP: no per-slot reclaim; just claim it so the caller doesn't
             * OS-unmap a sub-slot. Full refcount lifecycle lands with slab/freelist. */
            if (ret) *ret = 0;
            return 1;
        }
    }
    return 0;   /* not a pooled address -> caller OS-unmaps */
}
