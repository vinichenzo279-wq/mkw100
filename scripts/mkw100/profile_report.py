#!/usr/bin/env python3
"""Summarise an MKW100_TEST_PROFILE stack profile (runtime/src/hle/test_profiler.cpp).

usage: profile_report.py PROFILE EXE [--map projects/mkwii/MAP.txt] [--nm llvm-nm] [--top 40]
       [--thread game|gx] [--buckets] [--callers REGEX] [--callees REGEX]

Exe frames resolve with `llvm-nm -n`; recompiled guest functions (func_8XXXXXXX, Name_8xxxxxxx) are
named from the game's MAP.txt when it has the address. Frames in other modules show as the module
name (webgpu_dawn.dll, d3d12core.dll, the GPU driver, ntdll.dll ...).
"""
import argparse, bisect, collections, os, re, subprocess, sys

GUEST = re.compile(r'(?:^func_|_)(8[0-9A-Fa-f]{7})$')


def load_nm(exe, nm):
    cache = exe + '.nm.txt'
    if not os.path.exists(cache) or os.path.getmtime(cache) < os.path.getmtime(exe):
        out = subprocess.run([nm, '-n', '--defined-only', exe], capture_output=True, text=True, check=True).stdout
        with open(cache, 'w') as f:
            f.write(out)
    addrs, names = [], []
    for line in open(cache):
        p = line.split()
        if len(p) == 3 and p[1] in 'Tt':
            addrs.append(int(p[0], 16)); names.append(p[2])
    return addrs, names


def load_map(path):
    m = {}
    if path and os.path.exists(path):
        for line in open(path, errors='replace'):
            p = line.split(None, 1)
            if len(p) == 2 and not p[1].startswith('0x'):
                m[int(p[0], 16)] = p[1].strip()
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('profile'); ap.add_argument('exe')
    ap.add_argument('--map', default=os.path.join(os.path.dirname(__file__), '../../projects/mkwii/MAP.txt'))
    ap.add_argument('--nm', default='llvm-nm')
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--thread', default=None)
    ap.add_argument('--callers'); ap.add_argument('--callees')
    ap.add_argument('--filter', help='only samples with a frame matching this regex')
    a = ap.parse_args()

    addrs, names = load_nm(a.exe, a.nm)
    gmap = load_map(a.map)
    gkeys = sorted(gmap)
    modules, threads, stacks = [], [], collections.defaultdict(list)
    exe_base = None
    for line in open(a.profile):
        p = line.split()
        if not p:
            continue
        if p[0] == 'module':
            base, size = int(p[1], 16), int(p[2], 16)
            name = os.path.basename(p[3].replace('\\', '/'))
            modules.append((base, base + size, name))
            if exe_base is None:  # the process image comes first (EnumProcessModules)
                exe_base = base
        elif p[0] == 'thread':
            threads.append((p[1], float(p[2]), float(p[3])))
        elif p[0] == 's':
            stacks[p[1]].append(tuple(int(x, 16) for x in p[2:]))
    modules.sort()
    mstarts = [m[0] for m in modules]
    cache = {}

    def resolve(pc):
        r = cache.get(pc)
        if r is not None:
            return r
        i = bisect.bisect_right(mstarts, pc) - 1
        if i < 0 or pc >= modules[i][1]:
            r = '?'
        elif modules[i][0] == exe_base:
            va = pc - exe_base + 0x140000000
            j = bisect.bisect_right(addrs, va) - 1
            r = names[j] if j >= 0 else '?exe'
            g = GUEST.search(r)
            if g:
                ga = int(g.group(1), 16)
                nm_ = gmap.get(ga)
                if nm_ is None:
                    k = bisect.bisect_right(gkeys, ga) - 1
                    nm_ = (gmap[gkeys[k]] + '+?') if k >= 0 and ga - gkeys[k] < 0x400 else None
                if nm_:
                    r = '%s [%s]' % (nm_, r)
        else:
            r = modules[i][2]
        cache[pc] = r
        return r

    print('== thread CPU time over the sampled span (ms) ==')
    for name, cpu, wall in sorted(threads, key=lambda t: -t[1])[:16]:
        print('  %-28s %9.1f  (%.0f%% of %.0f ms wall)' % (name, cpu, 100 * cpu / wall if wall else 0, wall))

    for tname, ss in stacks.items():
        if a.thread and tname != a.thread:
            continue
        n = len(ss)
        if n == 0:
            continue
        rs = [[resolve(pc) for pc in s] for s in ss]
        if a.filter:
            fx = re.compile(a.filter)
            rs = [r for r in rs if any(fx.search(x) for x in r)]
            print('(filter /%s/: %d of %d samples, %.1f%%; percentages below are of all samples)' % (a.filter, len(rs), n, 100.0 * len(rs) / n))
        print('\n== thread %s: %d samples ==' % (tname, n))
        leaf = collections.Counter(r[0] for r in rs if r)
        mod = collections.Counter((r[0] if r and ' ' not in r[0] and r[0].endswith('.dll') else 'exe') for r in rs if r)
        print('-- by module (leaf) --')
        for k, v in mod.most_common(12):
            print('  %5.1f%%  %s' % (100.0 * v / n, k))
        print('-- exclusive (leaf) --')
        for k, v in leaf.most_common(a.top):
            print('  %5.1f%%  %s' % (100.0 * v / n, k))
        incl = collections.Counter()
        for r in rs:
            for k in set(r):
                incl[k] += 1
        print('-- inclusive --')
        for k, v in incl.most_common(a.top * 2):
            print('  %5.1f%%  %s' % (100.0 * v / n, k))
        if a.callers or a.callees:
            rx = re.compile(a.callers or a.callees)
            c = collections.Counter()
            hit = 0
            for r in rs:
                for i, k in enumerate(r):
                    if rx.search(k):
                        hit += 1
                        if a.callers and i + 1 < len(r):
                            c[r[i + 1]] += 1
                        if a.callees:
                            c[r[i - 1] if i > 0 else '(self)'] += 1
                        break
            print('-- %s of /%s/ (%d samples, %.1f%%) --' % ('callers' if a.callers else 'callees', rx.pattern, hit, 100.0 * hit / n))
            for k, v in c.most_common(a.top):
                print('  %5.1f%%  %s' % (100.0 * v / n, k))


if __name__ == '__main__':
    main()
