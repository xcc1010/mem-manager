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
            rec = dict(addr=int(p[1], 16), size=int(p[2]), weight=int(p[3]),
                       flags=int(p[4], 16), stack=tuple(p[5:]))
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


def make_resolver(enabled, depth):
    cache = {}

    def sym(fr):
        if not enabled:
            return fr
        if fr in cache:
            return cache[fr]
        out = fr
        try:
            mod, off = fr.rsplit('+', 1)
            r = subprocess.run(['addr2line', '-f', '-C', '-e', mod, off],
                               capture_output=True, text=True, timeout=10)
            name = r.stdout.split('\n')[0].strip()
            if name and name != '??':
                out = '%s  (%s)' % (name, mod.split('/')[-1])
        except Exception:
            pass
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
    ap.add_argument('--depth', type=int, default=6, help='stack frames to group/show')
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--hist', action='store_true', help='size histogram of live malloc')
    a = ap.parse_args()

    a_recs, m_recs = parse_dump(a.dump)
    show = make_resolver(a.resolve, a.depth)

    a_live = sum(r['weight'] for r in a_recs)
    m_live = sum(r['weight'] for r in m_recs)
    print('# malloc/new live (sampled est): %.1f MiB across %d samples' % (a_live / MiB, len(a_recs)))
    print('# mmap live (exact):            %.1f MiB across %d mappings' % (m_live / MiB, len(m_recs)))

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
        g, unattr, total = attribute_rss(m_recs, parse_smaps(a.smaps))
        attributed = sum(g.values())
        print('\n## RSS breakdown by function (smaps RSS x mmap stacks)')
        print('# total RSS %.1f MiB | attributed %.1f MiB | UNATTRIBUTED %.1f MiB'
              ' (glibc arena/brk, libs, stacks)' % (total / MiB, attributed / MiB, unattr / MiB))
        for stack, b in sorted(g.items(), key=lambda kv: kv[1], reverse=True)[:a.top]:
            print('\n%9.1f MiB  (%.0f%%)' % (b / MiB, 100 * b / total if total else 0))
            for fr in show(stack):
                print('    ' + fr)

    print('\n## malloc/new live by function (sampled estimate)')
    for stack, (b, c) in rank_by_stack(a_recs, a.depth)[:a.top]:
        print('\n%9.1f MiB  x%d samples' % (b / MiB, c))
        for fr in show(stack):
            print('    ' + fr)

    if not a.smaps and m_recs:
        print('\n## mmap live by function (exact; pass --smaps to convert to RSS)')
        for stack, (b, c) in rank_by_stack(m_recs, a.depth)[:a.top]:
            print('\n%9.1f MiB  x%d' % (b / MiB, c))
            for fr in show(stack):
                print('    ' + fr)


if __name__ == '__main__':
    main()
