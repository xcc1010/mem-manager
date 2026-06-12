# Step 1 — Profiling pass-through

`Platform_ShmMap` / `Platform_ShmMapOnPg` / `Platform_ShmUnmap` forward 1:1 to the
OS `ShmMap` / `ShmMapOnPg` / `ShmUnmap`. Under a **Debug** build
(`MEM_MANAGER_PROFILE` defined) every call is also recorded so we can study real
allocation patterns before designing the pool allocator (Step 2). Release builds
do not reference the profiler at all — zero overhead.

All allocations on both the control plane (CP) and data plane (DP) go through
these wrappers, so the profiler sees everything. The implementation is **C**
(DP is C-only); the whole of `src/profiler/shm_profiler.c` is wrapped in
`#ifdef MEM_MANAGER_PROFILE`, so release builds contain no profiler code and no
pthread dependency.

## Mapping to the OS API

| Wrapper                  | OS call      | Signature |
|--------------------------|--------------|-----------|
| `Platform_ShmMap`        | `ShmMap`     | `INT32(INT32 flags, char* shmname, UINT32 size, void** shm)` |
| `Platform_ShmMapOnPg`    | `ShmMapOnPg` | `INT32(INT32 flags, char* pgname, char* shmname, UINT32 size, void** shm)` |
| `Platform_ShmUnmap`      | `ShmUnmap`   | `INT32(void* shm)` |

`ShmUnmap` receives only the address, so map events are correlated to their
unmap by the returned address (`*shm`); `shmname` is recorded at map time and
echoed back on the matched unmap.

## `flags` semantics (GetShmFlags)

`flags` is not opaque — it is the value from `GetShmFlags(pgId, lpId, attr)` and
encodes the CP/DP sharing scope and whether every process sees the **same
virtual address (VA)** for the region:

| flags | meaning | VA class |
|------:|---------|----------|
| 0 | CP kernel thread + DP inter-PG shared; DP inter-PG shared | per-proc VA **differs** |
| 1 | CP + DP inter-PG shared; DP inter-PG shared | per-proc VA **differs** |
| 2 | CP + DP inter-PG shared; DP intra-PG shared | per-proc VA **differs** |
| 3 | CP + DP process shared | process-shared |
| 4 | CP + DP inter-PG shared; DP inter-PG shared | per-proc VA **same** |
| 5 | CP + DP inter-PG shared; DP intra-PG shared | per-proc VA **same** |

**Step-2 constraint:** VA-same regions (4/5) may store raw pointers in shared
memory; VA-differs regions (0/1/2) must use offsets. The two must **not** share a
pool. In practice ~99% of allocations are VA-same, so v1 can be a single VA-same
pool and pass the rare VA-differs straight through to `ShmMap`.

## Output

JSONL (one JSON object per line). Path comes from `MEM_PROFILE_PATH`
(default `shm_profile.jsonl`). Every event carries `pid`, `live_count`,
`live_bytes` (live-set after the event) and `tid`.

Map / map_on_pg:

```json
{"ts_ns":..,"event":"map","shmname":"..","addr":"0x..","size":4096,"flags":4,"ret":0,"pid":..,"live_count":..,"live_bytes":..,"tid":".."}
{"ts_ns":..,"event":"map_on_pg",..,"pgname":"..",..}
```

Unmap (lifetime resolved from the matching map):

```json
{"ts_ns":..,"event":"unmap","addr":"0x..","ret":0,"matched":true,"shmname":"..","size":4096,"lifetime_ns":..,"pid":..,..}
```

At process exit the profiler emits one `live_at_exit` line per still-mapped
region (long-lived / leaked) and a final `summary`:

```json
{"ts_ns":..,"event":"live_at_exit","shmname":"..","addr":"0x..","size":..,"age_ns":..,"pid":..}
{"ts_ns":..,"event":"summary","pid":..,"peak_live_count":..,"peak_live_bytes":..,"total_maps":..,"total_map_failures":..,"total_unmaps":..,"unmatched_unmaps":..,"still_live_at_exit":..}
```

## Offline analysis

`tools/analyze_profile.py` (Python 3, standard library only) reads one or more
profile files and reports: size distribution, **waste vs the 2MB granularity**,
peak simultaneously-live (pool-size lower bound), allocation frequency, lifetime
distribution, size×lifetime cross-tab, per-pgname/NUMA breakdown, sharing
semantics by `flags` with a VA-class rollup, and never-freed blocks.

```sh
python tools/analyze_profile.py shm_profile.jsonl
python tools/analyze_profile.py "logs/*.jsonl"   # aggregate many processes
```

## Integration — build flags

Two knobs, set once per build config:

| Macro | When | Effect |
|-------|------|--------|
| `MEM_MANAGER_USE_API_H` | always (real integration) | `INT32`/`UINT32`, the OS `Shm*`, and the business `IS_CP` macro come from the internal `api.h` instead of the standalone shims + heap stub |
| `MEM_MANAGER_PROFILE` | debug builds | turns the profiler on (release = empty TU, pure pass-through) |

**There is no mem-manager-specific CP/DP flag.** mem-manager reads the
business's own `IS_CP` macro (defined on the control plane, absent on the data
plane) and adapts both plane-specific behaviours from it:
- `Platform_ShmMapOnPg` is compiled only when `IS_CP` is defined — `ShmMapOnPg`
  is a CP-only OS symbol, so a DP `.a` that referenced it would fail at link
  with "undefined reference to ShmMapOnPg".
- The profiler backend follows `IS_CP` too: file backend on CP, log backend on
  DP (which has no libc). See below.

So CP and DP builds use the *same* mem-manager flags; the plane split rides
entirely on `IS_CP`. (`IS_CP` must be visible at compile time — a compile `-D`
or defined in `api.h`. For a standalone build with no `api.h`, mem-manager
defaults to `IS_CP`; pass `-DIS_DP` to exercise the DP path locally.)

The parts to hand-port are `src/platform_shm.c` and `src/profiler/*`; the
`src/os` stub exists only for standalone build/test.

### Enforcing the wrapper (two-header layout)

To guarantee business code can't bypass the wrapper (which the profiler — and
the Step-2 pool — rely on seeing every allocation), there are two include
entry points:

| Header | Included by | Sees |
|--------|-------------|------|
| `platform/platform_shm.h` + `api.h` (raw) | the wrapper impl `platform_shm.c` | the real `ShmMap` — it must call it |
| `platform/platform_shm_api.h` (facade) | **business code** | OS constants + `Platform_Shm*`; raw `ShmMap`/`ShmMapOnPg`/`ShmUnmap` are **poisoned** |

The facade re-exports `api.h` (so business gets the OS constants from one
include) and then `#pragma GCC poison`s the raw calls. Any business file that
writes `ShmMap(...)` fails to compile (`error: attempt to use poisoned
'ShmMap'`) and must use `Platform_ShmMap` instead. Because `#pragma GCC poison`
is per-translation-unit, the wrapper implementation — which does **not** include
the facade — keeps calling the real OS functions normally. (GCC/Clang only;
other compilers skip the poison and lose just the compile-time enforcement.)

## Two profiler backends (CP vs DP)

The profiler has two backends, selected at compile time, because the DP link
environment has **no libc** (no malloc/stdio/snprintf/open/write under the names
we'd call — the OS implements its own primitives under internal names).

**CP backend (default).** Normal libc available. Writes JSONL straight to a
per-process file via `open`/`write`/`close`, resolves lifetimes inline, and
flushes a `summary` + `live_at_exit` lines at `atexit`. Schema unchanged.

**DP via the OS log (selected when `IS_CP` is absent).** Stateless: each event is
formatted as a single JSON line and handed to the OS
`LOG_FILE_INFO(module, fmt, ...)` macro (custom module, default `"SHMPROF"`,
override with `-DMEM_PROFILE_LOG_MODULE`). The payload omits time/pid — the
log's own line prefix already carries them (wall-clock `+us` on CP,
`[pg][vcpu][TSC]` on DP) and `analyze_profile.py` recovers them offline. No
buffer, no dump, no init call. A single atomic reentrancy flag guards against
the case where a log path internally calls `ShmMap` (it makes the nested call a
no-op, which also drops the log's own shm allocations from the profile).
Assumes single-thread-per-context (true for core-bound DP). Only external
symbols: `LOG_FILE_INFO` (OS) + `memcpy`.

Wiring: nothing — defining the macro is enough; events flow to the log. Filter
them offline by module:

```sh
python tools/analyze_profile.py --module SHMPROF b.log        # DP (TSC times)
python tools/analyze_profile.py --module SHMPROF --tsc-ghz 2.5 b.log  # TSC->ns
python tools/analyze_profile.py --module SHMPROF a.log        # CP (wall-clock)
```

The DP backend records **raw** map/unmap events; lifetimes, peak
simultaneously-live and never-freed are reconstructed offline by
`analyze_profile.py`, which correlates map↔unmap by address (per pid / `[pg]
[vcpu]` context) and unwraps log-prefixed lines, so it handles both schemas.
