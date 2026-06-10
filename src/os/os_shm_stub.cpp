// Local-only fake implementation of the OS shared-memory primitives, backed by
// the heap. Lets mem-manager build, run, and be tested without the real OS
// library. NOT compiled when MEM_MANAGER_USE_API_H is set (then the real
// implementations from api.h / the OS link are used instead).
#include "os/os_shm.h"

#include <cstdlib>

extern "C" {

INT32 ShmMap(INT32 /*flags*/, char* /*shmname*/, uint32 size, void** shm) {
    if (!shm) {
        return -1;
    }
    *shm = std::malloc(size);
    return *shm ? 0 : -1;
}

INT32 ShmMapOnPg(INT32 flags, char* /*pgname*/, char* shmname, uint32 size, void** shm) {
    // The stub ignores numa placement; it only needs to behave like a mapping.
    return ShmMap(flags, shmname, size, shm);
}

INT32 ShmUnmap(void* shm) {
    std::free(shm);
    return 0;
}

} // extern "C"
