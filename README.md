# mem-manager

Shared-memory pooling for the business platform. The OS shm primitives
(`ShmMap` / `ShmMapOnPg` / `ShmUnmap`) have a 2MB minimum granularity, so many
small allocations waste memory. mem-manager wraps them as `Platform_Shm*` and
will eventually pre-allocate large regions and sub-allocate from a pool.

Written in **C (C11)** so the data-plane (DP) C-only toolchain can compile it; a
C++ control plane calls the same functions via the `extern "C"` headers.

- **Step 1 (current):** `Platform_Shm*` pass through to the OS calls, plus a
  Debug-only profiler that records request size / frequency / lifetime / sharing
  semantics. See [docs/STEP1.md](docs/STEP1.md).
- **Step 2 (planned):** a configurable pool allocator informed by Step-1 data.

Full design, architecture and constraints: **[docs/DESIGN.md](docs/DESIGN.md)**.

## Requirements

- A C11 compiler (GCC / Clang; MSVC for the parts that build there)
- CMake 3.16+
- pthreads (only the Debug profiler; release needs none)

## Build

```sh
cmake -S . -B build
cmake --build build
```

Debug builds (the default) enable profiling; Release builds are pure
pass-through (the profiler compiles to nothing). To build against the real OS
API instead of the bundled heap stub:

```sh
cmake -S . -B build -DMEM_MANAGER_USE_API_H=ON
```

## Run

```sh
./build/mem_manager              # Linux/macOS
.\build\Debug\mem_manager.exe    # Windows
```

A Debug run writes profile records to `shm_profile.<pid>.jsonl` (override with
`MEM_PROFILE_PATH`; a `%p` token in it is replaced by the pid).

## Test

```sh
ctest --test-dir build --output-on-failure
```

## Analyse a profile

```sh
python tools/analyze_profile.py "shm_profile.*.jsonl"
```

## Project layout

```
include/platform/      public API: platform_shm.h, platform_types.h
src/                    Platform_Shm* wrappers (platform_shm.c)
src/os/                 OS Shm* declarations + heap stub for standalone builds
src/profiler/           thread-safe JSONL profiler, Debug-only (shm_profiler.c)
tools/analyze_profile.py  offline profile analyser
tests/                  unit tests (CTest)
docs/                   DESIGN.md (architecture) + STEP1.md (profile format)
```
