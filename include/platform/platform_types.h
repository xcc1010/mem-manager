#ifndef MEM_MANAGER_PLATFORM_TYPES_H
#define MEM_MANAGER_PLATFORM_TYPES_H

/* INT32 / UINT32 come from the business project's internal api.h. For a
 * standalone build of mem-manager we define compatible aliases here; when
 * integrating, define MEM_MANAGER_USE_API_H so they come from api.h instead.
 * Valid in both C and C++. */
#ifdef MEM_MANAGER_USE_API_H
#  include "api.h"
#else
#  include <stdint.h>
typedef int32_t  INT32;
typedef uint32_t UINT32;
#endif

/* One switch for a data-plane build. The DP link environment has no ShmMapOnPg
 * symbol and no libc, so a DP build always wants the OnPg wrapper dropped and
 * the log-based profiler backend. Deriving both from MEM_MANAGER_DP keeps the
 * DP build to a single extra -D; the fine-grained macros still work on their
 * own for unusual combinations. */
#ifdef MEM_MANAGER_DP
#  ifndef MEM_MANAGER_NO_SHMMAPONPG
#    define MEM_MANAGER_NO_SHMMAPONPG
#  endif
#  ifndef MEM_PROFILE_VIA_LOG
#    define MEM_PROFILE_VIA_LOG
#  endif
#endif

#endif /* MEM_MANAGER_PLATFORM_TYPES_H */
