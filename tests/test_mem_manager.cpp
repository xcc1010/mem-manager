#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "platform/platform_shm.h"

namespace {

void set_env(const char* key, const char* value) {
#ifdef _WIN32
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}

} // namespace

int main() {
    // Direct the profiler at a test-local file BEFORE any Platform_Shm* call,
    // since the profiler reads MEM_PROFILE_PATH when first constructed.
    const char* profile = "test_shm_profile.jsonl";
    std::remove(profile);
    set_env("MEM_PROFILE_PATH", profile);

    // --- pass-through correctness (backed by the malloc stub) ---
    void* p = nullptr;
    char name[] = "test/block";
    INT32 ret = Platform_ShmMap(0, name, 256, &p);
    assert(ret == 0);
    assert(p != nullptr);
    std::memset(p, 0xAB, 256);
    assert(*static_cast<unsigned char*>(p) == 0xAB);
    assert(Platform_ShmUnmap(p) == 0);

    void* q = nullptr;
    char name2[] = "test/onpg";
    char pg[] = "pg0";
    ret = Platform_ShmMapOnPg(0, pg, name2, 128, &q);
    assert(ret == 0);
    assert(q != nullptr);
    assert(Platform_ShmUnmap(q) == 0);

#ifdef MEM_MANAGER_PROFILE
    // In a Debug build the calls above must have produced profile records,
    // including a resolved lifetime for the matched unmap.
    std::ifstream in(profile);
    assert(in.good());
    const std::string contents((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    assert(contents.find("\"event\":\"map\"") != std::string::npos);
    assert(contents.find("\"event\":\"map_on_pg\"") != std::string::npos);
    assert(contents.find("\"event\":\"unmap\"") != std::string::npos);
    assert(contents.find("\"lifetime_ns\"") != std::string::npos);
    assert(contents.find("\"pgname\":\"pg0\"") != std::string::npos);
    std::cout << "profile records verified\n";
#endif

    std::cout << "all tests passed\n";
    return 0;
}
