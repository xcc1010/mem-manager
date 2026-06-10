#include "profiler/shm_profiler.h"

/* The entire profiler exists only in Debug builds. In release this file is an
 * empty translation unit: no code, no pthread / stdio dependency. */
#ifdef MEM_MANAGER_PROFILE

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  include <process.h>
#  define mm_getpid _getpid
#else
#  include <unistd.h>
#  define mm_getpid getpid
#endif

/* ----------------------------------------------------- small utilities --- */
static int64_t now_ns(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static char* mm_strdup(const char* s) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char* d = (char*)malloc(n);
    if (d) {
        memcpy(d, s, n);
    }
    return d;
}

static void json_escape_fputs(FILE* f, const char* s) {
    if (!s) {
        return;
    }
    for (const char* p = s; *p; ++p) {
        switch (*p) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f);  break;
        case '\r': fputs("\\r", f);  break;
        case '\t': fputs("\\t", f);  break;
        default:   fputc(*p, f);     break;
        }
    }
}

/* ------------------------------- live set: open-addressing hash table ---- */
typedef struct {
    const void* key;   /* mapping address returned by ShmMap */
    char*       name;  /* owned copy of shmname */
    uint32      size;
    int64_t     t_map;
    int         state; /* 0 empty, 1 used, 2 tombstone */
} Entry;

typedef struct {
    Entry*  slots;
    size_t  cap;
    size_t  count; /* used slots */
    size_t  tombs;
} Table;

static size_t hash_ptr(const void* p) {
    uintptr_t x = (uintptr_t)p;
    x ^= x >> 33;
    x *= (uintptr_t)0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (size_t)x;
}

static void table_init(Table* t) {
    t->cap = 64;
    t->count = 0;
    t->tombs = 0;
    t->slots = (Entry*)calloc(t->cap, sizeof(Entry));
}

static void table_grow(Table* t) {
    size_t oldcap = t->cap;
    Entry* old = t->slots;
    t->cap *= 2;
    t->count = 0;
    t->tombs = 0;
    t->slots = (Entry*)calloc(t->cap, sizeof(Entry));
    for (size_t i = 0; i < oldcap; ++i) {
        if (old[i].state == 1) {
            size_t h = hash_ptr(old[i].key) & (t->cap - 1);
            while (t->slots[h].state == 1) {
                h = (h + 1) & (t->cap - 1);
            }
            t->slots[h] = old[i];
            t->count++;
        }
    }
    free(old);
}

static void table_put(Table* t, const void* key, const char* name,
                      uint32 size, int64_t t_map) {
    if ((t->count + t->tombs) * 4 >= t->cap * 3) {
        table_grow(t);
    }
    size_t h = hash_ptr(key) & (t->cap - 1);
    size_t first_tomb = (size_t)-1;
    for (;;) {
        int st = t->slots[h].state;
        if (st == 0) {
            break;
        }
        if (st == 2 && first_tomb == (size_t)-1) {
            first_tomb = h;
        }
        if (st == 1 && t->slots[h].key == key) { /* re-map of same address */
            free(t->slots[h].name);
            t->slots[h].name = mm_strdup(name);
            t->slots[h].size = size;
            t->slots[h].t_map = t_map;
            return;
        }
        h = (h + 1) & (t->cap - 1);
    }
    if (first_tomb != (size_t)-1) {
        h = first_tomb;
        t->tombs--;
    }
    t->slots[h].key = key;
    t->slots[h].name = mm_strdup(name);
    t->slots[h].size = size;
    t->slots[h].t_map = t_map;
    t->slots[h].state = 1;
    t->count++;
}

static Entry* table_find(Table* t, const void* key) {
    if (!t->slots) {
        return NULL;
    }
    size_t h = hash_ptr(key) & (t->cap - 1);
    for (;;) {
        int st = t->slots[h].state;
        if (st == 0) {
            return NULL;
        }
        if (st == 1 && t->slots[h].key == key) {
            return &t->slots[h];
        }
        h = (h + 1) & (t->cap - 1);
    }
}

static void table_erase(Table* t, Entry* e) {
    free(e->name);
    e->name = NULL;
    e->state = 2;
    t->count--;
    t->tombs++;
}

/* ------------------------------------------------------- global state ---- */
static struct {
    pthread_mutex_t mu;
    FILE*    out;
    int      inited;
    int      pid;
    Table    live;
    uint64_t live_bytes;
    uint64_t peak_count;
    uint64_t peak_bytes;
    uint64_t total_map_calls;
    uint64_t total_map_failures;
    uint64_t total_unmaps;
    uint64_t unmatched_unmaps;
    unsigned long tid_counter;
} g = { PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0,
        { NULL, 0, 0, 0 }, 0, 0, 0, 0, 0, 0, 0, 0 };

/* Stable, portable per-thread id (avoids non-portable pthread_self formatting). */
static _Thread_local unsigned long t_tid = 0;

static void build_path(char* buf, size_t n, int pid) {
    const char* env = getenv("MEM_PROFILE_PATH");
    if (env && env[0]) {
        const char* pc = strstr(env, "%p");
        if (pc) {
            snprintf(buf, n, "%.*s%d%s", (int)(pc - env), env, pid, pc + 2);
        } else {
            snprintf(buf, n, "%s", env);
        }
    } else {
        snprintf(buf, n, "shm_profile.%d.jsonl", pid);
    }
}

static void profiler_atexit(void);

/* Caller must hold g.mu. */
static void ensure_init(void) {
    if (g.inited) {
        return;
    }
    g.pid = (int)mm_getpid();
    table_init(&g.live);
    char path[1024];
    build_path(path, sizeof path, g.pid);
    g.out = fopen(path, "ab");
    atexit(profiler_atexit);
    g.inited = 1;
}

static void write_common_tail(FILE* f) {
    fprintf(f, ",\"pid\":%d,\"live_count\":%zu,\"live_bytes\":%llu,\"tid\":\"%lu\"}\n",
            g.pid, g.live.count, (unsigned long long)g.live_bytes, t_tid);
}

/* ------------------------------------------------------------- public ---- */
void shm_profiler_on_map(const char* op, const char* shmname, const void* addr,
                         uint32 size, INT32 flags, INT32 ret, const char* pgname) {
    pthread_mutex_lock(&g.mu);
    ensure_init();
    if (t_tid == 0) {
        t_tid = ++g.tid_counter;
    }
    int64_t t = now_ns();

    ++g.total_map_calls;
    if (ret != 0) {
        ++g.total_map_failures;
    }
    if (ret == 0 && addr) {
        table_put(&g.live, addr, shmname, size, t);
        g.live_bytes += size;
        if (g.live.count > g.peak_count) {
            g.peak_count = g.live.count;
        }
        if (g.live_bytes > g.peak_bytes) {
            g.peak_bytes = g.live_bytes;
        }
    }

    if (g.out) {
        fprintf(g.out, "{\"ts_ns\":%lld,\"event\":\"%s\",\"shmname\":\"",
                (long long)t, op);
        json_escape_fputs(g.out, shmname);
        fprintf(g.out, "\",\"addr\":\"%p\",\"size\":%u,\"flags\":%d,\"ret\":%d",
                addr, (unsigned)size, (int)flags, (int)ret);
        if (pgname) {
            fputs(",\"pgname\":\"", g.out);
            json_escape_fputs(g.out, pgname);
            fputc('"', g.out);
        }
        write_common_tail(g.out);
        fflush(g.out);
    }
    pthread_mutex_unlock(&g.mu);
}

void shm_profiler_on_unmap(const void* addr, INT32 ret) {
    pthread_mutex_lock(&g.mu);
    ensure_init();
    if (t_tid == 0) {
        t_tid = ++g.tid_counter;
    }
    int64_t t = now_ns();

    ++g.total_unmaps;
    Entry* e = table_find(&g.live, addr);

    if (g.out) {
        fprintf(g.out, "{\"ts_ns\":%lld,\"event\":\"unmap\",\"addr\":\"%p\",\"ret\":%d",
                (long long)t, addr, (int)ret);
        if (e) {
            g.live_bytes -= e->size;
            fputs(",\"matched\":true,\"shmname\":\"", g.out);
            json_escape_fputs(g.out, e->name);
            fprintf(g.out, "\",\"size\":%u,\"lifetime_ns\":%lld",
                    (unsigned)e->size, (long long)(t - e->t_map));
            table_erase(&g.live, e);
        } else {
            ++g.unmatched_unmaps;
            fputs(",\"matched\":false", g.out);
        }
        write_common_tail(g.out);
        fflush(g.out);
    } else if (e) {
        g.live_bytes -= e->size;
        table_erase(&g.live, e);
    } else {
        ++g.unmatched_unmaps;
    }
    pthread_mutex_unlock(&g.mu);
}

static void profiler_atexit(void) {
    pthread_mutex_lock(&g.mu);
    if (g.out) {
        int64_t t = now_ns();
        for (size_t i = 0; i < g.live.cap; ++i) {
            if (g.live.slots[i].state == 1) {
                Entry* e = &g.live.slots[i];
                fprintf(g.out, "{\"ts_ns\":%lld,\"event\":\"live_at_exit\",\"shmname\":\"",
                        (long long)t);
                json_escape_fputs(g.out, e->name);
                fprintf(g.out, "\",\"addr\":\"%p\",\"size\":%u,\"age_ns\":%lld,\"pid\":%d}\n",
                        e->key, (unsigned)e->size, (long long)(t - e->t_map), g.pid);
            }
        }
        fprintf(g.out,
                "{\"ts_ns\":%lld,\"event\":\"summary\",\"pid\":%d,"
                "\"peak_live_count\":%llu,\"peak_live_bytes\":%llu,"
                "\"total_maps\":%llu,\"total_map_failures\":%llu,"
                "\"total_unmaps\":%llu,\"unmatched_unmaps\":%llu,"
                "\"still_live_at_exit\":%zu}\n",
                (long long)t, g.pid,
                (unsigned long long)g.peak_count, (unsigned long long)g.peak_bytes,
                (unsigned long long)g.total_map_calls,
                (unsigned long long)g.total_map_failures,
                (unsigned long long)g.total_unmaps,
                (unsigned long long)g.unmatched_unmaps,
                g.live.count);
        fflush(g.out);
        fclose(g.out);
        g.out = NULL;
    }
    pthread_mutex_unlock(&g.mu);
}

#endif /* MEM_MANAGER_PROFILE */
