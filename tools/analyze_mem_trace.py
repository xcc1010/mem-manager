#!/usr/bin/env python3
"""Analyze a mem_trace.so live-set dump: attribute live memory to functions.

The dump is already the LIVE set (net of frees). Each line:
    <K> <addr> <size> <weight> <flags> <frame> <frame> ...
  K = 'A' (malloc/new family, byte-sampled -> use <weight> as estimated bytes)
      'M' (mmap family, exact -> weight == size)

Usage:
  # which functions hold the most memory (malloc side uses sampled weight):
  python3 analyze_mem_trace.py snap.dump --resolve

  # attribute the *real* RSS of the mmap side using a smaps snapshot:
  python3 analyze_mem_trace.py snap.dump --smaps smaps.txt --resolve

  # is it many-small or few-big? show the size distribution of live malloc:
  python3 analyze_mem_trace.py snap.dump --hist
"""
import argparse
import bisect
import collections
import os
import sys
import subprocess

MAP_ANONYMOUS = 0x20
MiB = 1 << 20


def parse_dump(path):
    a_recs, m_recs = [], []     # malloc, mmap
    with open(path) as f:
        for line in f:
            if not line or line[0] == '#':
                continue
            p = line.split()
            if len(p) < 5 or p[0] not in ('A', 'M'):
                continue
            # two layouts: with resident (newer) or without (older).
            #   K addr size weight resident flags <frames...>
            #   K addr size weight flags          <frames...>
            if p[4].startswith('0x'):
                resident, flags, fi = 0, int(p[4], 16), 5
            else:
                resident, flags, fi = int(p[4]), int(p[5], 16), 6
            # estimated resident scaled like weight (exact for big allocs where
            # weight == size; statistical for sampled small ones). Clamp resident
            # to the alloc's own bytes first: mincore is page-granular, so a
            # sub-page / page-sharing alloc reports a full page (> size); left
            # unclamped the weight/size scale-up explodes est_res for many-small
            # allocations (RESIDENT would exceed requested, which is impossible).
            size = int(p[2]); weight = int(p[3])
            est_res = min(resident, size) * (weight / size) if size else 0
            rec = dict(addr=int(p[1], 16), size=size, weight=weight,
                       resident=resident, est_res=est_res, flags=flags,
                       stack=tuple(p[fi:]))
            (a_recs if p[0] == 'A' else m_recs).append(rec)
    return a_recs, m_recs


def parse_smaps(path):
    vmas, start = [], None
    end = rss = 0

    def is_hdr(tok):
        if not tok or '-' not in tok[0] or ':' in tok[0]:
            return False
        s = tok[0].split('-')
        return len(s) == 2 and all(c in '0123456789abcdefABCDEF' for c in s[0] + s[1])

    with open(path) as f:
        for line in f:
            tok = line.split()
            if is_hdr(tok):
                if start is not None:
                    vmas.append((start, end, rss))
                s, e = tok[0].split('-')
                start, end, rss = int(s, 16), int(e, 16), 0
            elif line.startswith('Rss:'):
                rss = int(line.split()[1]) * 1024
    if start is not None:
        vmas.append((start, end, rss))
    return vmas


def smaps_categories(path):
    """Sum RSS by mapping category: anon / heap / file / stack / other."""
    cats = collections.defaultdict(int)
    cur = None

    def is_hdr(tok):
        if not tok or '-' not in tok[0] or ':' in tok[0]:
            return False
        s = tok[0].split('-')
        return len(s) == 2 and all(c in '0123456789abcdefABCDEF' for c in s[0] + s[1])

    with open(path) as f:
        for line in f:
            tok = line.split()
            if is_hdr(tok):
                p = tok[5] if len(tok) >= 6 else ''
                if p == '':
                    cur = 'anon'
                elif p == '[heap]':
                    cur = 'heap'
                elif p.startswith('[stack'):
                    cur = 'stack'
                elif p.startswith('/'):
                    cur = 'file'
                else:
                    cur = 'other'
            elif line.startswith('Rss:') and cur is not None:
                cats[cur] += int(line.split()[1]) * 1024
    return cats


def load_symmap(path):
    """Load a precomputed 'frame<TAB>function' map (from resolve_dump.sh)."""
    m = {}
    if not path:
        return m
    with open(path) as f:
        for line in f:
            line = line.rstrip('\n')
            if '\t' in line:
                fr, name = line.split('\t', 1)
                if name and name != '??':
                    m[fr] = name
    return m


def make_resolver(enabled, depth, symmap=None, bin_dirs=None, path_maps=None,
                  a2l='addr2line'):
    symmap = symmap or {}
    bin_dirs = bin_dirs or []
    path_maps = path_maps or []
    cache = {}
    located = {}          # dump module path -> local file path (or None)
    missing = set()

    def locate(mod):
        if mod in located:
            return located[mod]
        p = None
        if os.path.isfile(mod):
            p = mod
        else:
            for old, new in path_maps:
                if mod.startswith(old):
                    cand = new + mod[len(old):]
                    if os.path.isfile(cand):
                        p = cand
                        break
            if p is None:
                base = os.path.basename(mod)
                for d in bin_dirs:
                    cand = os.path.join(d, base)
                    if os.path.isfile(cand):
                        p = cand
                        break
        if p is None and mod not in missing:
            missing.add(mod)
            sys.stderr.write('WARN: cannot locate %s (use --bin-dir/--map)\n' % mod)
        located[mod] = p
        return p

    def sym(fr):
        if fr in cache:
            return cache[fr]
        out = fr
        if fr in symmap:                       # precomputed (e.g. run-machine)
            out = symmap[fr]
        elif enabled and '+' in fr:
            mod, off = fr.rsplit('+', 1)
            local = locate(mod)
            if local:
                try:
                    r = subprocess.run([a2l, '-f', '-C', '-e', local, off],
                                       capture_output=True, text=True, timeout=10)
                    name = r.stdout.split('\n')[0].strip()
                    if name and name != '??':
                        out = '%s  (%s)' % (name, os.path.basename(mod))
                except Exception as ex:
                    sys.stderr.write('WARN: addr2line failed (%s): %s\n' % (a2l, ex))
        cache[fr] = out
        return out

    def show(stack):
        return [sym(fr) for fr in stack[:depth]]
    return show


def rank_by_stack(recs, depth, weight_key='weight'):
    g = collections.defaultdict(lambda: [0, 0])
    for r in recs:
        k = r['stack'][:depth]
        g[k][0] += r[weight_key]
        g[k][1] += 1
    return sorted(g.items(), key=lambda kv: kv[1][0], reverse=True)


def growth_by_stack(base_recs, cur_recs, depth, key):
    """Per-stack (current - baseline) of `key`, sorted by biggest growth.

    Yields (stack, delta, base, cur, cur_count). A leak grows monotonically,
    so its stack floats to the top by delta while bounded stacks sit near 0."""
    gb = dict(rank_by_stack(base_recs, depth, key))
    gc = dict(rank_by_stack(cur_recs, depth, key))
    rows = []
    for k in set(gb) | set(gc):
        b = gb.get(k, [0, 0])[0]
        c, n = gc.get(k, [0, 0])
        rows.append((k, c - b, b, c, n))
    rows.sort(key=lambda x: x[1], reverse=True)
    return rows


def print_growth(title, rows, show, top, hours):
    print('\n## %s' % title)
    shown = 0
    for stack, delta, b, c, n in rows:
        if delta < 0.05 * MiB:          # drop noise / shrinking stacks
            break
        rate = '  = %+.1f MiB/h' % (delta / MiB / hours) if hours else ''
        print('\n  %+9.1f MiB%s   | %.1f -> %.1f MiB  x%d samples'
              % (delta / MiB, rate, b / MiB, c / MiB, n))
        for fr in show(stack):
            print('    ' + fr)
        shown += 1
        if shown >= top:
            break
    if shown == 0:
        print('  (no stack grew by >= 0.05 MiB)')


def attribute_rss(m_recs, vmas):
    recs = sorted(m_recs, key=lambda r: r['addr'])
    starts = [r['addr'] for r in recs]
    g = collections.defaultdict(float)
    unattributed = total = 0.0
    for vstart, vend, rss in vmas:
        total += rss
        vlen = vend - vstart
        if vlen <= 0 or rss == 0:
            continue
        i = bisect.bisect_right(starts, vend) - 1
        covered, hits = 0, []
        while i >= 0:
            r = recs[i]
            if r['addr'] + r['size'] <= vstart:
                if r['addr'] < vstart - (1 << 34):
                    break
                i -= 1
                continue
            ov = min(r['addr'] + r['size'], vend) - max(r['addr'], vstart)
            if ov > 0:
                hits.append((r, ov))
                covered += ov
            i -= 1
        for r, ov in hits:
            g[r['stack']] += rss * (ov / vlen)
        unattributed += rss * ((vlen - covered) / vlen)
    return g, unattributed, total


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dump')
    ap.add_argument('--smaps', help='/proc/<pid>/smaps snapshot -> real RSS for mmap side')
    ap.add_argument('--resolve', action='store_true', help='addr2line module+off -> function')
    ap.add_argument('--symmap', help='precomputed frame<TAB>function map (from resolve_dump.sh)')
    ap.add_argument('--bin-dir', action='append', default=[], dest='bin_dir',
                    help='dir to search for libraries by basename (repeatable)')
    ap.add_argument('--map', action='append', default=[], dest='path_map',
                    metavar='OLD=NEW', help='remap module path prefix (repeatable)')
    ap.add_argument('--addr2line', default='addr2line',
                    help='addr2line binary (e.g. aarch64-linux-gnu-addr2line)')
    ap.add_argument('--depth', type=int, default=6, help='stack frames to group/show')
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--hist', action='store_true', help='size histogram of live malloc')
    ap.add_argument('--baseline', help='earlier dump; report per-function GROWTH '
                    '(current - baseline) instead of totals -> pinpoints the leak')
    ap.add_argument('--hours', type=float, default=0.0,
                    help='hours between --baseline and dump; extrapolates MiB/h')
    ap.add_argument('--by', choices=('resident', 'requested'), default='resident',
                    help='growth metric for --baseline: resident (mincore RSS, '
                    'default) or requested (allocated/virtual bytes -- use when '
                    'VmSize grows but pages are not yet faulted in at snapshot)')
    a = ap.parse_args()

    path_maps = []
    for m in a.path_map:
        if '=' in m:
            old, new = m.split('=', 1)
            path_maps.append((old, new))

    a_recs, m_recs = parse_dump(a.dump)
    symmap = load_symmap(a.symmap)
    show = make_resolver(a.resolve or bool(symmap), a.depth, symmap,
                         a.bin_dir, path_maps, a.addr2line)

    a_live = sum(r['weight'] for r in a_recs)
    m_live = sum(r['weight'] for r in m_recs)
    a_res = sum(r['est_res'] for r in a_recs)
    m_res = sum(r['est_res'] for r in m_recs)
    have_res = any(r['resident'] for r in a_recs) or any(r['resident'] for r in m_recs)
    print('# malloc/new : requested %.1f MiB | RESIDENT %.1f MiB | %d samples'
          % (a_live / MiB, a_res / MiB, len(a_recs)))
    print('# mmap       : requested %.1f MiB | RESIDENT %.1f MiB | %d mappings'
          % (m_live / MiB, m_res / MiB, len(m_recs)))
    if not have_res:
        print('# (no resident data in dump -- rebuild mem_trace.so to get mincore RSS)')

    if a.baseline:
        b_a, b_m = parse_dump(a.baseline)
        b_have = any(r['resident'] for r in b_a) or any(r['resident'] for r in b_m)
        use_res = have_res and b_have and a.by == 'resident'
        gkey = 'est_res' if use_res else 'weight'
        glabel = 'RESIDENT' if use_res else 'requested'
        cur_tot = sum(r[gkey] for r in a_recs + m_recs)
        base_tot = sum(r[gkey] for r in b_a + b_m)
        d = cur_tot - base_tot
        hrs = a.hours or 0.0
        rate = '  = %+.1f MiB/h' % (d / MiB / hrs) if hrs else ''
        print('\n# GROWTH by %s  (dump - baseline)%s'
              % (glabel, '' if use_res else '  [no mincore data -> requested bytes]'))
        print('# total live grew %+.1f MiB%s   (baseline %.1f -> %.1f MiB)'
              % (d / MiB, rate, base_tot / MiB, cur_tot / MiB))
        print_growth('malloc/new growth by function (ranked by %s delta)' % glabel,
                     growth_by_stack(b_a, a_recs, a.depth, gkey), show, a.top, hrs)
        if b_m or m_recs:
            print_growth('mmap growth by function (ranked by %s delta)' % glabel,
                         growth_by_stack(b_m, m_recs, a.depth, gkey), show, a.top, hrs)
        return

    if a.hist:
        buckets = [(0, '<4K'), (4 << 10, '4-16K'), (16 << 10, '16-64K'),
                   (64 << 10, '64-256K'), (256 << 10, '256K-1M'),
                   (1 << 20, '1-4M'), (4 << 20, '>=4M')]
        bnd = [b[0] for b in buckets]
        agg = collections.defaultdict(float)
        for r in a_recs:
            i = bisect.bisect_right(bnd, r['size']) - 1
            agg[buckets[i][1]] += r['weight']
        print('\n## live malloc bytes by object size (answers many-small vs few-big)')
        for _, label in buckets:
            if agg[label]:
                print('  %-9s %8.1f MiB  (%.0f%%)' % (label, agg[label] / MiB,
                      100 * agg[label] / a_live if a_live else 0))

    if a.smaps:
        cats = smaps_categories(a.smaps)
        smaps_total = sum(cats.values())
        territory = cats['anon'] + cats['heap']    # malloc/allocator land
        our = a_res + m_res
        print('\n## RECONCILIATION: measured resident vs smaps RSS')
        print('  smaps total RSS          %9.1f MiB' % (smaps_total / MiB))
        print('    anon (mmap)            %9.1f MiB' % (cats['anon'] / MiB))
        print('    [heap] (brk)           %9.1f MiB' % (cats['heap'] / MiB))
        print('    => allocator territory %9.1f MiB  (anon+heap, should be ours)'
              % (territory / MiB))
        print('    file-backed (code/lib) %9.1f MiB  (not ours)' % (cats['file'] / MiB))
        print('    stack/other            %9.1f MiB' %
              ((cats['stack'] + cats['other']) / MiB))
        print('  our measured resident    %9.1f MiB  (malloc %.1f + mmap %.1f)'
              % (our / MiB, a_res / MiB, m_res / MiB))
        if not have_res:
            print('  (!) dump has no resident data -- rebuild mem_trace.so for mincore RSS')
        elif territory > 0:
            cov = 100.0 * our / territory
            gap = territory - our
            print('  coverage of territory    %5.0f%%   unexplained %9.1f MiB'
                  % (cov, gap / MiB))
            if gap > 0.2 * territory:
                print('  -> large unexplained allocator RSS: likely an allocator using')
                print('     direct syscalls (consider syscall() interception), allocations')
                print('     below the sampling interval, or allocator-retained freed pages.')
            else:
                print('  -> territory well explained; the per-function ranking covers the RSS.')

        g, unattr, total = attribute_rss(m_recs, parse_smaps(a.smaps))
        attributed = sum(g.values())
        print('\n## RSS breakdown by function (smaps RSS x mmap stacks)')
        print('# total RSS %.1f MiB | attributed %.1f MiB | UNATTRIBUTED %.1f MiB'
              ' (glibc arena/brk, libs, stacks)' % (total / MiB, attributed / MiB, unattr / MiB))
        for stack, b in sorted(g.items(), key=lambda kv: kv[1], reverse=True)[:a.top]:
            print('\n%9.1f MiB  (%.0f%%)' % (b / MiB, 100 * b / total if total else 0))
            for fr in show(stack):
                print('    ' + fr)

    # rank by RESIDENT when we have mincore data (that's the real footprint),
    # else fall back to requested bytes.
    key = 'est_res' if have_res else 'weight'
    label = 'RESIDENT' if have_res else 'requested'
    print('\n## malloc/new by function (ranked by %s)' % label)
    req_by = dict((s, bc) for s, bc in rank_by_stack(a_recs, a.depth, 'weight'))
    for stack, (b, c) in rank_by_stack(a_recs, a.depth, key)[:a.top]:
        req = req_by.get(stack, [0, 0])[0]
        print('\n  RESIDENT %8.1f MiB  | requested %8.1f MiB  x%d samples'
              % (b / MiB, req / MiB, c) if have_res
              else '\n  requested %8.1f MiB  x%d samples' % (b / MiB, c))
        for fr in show(stack):
            print('    ' + fr)

    if not a.smaps and m_recs:
        mlabel = 'RESIDENT' if have_res else 'requested'
        mkey = 'est_res' if have_res else 'weight'
        print('\n## mmap by function (ranked by %s)' % mlabel)
        for stack, (b, c) in rank_by_stack(m_recs, a.depth, mkey)[:a.top]:
            print('\n  %s %8.1f MiB  x%d' % (mlabel, b / MiB, c))
            for fr in show(stack):
                print('    ' + fr)


if __name__ == '__main__':
    main()
