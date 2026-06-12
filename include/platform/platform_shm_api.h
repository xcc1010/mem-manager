#ifndef MEM_MANAGER_PLATFORM_SHM_API_H
#define MEM_MANAGER_PLATFORM_SHM_API_H

/* Business-facing facade header. Include THIS (and only this) from business
 * code that allocates shared memory. It surfaces:
 *   - the OS constants / types from api.h (so business need not import the OS
 *     header itself), and
 *   - the Platform_Shm* wrappers,
 * and it POISONS the raw OS calls ShmMap / ShmMapOnPg / ShmUnmap so business
 * code cannot bypass the wrapper. Routing every allocation through the wrapper
 * is what lets the profiler — and the Step-2 pool — see all of them.
 *
 * IMPORTANT: the wrapper implementation (src/platform_shm.c) must NOT include
 * this header. It legitimately calls the raw ShmMap and includes the
 * un-poisoned headers (api.h / os_shm.h) instead. Poisoning is per-translation-
 * unit, so keeping this facade out of the implementation's include path is
 * exactly what lets the wrapper still call the real OS functions. */

#ifdef MEM_MANAGER_USE_API_H
#  include "api.h"                   /* OS constants + types (real build only) */
#endif
#include "platform/platform_shm.h"   /* Platform_ShmMap / _ShmMapOnPg / _ShmUnmap */

/* Forbid direct use of the raw OS shm calls in any file that includes this
 * facade. Must come AFTER the includes above, so the OS header's own
 * declarations of these names are processed while still legal. GCC/Clang only;
 * other compilers simply skip the guard (losing the compile-time enforcement,
 * nothing else). */
#ifdef __GNUC__
#  pragma GCC poison ShmMap ShmMapOnPg ShmUnmap
#endif

#endif /* MEM_MANAGER_PLATFORM_SHM_API_H */
