/* Step-2 pool allocator (v0) — BUMP strategy behind a pluggable framework.
 * See mm_pool.h and docs/STEP2_DESIGN.zh.md. */
#include "pool/mm_pool.h"

#include "os/os_shm.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* v0 fixed capacities (process-local metadata; simple, no dynamic memory —
 * fits the C-only DP world too once the lock is swapped for a DP primitive). */
#define MM_MAX_BLOCKS   256
#define MM_NAME_CAP     4096            /* open-addressing table; power of two   */
#define MM_NAME_MAX     64
#define MM_SLOT_ALIGN   16u

typedef struct {
    void*  base;     /* OS region base (VA-same); sub-slots are base+off         */
    UINT32 size;     /* == cfg.block_size                                        */
    UINT32 next;     /* bump cursor (bytes used)                                 */
    INT32  flags;    /* every block belongs to one flags value (isolation)       */
} PoolBlock;

typedef struct {
    char   name[MM_NAME_MAX];
    int    used;                        /* 0 empty slot (never tombstoned in v0) */
    int    block_idx;
    UINT32 off;
    UINT32 size;                        /* original requested size               */
    INT32  flags;
    INT32  refcount;
} NameEntry;

static Mm_PoolCfg      g_cfg;
static int             g_inited;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static PoolBlock       g_blocks[MM_MAX_BLOCKS];
static int             g_nblocks;
static NameEntry       g_names[MM_NAME_CAP];

/* ---------------- config ---------------- */

static UINT32 env_u32(const char* key, UINT32 dflt) {
    const char* v = getenv(key);
    if (!v || !*v) return dflt;
    return (UINT32)strtoul(v, NULL, 0);   /* base 0 -> honours 0x.. */
}

void mm_pool_default_cfg(Mm_PoolCfg* cfg) {
    if (!cfg) return;
    cfg->enable              = (int)env_u32("MEM_POOL_ENABLE", 1);
    cfg->threshold           = env_u32("MEM_POOL_THRESHOLD", 0x200000u);   /* 2MB */
    cfg->block_size          = env_u32("MEM_POOL_BLOCK_SIZE", 0x200000u);  /* 2MB */
    cfg->poolable_flags_mask = env_u32("MEM_POOL_FLAGS", (1u << 4) | (1u << 5));
    cfg->strategy            = MM_ALLOC_BUMP;
    const char* s = getenv("MEM_POOL_STRATEGY");
    if (s) {
        if      (!strcmp(s, "slab"))     cfg->strategy = MM_ALLOC_SLAB;
        else if (!strcmp(s, "freelist")) cfg->strategy = MM_ALLOC_FREELIST;
        else                             cfg->strategy = MM_ALLOC_BUMP;
    }
}

void mm_pool_init(const Mm_PoolCfg* cfg) {
    pthread_mutex_lock(&g_lock);
    if (!g_inited) {
        if (cfg) g_cfg = *cfg;
        else     mm_pool_default_cfg(&g_cfg);
        g_nblocks = 0;
        memset(g_names, 0, sizeof g_names);
        g_inited = 1;
    }
    pthread_mutex_unlock(&g_lock);
}

static void ensure_init(void) {
    if (!g_inited) mm_pool_init(NULL);
}

/* ---------------- routing predicate ---------------- */

static int poolable(INT32 flags, UINT32 size) {
    if (!g_cfg.enable)                                return 0;
    if (size == 0 || size >= g_cfg.threshold)        return 0;
    if (flags < 0 || flags >= 32)                    return 0;
    if (!((g_cfg.poolable_flags_mask >> flags) & 1u)) return 0;
    return 1;
}

/* ---------------- name registry (open addressing) ---------------- */

static unsigned name_hash(const char* s) {
    unsigned h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

/* v0 never tombstones entries (bump keeps the memory, so a name->slot mapping
 * stays valid forever), so a probe can stop at the first empty slot. */
static NameEntry* name_lookup(const char* name) {
    unsigned h = name_hash(name) & (MM_NAME_CAP - 1);
    for (unsigned i = 0; i < MM_NAME_CAP; i++) {
        NameEntry* e = &g_names[(h + i) & (MM_NAME_CAP - 1)];
        if (!e->used) return NULL;
        if (!strcmp(e->name, name)) return e;
    }
    return NULL;
}

static NameEntry* name_insert(const char* name, int blk, UINT32 off, UINT32 size, INT32 flags) {
    unsigned h = name_hash(name) & (MM_NAME_CAP - 1);
    for (unsigned i = 0; i < MM_NAME_CAP; i++) {
        NameEntry* e = &g_names[(h + i) & (MM_NAME_CAP - 1)];
        if (!e->used) {
            e->used = 1;
            snprintf(e->name, sizeof e->name, "%s", name);
            e->block_idx = blk;
            e->off = off;
            e->size = size;
            e->flags = flags;
            e->refcount = 0;
            return e;
        }
    }
    return NULL;   /* table full */
}

static NameEntry* name_by_slot(int blk, UINT32 off) {
    for (unsigned i = 0; i < MM_NAME_CAP; i++) {
        NameEntry* e = &g_names[i];
        if (e->used && e->block_idx == blk && e->off == off) return e;
    }
    return NULL;
}

/* ---------------- strategy: carve / release ---------------- */

static UINT32 align_up(UINT32 v, UINT32 a) { return (v + (a - 1)) & ~(a - 1); }

/* BUMP: find a same-flags block with room, else make a new OS block. Holds g_lock. */
static int carve_bump(UINT32 size, INT32 flags, int* blk_out, UINT32* off_out) {
    UINT32 need = align_up(size, MM_SLOT_ALIGN);
    for (int i = 0; i < g_nblocks; i++) {
        PoolBlock* b = &g_blocks[i];
        if (b->flags != flags) continue;
        UINT32 start = align_up(b->next, MM_SLOT_ALIGN);
        if (start <= b->size && need <= b->size - start) {   /* no overflow */
            b->next = start + need;
            *blk_out = i; *off_out = start;
            return 1;
        }
    }
    if (g_nblocks >= MM_MAX_BLOCKS) return 0;
    if (need > g_cfg.block_size)    return 0;   /* can't fit even in a fresh block */

    void* base = NULL;
    char  blkname[MM_NAME_MAX];
    snprintf(blkname, sizeof blkname, "mmpool/blk-%d-%d", (int)flags, g_nblocks);
    INT32 r = ShmMap(flags, blkname, g_cfg.block_size, &base);
    if (r < 0 || !base) return 0;

    PoolBlock* b = &g_blocks[g_nblocks];
    b->base = base; b->size = g_cfg.block_size; b->next = need; b->flags = flags;
    *blk_out = g_nblocks; *off_out = 0;
    g_nblocks++;
    return 1;
}

static int carve(UINT32 size, INT32 flags, int* blk, UINT32* off) {
    switch (g_cfg.strategy) {
    case MM_ALLOC_BUMP:     return carve_bump(size, flags, blk, off);
    case MM_ALLOC_SLAB:                 /* reserved: fall through */
    case MM_ALLOC_FREELIST:
    default:                return 0;   /* not implemented -> caller passes through */
    }
}

static void release(int blk, UINT32 off, UINT32 size) {
    (void)blk; (void)off; (void)size;
    /* BUMP: no per-slot reclaim; the whole block is freed at process teardown. */
}

/* ---------------- public entry points ---------------- */

int mm_pool_try_map(INT32 flags, char* name, UINT32 size, void** out, INT32* ret) {
    ensure_init();
    if (!out || !name)                 return 0;
    if (!poolable(flags, size))        return 0;
    if (strlen(name) >= MM_NAME_MAX)   return 0;   /* too long -> passthrough */

    pthread_mutex_lock(&g_lock);

    NameEntry* e = name_lookup(name);              /* name-based sharing (attach) */
    if (e) {
        e->refcount++;
        *out = (char*)g_blocks[e->block_idx].base + e->off;
        *ret = e->refcount;
        pthread_mutex_unlock(&g_lock);
        return 1;
    }

    int blk; UINT32 off;
    if (!carve(size, flags, &blk, &off)) {         /* pool full / strategy off */
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    e = name_insert(name, blk, off, size, flags);
    if (!e) {                                      /* name table full */
        release(blk, off, size);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    e->refcount = 1;
    *out = (char*)g_blocks[blk].base + off;
    *ret = 1;
    pthread_mutex_unlock(&g_lock);
    return 1;
}

int mm_pool_try_unmap(void* addr, INT32* ret) {
    if (!g_inited || !addr) return 0;

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nblocks; i++) {
        PoolBlock* b = &g_blocks[i];
        char* lo = (char*)b->base;
        if ((char*)addr >= lo && (char*)addr < lo + b->size) {   /* inside this block */
            UINT32 off = (UINT32)((char*)addr - lo);
            NameEntry* e = name_by_slot(i, off);
            if (e && e->refcount > 0) {
                if (--e->refcount == 0)
                    release(i, off, e->size);       /* bump: no-op; entry kept for re-attach */
            }
            if (ret) *ret = 0;                      /* success; never OS-unmap a sub-slot */
            pthread_mutex_unlock(&g_lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return 0;   /* not a pooled address -> caller OS-unmaps */
}
