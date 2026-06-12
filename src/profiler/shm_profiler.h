#ifndef MEM_MANAGER_SHM_PROFILER_H
#define MEM_MANAGER_SHM_PROFILER_H

#include "platform/platform_types.h"

/* Thread-safe profiler that records Platform_Shm* activity to a per-process
 * JSONL file. Written in C so it can be compiled by the DP-plane C toolchain.
 *
 * The whole implementation (shm_profiler.c) is compiled only when
 * MEM_MANAGER_PROFILE is defined (Debug builds); release builds reference
 * none of it, so there is zero overhead and no pthread dependency in
 * production.
 *
 * Output path: MEM_PROFILE_PATH (a literal "%p" in it is replaced by the pid),
 * default "shm_profile.<pid>.jsonl". */
#ifdef __cplusplus
extern "C" {
#endif

/* Record a map / map_on_pg. addr is the returned mapping (*shm), or NULL if
 * the call failed. op is "map" or "map_on_pg"; pgname is non-NULL only for
 * map_on_pg. */
void shm_profiler_on_map(const char* op, const char* shmname, const void* addr,
                         UINT32 size, INT32 flags, INT32 ret, const char* pgname);

/* Record an unmap by address; the matching shmname / size / lifetime are
 * resolved from the live set populated by shm_profiler_on_map. */
void shm_profiler_on_unmap(const void* addr, INT32 ret);

#if defined(MEM_MANAGER_PROFILE) && defined(MEM_PROFILE_SELF_CONTAINED)
/* ---- Self-contained (data-plane) backend -----------------------------------
 * The DP link environment has no libc and no usable platform log (the log path
 * itself calls ShmMap, which would recurse). In this mode the profiler records
 * each event into a fixed static array in BSS — no allocation, no stdio, no
 * log, no syscalls on the hot path (only a C11 atomic_flag spinlock). Time and
 * pid are supplied by the integrator via shm_profiler_set_env (both optional).
 * Accumulated events are turned into JSONL only when shm_profiler_dump() is
 * called, off the hot path, with a sink the caller provides (e.g. at teardown,
 * or from a process/context that can do I/O). */

typedef struct {
    /* Current time in nanoseconds. NULL -> events carry ts_ns 0.
     * MUST NOT call back into Platform_ShmMap (no reentrancy guard is kept). */
    long long (*now_ns)(void);
    /* Process / instance id for the records. NULL -> 0. */
    int (*get_pid)(void);
} shm_profiler_env;

/* Install the time/pid hooks. Safe to call once, early, before any mapping. */
void shm_profiler_set_env(const shm_profiler_env* env);

/* Format every recorded event as one JSONL line and hand it to `emit`
 * (called once per line, plus a final "summary" line). `emit` may do I/O —
 * it runs off the hot path. No-op if emit is NULL. */
void shm_profiler_dump(void (*emit)(const char* buf, unsigned len));

/* Number of events dropped because the static buffer was full. */
unsigned long shm_profiler_dropped(void);
#endif /* MEM_MANAGER_PROFILE && MEM_PROFILE_SELF_CONTAINED */

#ifdef __cplusplus
}
#endif

#endif /* MEM_MANAGER_SHM_PROFILER_H */
