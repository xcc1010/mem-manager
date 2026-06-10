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
                         uint32 size, INT32 flags, INT32 ret, const char* pgname);

/* Record an unmap by address; the matching shmname / size / lifetime are
 * resolved from the live set populated by shm_profiler_on_map. */
void shm_profiler_on_unmap(const void* addr, INT32 ret);

#ifdef __cplusplus
}
#endif

#endif /* MEM_MANAGER_SHM_PROFILER_H */
