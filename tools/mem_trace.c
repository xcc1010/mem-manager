/* mem_trace.c -- LD_PRELOAD profiler that attributes a process's *live* memory
 * to the functions that allocated it, covering BOTH malloc/new AND direct mmap.
 *
 * Designed for the awkward case you actually have:
 *   - program uses a custom / mixed allocator  -> tcmalloc aborts with
 *     "free invalid pointer". We do NOT replace the allocator; we wrap it and
 *     delegate to the real malloc/free, so we can never cause that abort.
 *   - valgrind is far too slow to even finish init -> we use byte-sampling, so
 *     overhead is a few percent and the program reaches steady state normally.
 *   - you don't know if the footprint is "many small allocs accumulating" or
 *     "a few big blocks" -> byte-sampling attributes total bytes correctly in
 *     BOTH cases (a callsite that leaks 2 GB as millions of tiny objects shows
 *     up with ~2 GB of sampled weight, same as one that mmaps 2 GB once).
 *   - run machine is old (glibc 2.34, libstdc++ 6.0.24) -> pure C, no libstdc++
 *     dependency. C++ operator new() calls malloc() underneath, so wrapping
 *     malloc already captures new (the backtrace shows operator new -> caller).
 *
 * What it captures:
 *   - malloc / calloc / realloc / free / posix_memalign / aligned_alloc /
 *     memalign  (byte-sampled)
 *   - mmap / munmap / mremap that the program/libraries call through the PLT
 *     (e.g. guest RAM, custom mmap-allocators). NOTE: glibc malloc's *internal*
 *     mmap/sbrk for its own arenas binds to a hidden symbol and bypasses
 *     LD_PRELOAD -- but that memory IS captured at the malloc() layer above.
 *
 * Output: a live-set snapshot (already net of frees), dumped on SIGUSR1 and at
 * exit. Each line is one live allocation with its call stack as module+offset;
 * resolve to functions offline with analyze_mem_trace.py (uses addr2line).
 *
 * Build on the aarch64 target (preferred), or on the build machine -- then
 * verify it needs no symbol newer than the run machine's glibc 2.34:
 *   gcc -shared -fPIC -O2 -o mem_trace.so mem_trace.c -ldl -lpthread
 *   objdump -T mem_trace.so | grep -oE 'GLIBC_[0-9.]+' | sort -V | tail -1
 *     # must print 2.34 or lower
 *
 * Run:
 *   MEM_TRACE_OUT=/tmp/memtrace.dump \
 *   MEM_TRACE_SAMPLE=0x80000 \         # byte sampling interval (512 KiB); lower = more accurate, slower
 *   LD_PRELOAD=$PWD/mem_trace.so ./your_program ...
 *   # at steady state (top shows the ~6G): grab a snapshot without stopping:
 *   kill -USR1 $(pidof your_program) ; cp /tmp/memtrace.dump /tmp/snap.dump
 *   # also grab RSS ground-truth at the same moment, for the mmap side:
 *   cat /proc/$(pidof your_program)/smaps > /tmp/smaps.txt
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#define MAX_FRAMES 64
#define NSHARD     64                 /* power of two */
#define SHARD_CAP  (1u << 14)         /* entries per shard (power of two) */
#define STK_CAP    (1u << 16)         /* max distinct call stacks */

/* ---- real libc entry points ---- */
static void *(*real_malloc)(size_t);
static void *(*real_calloc)(size_t, size_t);
static void *(*real_realloc)(void *, size_t);
static void  (*real_free)(void *);
static int   (*real_posix_memalign)(void **, size_t, size_t);
static void *(*real_aligned_alloc)(size_t, size_t);
static void *(*real_memalign)(size_t, size_t);
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
static int   (*real_munmap)(void *, size_t);
static void *(*real_mremap)(void *, size_t, size_t, int, ...);

/* ---- config ---- */
static const char *out_path = "/tmp/mem_trace.dump";
static size_t sample_bytes  = 1u << 19;   /* 512 KiB */
static size_t mmap_min      = 1u << 12;   /* track mmaps >= 4 KiB */
static int    dump_sig      = SIGUSR1;

static __thread int    in_hook = 0;       /* recursion guard */
static __thread ssize_t to_sample = 0;    /* bytes until next malloc sample */

/* ---- live allocation table (sharded open addressing) ---- */
typedef struct {
    void  *addr;          /* NULL=empty, (void*)1=tombstone */
    size_t size;          /* object's real size */
    size_t weight;        /* estimated bytes this sample represents */
    int    stackid;
    unsigned flags;       /* mmap flags (0 for malloc) */
    char   kind;          /* 'A'=malloc family, 'M'=mmap family */
} Entry;

typedef struct {
    pthread_mutex_t mtx;
    Entry *tab;
} Shard;
static Shard shards[NSHARD];

/* ---- call-stack intern table (append-only; ids stay valid forever) ---- */
typedef struct { uint64_t h; int n; void *fr[MAX_FRAMES]; } Stk;
static Stk  *stks;
static int  *stk_idx;                 /* hash -> stk slot, -1 empty */
static int   stk_used = 0;
static pthread_mutex_t stk_mtx = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t dump_mtx = PTHREAD_MUTEX_INITIALIZER;
static int dump_seq = 0;

/* ---- bootstrap allocator (used only before dlsym resolves real_malloc) ---- */
static char   bootbuf[1 << 16];
static size_t bootoff = 0;
static int from_boot(void *p){ return (char *)p >= bootbuf && (char *)p < bootbuf + sizeof bootbuf; }
static void *boot_alloc(size_t n){
    n = (n + 15) & ~(size_t)15;
    if (bootoff + n > sizeof bootbuf) return NULL;
    void *p = bootbuf + bootoff; bootoff += n; return p;
}

static inline uint64_t mix(uint64_t x){
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33; return x;
}

static void resolve_syms(void){
    in_hook = 1;
    real_malloc         = dlsym(RTLD_NEXT, "malloc");
    real_calloc         = dlsym(RTLD_NEXT, "calloc");
    real_realloc        = dlsym(RTLD_NEXT, "realloc");
    real_free           = dlsym(RTLD_NEXT, "free");
    real_posix_memalign = dlsym(RTLD_NEXT, "posix_memalign");
    real_aligned_alloc  = dlsym(RTLD_NEXT, "aligned_alloc");
    real_memalign       = dlsym(RTLD_NEXT, "memalign");
    real_mmap           = dlsym(RTLD_NEXT, "mmap");
    real_munmap         = dlsym(RTLD_NEXT, "munmap");
    real_mremap         = dlsym(RTLD_NEXT, "mremap");
    in_hook = 0;
}

/* intern a freshly captured stack -> id (append-only). Caller not in_hook-safe;
 * uses real_malloc/real_calloc for its own storage, never the wrappers. */
static int intern_stack(void **fr, int n){
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < n; i++){ h ^= (uintptr_t)fr[i]; h *= 1099511628211ULL; }
    pthread_mutex_lock(&stk_mtx);
    if (!stks){
        stks    = real_calloc(STK_CAP, sizeof(Stk));
        stk_idx = real_malloc(STK_CAP * sizeof(int));
        if (!stks || !stk_idx){ pthread_mutex_unlock(&stk_mtx); return -1; }
        for (size_t i = 0; i < STK_CAP; i++) stk_idx[i] = -1;
    }
    size_t mask = STK_CAP - 1, s = h & mask;
    for (;;){
        int id = stk_idx[s];
        if (id < 0){
            if ((size_t)stk_used >= STK_CAP){ pthread_mutex_unlock(&stk_mtx); return -1; }
            id = stk_used++;
            stks[id].h = h; stks[id].n = n;
            memcpy(stks[id].fr, fr, n * sizeof(void *));
            stk_idx[s] = id;
            pthread_mutex_unlock(&stk_mtx);
            return id;
        }
        if (stks[id].h == h && stks[id].n == n &&
            memcmp(stks[id].fr, fr, n * sizeof(void *)) == 0){
            pthread_mutex_unlock(&stk_mtx);
            return id;
        }
        s = (s + 1) & mask;
    }
}

static void table_put(void *addr, size_t size, size_t weight,
                      unsigned flags, char kind, int stackid){
    uint64_t h = mix((uintptr_t)addr);
    Shard *sh = &shards[h & (NSHARD - 1)];
    pthread_mutex_lock(&sh->mtx);
    if (!sh->tab) sh->tab = real_calloc(SHARD_CAP, sizeof(Entry));
    if (sh->tab){
        size_t mask = SHARD_CAP - 1, s = (h >> 6) & mask, first_tomb = (size_t)-1;
        for (size_t i = 0; i < SHARD_CAP; i++){
            Entry *e = &sh->tab[s];
            if (e->addr == NULL){
                Entry *slot = (first_tomb != (size_t)-1) ? &sh->tab[first_tomb] : e;
                slot->addr = addr; slot->size = size; slot->weight = weight;
                slot->flags = flags; slot->kind = kind; slot->stackid = stackid;
                break;
            }
            if (e->addr == (void *)1){ if (first_tomb == (size_t)-1) first_tomb = s; }
            else if (e->addr == addr){          /* overwrite (e.g. realloc same addr) */
                e->size = size; e->weight = weight;
                e->flags = flags; e->kind = kind; e->stackid = stackid; break;
            }
            s = (s + 1) & mask;
        }
    }
    pthread_mutex_unlock(&sh->mtx);
}

/* remove addr if tracked; returns 1 if it was. */
static int table_del(void *addr){
    uint64_t h = mix((uintptr_t)addr);
    Shard *sh = &shards[h & (NSHARD - 1)];
    int found = 0;
    pthread_mutex_lock(&sh->mtx);
    if (sh->tab){
        size_t mask = SHARD_CAP - 1, s = (h >> 6) & mask;
        for (size_t i = 0; i < SHARD_CAP; i++){
            Entry *e = &sh->tab[s];
            if (e->addr == NULL) break;
            if (e->addr == addr){ e->addr = (void *)1; found = 1; break; }
            s = (s + 1) & mask;
        }
    }
    pthread_mutex_unlock(&sh->mtx);
    return found;
}

/* decide + record a malloc-family allocation using byte sampling */
static void record_alloc(void *p, size_t size){
    if (!p || in_hook) return;
    in_hook = 1;
    size_t weight = 0;
    if (size >= sample_bytes){
        weight = size;                       /* big: always sampled, exact */
    } else {
        to_sample -= (ssize_t)size;
        if (to_sample <= 0){ weight = sample_bytes; to_sample = (ssize_t)sample_bytes; }
    }
    if (weight){
        void *fr[MAX_FRAMES];
        int n = backtrace(fr, MAX_FRAMES);
        int sid = (n > 2) ? intern_stack(fr + 2, n - 2) : -1;  /* drop our 2 frames */
        if (sid >= 0) table_put(p, size, weight, 0, 'A', sid);
    }
    in_hook = 0;
}

static void record_mmap(void *p, size_t size, unsigned flags){
    if (in_hook || size < mmap_min) return;
    in_hook = 1;
    void *fr[MAX_FRAMES];
    int n = backtrace(fr, MAX_FRAMES);
    int sid = (n > 2) ? intern_stack(fr + 2, n - 2) : -1;
    if (sid >= 0) table_put(p, size, size, flags, 'M', sid);
    in_hook = 0;
}

/* ---- dump ---- */
static int append_frame(char *buf, int n, int cap, void *pc){
    Dl_info info;
    if (dladdr(pc, &info) && info.dli_fbase){
        uintptr_t off = (uintptr_t)pc - (uintptr_t)info.dli_fbase;
        return snprintf(buf + n, cap - n, " %s+0x%lx",
                        info.dli_fname ? info.dli_fname : "?", (unsigned long)off);
    }
    return snprintf(buf + n, cap - n, " ?+0x%lx", (unsigned long)(uintptr_t)pc);
}

static void do_dump(const char *reason){
    pthread_mutex_lock(&dump_mtx);
    char path[1024];
    int seq = dump_seq++;
    if (seq == 0) snprintf(path, sizeof path, "%s", out_path);
    else          snprintf(path, sizeof path, "%s.%d", out_path, seq);
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0){ pthread_mutex_unlock(&dump_mtx); return; }

    char hdr[256];
    int hn = snprintf(hdr, sizeof hdr,
        "# mem_trace dump (%s) sample=%zu : <K> <addr> <size> <weight> <flags> <frame...>\n",
        reason, sample_bytes);
    if (write(fd, hdr, hn) < 0) {}

    for (int sx = 0; sx < NSHARD; sx++){
        Shard *sh = &shards[sx];
        pthread_mutex_lock(&sh->mtx);
        if (sh->tab){
            for (size_t i = 0; i < SHARD_CAP; i++){
                Entry *e = &sh->tab[i];
                if (e->addr == NULL || e->addr == (void *)1) continue;
                char line[8192];
                int n = snprintf(line, sizeof line, "%c %p %zu %zu 0x%x",
                                 e->kind, e->addr, e->size, e->weight, e->flags);
                if (e->stackid >= 0 && e->stackid < stk_used){
                    Stk *st = &stks[e->stackid];
                    for (int f = 0; f < st->n && n < (int)sizeof(line) - 256; f++)
                        n += append_frame(line, n, (int)sizeof line, st->fr[f]);
                }
                line[n++] = '\n';
                if (write(fd, line, n) < 0) {}
            }
        }
        pthread_mutex_unlock(&sh->mtx);
    }
    close(fd);
    pthread_mutex_unlock(&dump_mtx);
}

static void *dumper_thread(void *_unused){
    (void)_unused;
    sigset_t set; sigemptyset(&set); sigaddset(&set, dump_sig);
    for (;;){ int s; if (sigwait(&set, &s) == 0) do_dump("signal"); }
    return NULL;
}

static void atexit_dump(void){ do_dump("atexit"); }

__attribute__((constructor))
static void mt_init(void){
    in_hook = 1;
    resolve_syms();
    for (int i = 0; i < NSHARD; i++) pthread_mutex_init(&shards[i].mtx, NULL);

    const char *e;
    if ((e = getenv("MEM_TRACE_OUT")))    out_path = e;
    if ((e = getenv("MEM_TRACE_SAMPLE"))) sample_bytes = (size_t)strtoull(e, NULL, 0);
    if ((e = getenv("MEM_TRACE_MMAP_MIN")))mmap_min   = (size_t)strtoull(e, NULL, 0);
    if ((e = getenv("MEM_TRACE_SIG")))    dump_sig    = (int)strtol(e, NULL, 0);
    if (sample_bytes == 0) sample_bytes = 1;
    to_sample = (ssize_t)sample_bytes;

    sigset_t set; sigemptyset(&set); sigaddset(&set, dump_sig);
    pthread_sigmask(SIG_BLOCK, &set, NULL);     /* so only the dumper gets it */
    pthread_t th; pthread_create(&th, NULL, dumper_thread, NULL);
    atexit(atexit_dump);
    in_hook = 0;
}

/* =================== interposed entry points =================== */
void *malloc(size_t size){
    if (!real_malloc){ resolve_syms(); if (!real_malloc) return boot_alloc(size); }
    void *p = real_malloc(size);
    record_alloc(p, size);
    return p;
}

void *calloc(size_t n, size_t sz){
    if (!real_calloc){
        resolve_syms();
        if (!real_calloc){ void *p = boot_alloc(n * sz); if (p) memset(p, 0, n * sz); return p; }
    }
    void *p = real_calloc(n, sz);
    record_alloc(p, n * sz);
    return p;
}

void *realloc(void *old, size_t size){
    if (!real_realloc){ resolve_syms(); }
    if (from_boot(old)){
        void *p = real_malloc ? real_malloc(size) : boot_alloc(size);
        if (p && size) memcpy(p, old, size);   /* old size unknown; copy up to new */
        record_alloc(p, size);
        return p;
    }
    if (old) table_del(old);
    void *p = real_realloc(old, size);
    record_alloc(p, size);
    return p;
}

void free(void *p){
    if (!p) return;
    if (from_boot(p)) return;                   /* bootstrap memory: never freed */
    if (!real_free){ resolve_syms(); }
    table_del(p);
    real_free(p);
}

int posix_memalign(void **mp, size_t al, size_t size){
    if (!real_posix_memalign){ resolve_syms(); }
    int r = real_posix_memalign(mp, al, size);
    if (r == 0) record_alloc(*mp, size);
    return r;
}

void *aligned_alloc(size_t al, size_t size){
    if (!real_aligned_alloc){ resolve_syms(); }
    void *p = real_aligned_alloc(al, size);
    record_alloc(p, size);
    return p;
}

void *memalign(size_t al, size_t size){
    if (!real_memalign){ resolve_syms(); }
    void *p = real_memalign(al, size);
    record_alloc(p, size);
    return p;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off){
    if (!real_mmap) return (void *)syscall(SYS_mmap, addr, len, prot, flags, fd, off);
    void *r = real_mmap(addr, len, prot, flags, fd, off);
    if (r != MAP_FAILED) record_mmap(r, len, (unsigned)flags);
    return r;
}

int munmap(void *addr, size_t len){
    if (!in_hook) table_del(addr);
    if (!real_munmap) return (int)syscall(SYS_munmap, addr, len);
    return real_munmap(addr, len);
}

void *mremap(void *old, size_t oldsz, size_t newsz, int flags, ...){
    void *newaddr = NULL;
    if (flags & MREMAP_FIXED){ va_list ap; va_start(ap, flags); newaddr = va_arg(ap, void *); va_end(ap); }
    if (!real_mremap) resolve_syms();
    void *r = real_mremap(old, oldsz, newsz, flags, newaddr);
    if (r != MAP_FAILED){ table_del(old); record_mmap(r, newsz, MAP_ANONYMOUS); }
    return r;
}
