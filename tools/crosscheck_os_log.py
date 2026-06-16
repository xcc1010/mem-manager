#!/usr/bin/env python3
"""Cross-validate the mem-manager profiler against the OS's own shm-allocation log.

The OS log lists every shm region on the machine, one per line, e.g.:

    [123456.789012] 0x2118c0000 200000 0x200000 1 0 0 my/region
     <timestamp>    <addr>      <req>  <aligned> <..meta..> <shmname>

Fields (positional): addr = 1st token after the [..] timestamp; req = 2nd
(requested size); aligned = 3rd (size rounded up to the OS granularity, hex);
shmname = LAST token. `req` is parsed as decimal unless it has a 0x prefix
(pass --req-hex if your requested size is hex without the prefix).

We compare the OS log (filtered to the shmnames we care about) against our own
profiler records (CP JSONL and/or DP log lines, loaded the same way
analyze_profile.py does) to confirm we captured the same regions and sizes:

  * regions the OS logged but we missed   (under-capture / wrong filter)
  * regions we logged but the OS did not  (should not happen if OS log is full)
  * per-region size mismatches            (our recorded size vs OS req)
  * our 2MB-rounding vs the OS aligned size
  * total requested / aligned bytes

Usage:
    crosscheck_os_log.py --os OSLOG --name-filter REGEX [--module M]
                         [--tsc-ghz G] [--req-hex] PROFILE [PROFILE ...]

`--name-filter` is a regex matched against the shmname (the OS log holds every
machine shm, so filter to the ones our program allocates, e.g. '^myapp/').
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze_profile as ap   # reuse load_records / _map_ok


def _num(s, req_hex=False):
    s = s.strip()
    if s.lower().startswith("0x"):
        return int(s, 16)
    return int(s, 16) if req_hex else int(s)


_TS = re.compile(r"^\s*\[[^\]]*\]\s*")


def parse_os_log(path, req_hex):
    """name -> {'req', 'aligned', 'addr', 'count'} (max size kept per name)."""
    regions, bad = {}, 0
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = _TS.sub("", line.strip())
            if not line:
                continue
            toks = line.split()
            if len(toks) < 4:
                bad += 1
                continue
            try:
                addr = toks[0]
                req = _num(toks[1], req_hex)
                aligned = _num(toks[2])
                name = toks[-1]
            except ValueError:
                bad += 1
                continue
            e = regions.setdefault(name, {"req": 0, "aligned": 0,
                                          "addr": addr, "count": 0})
            e["count"] += 1
            e["req"] = max(e["req"], req)
            e["aligned"] = max(e["aligned"], aligned)
    return regions, bad


def load_profiler_regions(profiles, module_tag, tsc_ghz):
    """name -> {'size', 'count'} from our profiler records (successful maps)."""
    records, paths, _ = ap.load_records(profiles, module_tag, tsc_ghz)
    regions = {}
    for r in records:
        if r.get("event") not in ("map", "map_on_pg"):
            continue
        if not ap._map_ok(r):
            continue
        name = r.get("shmname")
        if not name:
            continue
        e = regions.setdefault(name, {"size": 0, "count": 0})
        e["count"] += 1
        e["size"] = max(e["size"], r.get("size", 0))
    return regions, paths


def main(argv):
    ap_ = argparse.ArgumentParser(description="Cross-check profiler vs OS shm log")
    ap_.add_argument("--os", required=True, help="OS shm-allocation log file")
    ap_.add_argument("--name-filter", default="", help="regex on shmname")
    ap_.add_argument("--module", default="SHMPROF", help="DP log module tag")
    ap_.add_argument("--tsc-ghz", type=float, default=0.0)
    ap_.add_argument("--req-hex", action="store_true",
                     help="OS requested-size column is hex without 0x")
    ap_.add_argument("--captured-only", action="store_true",
                     help="only validate regions our profiler captured (scope the "
                          "OS log to our shmnames); skip OS-only 'missing' detection")
    ap_.add_argument("profiles", nargs="+", help="our profiler logs / JSONL")
    a = ap_.parse_args(argv[1:])

    pat = re.compile(a.name_filter) if a.name_filter else None

    def keep(name):
        return pat.search(name) is not None if pat else True

    os_all, bad = parse_os_log(a.os, a.req_hex)
    os_r = {n: v for n, v in os_all.items() if keep(n)}
    prof_all, paths = load_profiler_regions(a.profiles, a.module, a.tsc_ghz)
    pr = {n: v for n, v in prof_all.items() if keep(n)}

    # Validate only what we captured: scope the OS log to our shmnames, so its
    # other (whole-machine) regions don't show up as 'missing'.
    if a.captured_only:
        os_r = {n: v for n, v in os_r.items() if n in pr}

    print(f"OS log         : {a.os}  ({len(os_all)} regions, {len(os_r)} after filter"
          + (f", {bad} unparsed lines" if bad else "") + ")")
    print(f"profiler logs  : {', '.join(paths)}  "
          f"({len(prof_all)} regions, {len(pr)} after filter)")
    if a.name_filter:
        print(f"name filter    : /{a.name_filter}/")

    os_names, pr_names = set(os_r), set(pr)
    missing = sorted(os_names - pr_names)   # OS has, we don't
    extra = sorted(pr_names - os_names)     # we have, OS doesn't
    common = sorted(os_names & pr_names)

    print(f"\nregions: {len(common)} matched, {len(missing)} missing (OS-only), "
          f"{len(extra)} extra (profiler-only)")

    if missing:
        print("\n  MISSING (OS logged, profiler did not capture):")
        for n in missing[:20]:
            print(f"    {n}  req={ap.human_bytes(os_r[n]['req'])}")
        if len(missing) > 20:
            print(f"    ... +{len(missing) - 20} more")

    if extra:
        print("\n  EXTRA (profiler captured, not in OS log):")
        for n in extra[:20]:
            print(f"    {n}  size={ap.human_bytes(pr[n]['size'])}")
        if len(extra) > 20:
            print(f"    ... +{len(extra) - 20} more")

    size_mismatch, align_mismatch = [], []
    for n in common:
        if pr[n]["size"] != os_r[n]["req"]:
            size_mismatch.append(n)
        if ap.round_up_2mb(pr[n]["size"]) != os_r[n]["aligned"]:
            align_mismatch.append(n)

    print(f"\nsizes: {len(common) - len(size_mismatch)}/{len(common)} match OS req; "
          f"{len(size_mismatch)} differ")
    for n in size_mismatch[:20]:
        print(f"    {n}: profiler={ap.human_bytes(pr[n]['size'])} "
              f"vs OS req={ap.human_bytes(os_r[n]['req'])}")

    print(f"2MB-rounding: {len(common) - len(align_mismatch)}/{len(common)} match OS "
          f"aligned; {len(align_mismatch)} differ")
    for n in align_mismatch[:20]:
        print(f"    {n}: ours={ap.human_bytes(ap.round_up_2mb(pr[n]['size']))} "
              f"vs OS aligned={ap.human_bytes(os_r[n]['aligned'])}")

    os_req = sum(v["req"] for v in os_r.values())
    os_al = sum(v["aligned"] for v in os_r.values())
    pr_sz = sum(v["size"] for v in pr.values())
    pr_al = sum(ap.round_up_2mb(v["size"]) for v in pr.values())
    print("\ntotals (distinct, filtered):")
    print(f"  requested : profiler {ap.human_bytes(pr_sz):>10}   OS {ap.human_bytes(os_req):>10}")
    print(f"  aligned   : profiler {ap.human_bytes(pr_al):>10}   OS {ap.human_bytes(os_al):>10}")

    ok = not missing and not extra and not size_mismatch and not align_mismatch
    print("\nRESULT:", "MATCH - profiler agrees with the OS log." if ok
          else "DIFFERENCES found (see above).")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
