#include <cstring>
#include <iostream>

#include "platform/platform_shm.h"

// Tiny smoke demo of the Step-1 wrappers (backed by the heap stub in a
// standalone build). In a Debug build this also produces shm_profile.jsonl.
int main() {
    void* p = nullptr;
    char name[] = "demo/block";

    INT32 ret = Platform_ShmMap(0, name, 4096, &p);
    std::cout << "Platform_ShmMap ret=" << ret << " addr=" << p << '\n';
    if (ret != 0) {
        return 1;
    }

    std::memset(p, 0, 4096);
    Platform_ShmUnmap(p);
    std::cout << "unmapped\n";
    return 0;
}
