# mem-manager

Shared-memory pooling for the business project. The OS shm primitives
(`ShmMap` / `ShmMapOnPg` / `ShmUnmap`) have a 2MB minimum granularity, so many
small allocations waste memory. mem-manager wraps them as `Platform_Shm*` and
will eventually pre-allocate large regions and sub-allocate from a pool.

- **Step 1 (current):** `Platform_Shm*` pass through to the OS calls, plus a
  Debug-only profiler that records request size / frequency / lifetime. See
  [docs/STEP1.md](docs/STEP1.md).
- **Step 2 (planned):** a configurable pool allocator informed by Step-1 data.

## Requirements

- A C++20 compiler (MSVC, GCC, or Clang)
- CMake 3.16+

## Build

```sh
cmake -S . -B build
cmake --build build
```

Debug builds (the default) enable profiling; Release builds are pure
pass-through. To build against the real OS API instead of the bundled heap stub:

```sh
cmake -S . -B build -DMEM_MANAGER_USE_API_H=ON
```

## Run

```sh
./build/mem_manager              # Linux/macOS
.\build\Debug\mem_manager.exe    # Windows (MSVC)
```

A Debug run writes profile records to `shm_profile.jsonl` (override with the
`MEM_PROFILE_PATH` environment variable).

## Test

```sh
ctest --test-dir build --output-on-failure
```

## Project layout

```
include/platform/      public API: platform_shm.h, platform_types.h
src/                    Platform_Shm* wrappers (platform_shm.cpp)
src/os/                 OS Shm* declarations + heap stub for standalone builds
src/profiler/           thread-safe JSONL profiler (Debug only)
tests/                  unit tests (CTest)
docs/STEP1.md           profile format + analysis notes
```
