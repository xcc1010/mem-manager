/* Step-2 pool allocator (v1) — shared-memory metadata + lock-free attach.
 *
 * v1 scope decisions (see docs/STEP2_DESIGN.zh.md §2.2, §2.5 and the 2026-07
 * scope updates):
 *   - BUMP strategy only (no per-slot reclaim; slab/freelist dropped for v1).
 *   - Poolable flags default = {0}: the business workload is ~all VA-DIFFERS,
 *     and business type-1 (CP+DP inter-PG shared) is #define'd as 0U in api.h.
 *     NOTE: the mask tests flag VALUES, not type numbers — type-1 == value 0.
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
 *     AAA_Atomic32ReadAcquire on the entry's published `state`,
 *     AAA_Atomic32IncReturn on the refcount, cached local base. Only the
 *     create path takes the cross-process spinlock (AAA_TrySpinLock + bounded
 *     spin + registry re-check, never deadlocks). All synchronisation — in the
 *     shared meta and process-local alike — uses the platform's AAA_*
 *     primitives (the standalone build maps them to C11 <stdatomic.h> in
 *     os_shm_stub.c) — no pthread — so the same code compiles for the C-only
 *     DP plane.
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
#include "os/aaa_atomic.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* <stdio.h> is CP-only: the DP has no full libc and the platform's own
 * headers define FILE differently — including <stdio.h> there fails with
 * "conflicting types for FILE". On the DP only snprintf is used; declare it
 * directly. Config-file reading and env overrides are CP-only as well: the
 * DP attaches the shared meta and uses its snapshot (设计文档.md §5.2). */
#ifdef IS_CP
#  include <stdio.h>
#else
int snprintf(char* s, size_t n, const char* fmt, ...);
#endif

#define MM_MAX_BLOCKS    1024
#define MM_NAME_CAP      8192            /* open-addressing table; power of two   */
#define MM_NAME_MAX      64
#define MM_MAX_PG        32              /* distinct pgnames (NUMA placement keys) */
#define MM_PGNAME_MAX    32
#define MM_SLOT_ALIGN    16u
#define MM_META_NAME     "mmpool/meta"
#define MM_META_FLAGS    0              /* business type-1 (CP+DP inter-PG, VA-  */
                                        /* differs) is #define'd as 0U in api.h  */
#define MM_META_MAGIC    0x4D4D504F4F4C3035ULL   /* "MMPOOL05" (layout id) */
#define MM_MAX_BLOCK_SIZE 0x8000000u               /* 128MB = platform OS max segment */
#define MM_LOCK_RETRIES  20000      /* one try_lock round (~ms)            */
#define MM_CREATE_ROUNDS 128        /* rounds of {try lock, re-lookup} before passthrough */
#define MM_READY_SPINS   50000000L  /* bounded wait for the meta ready flag (~0.1-0.5s) */

/* NameEntry.state values */
enum { ST_EMPTY = 0, ST_USED = 1 };

typedef struct {
    INT32        state;     /* ST_EMPTY -> ST_USED, published AAA_Atomic32SetRelease / read Acquire */
    INT32        refcount;  /* attach count: creator sets 1, attachers AAA_Atomic32IncReturn        */
    int          block_idx;
    UINT32       off;
    UINT32       size;
    INT32        flags;
    INT32        pgidx;     /* -1 = plain ShmMap slot; >=0 = created via ShmMapOnPg(pgnames[pgidx]) */
    char         name[MM_NAME_MAX];
} NameEntry;

typedef struct {
    UINT32 size;
    UINT32 next;            /* bump cursor; only touched under the create lock       */
    INT32  flags;
    INT32  pgidx;           /* -1 = plain block; >=0 = OnPg block on pgnames[pgidx]  */
} PoolBlock;                /* NOTE: no base pointer — VA-differs blocks have a      */
                            /* different VA in every process; bases are per-process  */

typedef struct {
    UINT64                   ready;   /* MM_META_MAGIC once the creator finished   */
    AAASpinLock              lock;    /* create-path spinlock (AAA_TrySpinLock)    */
    /* config snapshot (creator wins; attachers use these, not their own cfg) */
    int          enable;
    UINT32       threshold;
    UINT32       block_size;
    UINT32       poolable_flags_mask;
    INT32        nblocks;             /* published SetRelease, read ReadAcquire    */
    INT32        npg;                 /* pgname table fill (only under the lock)   */
    char         pgnames[MM_MAX_PG][MM_PGNAME_MAX];
    PoolBlock    blocks[MM_MAX_BLOCKS];
    NameEntry    names[MM_NAME_CAP];
} PoolMeta;

/* The meta must fit one OS region (2MB granularity). Catch cap changes that
 * would silently overflow at compile time. */
_Static_assert(sizeof(PoolMeta) <= 0x200000u, "PoolMeta must fit one 2MB OS region");

/* g_meta is stored as a pointer-width INT64 so that BOTH publication (by the
 * init-winning thread) and every read go through the platform's 64-bit atomic
 * interfaces. A plain global pointer read races the publication and hands the
 * optimiser licence to sink/speculate the first meta dereference above the
 * NULL guard — observed as a DP-boot coredump: ldr w0,[x20,#192] (m->enable,
 * offsetof 192 with the platform's 176-byte AAASpinLock) with x20 = g_meta
 * still unpublished (NULL). The opaque atomic accessors forbid that. */
static INT64        g_meta_va;
static INT32        g_init_state;       /* 0 none, 1 in-progress, 2 done (AAA atomics) */

static PoolMeta* meta_get(void) {
    return (PoolMeta*)(uintptr_t)AAA_Atomic64ReadAcquire(&g_meta_va);
}
static void meta_put(PoolMeta* m) {
    AAA_Atomic64SetRelease(&g_meta_va, (INT64)(uintptr_t)m);
}

/* Read nblocks with a defensive clamp: a corrupt meta must never drive
 * blocks[]/g_blk_base[] indexing out of bounds. */
static INT32 nblocks_read(PoolMeta* m);

/* ---------------- execution-context identity (fork / per-vcpu guard) ----------------
 * The process-local caches (g_meta_va, g_blk_base) are only valid in the
 * execution context that created them. Two platform realities can invalidate
 * them:
 *   S1: the platform FORKS processes after the pool was initialised — the
 *       child inherits non-NULL but dangling VAs (VA-differs: even an
 *       inherited mapping may live at a different VA);
 *   S2: shm VA scope is PER-VCPU on this platform (CONFIRMED by A/B test:
 *       the pgid-only watchdog coredumped on the meta deref, the
 *       (pgid,vcpu) watchdog does not; a process that attached meta on one
 *       vcpu faults when another vcpu dereferences the cached VA).
 * Wire the platform's pg/vcpu-id APIs through these macros; ensure_init()
 * then detects the context switch, drops the stale caches and re-attaches in
 * the current context (DP threads are core-pinned, so the context cannot
 * change mid-call). AAA_GetPgId/AAA_GetVcpuId exist only in the DP link
 * environment, so the defaults bind them on the DP (MEM_MANAGER_USE_API_H
 * without IS_CP) and compile the check out everywhere else (zero cost). On
 * the CP the watchdog is therefore off — single process, no fork observed;
 * override MM_POOL_GETPID (e.g. getpid) if that ever changes. */
#ifndef MM_POOL_GETPID
#  if defined(MEM_MANAGER_USE_API_H) && !defined(IS_CP)
#    define MM_POOL_GETPID()  ((int)AAA_GetPgId())   /* DP-only symbol: pg id   */
#  else
#    define MM_POOL_GETPID()  0    /* CP/standalone: watchdog off (AAA_GetPgId  */
#  endif                           /* does not exist in the CP link env)        */
#endif
#ifndef MM_POOL_GETVCPU
#  if defined(MEM_MANAGER_USE_API_H) && !defined(IS_CP)
#    define MM_POOL_GETVCPU() ((int)AAA_GetVcpuId()) /* DP-only symbol: core no.*/
#  else
#    define MM_POOL_GETVCPU() 0
#  endif
#endif

static INT32 g_owner_pid;
static INT32 g_owner_vcpu;

/* ---------------- diagnostics ---------------- */
#ifdef IS_CP
#  define mm_pool_log(...)  do { fprintf(stderr, "mmpool: " __VA_ARGS__); fputc('\n', stderr); } while (0)
#elif defined(LOG_FILE_INFO)
   /* LOG_FILE_INFO does not append a newline itself — add one to the fmt */
#  define mm_pool_log(fmt, ...)  LOG_FILE_INFO("MMPOOL", fmt "\n", __VA_ARGS__)
#else
#  define mm_pool_log(...)  ((void)0)
#endif

/* Rate-limited per-stage trace (first 64 per stage): the last trace line
 * before a coredump tells us exactly which stage faulted. */
#define MM_TRACE(slot, ...) do { \
    static INT32 tr_[8]; \
    if ((slot) < 8 && AAA_Atomic32IncReturn(&tr_[(slot)]) <= 64) \
        mm_pool_log(__VA_ARGS__); \
} while (0)

/* Process-local VA of each pool block, stored as INT64 (pointer-width) so the
 * platform's 64-bit atomic interfaces apply. Business flags are VA-differs, so
 * the same pool block lives at a DIFFERENT address in every process: each
 * process attaches the block's OS region by name (once, lazily) and keeps its
 * own base here. The shared meta only ever stores offsets/indices, never
 * pointers. */
static INT64 g_blk_base[MM_MAX_BLOCKS];

/* Resolve this process's VA for block i. Same-name attach is idempotent within
 * a process (OS refcount++), so a racy double resolution is harmless. Returns
 * NULL on OS failure. */
static void* local_base(PoolMeta* m, int i) {
    INT64 v = AAA_Atomic64ReadAcquire(&g_blk_base[i]);
    if (v) return (void*)(uintptr_t)v;
    char nm[MM_NAME_MAX];
    snprintf(nm, sizeof nm, "mmpool/blk-%d-p%d-%d",
             (int)m->blocks[i].flags, (int)m->blocks[i].pgidx, i);
    void* p = NULL;
    /* attach is always a plain ShmMap by name — works for OnPg-created
     * blocks too (name is the unique identifier) */
    INT32 r = ShmMap(m->blocks[i].flags, nm, m->block_size, &p);
    mm_pool_log("block %d attach (%s): pid=%d vcpu=%d ret=%d va=%p", i, nm,
                (int)MM_POOL_GETPID(), (int)MM_POOL_GETVCPU(), (int)r, p);
    if (r < 0 || !p) return NULL;
    AAA_Atomic64SetRelease(&g_blk_base[i], (INT64)(uintptr_t)p);
    return p;
}

/* ---------------- config: built-in defaults + JSON file (CP only) -------- */

#ifdef IS_CP
/* env_u32 was dropped: configuration comes from exactly two places —
 * the built-in defaults and the JSON file (path from MEM_POOL_CONFIG).
 * Minimal hand-written parser for a FLAT JSON object (plus a flat int array).
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
#endif /* IS_CP (JSON config file) */

#ifndef IS_CP
/* DP has no file I/O: config comes from the shared-meta snapshot. Keep the
 * public symbol so DP links never miss it. */
int mm_pool_load_config(const char* path, Mm_PoolCfg* cfg) {
    (void)path; (void)cfg;
    return -1;
}
#endif

void mm_pool_default_cfg(Mm_PoolCfg* cfg) {
    if (!cfg) return;
    cfg->enable              = 1;
    cfg->threshold           = 0x200000u;
    cfg->block_size          = 0x4000000u;          /* 64MB blocks (2026-07 trial value) */
    cfg->poolable_flags_mask = (1u << 0);           /* v1: flags VALUE 0 = business type-1 (api.h #define 0U) */
#ifdef IS_CP
    const char* path = getenv("MEM_POOL_CONFIG");   /* the only env var: where the file is */
    if (path && *path) mm_pool_load_config(path, cfg);   /* file overrides defaults */
#endif
}

/* ---------------- init-once (attach or create the shared meta) ---------------- */

static void meta_attach(const Mm_PoolCfg* cfg) {
    void* p = NULL;
    INT32 ret = ShmMap(MM_META_FLAGS, (char*)MM_META_NAME, (UINT32)sizeof(PoolMeta), &p);
    mm_pool_log("meta ShmMap: pid=%d vcpu=%d ret=%d va=%p",
                (int)MM_POOL_GETPID(), (int)MM_POOL_GETVCPU(), (int)ret, p);
    if (ret < 0 || !p) return;          /* no meta -> g_meta stays NULL -> passthrough */
    PoolMeta* m = (PoolMeta*)p;

    if (ret == 1) {                     /* we are the creator */
#ifndef IS_CP
        /* A PG/DP process should NEVER be the creator: the Simulator (CP) is
         * required to pre-create the meta (mm_pool_init) before loadPG, so
         * every PG only ever attaches. Getting ret==1 here means the boot
         * ordering was violated (or a concurrent-create race produced a
         * duplicate segment). We still initialise as a safe fallback. */
        mm_pool_log("WARN: meta created by a PG process (pid=%d vcpu=%d) - "
                    "Simulator must pre-create it before loadPG",
                    (int)MM_POOL_GETPID(), (int)MM_POOL_GETVCPU());
#endif
        memset(m, 0, sizeof *m);        /* OS shm may not be zero-filled; be explicit */
        Mm_PoolCfg c;
        if (cfg) c = *cfg; else mm_pool_default_cfg(&c);
        if (c.enable && (c.block_size == 0 || c.threshold == 0)) {
            mm_pool_log("bad pool config (block_size/threshold = 0); pool disabled");
            c.enable = 0;
        }
        m->enable              = c.enable;
        m->threshold           = c.threshold;
        m->block_size          = c.block_size;
        m->poolable_flags_mask = c.poolable_flags_mask;
        AAA_InitSpinLock(&m->lock);     /* explicit init; memset alone is not enough */
        AAA_Atomic32SetRelaxed(&m->nblocks, 0);
        /* publish everything (incl. the initialised lock) with a release */
        AAA_Atomic64SetRelease((INT64*)&m->ready, (INT64)MM_META_MAGIC);
    } else {                            /* attacher: wait for the creator */
        long spins = 0;
        while (AAA_Atomic64ReadAcquire((INT64*)&m->ready) != (INT64)MM_META_MAGIC) {
            if (++spins > MM_READY_SPINS) return;   /* creator died mid-init -> passthrough */
        }
    }
    mm_pool_log("meta cfg: enable=%d threshold=0x%x block_size=0x%x mask=0x%x",
                (int)m->enable, (unsigned)m->threshold, (unsigned)m->block_size,
                (unsigned)m->poolable_flags_mask);
    /* Sanity-check the snapshot before trusting it. A stale meta segment
     * (old layout, same magic — observed: block_size read as 0, then every
     * block ShmMap failed with 'size: 0' and a garbage nblocks could walk
     * blocks[] out of the segment) would otherwise be used blindly. Treat
     * corruption as "no meta" -> clean passthrough instead of coredump. */
    if ((m->enable != 0 && m->enable != 1) ||
        m->threshold == 0 || m->block_size == 0 ||
        m->block_size > MM_MAX_BLOCK_SIZE || (m->block_size & 0x1FFFFFu) != 0) {
        mm_pool_log("corrupt meta cfg -> passthrough (stale segment? mixed"
                    " CP/DP builds? destroy mmpool/meta and restart)");
        return;
    }
    meta_put(m);                        /* atomic release publication (see g_meta_va) */
}

void mm_pool_init(const Mm_PoolCfg* cfg) {
    if (AAA_Atomic32ReadAcquire(&g_init_state) == 2) return;
    /* In-process init-once gate: exactly ONE thread per process enters
     * meta_attach (issuing the single OS ShmMap for the meta). Letting every
     * concurrent caller into meta_attach relied on the OS serialising
     * same-name creation (refcount==1 -> unique creator); if the platform's
     * ShmMap does not serialise concurrent same-name maps from one process,
     * two threads both take the creator branch and the loser's memset() /
     * AAA_InitSpinLock() runs while attachers are already past the ready
     * barrier and holding the lock -> coredump. The CAS gate removes that
     * assumption. */
    if (AAA_Atomic32CmpAndStoreAcquire(&g_init_state, 0, 1)) {
        meta_attach(cfg);               /* single in-process creator/attacher */
        AAA_Atomic32SetRelaxed(&g_owner_pid,  (INT32)MM_POOL_GETPID());
        AAA_Atomic32SetRelaxed(&g_owner_vcpu, (INT32)MM_POOL_GETVCPU());
        AAA_Atomic32SetRelease(&g_init_state, 2);
        return;
    }
    /* Lost the race: bounded wait for the winner to publish g_meta. Never
     * re-enter meta_attach — a second in-process ShmMap of the meta is what
     * we are gating out. On timeout the winner is presumed dead; state stays
     * 1 and the pool degrades to passthrough rather than racing a retry. */
    long spins = 0;
    while (AAA_Atomic32ReadAcquire(&g_init_state) != 2)
        if (++spins > MM_READY_SPINS) return;
}

static void drop_local_state(void) {
    meta_put(NULL);
    for (int i = 0; i < MM_MAX_BLOCKS; i++)
        AAA_Atomic64SetRelease(&g_blk_base[i], 0);
    AAA_Atomic32SetRelease(&g_init_state, 0);
}

void mm_pool_uninit(void) {
    if (AAA_Atomic32ReadAcquire(&g_init_state) != 2) return;   /* never init'd */
    PoolMeta* m = meta_get();
    /* Detach our block mappings first (raw ShmUnmap — NOT the pooled wrapper,
     * which would claim these addresses). nblocks is read from the live meta
     * only to bound the scan; the cache itself is the source of truth. */
    if (m) {
        INT32 n = nblocks_read(m);
        for (int i = 0; i < n; i++) {
            void* p = (void*)(uintptr_t)AAA_Atomic64ReadAcquire(&g_blk_base[i]);
            if (p) ShmUnmap(p);
        }
        mm_pool_log("uninit: pid=%d vcpu=%d detached meta + %d blocks",
                    (int)MM_POOL_GETPID(), (int)MM_POOL_GETVCPU(), (int)n);
        ShmUnmap(m);
    }
    drop_local_state();
}

/* ---------------- boot-time cleanup (backstop) ---------------- */
/* Drain one named segment: attach (learning the refcount), then unmap that
 * many times. If the name does not exist the attach creates and immediately
 * destroys it — a net no-op. */
static void shm_drain(INT32 flags, const char* name, UINT32 size) {
    void* p = NULL;
    INT32 r = ShmMap(flags, (char*)name, size, &p);
    if (r <= 0 || !p) return;
    mm_pool_log("cleanup: drain %s (refcount %d)", name, (int)r);
    for (INT32 i = 0; i < r; i++)
        if (ShmUnmap(p) != 0) break;
}

void mm_pool_cleanup(void) {
    /* Attach the leftover meta. If it carries OUR layout magic, its block
     * table is trustworthy — drain exactly the recorded blocks first. */
    void* mp = NULL;
    INT32 r = ShmMap(MM_META_FLAGS, (char*)MM_META_NAME,
                     (UINT32)sizeof(PoolMeta), &mp);
    if (r > 1 && mp) {                          /* meta existed before us */
        PoolMeta* m = (PoolMeta*)mp;
        if (AAA_Atomic64ReadAcquire((INT64*)&m->ready) == (INT64)MM_META_MAGIC) {
            INT32 n = nblocks_read(m);
            for (int i = 0; i < n; i++) {
                char nm[MM_NAME_MAX];
                snprintf(nm, sizeof nm, "mmpool/blk-%d-p%d-%d",
                         (int)m->blocks[i].flags, (int)m->blocks[i].pgidx, i);
                shm_drain(m->blocks[i].flags, nm, m->block_size);
            }
        }
    }
    if (r > 0) {                                /* drain the meta itself */
        mm_pool_log("cleanup: drain %s (refcount %d)", MM_META_NAME, (int)r);
        for (INT32 i = 0; i < r; i++)
            if (ShmUnmap(mp) != 0) break;
    }
}

static void ensure_init(void) {
    if (AAA_Atomic32ReadAcquire(&g_init_state) == 2) {
        INT32 pid = (INT32)MM_POOL_GETPID();
        INT32 vc  = (INT32)MM_POOL_GETVCPU();
        INT32 opid = AAA_Atomic32ReadAcquire(&g_owner_pid);
        INT32 ovc  = AAA_Atomic32ReadAcquire(&g_owner_vcpu);
        if (opid == pid && ovc == vc) return;   /* same context: caches valid */
        /* fork child / different vcpu: the cached VAs dangle HERE (VA scope
         * is per-vcpu on this platform — CONFIRMED). Drop and re-attach in
         * this context (contexts are pinned, no mid-call switch). */
        mm_pool_log("context change (pg %d->%d vcpu %d->%d): re-attaching",
                    (int)opid, (int)pid, (int)ovc, (int)vc);
        drop_local_state();
    }
    mm_pool_init(NULL);
}

/* TEST ONLY: forget our attachment so the next init re-runs meta_attach. The
 * shared region itself is left mapped (not OS-unmapped), so re-attaching sees
 * OS refcount > 1 and takes the attacher (ready-barrier) branch. The local
 * block-VA cache is also dropped, simulating a fresh process that must
 * re-resolve every pool block's VA via local_base(). */
void mm_pool_reset_for_test(void) {
    drop_local_state();
}

/* ---------------- cross-process create-path spinlock ---------------- */

static int try_lock(PoolMeta* m) {
    for (int i = 0; i < MM_LOCK_RETRIES; i++)
        if (AAA_TrySpinLock(&m->lock) == 0)
            return 1;
    return 0;                           /* contended/stuck -> caller keeps spinning/re-checking */
}
static void unlock(PoolMeta* m) {
    (void)AAA_SpinUnlock(&m->lock);
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

/* Lock-free lookup: AAA_Atomic32ReadAcquire on state; entries are never
 * tombstoned in v1, so a probe can stop at the first ST_EMPTY slot. */
static NameEntry* lookup(PoolMeta* m, const char* name) {
    unsigned h = name_hash(name) & (MM_NAME_CAP - 1);
    for (unsigned i = 0; i < MM_NAME_CAP; i++) {
        NameEntry* e = &m->names[(h + i) & (MM_NAME_CAP - 1)];
        INT32 st = AAA_Atomic32ReadAcquire(&e->state);
        if (st == ST_EMPTY) return NULL;
        if (st == ST_USED && !strcmp(e->name, name)) return e;
    }
    return NULL;
}

/* Reserve an empty slot and fill its fields, leaving state ST_EMPTY (unpublished).
 * Caller (holding the lock) sets refcount then publishes state with SetRelease. */
static NameEntry* reserve(PoolMeta* m, const char* name, int blk, UINT32 off,
                          UINT32 size, INT32 flags, INT32 pgidx) {
    unsigned h = name_hash(name) & (MM_NAME_CAP - 1);
    for (unsigned i = 0; i < MM_NAME_CAP; i++) {
        NameEntry* e = &m->names[(h + i) & (MM_NAME_CAP - 1)];
        if (e->state == ST_EMPTY) {                     /* plain read: under the lock */
            snprintf(e->name, sizeof e->name, "%s", name);
            e->block_idx = blk; e->off = off; e->size = size; e->flags = flags;
            e->pgidx = pgidx;
            return e;
        }
    }
    return NULL;                        /* table full */
}

/* ---------------- bump carve (holds the lock) ---------------- */

/* Read nblocks with a defensive clamp: a corrupt meta must never drive
 * blocks[]/g_blk_base[] indexing out of bounds. */
static INT32 nblocks_read(PoolMeta* m) {
    INT32 n = AAA_Atomic32ReadAcquire(&m->nblocks);
    if (n < 0) return 0;
    return n > MM_MAX_BLOCKS ? MM_MAX_BLOCKS : n;
}

/* Resolve pgname -> index into the shared pgname table (insert if new).
 * Called only under the create lock. Returns -1 when the table is full. */
static int pg_resolve(PoolMeta* m, const char* pgname) {
    for (INT32 i = 0; i < m->npg; i++)
        if (!strncmp(m->pgnames[i], pgname, MM_PGNAME_MAX)) return (int)i;
    if (m->npg >= MM_MAX_PG) return -1;
    INT32 idx = m->npg;
    snprintf(m->pgnames[idx], MM_PGNAME_MAX, "%s", pgname);
    m->npg = idx + 1;                       /* plain store: under the lock */
    return (int)idx;
}

static UINT32 align_up(UINT32 v, UINT32 a) { return (v + (a - 1)) & ~(a - 1); }

/* Carve `size` bytes from a block matching (flags, pgidx); create a new block
 * if none has room. pgidx >= 0 means the block must live on the NUMA node of
 * pgnames[pgidx] (created via ShmMapOnPg — CP-only symbol; the DP never takes
 * that branch because the OnPg wrapper exists only on the CP). */
static int carve_bump(PoolMeta* m, UINT32 size, INT32 flags, INT32 pgidx,
                      int* blk_out, UINT32* off_out) {
    UINT32 need = align_up(size, MM_SLOT_ALIGN);
    INT32 n = nblocks_read(m);
    for (int i = 0; i < n; i++) {
        PoolBlock* b = &m->blocks[i];
        if (b->flags != flags || b->pgidx != pgidx) continue;
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
    snprintf(nm, sizeof nm, "mmpool/blk-%d-p%d-%d", (int)flags, (int)pgidx, n);
    INT32 r;
    if (pgidx < 0) {
        r = ShmMap(flags, nm, m->block_size, &base);
    } else {
#ifdef IS_CP
        r = ShmMapOnPg(flags, m->pgnames[pgidx], nm, m->block_size, &base);
#else
        return 0;           /* DP has no ShmMapOnPg; unreachable via wrappers  */
#endif
    }
    if (r < 0 || !base) return 0;

    AAA_Atomic64SetRelease(&g_blk_base[n], (INT64)(uintptr_t)base);  /* our VA */
    PoolBlock* b = &m->blocks[n];
    b->size = m->block_size; b->next = need; b->flags = flags; b->pgidx = pgidx;
    AAA_Atomic32SetRelease(&m->nblocks, n + 1);          /* publish block */
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
    if (e->block_idx < 0 || e->block_idx >= MM_MAX_BLOCKS) {  /* corrupt entry */
        *out = NULL; *ret = -1;
        return -1;
    }
    void* base = local_base(m, e->block_idx);
    if (!base) {                    /* OS attach failed: visible error, never split */
        *out = NULL; *ret = -1;
        return -1;
    }
    *ret = AAA_Atomic32IncReturn(&e->refcount);   /* new value; >= 2 for an attach */
    *out = (char*)base + e->off;
    return 1;
}

/* Shared routing for plain ShmMap (pgname == NULL) and ShmMapOnPg. A name is
 * ONE slot regardless of which API mapped it first: an OnPg caller attaching
 * a slot pinned to a different pg's node still gets the SAME data (only the
 * NUMA locality differs) — warn once, never split. */
static int try_map_common(INT32 flags, const char* pgname, char* name,
                          UINT32 size, void** out, INT32* ret) {
    ensure_init();
    PoolMeta* m = meta_get();      /* opaque atomic load: the NULL guard below
                                      cannot be reordered/speculated away     */
    /* rate-limited rejection diagnostics: why requests pass through */
    #define MM_REJECT_LOG(reason) do { \
        static INT32 nrej; \
        if (AAA_Atomic32IncReturn(&nrej) <= 32) \
            mm_pool_log("passthrough [%s]: name=%s flags=%d size=0x%x " \
                        "(enable=%d thr=0x%x blk=0x%x mask=0x%x)", reason, \
                        name ? name : "(null)", (int)flags, (unsigned)size, \
                        m ? (int)m->enable : -1, m ? (unsigned)m->threshold : 0, \
                        m ? (unsigned)m->block_size : 0, \
                        m ? (unsigned)m->poolable_flags_mask : 0); \
    } while (0)
    if (!m)                    { MM_REJECT_LOG("no meta");       return 0; }
    if (!m->enable)            { MM_REJECT_LOG("disabled");      return 0; }
    if (!out || !name)         { MM_REJECT_LOG("bad args");      return 0; }
    if (name_len(name, MM_NAME_MAX) >= MM_NAME_MAX)
                               { MM_REJECT_LOG("name too long"); return 0; }
    if (!poolable(m, flags, size)) { MM_REJECT_LOG("not poolable"); return 0; }
    MM_TRACE(0, "try_map: name=%s flags=%d size=0x%x pg=%s",
             name, (int)flags, (unsigned)size, pgname ? pgname : "-");

    /* attach hot path: lock-free lookup + atomic refcount */
    NameEntry* e = lookup(m, name);
    if (e) {
        if (!entry_ok(e, flags, size)) goto conflict;
        MM_TRACE(1, "attach: name=%s blk=%d off=0x%x", name, (int)e->block_idx, (unsigned)e->off);
        goto have_entry;
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
            MM_TRACE(2, "create lock: name=%s round=%d", name, round);
            e = lookup(m, name);                    /* double-check under lock */
            if (!e) {
                INT32 pgidx = -1;
                if (pgname) {
                    pgidx = pg_resolve(m, pgname);
                    if (pgidx < 0) {                /* pgname table full */
                        unlock(m);
                        MM_REJECT_LOG("pg table full");
                        return 0;
                    }
                }
                int blk; UINT32 off;
                if (!carve_bump(m, size, flags, pgidx, &blk, &off)) {
                    unlock(m);
                    MM_REJECT_LOG("carve failed (pool full)");
                    return 0;
                }
                void* base = local_base(m, blk);    /* cached for a fresh block */
                if (!base)                                 { unlock(m); return 0; }
                e = reserve(m, name, blk, off, size, flags, pgidx);
                if (!e)                                    { unlock(m); return 0; }
                AAA_Atomic32SetRelaxed(&e->refcount, 1);
                AAA_Atomic32SetRelease(&e->state, ST_USED);  /* publish */
                unlock(m);
                MM_TRACE(3, "created: name=%s blk=%d off=0x%x va=%p",
                         name, blk, (unsigned)off, (char*)base + off);
                *out = (char*)base + off;
                *ret = 1;                           /* creator: exactly 1 */
                return 1;
            }
            if (!entry_ok(e, flags, size)) {
                unlock(m);
                goto conflict;
            }
            int rc;
            goto have_entry_locked;                 /* attach under the lock */
have_entry_locked:
            rc = attach(m, e, out, ret);
            unlock(m);
            return rc;
        }
        e = lookup(m, name);                        /* creator may have published meanwhile */
        if (e) {
            if (!entry_ok(e, flags, size)) goto conflict;
            goto have_entry;
        }
    }
    /* Budget exhausted with the entry still missing: either the create lock
     * is FROZEN (a holder crashed — the lock lives in the persistent meta
     * segment and stays set across restarts -> pool silently frozen), or the
     * pool/name table is full. Both degrade to a consistent OS passthrough;
     * log so the freeze is visible instead of silent. */
    MM_REJECT_LOG("create exhausted (lock frozen? pool/table full?)");
    return 0;

have_entry:
    /* Same slot for every API: only warn when an OnPg caller lands on a slot
     * pinned to a DIFFERENT pg's node (data is still correct, locality is
     * not what it asked for). */
    if (pgname && (e->pgidx < 0 ||
                   strncmp(m->pgnames[e->pgidx], pgname, MM_PGNAME_MAX))) {
        static INT32 nwarn;
        if (AAA_Atomic32IncReturn(&nwarn) <= 16)
            mm_pool_log("WARN: %s mapped OnPg(%s) but slot lives on %s", name,
                        pgname,
                        e->pgidx < 0 ? "a plain block" : m->pgnames[e->pgidx]);
    }
    return attach(m, e, out, ret);

conflict:
    *out = NULL;
    *ret = -1;              /* OS-style failure: visible to the caller, not silent */
    return -1;
}

int mm_pool_try_map(INT32 flags, char* name, UINT32 size, void** out, INT32* ret) {
    return try_map_common(flags, NULL, name, size, out, ret);
}

int mm_pool_try_map_pg(INT32 flags, char* pgname, char* name, UINT32 size,
                       void** out, INT32* ret) {
#ifdef IS_CP
    if (!pgname) return try_map_common(flags, NULL, name, size, out, ret);
    return try_map_common(flags, pgname, name, size, out, ret);
#else
    /* DP link env has no ShmMapOnPg and no OnPg wrapper — unreachable. */
    (void)flags; (void)pgname; (void)name; (void)size; (void)out; (void)ret;
    return 0;
#endif
}

int mm_pool_try_unmap(void* addr, INT32* ret) {
    ensure_init();
    PoolMeta* m = meta_get();      /* opaque atomic load (see mm_pool_try_map) */
    if (!m || !addr) return 0;
    MM_TRACE(4, "try_unmap: addr=%p", addr);
    INT32 n = nblocks_read(m);
    for (int i = 0; i < n; i++) {
        char* lo = (char*)(uintptr_t)AAA_Atomic64ReadAcquire(&g_blk_base[i]);
        if (lo && (char*)addr >= lo && (char*)addr < lo + m->blocks[i].size) {
            /* BUMP: no per-slot reclaim; just claim it so the caller doesn't
             * OS-unmap a sub-slot. Full refcount lifecycle lands with slab/freelist. */
            MM_TRACE(5, "unmap claimed: addr=%p blk=%d", addr, i);
            if (ret) *ret = 0;
            return 1;
        }
    }
    return 0;   /* not a pooled address -> caller OS-unmaps */
}
