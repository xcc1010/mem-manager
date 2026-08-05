#include "platform/platform_shm.h"

#include "os/os_shm.h"

#include <stddef.h>

#ifdef MEM_MANAGER_PROFILE
#  include "profiler/shm_profiler.h"
#endif

#ifdef MEM_MANAGER_POOL
#  include "pool/mm_pool.h"
#endif

INT32 Platform_ShmMap(INT32 flags, char* shmname, UINT32 size, void** shm) {
#ifdef MEM_MANAGER_POOL
    INT32 pooled;
    if (mm_pool_try_map(flags, shmname, size, shm, &pooled)) {
#  ifdef MEM_MANAGER_PROFILE
        shm_profiler_on_map("map", shmname, (shm ? *shm : NULL),
                            size, flags, pooled, NULL);
#  endif
        return pooled;
    }
#endif
    INT32 ret = ShmMap(flags, shmname, size, shm);
#ifdef MEM_MANAGER_PROFILE
    shm_profiler_on_map("map", shmname, (ret >= 0 && shm) ? *shm : NULL,
                        size, flags, ret, NULL);
#endif
    return ret;
}

// ShmMapOnPg exists only in the control-plane link environment, so this wrapper
// is compiled only when IS_CP is defined (see platform_shm.h).
#ifdef IS_CP
INT32 Platform_ShmMapOnPg(INT32 flags, char* pgname, char* shmname, UINT32 size, void** shm) {
#ifdef MEM_MANAGER_POOL
    INT32 pooled;
    if (mm_pool_try_map_pg(flags, pgname, shmname, size, shm, &pooled)) {
#  ifdef MEM_MANAGER_PROFILE
        shm_profiler_on_map("map_on_pg", shmname, (shm ? *shm : NULL),
                            size, flags, pooled, pgname);
#  endif
        return pooled;
    }
#endif
    INT32 ret = ShmMapOnPg(flags, pgname, shmname, size, shm);
#ifdef MEM_MANAGER_PROFILE
    shm_profiler_on_map("map_on_pg", shmname, (ret >= 0 && shm) ? *shm : NULL,
                        size, flags, ret, pgname);
#endif
    return ret;
}
#endif

INT32 Platform_ShmUnmap(void* shm) {
#ifdef MEM_MANAGER_POOL
    INT32 pooled;
    if (mm_pool_try_unmap(shm, &pooled)) {
#  ifdef MEM_MANAGER_PROFILE
        shm_profiler_on_unmap(shm, pooled);
#  endif
        return pooled;
    }
#endif
    INT32 ret = ShmUnmap(shm);
#ifdef MEM_MANAGER_PROFILE
    shm_profiler_on_unmap(shm, ret);
#endif
    return ret;
}
