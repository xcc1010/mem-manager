#ifndef MEM_MANAGER_POOL_H
#define MEM_MANAGER_POOL_H

#include "platform/platform_types.h"

/* Step-2 pool allocator (v1).
 *
 * Routes small, poolable Platform_ShmMap requests into large OS blocks so many
 * sub-2MB allocations share one 2MB-granular OS region (the whole point: kill
 * the per-region rounding waste). BUMP strategy only, but the framework is
 * strategy-pluggable: routing, the name registry, refcount and address
 * reverse-lookup are all strategy-agnostic; only carve()/release() differ per
 * strategy, selected by config. SLAB/FREELIST are reserved.
 *
 * v1: metadata (config, block table, name registry, lock) lives in a shared OS
 * region so every process/plane resolves the same name -> slot; the attach hot
 * path is LOCK-FREE (atomic-published entries + atomic refcount), only creation
 * takes a cross-process spinlock (TrySpinLock + passthrough fallback). All sync
 * is C11 <stdatomic.h> (no pthread) so it also compiles for the C-only DP plane.
 * See docs/STEP2_DESIGN.zh.md §2.2/§2.5. Compiled only when MEM_MANAGER_POOL is
 * defined; otherwise Platform_ShmMap is pure Step-1 passthrough. */

#ifdef __cplusplus
extern "C" {
#endif

/* Pluggable allocation strategy. v0 implements BUMP; SLAB/FREELIST are accepted
 * by config but their carve() returns 0 -> the request passes through to the OS. */
typedef enum {
    MM_ALLOC_BUMP = 0,   /* pointer-advance; O(1); no per-slot reclaim (block freed at teardown) */
    MM_ALLOC_SLAB,       /* reserved */
    MM_ALLOC_FREELIST    /* reserved */
} Mm_AllocKind;

typedef struct {
    int          enable;              /* 0 -> everything passes through (one-key rollback = Step 1) */
    UINT32       threshold;           /* size >= threshold -> passthrough        (default 0x200000) */
    UINT32       block_size;          /* OS block size carved from               (default 0x200000) */
    UINT32       poolable_flags_mask; /* bit f set -> flags==f may pool     (default (1<<4)|(1<<5)) */
    Mm_AllocKind strategy;            /* v0: BUMP */
} Mm_PoolCfg;

/* Fill cfg with defaults, then apply MEM_POOL_* environment overrides:
 *   MEM_POOL_ENABLE, MEM_POOL_THRESHOLD, MEM_POOL_BLOCK_SIZE,
 *   MEM_POOL_FLAGS (mask), MEM_POOL_STRATEGY (bump|slab|freelist).
 * Numeric values accept 0x.. hex or decimal. */
void mm_pool_default_cfg(Mm_PoolCfg* cfg);

/* Initialise the pool from cfg (idempotent; pass NULL to use mm_pool_default_cfg).
 * Called lazily by the try_* entry points if not called explicitly. */
void mm_pool_init(const Mm_PoolCfg* cfg);

/* Try to satisfy a map from the pool. Returns 1 if handled (*out and *ret set,
 * *ret = OS-style success = the reference count), 0 if the caller must fall back
 * to the OS (disabled / not poolable / pool or name table full). */
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
