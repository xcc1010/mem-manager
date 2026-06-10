#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform_shm.h"

static void set_env(const char* key, const char* value) {
#ifdef _WIN32
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}

int main(void) {
    /* Direct the profiler at a test-local file BEFORE any Platform_Shm* call,
     * since the profiler reads MEM_PROFILE_PATH when first used. */
    const char* profile = "test_shm_profile.jsonl";
    remove(profile);
    set_env("MEM_PROFILE_PATH", profile);

    /* --- pass-through correctness (backed by the malloc stub) --- */
    void* p = NULL;
    char name[] = "test/block";
    INT32 ret = Platform_ShmMap(0, name, 256, &p);
    assert(ret == 0);
    assert(p != NULL);
    memset(p, 0xAB, 256);
    assert(*(unsigned char*)p == 0xAB);
    assert(Platform_ShmUnmap(p) == 0);

    void* q = NULL;
    char name2[] = "test/onpg";
    char pg[] = "pg0";
    ret = Platform_ShmMapOnPg(0, pg, name2, 128, &q);
    assert(ret == 0);
    assert(q != NULL);
    assert(Platform_ShmUnmap(q) == 0);

#ifdef MEM_MANAGER_PROFILE
    /* In a Debug build the calls above must have produced profile records. */
    FILE* f = fopen(profile, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)n + 1);
    assert(buf);
    size_t rd = fread(buf, 1, (size_t)n, f);
    buf[rd] = '\0';
    fclose(f);

    assert(strstr(buf, "\"event\":\"map\""));
    assert(strstr(buf, "\"event\":\"map_on_pg\""));
    assert(strstr(buf, "\"event\":\"unmap\""));
    assert(strstr(buf, "\"lifetime_ns\""));
    assert(strstr(buf, "\"pgname\":\"pg0\""));
    assert(strstr(buf, "\"pid\":"));
    assert(strstr(buf, "\"live_count\":"));
    free(buf);
    printf("profile records verified\n");
#endif

    printf("all tests passed\n");
    return 0;
}
