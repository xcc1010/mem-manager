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

## Integration

When wiring into the business project, configure with
`-DMEM_MANAGER_USE_API_H=ON` so `INT32`/`UINT32` and the OS `Shm*` functions are
taken from the internal `api.h` instead of the standalone shims and heap stub.
The parts to hand-port are `src/platform_shm.c` and `src/profiler/*`; the
`src/os` stub exists only for standalone build/test.

Data-plane (DP) builds must additionally define `MEM_MANAGER_NO_SHMMAPONPG`:
`ShmMapOnPg` is a control-plane-only OS symbol, so compiling
`Platform_ShmMapOnPg` into a DP `.a` would fail at link with "undefined
reference to ShmMapOnPg". The macro drops that one wrapper; CP builds leave it
undefined.
