#!/usr/bin/env python3
"""Offline analyser for mem-manager Step-1 profile logs (JSONL).

Reads one or more shm_profile.jsonl files (e.g. one per business process) and
prints a report aimed at the Step-2 pool-allocator design:

  * size distribution + percentiles
  * waste vs the OS 2MB granularity  (the core KPI)
  * allocation frequency / peak rate
  * lifetime distribution + size x lifetime cross-tab
  * peak simultaneously-live count/bytes (pool-size lower bound)
  * per-pgname (NUMA) breakdown for ShmMapOnPg
  * flags distribution
  * never-freed blocks (live_at_exit) and unmatched unmaps

Usage:
    python analyze_profile.py shm_profile.jsonl
    python analyze_profile.py path\\to\\*.jsonl other.jsonl

Pure standard library; runs on any Python 3.
"""

import calendar
import glob
import json
import re
import sys
from collections import Counter, defaultdict

MB2 = 2 * 1024 * 1024  # OS shared-memory minimum granularity (2 MiB).

SIZE_BUCKETS = [64, 256, 1024, 4096, 16384, 65536, 262144, 1048576, MB2]
LIFE_BUCKETS_NS = [1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10]

# Meaning of the `flags` value returned by GetShmFlags(pgId, lpId, attr).
# These encode the CP/DP sharing scope and whether each process sees the same
# virtual address (VA) for the region.
FLAGS_MEANING = {
    0: "CP kthread + DP inter-PG shared; per-proc VA differs",
    1: "CP + DP inter-PG shared; per-proc VA differs",
    2: "CP + DP; DP intra-PG shared; per-proc VA differs",
    3: "CP + DP process shared",
    4: "CP + DP inter-PG shared; per-proc VA SAME",
    5: "CP + DP; DP intra-PG shared; per-proc VA SAME",
}
VA_SAME_FLAGS = {4, 5}      # raw pointers in shm are valid across processes
VA_DIFFERS_FLAGS = {0, 1, 2}  # pool must hand out offsets, not pointers


def va_class(flags):
    if flags in VA_SAME_FLAGS:
        return "VA-same"
    if flags in VA_DIFFERS_FLAGS:
        return "VA-differs"
    if flags == 3:
        return "proc-shared"
    return "unknown"


# ---------------------------------------------------------------- helpers ----
def human_bytes(n):
    n = float(n)
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if abs(n) < 1024 or unit == "TB":
            return f"{n:.1f}{unit}" if unit != "B" else f"{int(n)}B"
        n /= 1024


def human_ns(ns):
    ns = float(ns)
    for unit, scale in (("ns", 1), ("us", 1e3), ("ms", 1e6), ("s", 1e9)):
        if ns < scale * 1000 or unit == "s":
            return f"{ns / scale:.1f}{unit}"


def percentiles(values, ps=(50, 90, 99, 100)):
    if not values:
        return {p: 0 for p in ps}
    s = sorted(values)
    out = {}
    for p in ps:
        if p >= 100:
            out[p] = s[-1]
        else:
            idx = min(len(s) - 1, int(round((p / 100.0) * (len(s) - 1))))
            out[p] = s[idx]
    return out


def bucket_label(value, edges, fmt):
    lo = 0
    for e in edges:
        if value <= e:
            return f"{fmt(lo)}-{fmt(e)}"
        lo = e
    return f">{fmt(edges[-1])}"


def round_up_2mb(size):
    return ((size + MB2 - 1) // MB2) * MB2


# ----------------------------------------------------------- log unwrap ------
# DP log prefix, e.g. [pg:1][vcpu:1][TSC:0117567917671138][INFO][SHMPROF][f 12]
_DP_PREFIX = re.compile(r"\[pg:(\d+)\]\[vcpu:(\d+)\]\[TSC:(\d+)\]")
# CP log prefix, e.g. [INFO][2026/06/12 14:13:03+413339us][SHMPROF][f 12]:
_CP_PREFIX = re.compile(
    r"\[(\d{4})/(\d{2})/(\d{2}) (\d{2}):(\d{2}):(\d{2})\+(\d+)us\]")


def unwrap_log(line, module_tag, tsc_ghz):
    """The VIA_LOG backend emits each event through the OS log macro, so a line
    looks like '<log prefix>[MODULE]<...> {json}'. Pull out the JSON payload and
    recover ts_ns + an identity ('pid') from the prefix the log itself stamped:
    DP carries [pg][vcpu][TSC]; CP carries a wall-clock [date+us]. Returns a
    record dict, or None if the line is not one of ours."""
    if module_tag and f"[{module_tag}]" not in line:
        return None
    i, j = line.find("{"), line.rfind("}")
    if i < 0 or j < i:
        return None
    try:
        rec = json.loads(line[i:j + 1])
    except json.JSONDecodeError:
        return None

    m = _DP_PREFIX.search(line)
    if m:
        pg, vcpu, tsc = m.group(1), m.group(2), int(m.group(3))
        rec.setdefault("pid", f"pg{pg}.vcpu{vcpu}")
        # TSC is a cycle counter; convert to ns if the frequency is known,
        # otherwise keep raw cycles (deltas are still monotonic / comparable).
        rec.setdefault("ts_ns", int(tsc / tsc_ghz) if tsc_ghz else tsc)
        rec.setdefault("_clock", "ns" if tsc_ghz else "tsc")
        return rec
    m = _CP_PREFIX.search(line)
    if m:
        y, mo, d, hh, mm, ss, us = (int(x) for x in m.groups())
        epoch_s = calendar.timegm((y, mo, d, hh, mm, ss, 0, 0, 0))
        rec.setdefault("ts_ns", (epoch_s * 1_000_000 + us) * 1000)
        rec.setdefault("pid", "cp")
        rec.setdefault("_clock", "ns")
        return rec
    return rec  # tagged but no recognised prefix: keep payload as-is


# ------------------------------------------------------------------ load -----
def load_records(patterns, module_tag="SHMPROF", tsc_ghz=0.0):
    paths = []
    for pat in patterns:
        hits = glob.glob(pat)
        paths.extend(hits if hits else [pat])
    records, bad = [], 0
    for path in paths:
        try:
            with open(path, "r", encoding="utf-8") as fh:
                for line in fh:
                    line = line.strip()
                    if not line:
                        continue
                    if line[0] == "{":          # pure JSONL (CP file / DP dump)
                        try:
                            records.append(json.loads(line))
                        except json.JSONDecodeError:
                            bad += 1
                        continue
                    rec = unwrap_log(line, module_tag, tsc_ghz)   # log-wrapped
                    if rec is not None:
                        records.append(rec)
                    # other (foreign) log lines are silently ignored
        except OSError as exc:
            print(f"  ! cannot read {path}: {exc}", file=sys.stderr)
    return records, paths, bad


# ------------------------------------------------------------- correlate -----
def correlate(records):
    """Reconstruct lifetimes / peak-live / never-freed from raw map+unmap
    events by replaying them per pid in timestamp order, correlating an unmap
    to its map by address. This makes the report work for BOTH the CP backend
    (which also writes these inline) and the DP self-contained backend (which
    records raw events only and leaves correlation to here)."""
    by_pid = defaultdict(list)
    for r in records:
        if r.get("event") in ("map", "map_on_pg", "unmap"):
            by_pid[r.get("pid")].append(r)

    lifetimes = []      # (size, lifetime_ns, flags)
    never_freed = []    # (name, size, age_ns, flags)
    peak_count = peak_bytes = 0
    matched = unmatched = 0

    for evs in by_pid.values():
        evs.sort(key=lambda r: r.get("ts_ns", 0))
        live = {}        # addr -> (size, t_map, name, flags)
        cur_bytes = 0
        last_ts = None
        for r in evs:
            last_ts = r.get("ts_ns", last_ts)
            ev = r.get("event")
            if ev in ("map", "map_on_pg"):
                if r.get("ret") != 0:
                    continue
                addr = r.get("addr")
                size = r.get("size", 0)
                if addr in live:          # address reused without an unmap
                    cur_bytes -= live[addr][0]
                live[addr] = (size, r.get("ts_ns"), r.get("shmname"), r.get("flags"))
                cur_bytes += size
                peak_count = max(peak_count, len(live))
                peak_bytes = max(peak_bytes, cur_bytes)
            elif ev == "unmap":
                addr = r.get("addr")
                if addr in live:
                    size, t_map, _name, flags = live.pop(addr)
                    cur_bytes -= size
                    matched += 1
                    if r.get("ts_ns") is not None and t_map is not None:
                        lifetimes.append((size, r["ts_ns"] - t_map, flags))
                else:
                    unmatched += 1
        for size, t_map, name, flags in live.values():
            age = (last_ts - t_map) if (last_ts is not None and t_map is not None) else 0
            never_freed.append((name, size, age, flags))

    return {
        "lifetimes": lifetimes,
        "never_freed": never_freed,
        "peak_count": peak_count,
        "peak_bytes": peak_bytes,
        "matched": matched,
        "unmatched": unmatched,
    }


# ---------------------------------------------------------------- report -----
def section(title):
    print(f"\n=== {title} ===")


def main(argv):
    # Minimal flag parsing: --module NAME (VIA_LOG tag, default SHMPROF),
    # --tsc-ghz G (convert DP TSC cycles to ns). Remaining args are file globs.
    module_tag, tsc_ghz, patterns = "SHMPROF", 0.0, []
    it = iter(argv[1:])
    for a in it:
        if a == "--module":
            module_tag = next(it, "SHMPROF")
        elif a == "--tsc-ghz":
            tsc_ghz = float(next(it, "0") or 0)
        elif a.startswith("--"):
            print(f"unknown flag: {a}", file=sys.stderr)
            return 1
        else:
            patterns.append(a)
    if not patterns:
        print(__doc__)
        return 1

    records, paths, bad = load_records(patterns, module_tag, tsc_ghz)
    if not records:
        print("No records parsed.", file=sys.stderr)
        return 1

    if any(r.get("_clock") == "tsc" for r in records):
        print("  NOTE: DP times are raw TSC cycles (pass --tsc-ghz G to convert "
              "to ns); time-labelled figures below are in cycles for those rows.")

    maps = [r for r in records if r.get("event") in ("map", "map_on_pg")]
    ok_maps = [r for r in maps if r.get("ret") == 0]
    unmaps = [r for r in records if r.get("event") == "unmap"]
    summaries = [r for r in records if r.get("event") == "summary"]
    leaks = [r for r in records if r.get("event") == "live_at_exit"]

    # Derive lifetimes / peak-live / never-freed by correlating map<->unmap.
    # Works for both the CP and DP (raw) log schemas.
    corr = correlate(records)

    sizes = [r["size"] for r in ok_maps if "size" in r]
    timestamps = [r["ts_ns"] for r in records if "ts_ns" in r]
    pids = sorted({r["pid"] for r in records if "pid" in r})

    print(f"Files: {', '.join(paths)}")
    if bad:
        print(f"  ({bad} unparseable line(s) skipped)")

    # -- overview -----------------------------------------------------------
    section("Overview")
    print(f"  processes (pids)    : {len(pids)} {pids}")
    print(f"  map calls           : {len(maps)}  (ok={len(ok_maps)}, "
          f"failed={len(maps) - len(ok_maps)})")
    print(f"    of which on_pg    : {sum(1 for r in maps if r['event'] == 'map_on_pg')}")
    print(f"  unmap calls         : {len(unmaps)}  "
          f"(matched={corr['matched']}, unmatched={corr['unmatched']})")
    print(f"  distinct shmnames   : {len({r.get('shmname') for r in ok_maps})}")
    if timestamps:
        span_ns = max(timestamps) - min(timestamps)
        print(f"  capture window      : {human_ns(span_ns)}")

    # -- size distribution + waste -----------------------------------------
    section("Size distribution (successful maps)")
    if sizes:
        pc = percentiles(sizes)
        print(f"  count={len(sizes)}  mean={human_bytes(sum(sizes) / len(sizes))}  "
              f"p50={human_bytes(pc[50])}  p90={human_bytes(pc[90])}  "
              f"p99={human_bytes(pc[99])}  max={human_bytes(pc[100])}")
        hist = Counter(bucket_label(s, SIZE_BUCKETS, human_bytes) for s in sizes)
        order = [bucket_label(e, SIZE_BUCKETS, human_bytes) for e in [0] + SIZE_BUCKETS]
        seen = set()
        for label in order + list(hist):
            if label in hist and label not in seen:
                seen.add(label)
                n = hist[label]
                bar = "#" * min(50, n * 50 // max(hist.values()))
                print(f"    {label:>16} : {n:>7}  {bar}")
        under = sum(1 for s in sizes if s < MB2)
        print(f"  allocations < 2MB   : {under}/{len(sizes)} "
              f"({100.0 * under / len(sizes):.1f}%)  <- each costs a full 2MB region today")

    # -- waste KPI ----------------------------------------------------------
    section("Waste vs 2MB granularity (the KPI)")
    if sizes:
        requested = sum(sizes)
        os_cost = sum(round_up_2mb(s) for s in sizes)
        print(f"  cumulative requested        : {human_bytes(requested)}")
        print(f"  cumulative OS cost (n*2MB)   : {human_bytes(os_cost)}")
        print(f"  cumulative waste            : {human_bytes(os_cost - requested)} "
              f"({100.0 * (os_cost - requested) / os_cost:.1f}% of OS cost)")

    # -- concurrency / pool-size lower bound -------------------------------
    section("Peak simultaneously-live (pool sizing)")
    print(f"  derived (map/unmap replay): peak_live_count={corr['peak_count']}  "
          f"peak_live_bytes={human_bytes(corr['peak_bytes'])}")
    if any("peak_live_count" in s for s in summaries):
        pc_count = max(s.get("peak_live_count", 0) for s in summaries)
        pc_bytes = max(s.get("peak_live_bytes", 0) for s in summaries)
        print(f"  from CP summary           : peak_live_count={pc_count}  "
              f"peak_live_bytes={human_bytes(pc_bytes)}")
    print("  -> a single pool only needs ~peak_live_bytes (rounded to pool granularity),")
    print("     vs today's (#distinct live mappings * 2MB).")

    # -- frequency ----------------------------------------------------------
    section("Allocation frequency")
    if timestamps and len(timestamps) > 1:
        span_s = (max(timestamps) - min(timestamps)) / 1e9
        if span_s > 0:
            print(f"  mean map rate       : {len(maps) / span_s:.1f} maps/s "
                  f"over {human_ns(max(timestamps) - min(timestamps))}")
        per_sec = Counter(int((r["ts_ns"] - min(timestamps)) // 1_000_000_000)
                          for r in maps if "ts_ns" in r)
        if per_sec:
            print(f"  peak map rate       : {max(per_sec.values())} maps/s (busiest 1s window)")

    # -- lifetime -----------------------------------------------------------
    section("Lifetime (matched unmaps)")
    lifetimes = [lt for (_sz, lt, _fl) in corr["lifetimes"]]
    if lifetimes:
        pc = percentiles(lifetimes)
        print(f"  count={len(lifetimes)}  p50={human_ns(pc[50])}  p90={human_ns(pc[90])}  "
              f"p99={human_ns(pc[99])}  max={human_ns(pc[100])}")
        hist = Counter(bucket_label(l, LIFE_BUCKETS_NS, human_ns) for l in lifetimes)
        for e in [0] + LIFE_BUCKETS_NS:
            label = bucket_label(e, LIFE_BUCKETS_NS, human_ns)
            if label in hist:
                n = hist.pop(label)
                print(f"    {label:>16} : {n}")

    # -- size x lifetime cross-tab -----------------------------------------
    section("Size x lifetime (pooling targets = small + short-lived)")
    if corr["lifetimes"]:
        cols = ["<1ms", "1ms-1s", ">=1s"]
        grid = defaultdict(lambda: [0, 0, 0])
        for size, life, _flags in corr["lifetimes"]:
            sl = bucket_label(size, SIZE_BUCKETS, human_bytes)
            c = 0 if life < 1e6 else (1 if life < 1e9 else 2)
            grid[sl][c] += 1
        print(f"    {'size \\ life':>16} | {cols[0]:>8} {cols[1]:>8} {cols[2]:>8}")
        for e in [0] + SIZE_BUCKETS:
            sl = bucket_label(e, SIZE_BUCKETS, human_bytes)
            if sl in grid:
                row = grid[sl]
                print(f"    {sl:>16} | {row[0]:>8} {row[1]:>8} {row[2]:>8}")

    # -- per-pgname (NUMA) --------------------------------------------------
    section("Per-pgname / NUMA (ShmMapOnPg)")
    onpg = [r for r in ok_maps if r.get("event") == "map_on_pg"]
    if onpg:
        by_pg = defaultdict(list)
        for r in onpg:
            by_pg[r.get("pgname", "?")].append(r.get("size", 0))
        for pg, szs in sorted(by_pg.items()):
            print(f"  {pg:>12}: {len(szs)} maps, total {human_bytes(sum(szs))}, "
                  f"mean {human_bytes(sum(szs) / len(szs))}")
        print("  -> Step 2 wants one pool per NUMA node these pgnames map to.")
    else:
        print("  (no ShmMapOnPg calls)")

    # -- flags / sharing semantics -----------------------------------------
    section("Sharing semantics by flags (GetShmFlags return)")
    by_flags_n = Counter()
    by_flags_bytes = defaultdict(int)
    for r in ok_maps:
        f = r.get("flags")
        by_flags_n[f] += 1
        by_flags_bytes[f] += r.get("size", 0)
    for f, n in sorted(by_flags_n.items(), key=lambda kv: -kv[1]):
        meaning = FLAGS_MEANING.get(f, "unknown flags value")
        print(f"  flags={f} [{va_class(f):>11}]: {n:>6} maps, "
              f"{human_bytes(by_flags_bytes[f]):>9}   ({meaning})")
    # VA-class rollup: the hard Step-2 constraint.
    roll = Counter(va_class(f) for f in by_flags_n.elements())
    print("  VA classes: " + ", ".join(f"{k}={v}" for k, v in sorted(roll.items())))
    print("  -> VA-differs regions cannot hold raw pointers in shm (pool hands out")
    print("     offsets); never mix VA-same and VA-differs in one pool.")

    # -- leaks / never-freed ------------------------------------------------
    section("Never-freed at exit (long-lived / leaked)")
    never = corr["never_freed"]
    print(f"  never-freed (derived)       : {len(never)} blocks, "
          f"total {human_bytes(sum(sz for _n, sz, _a, _f in never))}")
    if never:
        by_name = Counter((n or "?") for n, _sz, _a, _f in never)
        for name, n in by_name.most_common(10):
            print(f"    {name}: {n}")
        print("  -> these are the always-resident portion the pool must hold permanently.")
    if leaks:  # CP backend also emits explicit live_at_exit lines
        print(f"  (CP live_at_exit lines      : {len(leaks)})")
    if corr["unmatched"]:
        print(f"  unmatched unmaps            : {corr['unmatched']} "
              "(double-free / freed-before-profiling / pre-capture map)")

    print()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
