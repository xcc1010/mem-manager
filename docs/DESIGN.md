# mem-manager — Design & Architecture (review doc)

Status: **Step 1 implemented & verified. Step 2 v1 (bump pool) implemented** and
in internal-project trial; slab/freelist strategies and OnPg/NUMA pooling are
out of scope for v1.
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
2. **Step 2 — pool allocator (v1 done, bump-only).** Driven by Step-1 data;
   configurable via a JSON config file; see §6.

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
| `Platform_ShmMap`      | `ShmMap`     | `INT32(INT32 flags, char* shmname, UINT32 size, void** shm)` |
| `Platform_ShmMapOnPg`  | `ShmMapOnPg` | `INT32(INT32 flags, char* pgname, char* shmname, UINT32 size, void** shm)` |
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
offset-based. Therefore **different flags values must never share a pool
block** (both across VA classes and across sharing scopes).

Measured reality (corrected 2026-07, supersedes the earlier "~99% VA-same"
estimate): **the business workload is ~all VA-differs**. Step 2 therefore
pools VA-differs directly: the shared metadata stores only offsets/indices,
and each process attaches every pool block by name and resolves addresses
against its own local VA. Default poolable flags = **{0}** — business type-1
(CP+DP inter-PG shared), which api.h #defines as `0U`: the mask tests flag
VALUES, not type numbers.

## 5. Step 1 — what is built

### Components (all C)
```
include/platform/platform_shm.h     public API (extern "C")
include/platform/platform_types.h   INT32/UINT32 (stdint; swap for api.h)
src/platform_shm.c                   the three wrappers (pass-through + profile)
src/os/os_shm.h, os_shm_stub.c       OS decls + heap stub (standalone only)
src/profiler/shm_profiler.{h,c}      thread-safe JSONL profiler
tools/analyze_profile.py             offline analyser (Python 3 stdlib)
```

### Profiler behaviour
- **Debug-only.** The whole of `shm_profiler.c` is wrapped in
  `#ifdef MEM_MANAGER_PROFILE` (set only in Debug). Release = empty TU: no
  profiler code, no dependency at all.
- **No pthread / no libc lock:** the one lock is a C11 `atomic_flag` spinlock,
  so the profiler links without `-lpthread` anywhere. (Debug-only diagnostic
  code — it keeps the original stdatomic implementation; only the production
  pool uses the platform `AAA_*` primitives.)
- **Two backends** (compile-time, because the DP link environment has no libc —
  see §3 and docs/STEP1.md):
  - **CP (default):** writes JSONL to a per-process file via `open`/`write`,
    keeps a live-set hash table to resolve lifetime / peak / leaks inline, and
    emits `live_at_exit` + `summary` at `atexit`. Records carry
    `ts_ns, pid, shmname, addr, size, flags, ret` (+ `pgname`), and on unmap
    `matched` / `lifetime_ns`.
  - **DP (selected when `IS_CP` is absent):** stateless; formats each event as one JSON
    line and emits it through the OS `LOG_FILE_INFO(module, …)` macro. Time and
    identity come from the log's own line prefix (wall-clock on CP, `[pg][vcpu]
    [TSC]` on DP); a single atomic reentrancy flag prevents recursion if a log
    path calls `ShmMap`. Records are **raw** map/unmap events.
- **Per-process file (CP):** `MEM_PROFILE_PATH` (a `%p` token → pid), default
  `shm_profile.<pid>.jsonl`.

### Offline analysis (`analyze_profile.py`)
Aggregates one or many files (glob) and reports: size distribution + percentiles,
**waste vs 2MB (KPI)**, peak simultaneously-live (pool-size lower bound),
allocation frequency / peak rate, lifetime distribution, **size × lifetime**
cross-tab (pooling targets = small + short-lived), per-`pgname`/NUMA breakdown,
**sharing semantics by `flags` with a VA-same/VA-differs rollup**, and
never-freed/unmatched.

It accepts both schemas: pure JSONL (CP file backend) and OS-log lines (DP
`VIA_LOG` backend — filter with `--module`, it strips the prefix, extracts the
`{json}`, and recovers `ts_ns`/identity from `[pg][vcpu][TSC]` or `[date+us]`;
`--tsc-ghz` converts TSC cycles to ns). Lifetime, peak simultaneously-live and
never-freed are reconstructed by correlating map↔unmap by address per context,
so the DP backend can stay a dumb raw-event emitter.

## 6. Step 2 — what is built (v1, bump-only)

Implemented in `src/pool/mm_pool.{h,c}` (compiled with `-DMEM_MANAGER_POOL=ON`;
default OFF keeps `Platform_ShmMap` a pure Step-1 passthrough):

- **Scope:** poolable flags default to **{0}** (business type-1 = VA-differs,
  CP+DP inter-PG shared — the dominant business usage; api.h #defines it as
  `0U`); requests `>= threshold` and any flags outside the mask pass straight
  through to `ShmMap`.
- **VA-differs addressing:** the shared metadata (`mmpool/meta`, flags 1)
  stores only offsets/indices — never pointers. Each process attaches every
  pool block's OS region by name and resolves `address = local_base[block_idx]
  + off` against its own VA (same code path also works for VA-same flags).
- **BUMP strategy only** (2026-07 scope decision: the workload is dominated by
  init-once/long-lived allocations, so bump's O(1) exact carve with no
  per-slot reclaim is enough; slab/freelist were dropped from v1).
- **Single shared pool, shared metadata.** Config, the block table, the name
  registry and the lock live in a fixed-name shared region `mmpool/meta`
  (flags 1, CP+DP inter-PG; it stores only offsets/indices, never pointers);
  the creator publishes a ready flag, other processes/planes attach and get
  the same name → slot resolution.
- **Lock-free attach hot path:** `AAA_Atomic32ReadAcquire` on the published
  entry + `AAA_Atomic32IncReturn` on the refcount. Only creation takes a
  cross-process spinlock (`AAA_TrySpinLock` + bounded retries). All
  synchronisation — shared meta and process-local alike — uses the platform's
  `AAA_*` primitives (standalone build maps them to C11 `<stdatomic.h>`;
  pointer-width state uses the 64-bit atomic interfaces), so the DP plane
  compiles it.
- **No split sharing:** a poolable name never falls back to the OS while a
  concurrent create of it may be in flight — the create path spins bounded
  rounds of {try lock, re-check registry} and attaches to the published entry.
  Passthrough happens only when the entry is still missing after the full
  budget (lock frozen by a dead holder, pool/table full), in which case every
  process fails identically and the name is consistently OS-shared.
- **Conflicting re-maps fail loudly:** attach requires identical `flags` and a
  request size that fits the registered slot; a mismatch returns an OS-style
  error (`ret < 0`, `*shm = NULL`) instead of silently truncating or splitting.
- **Name-based sharing preserved:** an open-addressing `shmname → (block,
  offset)` registry with the OS-style refcount returned from `Platform_ShmMap`.
- **Per-flags isolation:** different flags values never share a pool block
  (different sharing scopes; blocks are segregated per flags).
- **Config:** a flat JSON file (path from `MEM_POOL_CONFIG`; see
  `config/pool.json`) read by the meta creator; attachers use the snapshot in
  shared meta. Exactly two sources — built-in defaults < JSON file; there are
  no per-knob env overrides. Rollback = `"enable": false` in the file, with
  the precondition that it only takes effect when the meta region is
  (re)created, i.e. after all processes have detached from it.

Deferred (not in v1): per-slot reclaim (needs slab/freelist), pooled-unmap
refcounting (profilers report pooled regions as never-freed),
passthrough-name markers, per-vcpu cache layout (the watchdog re-attaches on
every context switch). OnPg IS pooled since MMPOOL05: blocks are segregated
per (flags, pgname) and created via ShmMapOnPg on the CP; any process
attaches the slot by name with plain ShmMap — a name is ONE slot regardless
of the API (mixed OnPg/plain usage warns on NUMA locality mismatch but never
splits).
Accepted trade-off: the OS `ShmMap` for a new pool block runs inside the
create lock (taking it out would need a CREATING-state machine to avoid
duplicate name registration); a holder crashing there freezes the pool, which
degrades safely to consistent passthrough for new names.

## 7. Key constraints (the "重点")

1. **DP is C-only** → entire implementation is C11.
2. **Profiling is Debug-only** → zero footprint in release.
3. **Different flags values must not share a pool block** (hard correctness
   rule: sharing scope / VA class differ).
4. **Pass-through must never change behaviour** in Step 1 (profile after the OS
   call, using its real return code / address).
5. **Per-process logs** (CP and DP are different processes).

## 8. Integration notes

- Hand-port `src/platform_shm.c` and `src/profiler/*`; `src/os/*` is only the
  standalone stub.
- Build with `-DMEM_MANAGER_USE_API_H=ON` so `INT32`/`UINT32`, the OS `Shm*` and
  the business `IS_CP` macro come from the internal `api.h`.
- Enable `MEM_MANAGER_PROFILE` in the platform's Debug build to capture profiles.
- **Boot-time cleanup + single-point creation (REQUIRED).** During its own
  init, before `loadPG` of any PG, the Simulator (CP) must:
  1. `mm_pool_cleanup()` — drain leftover `mmpool/meta` and `mmpool/blk-*`
     segments from previous runs. Works with only `ShmMap`/`ShmUnmap`
     (attach to learn the refcount, then unmap that many times) — no
     delete-by-name API needed. Backstop for everything refcount-based
     teardown cannot cover (crashed processes, multi-vcpu attach churn).
  2. `mm_pool_init(NULL)` — or any `Platform_ShmMap` call — creating
     `mmpool/meta` exactly once; every PG then only attaches (`ret >= 2`).
     A PG that still ends up creator (`ret == 1`) logs a WARN (ordering
     violated).
- **Pair every process exit with `mm_pool_uninit()` (REQUIRED).** Each
  process — Simulator and every PG — calls it during teardown (after all
  pool usage has stopped). This detaches its meta/block mappings, so a full
  system stop drops the OS refcounts to zero and the segments are reclaimed:
  the next boot starts from a fresh registry and the "ret==1 ⇒ creator
  initialises" protocol keeps working. Without it the segments (and the
  registry) live forever.
- **No mem-manager CP/DP flag.** mem-manager keys off the business's own `IS_CP`
  macro: when absent (data plane) it drops `Platform_ShmMapOnPg` (a CP-only OS
  symbol — otherwise the DP `.a` fails at link) and selects the log profiler
  backend (DP has no libc). CP and DP builds use the same mem-manager flags. See
  docs/STEP1.md.
