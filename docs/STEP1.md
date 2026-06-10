# Step 1 — Profiling pass-through

`Platform_ShmMap` / `Platform_ShmMapOnPg` / `Platform_ShmUnmap` forward 1:1 to the
OS `ShmMap` / `ShmMapOnPg` / `ShmUnmap`. Under a **Debug** build
(`MEM_MANAGER_PROFILE` defined) every call is also recorded so we can study real
allocation patterns before designing the pool allocator (Step 2). Release builds
do not reference the profiler at all — zero overhead.

## Mapping to the OS API

| Wrapper                  | OS call      | Signature |
|--------------------------|--------------|-----------|
| `Platform_ShmMap`        | `ShmMap`     | `INT32(INT32 flags, char* shmname, uint32 size, void** shm)` |
| `Platform_ShmMapOnPg`    | `ShmMapOnPg` | `INT32(INT32 flags, char* pgname, char* shmname, uint32 size, void** shm)` |
| `Platform_ShmUnmap`      | `ShmUnmap`   | `INT32(void* shm)` |

`ShmUnmap` receives only the address, so map events are correlated to their
unmap by the returned address (`*shm`); `shmname` is recorded at map time and
echoed back on the matched unmap.

## Output

JSONL (one JSON object per line). Path comes from `MEM_PROFILE_PATH`
(default `shm_profile.jsonl`).

Map / map_on_pg event:

```json
{"ts_ns":..., "event":"map", "shmname":"...", "addr":"0x...", "size":4096, "flags":0, "ret":0, "tid":"..."}
{"ts_ns":..., "event":"map_on_pg", "shmname":"...", "addr":"0x...", "size":..., "flags":0, "ret":0, "tid":"...", "pgname":"..."}
```

Unmap event (lifetime resolved from the matching map):

```json
{"ts_ns":..., "event":"unmap", "addr":"0x...", "ret":0, "matched":true, "shmname":"...", "size":4096, "lifetime_ns":..., "tid":"..."}
```

`matched:false` means no live map was found for that address (e.g. mapped before
profiling started).

## Analysis hints

- **Size distribution / waste vs. 2MB granularity:** histogram `size` over `map`
  events; each distinct small mapping today costs a full 2MB region.
- **Frequency:** count events over time (`ts_ns`), optionally grouped by `shmname`.
- **Lifetime:** read `lifetime_ns` on `unmap` events — short-lived small blocks
  are the prime candidates for pooling in Step 2.

## Integration

When wiring into the business project, configure with
`-DMEM_MANAGER_USE_API_H=ON` so `INT32`/`uint32` and the OS `Shm*` functions are
taken from the internal `api.h` instead of the standalone shims and heap stub.
