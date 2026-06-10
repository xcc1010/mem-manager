#include "platform/platform_shm.h"

#include "os/os_shm.h"

#ifdef MEM_MANAGER_PROFILE
#  include "profiler/shm_profiler.h"
#endif

extern "C" {

INT32 Platform_ShmMap(INT32 flags, char* shmname, uint32 size, void** shm) {
    const INT32 ret = ShmMap(flags, shmname, size, shm);
#ifdef MEM_MANAGER_PROFILE
    platform::ShmProfiler::instance().on_map(
        "map", shmname, (ret == 0 && shm) ? *shm : nullptr, size, flags, ret, nullptr);
#endif
    return ret;
}

INT32 Platform_ShmMapOnPg(INT32 flags, char* pgname, char* shmname, uint32 size, void** shm) {
    const INT32 ret = ShmMapOnPg(flags, pgname, shmname, size, shm);
#ifdef MEM_MANAGER_PROFILE
    platform::ShmProfiler::instance().on_map(
        "map_on_pg", shmname, (ret == 0 && shm) ? *shm : nullptr, size, flags, ret, pgname);
#endif
    return ret;
}

INT32 Platform_ShmUnmap(void* shm) {
    const INT32 ret = ShmUnmap(shm);
#ifdef MEM_MANAGER_PROFILE
    platform::ShmProfiler::instance().on_unmap(shm, ret);
#endif
    return ret;
}

} // extern "C"
