#pragma once

#include "platform/platform_types.h"

namespace platform {

// Thread-safe profiler that records Platform_Shm* activity to a JSONL file
// (one JSON object per line). Active only when MEM_MANAGER_PROFILE is defined
// (Debug builds); release builds never reference it, so production is a pure
// pass-through with zero overhead.
//
// The output path is taken from the MEM_PROFILE_PATH environment variable,
// defaulting to "shm_profile.jsonl" in the working directory.
class ShmProfiler {
public:
    static ShmProfiler& instance();

    // Record a map / map_on_pg. `addr` is the returned mapping (*shm), or
    // nullptr if the call failed. `op` is "map" or "map_on_pg"; `pgname` is
    // non-null only for map_on_pg.
    void on_map(const char* op, const char* shmname, const void* addr,
                uint32 size, INT32 flags, INT32 ret, const char* pgname);

    // Record an unmap by address. The matching shmname / size / lifetime are
    // resolved from the live table populated by on_map.
    void on_unmap(const void* addr, INT32 ret);

    ShmProfiler(const ShmProfiler&) = delete;
    ShmProfiler& operator=(const ShmProfiler&) = delete;

private:
    ShmProfiler();
    ~ShmProfiler();

    struct Impl;
    Impl* impl_;
};

} // namespace platform
