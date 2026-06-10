# mem-manager — Design & Architecture (review doc)

Status: **Step 1 implemented & verified.** Step 2 not started.
Last updated for the C rewrite (DP-plane is C-only).

## 1. Problem

The business platform wraps the OS shared-memory primitives. Their **minimum
allocation granularity is 2MB**. Business code allocates many *small* blocks, so
each tiny request burns a full 2MB region — heavy waste.

**End goal:** pre-allocate large shared-memory regions as a pool and
sub-allocate from them, with pool size / policy configurable.

## 2. Two-step plan

1. **Step 1 — profiling pass-through (done).** `Platform_Shm*` forward 1:1 to the
   OS calls; under a Debug build only, each call is recorded so we can measure
   real usage (size / frequency / lifetime / waste) before committing to an
   allocator.
2. **Step 2 — pool allocator (next).** Driven by Step-1 data; configurable pool
   size; see §6.

## 3. System architecture (the platform)

The OS / platform splits into two planes:

- **Control plane (CP):** a single process. The platform `dlopen`s the N
  business `.so`s into it. CP cross-business "sharing" is therefore
  intra-process. CP may use C++.
- **Data plane (DP):** registered to the OS and **pinned to CPU cores**
  (NUMA / core affinity, run-to-completion). DP runs as separate **process
  groups (PGs)** — i.e. distinct processes from CP. `ShmMapOnPg(pgname, …)`
  exists to place memory on the **NUMA node of the bound core**.
  **DP code must be C — no C++.**

Consequences:
- The wrapper + profiler are written in **C** (callable from a C++ CP via
  `extern "C"`, compilable by the DP C toolchain).
- CP and DP are different processes → the profiler writes **one file per
  process** so logs never interleave.
- Most business programs deploy on the **same NUMA as the platform** by default,
  so the NUMA dimension is mostly uniform; per-NUMA pooling matters chiefly for
  the `OnPg` (DP) cases.

## 4. The OS API being wrapped

| Wrapper                | OS call      | Signature |
|------------------------|--------------|-----------|
| `Platform_ShmMap`      | `ShmMap`     | `INT32(INT32 flags, char* shmname, uint32 size, void** shm)` |
| `Platform_ShmMapOnPg`  | `ShmMapOnPg` | `INT32(INT32 flags, char* pgname, char* shmname, uint32 size, void** shm)` |
| `Platform_ShmUnmap`    | `ShmUnmap`   | `INT32(void* shm)` |

- Return code `0` = success (assumed).
- `ShmUnmap` takes only the address → map/unmap are correlated by the returned
  address `*shm`. `shmname` is recorded at map and echoed on the matched unmap.
- **All** allocations (CP and DP) go through these wrappers → the profiler sees
  everything.

### `flags` = GetShmFlags(pgId, lpId, attr) — sharing semantics

`flags` is **not** opaque. It encodes the CP/DP sharing scope and whether every
process sees the **same virtual address (VA)** for the region:

| flags | sharing | VA class |
|------:|---------|----------|
| 0 | CP kernel-thread + DP inter-PG shared | per-proc VA **differs** |
| 1 | CP + DP inter-PG shared | per-proc VA **differs** |
| 2 | CP + DP; DP intra-PG shared | per-proc VA **differs** |
| 3 | CP + DP process shared | process-shared |
| 4 | CP + DP inter-PG shared | per-proc VA **same** |
| 5 | CP + DP; DP intra-PG shared | per-proc VA **same** |

**Why VA matters for Step 2:** in a region whose VA is the **same** across
processes (4/5), you may store raw pointers in shared memory. Where VA
**differs** (0/1/2), stored pointers are meaningless in another process — the
allocator must hand out **offsets**, and metadata inside the region must be
offset-based. Therefore **VA-same and VA-differs memory must never share a
pool.**

Measured reality: **~99% of allocations are VA-same** (user code review). So
Step 2 can be a single VA-same pool covering ~99%, and let the rare VA-differs
pass straight through to `ShmMap` unpooled in v1.

## 5. Step 1 — what is built

### Components (all C)
```
include/platform/platform_shm.h     public API (extern "C")
include/platform/platform_types.h   INT32/uint32 (stdint; swap for api.h)
src/platform_shm.c                   the three wrappers (pass-through + profile)
src/os/os_shm.h, os_shm_stub.c       OS decls + heap stub (standalone only)
src/profiler/shm_profiler.{h,c}      thread-safe JSONL profiler
tools/analyze_profile.py             offline analyser (Python 3 stdlib)
```

### Profiler behaviour
- **Debug-only.** The whole of `shm_profiler.c` is wrapped in
  `#ifdef MEM_MANAGER_PROFILE` (set only in Debug). Release = empty TU: no
  profiler code, **no pthread dependency** (verified: release links without
  `-lpthread`).
- **Thread-safe** (one `pthread_mutex`); negligible vs. a shm syscall.
- **Per-process file:** `MEM_PROFILE_PATH` (a `%p` token → pid), default
  `shm_profile.<pid>.jsonl`.
- **Records** (JSONL, one object/line): every event carries
  `pid, live_count, live_bytes, tid`.
  - `map` / `map_on_pg`: `shmname, addr, size, flags, ret` (+ `pgname`).
  - `unmap`: `addr, ret, matched`; if matched, `shmname, size, lifetime_ns`.
  - At exit: one `live_at_exit` per never-freed block, then a `summary`
    (`peak_live_count/bytes`, total maps/failures/unmaps/unmatched, still-live).
- **Live set:** open-addressing hash table keyed by address (insert on map,
  erase on matched unmap) → resolves lifetime, peak concurrency, leaks.

### Offline analysis (`analyze_profile.py`)
Aggregates one or many files (glob) and reports: size distribution + percentiles,
**waste vs 2MB (KPI)**, peak simultaneously-live (pool-size lower bound),
allocation frequency / peak rate, lifetime distribution, **size × lifetime**
cross-tab (pooling targets = small + short-lived), per-`pgname`/NUMA breakdown,
**sharing semantics by `flags` with a VA-same/VA-differs rollup**, and
never-freed/unmatched.

## 6. Step 2 — direction (not yet built)

Informed by Step-1 data:
- One **VA-same pool** with raw-pointer metadata, covering ~99%.
- **Per-NUMA** pools keyed by `pgname` for the DP/`OnPg` path.
- Pool size from configuration, lower-bounded by observed `peak_live_bytes`.
- A `shmname → (pool, offset)` registry inside `Platform_ShmMap` to preserve
  name-based sharing without a real per-name OS object.
- Rare VA-differs (~1%): pass through to `ShmMap` in v1 (negligible waste); an
  offset-based pool later if needed.
- Must stay **C** (DP constraint).

## 7. Key constraints (the "重点")

1. **DP is C-only** → entire implementation is C11.
2. **Profiling is Debug-only** → zero footprint in release.
3. **VA-same vs VA-differs must not share a pool** (hard correctness rule).
4. **Pass-through must never change behaviour** in Step 1 (profile after the OS
   call, using its real return code / address).
5. **Per-process logs** (CP and DP are different processes).

## 8. Integration notes

- Hand-port `src/platform_shm.c` and `src/profiler/*`; `src/os/*` is only the
  standalone stub.
- Build with `-DMEM_MANAGER_USE_API_H=ON` so `INT32`/`uint32` and the OS `Shm*`
  come from the internal `api.h`.
- Enable `MEM_MANAGER_PROFILE` in the platform's Debug build to capture profiles.
