#ifndef MEM_MANAGER_PLATFORM_TYPES_H
#define MEM_MANAGER_PLATFORM_TYPES_H

/* INT32 / UINT32 come from the business project's internal api.h. For a
 * standalone build of mem-manager we define compatible aliases here; when
 * integrating, define MEM_MANAGER_USE_API_H so they come from api.h instead.
 * Valid in both C and C++. */
#ifdef MEM_MANAGER_USE_API_H
#  include "api.h"   /* INT32/UINT32, the OS Shm*, and the business IS_CP macro */
#else
#  include <stdint.h>
typedef int32_t  INT32;
typedef uint32_t UINT32;
typedef int64_t  INT64;
typedef uint64_t UINT64;
/* Standalone build has no api.h and hence no IS_CP. Default to the control
 * plane (file profiler backend, ShmMapOnPg present); pass -DIS_DP to exercise
 * the data-plane path locally. */
#  if !defined(IS_CP) && !defined(IS_DP)
#    define IS_CP
#  endif
#endif

/* CP vs DP is the business's own IS_CP macro: defined on the control plane,
 * absent on the data plane. mem-manager keys both of its plane-specific
 * behaviours off it directly — the OnPg wrapper (CP-only OS symbol) and the
 * profiler backend (DP has no libc). No mem-manager-specific plane flag. */

#endif /* MEM_MANAGER_PLATFORM_TYPES_H */
