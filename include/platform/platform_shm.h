#pragma once

#include "platform/platform_types.h"

// Step-1 wrappers around the OS shared-memory APIs. Behaviour is a pure
// pass-through to ShmMap / ShmMapOnPg / ShmUnmap; under a Debug build
// (MEM_MANAGER_PROFILE defined) each call is additionally recorded by the
// profiler so we can analyse request size / frequency / lifetime before
// designing the real pool allocator (Step 2). See docs/STEP1.md.
#ifdef __cplusplus
extern "C" {
#endif

// Map (or create) a shared-memory region. Mirrors ShmMap.
INT32 Platform_ShmMap(INT32 flags, char* shmname, UINT32 size, void** shm);

// Map on the numa node where pgname resides. Mirrors ShmMapOnPg
// (note: pgname is the second parameter).
//
// ShmMapOnPg is a control-plane-only OS symbol; the data-plane link environment
// does not provide it. The wrapper is compiled only on the control plane (where
// the business defines IS_CP); on the data plane it is omitted, so linking the
// DP .a does not fail with "undefined reference to ShmMapOnPg".
#ifdef IS_CP
INT32 Platform_ShmMapOnPg(INT32 flags, char* pgname, char* shmname, UINT32 size, void** shm);
#endif

// Unmap a region previously returned via *shm. Mirrors ShmUnmap.
INT32 Platform_ShmUnmap(void* shm);

#ifdef __cplusplus
}
#endif
