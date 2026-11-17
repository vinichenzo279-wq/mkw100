#!/usr/bin/env python3
"""Tabulates run-perf.ps1 results: perf_table.py RESULTS_DIR [RESULTS_DIR...] [--prefix m128]
Per case: frames/s (1200 race frames, wall clock), the game thread's busy ms/frame (median of the
MKW_PERF_STATS windows after the race start), other processes' mean CPU (cores) and the trajectory."""
import glob, os, re, statistics, sys

def main():
    dirs = [a for a in sys.argv[1:] if not a.startswith('--')]
    prefix = ''
    if '--prefix' in sys.argv:
        prefix = sys.argv[sys.argv.index('--prefix') + 1]
    rows = []
    for d in dirs:
        for f in sorted(glob.glob(os.path.join(d, prefix + '*.stdout.txt'))):
            name = os.path.basename(f)[:-len('.stdout.txt')]
            text = open(f, errors='replace').read()
            err = f[:-len('.stdout.txt')] + '.stderr.txt'
            if os.path.exists(err): text += open(err, errors='replace').read()
            res = os.path.join(d, name + '.result.txt')
            other = ''
            if os.path.exists(res):
                m = re.search(r"other processes' CPU \(cores, 5 s samples\): ([\d. ]+)", open(res, errors='replace').read())
                if m and m.group(1).split():
                    other = '%.1f' % statistics.mean(float(x) for x in m.group(1).split())
            hz = re.search(r'simulation_hz=([\d.]+) trajectory=([0-9a-f]+)', text)
            busy = [float(x) for x in re.findall(r'busy ms/frame over 240 frames: avg [\d.]+ median ([\d.]+)', text)]
            race = busy[2:] if len(busy) > 2 else busy
            rows.append((name, hz.group(1) if hz else 'FAIL', '%.1f' % statistics.median(race) if race else '', other,
                         hz.group(2) if hz else ''))
    print('%-14s %8s %10s %7s  %s' % ('case', 'fps', 'busy ms', 'other', 'trajectory'))
    for r in rows:
        print('%-14s %8s %10s %7s  %s' % r)

if __name__ == '__main__':
    main()
