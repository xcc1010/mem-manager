#pragma once

#include "platform/platform_types.h"

/* Synchronisation primitives used by the Step-2 pool's shared metadata.
 * In the platform build (MEM_MANAGER_USE_API_H) these come from the internal
 * api.h; the standalone build declares them here and os_shm_stub.c provides
 * C11-stdatomic equivalents so local build/test keeps working. Call sites
 * always use the AAA_* names directly. */
#ifdef MEM_MANAGER_USE_API_H
#  include "api.h"
#else
#  ifdef __cplusplus
extern "C" {
#  endif

typedef void VOID;

typedef struct AAASpinLock {
    UINT32 magic;
    volatile UINT32 lock;
    UINT32 pidmask;
    UINT64 lr;
    UINT64 tsc;
    UINT32 nextoffset;
    UINT32 usingNum;
} __attribute__((aligned(16))) AAASpinLock;

VOID  AAA_InitSpinLock(AAASpinLock *Lock);
INT32 AAA_TrySpinLock(AAASpinLock *Lock);   /* 0 = acquired */
INT32 AAA_SpinLock(AAASpinLock *lockId);    /* 0 = acquired */
INT32 AAA_SpinUnlock(AAASpinLock *lockId);  /* 0 = success  */

INT32 AAA_Atomic32ReadAcquire(INT32 *ptr);
INT32 AAA_Atomic32Read(INT32 *ptr);              /* acq_rel */
VOID  AAA_Atomic32SetRelaxed(INT32 *ptr, INT32 v);
VOID  AAA_Atomic32SetRelease(INT32 *ptr, INT32 v);
INT32 AAA_Atomic32IncReturn(INT32 *ptr);         /* returns the NEW value */

UINT64 AAA_Atomic64ReadAcquire(UINT64 *ptr);
VOID   AAA_Atomic64SetRelease(UINT64 *ptr, UINT64 v);

#  ifdef __cplusplus
}
#  endif
#endif
