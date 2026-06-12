#include "profiler/shm_profiler.h"

/* The entire profiler exists only in Debug builds. In release this file is an
 * empty translation unit: no code, no dependency at all.
 *
 * DP-friendly I/O: output goes through raw open()/write()/close() (no stdio
 * FILE handles or fflush) and the lock is a C11 atomic_flag spinlock (no
 * pthread). The
 * only runtime symbols required are open/write/close, atexit, malloc/free,
 * getenv and snprintf/vsnprintf (string formatting only, no FILE*). This lets
 * the profiler link in the data-plane (DP) toolchain, which provides neither
 * buffered stdio nor pthread. */
#ifdef MEM_MANAGER_PROFILE

#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>   /* snprintf / vsnprintf only — no FILE* is used */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <fcntl.h>
#ifdef _WIN32
#  include <io.h>
#  include <process.h>
#  include <sys/stat.h>
#  define mm_getpid _getpid
#  define mm_open(path) _open((path), _O_WRONLY | _O_CREAT | _O_APPEND | _O_BINARY, \
                              _S_IREAD | _S_IWRITE)
#  define mm_write     _write
#  define mm_close     _close
#else
#  include <unistd.h>
#  define mm_getpid getpid
#  define mm_open(path) open((path), O_WRONLY | O_CREAT | O_APPEND, 0644)
#  define mm_write     write
#  define mm_close     close
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

static void mm_write_all(int fd, const char* buf, size_t len) {
    if (fd < 0) {
        return;
    }
    size_t off = 0;
    while (off < len) {
        long n = (long)mm_write(fd, buf + off, (unsigned)(len - off));
        if (n <= 0) {
            break;
        }
        off += (size_t)n;
    }
}

/* ------------------------------------------- bounded line buffer (no stdio) */
/* Builds one JSON line in a fixed stack buffer, then a single write() emits it.
 * A single write keeps concurrent lines from interleaving and avoids per-token
 * syscalls. Over-long lines are truncated rather than overflowing the buffer
 * (shmnames are short identifiers, so this is not a concern in practice). */
typedef struct {
    char*  b;
    size_t cap;
    size_t len;
} LineBuf;

static void lb_raw(LineBuf* lb, const char* s, size_t n) {
    size_t room = (lb->len < lb->cap) ? lb->cap - lb->len : 0;
    if (n > room) {
        n = room;
    }
    memcpy(lb->b + lb->len, s, n);
    lb->len += n;
}

static void lb_str(LineBuf* lb, const char* s) {
    if (s) {
        lb_raw(lb, s, strlen(s));
    }
}

static void lb_ch(LineBuf* lb, char c) {
    lb_raw(lb, &c, 1);
}

/* printf-style append for the numeric/format chunks (formatting only). */
static void lb_fmt(LineBuf* lb, const char* fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) {
        lb_raw(lb, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
    }
}

static void lb_json(LineBuf* lb, const char* s) {
    if (!s) {
        return;
    }
    for (const char* p = s; *p; ++p) {
        switch (*p) {
        case '"':  lb_str(lb, "\\\""); break;
        case '\\': lb_str(lb, "\\\\"); break;
        case '\n': lb_str(lb, "\\n");  break;
        case '\r': lb_str(lb, "\\r");  break;
        case '\t': lb_str(lb, "\\t");  break;
        default:   lb_ch(lb, *p);      break;
        }
    }
}

/* ------------------------------- live set: open-addressing hash table ---- */
typedef struct {
    const void* key;   /* mapping address returned by ShmMap */
    char*       name;  /* owned copy of shmname */
    UINT32      size;
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
                      UINT32 size, int64_t t_map) {
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
/* Spinlock instead of pthread_mutex so the profiler needs no pthread. */
static atomic_flag g_lock = ATOMIC_FLAG_INIT;

static void mm_lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_lock, memory_order_acquire)) {
        /* spin: contention is rare and each critical section is tiny */
    }
}

static void mm_unlock(void) {
    atomic_flag_clear_explicit(&g_lock, memory_order_release);
}

static struct {
    int      fd;     /* output file descriptor, -1 until opened */
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
} g = { -1, 0, 0, { NULL, 0, 0, 0 }, 0, 0, 0, 0, 0, 0, 0, 0 };

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

/* Caller must hold the lock. */
static void ensure_init(void) {
    if (g.inited) {
        return;
    }
    g.pid = (int)mm_getpid();
    table_init(&g.live);
    char path[1024];
    build_path(path, sizeof path, g.pid);
    g.fd = mm_open(path);
    atexit(profiler_atexit);
    g.inited = 1;
}

static void lb_common_tail(LineBuf* lb) {
    lb_fmt(lb, ",\"pid\":%d,\"live_count\":%zu,\"live_bytes\":%llu,\"tid\":\"%lu\"}\n",
           g.pid, g.live.count, (unsigned long long)g.live_bytes, t_tid);
}

/* ------------------------------------------------------------- public ---- */
void shm_profiler_on_map(const char* op, const char* shmname, const void* addr,
                         UINT32 size, INT32 flags, INT32 ret, const char* pgname) {
    mm_lock();
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

    if (g.fd >= 0) {
        char buf[2048];
        LineBuf lb = { buf, sizeof buf, 0 };
        lb_fmt(&lb, "{\"ts_ns\":%lld,\"event\":\"", (long long)t);
        lb_str(&lb, op);
        lb_str(&lb, "\",\"shmname\":\"");
        lb_json(&lb, shmname);
        lb_fmt(&lb, "\",\"addr\":\"%p\",\"size\":%u,\"flags\":%d,\"ret\":%d",
               addr, (unsigned)size, (int)flags, (int)ret);
        if (pgname) {
            lb_str(&lb, ",\"pgname\":\"");
            lb_json(&lb, pgname);
            lb_ch(&lb, '"');
        }
        lb_common_tail(&lb);
        mm_write_all(g.fd, lb.b, lb.len);
    }
    mm_unlock();
}

void shm_profiler_on_unmap(const void* addr, INT32 ret) {
    mm_lock();
    ensure_init();
    if (t_tid == 0) {
        t_tid = ++g.tid_counter;
    }
    int64_t t = now_ns();

    ++g.total_unmaps;
    Entry* e = table_find(&g.live, addr);

    if (g.fd >= 0) {
        char buf[2048];
        LineBuf lb = { buf, sizeof buf, 0 };
        lb_fmt(&lb, "{\"ts_ns\":%lld,\"event\":\"unmap\",\"addr\":\"%p\",\"ret\":%d",
               (long long)t, addr, (int)ret);
        if (e) {
            g.live_bytes -= e->size;
            lb_str(&lb, ",\"matched\":true,\"shmname\":\"");
            lb_json(&lb, e->name);
            lb_fmt(&lb, "\",\"size\":%u,\"lifetime_ns\":%lld",
                   (unsigned)e->size, (long long)(t - e->t_map));
            table_erase(&g.live, e);
        } else {
            ++g.unmatched_unmaps;
            lb_str(&lb, ",\"matched\":false");
        }
        lb_common_tail(&lb);
        mm_write_all(g.fd, lb.b, lb.len);
    } else if (e) {
        g.live_bytes -= e->size;
        table_erase(&g.live, e);
    } else {
        ++g.unmatched_unmaps;
    }
    mm_unlock();
}

static void profiler_atexit(void) {
    mm_lock();
    if (g.fd >= 0) {
        int64_t t = now_ns();
        for (size_t i = 0; i < g.live.cap; ++i) {
            if (g.live.slots[i].state == 1) {
                Entry* e = &g.live.slots[i];
                char buf[2048];
                LineBuf lb = { buf, sizeof buf, 0 };
                lb_fmt(&lb, "{\"ts_ns\":%lld,\"event\":\"live_at_exit\",\"shmname\":\"",
                       (long long)t);
                lb_json(&lb, e->name);
                lb_fmt(&lb, "\",\"addr\":\"%p\",\"size\":%u,\"age_ns\":%lld,\"pid\":%d}\n",
                       e->key, (unsigned)e->size, (long long)(t - e->t_map), g.pid);
                mm_write_all(g.fd, lb.b, lb.len);
            }
        }
        char buf[512];
        LineBuf lb = { buf, sizeof buf, 0 };
        lb_fmt(&lb,
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
        mm_write_all(g.fd, lb.b, lb.len);
        mm_close(g.fd);
        g.fd = -1;
    }
    mm_unlock();
}

#endif /* MEM_MANAGER_PROFILE */
