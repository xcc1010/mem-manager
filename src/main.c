#include <stdio.h>
#include <string.h>

#include "platform/platform_shm.h"

/* Tiny smoke demo of the Step-1 wrappers (backed by the heap stub in a
 * standalone build). In a Debug build this also produces shm_profile.<pid>.jsonl. */
int main(void) {
    void* p = NULL;
    char name[] = "demo/block";

    INT32 ret = Platform_ShmMap(0, name, 4096, &p);
    printf("Platform_ShmMap ret=%d addr=%p\n", (int)ret, p);
    if (ret != 0) {
        return 1;
    }

    memset(p, 0, 4096);
    Platform_ShmUnmap(p);
    printf("unmapped\n");
    return 0;
}
