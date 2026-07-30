# mem-manager

Shared-memory pooling for the business platform. The OS shm primitives
(`ShmMap` / `ShmMapOnPg` / `ShmUnmap`) have a 2MB minimum granularity, so many
small allocations waste memory. mem-manager wraps them as `Platform_Shm*` and
will eventually pre-allocate large regions and sub-allocate from a pool.

Written in **C (C11)** so the data-plane (DP) C-only toolchain can compile it; a
C++ control plane calls the same functions via the `extern "C"` headers.

- **Step 1 (done):** `Platform_Shm*` pass through to the OS calls, plus a
  Debug-only profiler that records request size / frequency / lifetime / sharing
  semantics. See [docs/STEP1.md](docs/STEP1.md).
- **Step 2 (v1 done, in trial):** a bump pool allocator driven by a JSON config
  file. See [docs/STEP2_DESIGN.zh.md](docs/STEP2_DESIGN.zh.md).

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

To enable the Step-2 pool allocator (bump, v1):

```sh
cmake -S . -B build -DMEM_MANAGER_POOL=ON
```

## Pool configuration

With the pool enabled, small poolable requests (VA-same flags 4/5, size below
`threshold`) are carved from shared 2MB-class OS blocks; everything else passes
through to the OS unchanged. Configuration (see [config/pool.json](config/pool.json)):

```json
{ "enable": true, "threshold": "0x200000", "block_size": "0x4000000",
  "poolable_flags": [0] }
```

- The meta creator reads the JSON file named by `MEM_POOL_CONFIG` (flat object;
  unknown keys ignored; sizes accept decimal numbers or `"0x.."` strings) and
  publishes the resulting config in shared metadata — attaching processes never
  read the file. `MEM_POOL_CONFIG` is the only environment variable used, and
  only to locate the file; there are no per-knob env overrides.
- **Boot order (required):** the Simulator (CP) must call `mm_pool_init(NULL)`
  — or make any `Platform_ShmMap` call — during its own init, before `loadPG`
  of any PG, so `mmpool/meta` is created exactly once and every PG only
  attaches. A PG that ends up creator anyway logs a WARN (ordering violated).
- Precedence: built-in defaults < JSON file. Rollback = `"enable": false`.

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
src/pool/               Step-2 bump pool allocator (mm_pool.*), -DMEM_MANAGER_POOL=ON
src/profiler/           thread-safe JSONL profiler, Debug-only (shm_profiler.c)
config/pool.json        pool configuration example (JSON)
tools/analyze_profile.py  offline profile analyser
tests/                  unit tests (CTest)
docs/                   DESIGN.md (architecture) + STEP1.md (profile format)
```
