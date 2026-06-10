#pragma once

#include "platform/platform_types.h"

// Declarations of the OS-provided shared-memory primitives. In the business
// project these live in the internal api.h; here we declare them so the wrapper
// compiles standalone, backed by os_shm_stub.cpp (a heap-based fake used for
// local build/test). Define MEM_MANAGER_USE_API_H to use the real ones.
#ifdef MEM_MANAGER_USE_API_H
#  include "api.h"
#else
#  ifdef __cplusplus
extern "C" {
#  endif

INT32 ShmMap(INT32 flags, char* shmname, uint32 size, void** shm);
INT32 ShmMapOnPg(INT32 flags, char* pgname, char* shmname, uint32 size, void** shm);
INT32 ShmUnmap(void* shm);

#  ifdef __cplusplus
}
#  endif
#endif
