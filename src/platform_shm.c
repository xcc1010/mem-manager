#include "platform/platform_shm.h"

#include "os/os_shm.h"

#ifdef MEM_MANAGER_PROFILE
#  include "profiler/shm_profiler.h"
#endif

INT32 Platform_ShmMap(INT32 flags, char* shmname, UINT32 size, void** shm) {
    INT32 ret = ShmMap(flags, shmname, size, shm);
#ifdef MEM_MANAGER_PROFILE
    shm_profiler_on_map("map", shmname, (ret == 0 && shm) ? *shm : NULL,
                        size, flags, ret, NULL);
#endif
    return ret;
}

// ShmMapOnPg exists only in the control-plane link environment; DP builds
// define MEM_MANAGER_NO_SHMMAPONPG to drop this wrapper (and its reference to
// the OS symbol) entirely. See platform_shm.h.
#ifndef MEM_MANAGER_NO_SHMMAPONPG
INT32 Platform_ShmMapOnPg(INT32 flags, char* pgname, char* shmname, UINT32 size, void** shm) {
    INT32 ret = ShmMapOnPg(flags, pgname, shmname, size, shm);
#ifdef MEM_MANAGER_PROFILE
    shm_profiler_on_map("map_on_pg", shmname, (ret == 0 && shm) ? *shm : NULL,
                        size, flags, ret, pgname);
#endif
    return ret;
}
#endif

INT32 Platform_ShmUnmap(void* shm) {
    INT32 ret = ShmUnmap(shm);
#ifdef MEM_MANAGER_PROFILE
    shm_profiler_on_unmap(shm, ret);
#endif
    return ret;
}
