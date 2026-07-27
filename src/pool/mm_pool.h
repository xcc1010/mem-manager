#ifndef MEM_MANAGER_POOL_H
#define MEM_MANAGER_POOL_H

#include "platform/platform_types.h"

/* Step-2 pool allocator (v1, BUMP only).
 *
 * Routes small, poolable Platform_ShmMap requests into large OS blocks so many
 * sub-2MB allocations share one 2MB-granular OS region (the whole point: kill
 * the per-region rounding waste). v1 implements the BUMP strategy only:
 * pointer-advance carve, O(1), no per-slot reclaim (best fit for the
 * init-once/long-lived profile; slab/freelist were dropped from scope).
 *
 * The business workload is ~all VA-DIFFERS (default poolable flags = {1},
 * CP+DP inter-PG shared). The shared metadata ("mmpool/meta") therefore stores
 * ONLY offsets/indices — never pointers; each process attaches every pool
 * block's OS region by name and resolves addresses against its own local VA.
 * (The same path also serves VA-same flags if they are enabled in the mask.)
 *
 * The attach hot path is LOCK-FREE (atomically-published entries +
 * AAA_Atomic32IncReturn refcount + cached local base); only creation takes a
 * cross-process spinlock (AAA_TrySpinLock + bounded spin + registry re-check,
 * never deadlocks, never double-maps a name). All synchronisation — shared
 * meta and process-local alike — uses the platform's AAA_* spinlock/atomic
 * primitives (the standalone build maps them to C11 <stdatomic.h>) — no
 * pthread — so it also compiles for the C-only DP plane. See
 * docs/STEP2_DESIGN.zh.md §2.2/§2.5. Compiled only when MEM_MANAGER_POOL is
 * defined; otherwise Platform_ShmMap is pure Step-1 passthrough. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int          enable;              /* 0 -> everything passes through (one-key rollback = Step 1) */
    UINT32       threshold;           /* size >= threshold -> passthrough        (default 0x200000) */
    UINT32       block_size;          /* OS block size carved from              (default 0x4000000) */
    UINT32       poolable_flags_mask; /* bit f set -> flags==f may pool        (default 1<<1) */
} Mm_PoolCfg;

/* Parse a flat JSON config file into cfg (only keys present are overwritten).
 * Returns 0 on success, -1 if the file cannot be read. Unknown keys are
 * ignored. Format (see config/pool.json):
 *   { "enable": true, "threshold": "0x200000", "block_size": "0x200000",
 *     "poolable_flags": [4, 5] }
 * Byte sizes accept a JSON number (decimal) or a string ("0x.." hex ok). */
int mm_pool_load_config(const char* path, Mm_PoolCfg* cfg);

/* Fill cfg with defaults, then apply (in increasing priority):
 *   1. the JSON file named by MEM_POOL_CONFIG (if set and readable), then
 *   2. MEM_POOL_* environment overrides:
 *      MEM_POOL_ENABLE, MEM_POOL_THRESHOLD, MEM_POOL_BLOCK_SIZE,
 *      MEM_POOL_FLAGS (mask). Numeric values accept 0x.. hex or decimal.
 * MEM_POOL_ENABLE=0 thus stays the one-key rollback even with a config file. */
void mm_pool_default_cfg(Mm_PoolCfg* cfg);

/* Initialise the pool from cfg (thread-safe init-once; pass NULL to use
 * mm_pool_default_cfg). A CAS gate serialises concurrent callers: exactly one
 * thread per process attaches/creates the shared meta, the rest wait for it.
 * Called lazily by the try_* entry points if not called explicitly. */
void mm_pool_init(const Mm_PoolCfg* cfg);

/* Try to satisfy a map from the pool. Returns:
 *   1  handled: *out and *ret set, *ret = OS-style success = the reference count;
 *   0  not handled: caller must fall back to the OS (disabled / not poolable /
 *      pool or name table full / create lock frozen by a dead holder);
 *  -1  rejected: the request conflicts with the pooled entry for this name
 *      (different flags, or size larger than the registered slot). *out = NULL,
 *      *ret < 0 — an OS-style failure the caller must see, not a silent split.
 * A poolable name is never passed through while a concurrent create of it may
 * still be in flight (bounded spin + registry re-check), so the same name can
 * never end up both pooled and OS-private. */
int mm_pool_try_map(INT32 flags, char* name, UINT32 size, void** out, INT32* ret);

/* Try to satisfy an unmap. Returns 1 if addr belonged to the pool (handled,
 * *ret set to 0 = success; the sub-allocation is NOT returned to the OS), 0 if
 * the caller must call OS ShmUnmap. */
int mm_pool_try_unmap(void* addr, INT32* ret);

/* TEST ONLY: drop this process's attachment to the shared meta (does NOT unmap
 * it). The next mm_pool_init re-runs the attach path, so a single-process test
 * can exercise the "second process attaches an existing meta" case (OS refcount
 * > 1 -> the ready-flag barrier). Not for production use. */
void mm_pool_reset_for_test(void);

#ifdef __cplusplus
}
#endif

#endif /* MEM_MANAGER_POOL_H */
