/* Local-only fake implementation of the OS shared-memory primitives, backed by
 * the heap. Lets mem-manager build, run, and be tested without the real OS
 * library. NOT compiled when MEM_MANAGER_USE_API_H is set. */
#include "os/os_shm.h"

#include <stdlib.h>

INT32 ShmMap(INT32 flags, char* shmname, UINT32 size, void** shm) {
    (void)flags;
    (void)shmname;
    if (!shm) {
        return -1;
    }
    *shm = malloc(size);
    /* Mirror the real OS convention: success returns a reference count (>0),
     * failure returns <0. (Returning 0 here previously masked the fact that
     * both planes use ret>0 for success.) */
    return *shm ? 1 : -1;
}

INT32 ShmMapOnPg(INT32 flags, char* pgname, char* shmname, UINT32 size, void** shm) {
    (void)pgname; /* the stub ignores numa placement */
    return ShmMap(flags, shmname, size, shm);
}

INT32 ShmUnmap(void* shm) {
    free(shm);
    return 0;
}
