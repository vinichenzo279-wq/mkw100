#!/usr/bin/env python3
"""List and compare the base-game hooks of Pulsar packs (Kamek Code.pul).

Each Kamek command with an absolute address patches the game (a branch to the module, a written
instruction, a pointer into the module); this tool lists them per pack with the containing base
function, and with two packs shows where both patch the same address or function.

    python -I scripts/mkw100/pul_hooks.py --map projects/mkwii/MAP.txt \
        --manifest build/base/mkwii_base_manifest.json \
        IF=<ItemFever>/Binaries/Code.pul RR=<RetroRewind6>/Binaries/Code.pul [--out DIR]

Prints module sizes, hook counts per area and the collision summary; with --out it also writes
<TAG>_hooks.txt (every hook) and shared.txt (the functions both packs patch).
Used for docs/mkw100/rr-item-fever.md.
"""
import argparse
import bisect
import collections
import json
import os
import struct

KINDS = {1: 'Addr32', 4: 'Lo16', 5: 'Hi16', 6: 'Ha16', 10: 'Rel24', 32: 'W32', 33: 'W16', 34: 'W8',
         35: 'CWPtr', 36: 'CW32', 37: 'CW16', 38: 'CW8', 64: 'B', 65: 'BL'}
NARGS = {k: (2 if 35 <= k <= 38 else 1) for k in KINDS}
REGIONS = 'PEJK'


def parse_pul(path, region='P'):
    """Return the region's Kamek chunk: sizes and the command list (combined Code.pul only)."""
    data = open(path, 'rb').read()
    if data[:4] == b'Kame':  # a raw chunk (combine_pul.py output)
        sizes, idx, off = (len(data), 0, 0, 0), 0, 0
    else:
        sizes = struct.unpack('>4I', data[:16])
        idx = REGIONS.index(region)
        off = 16 + sum(sizes[:idx])
    m0, _m1, bss, code, ctor_start, ctor_end, _csz, _ = struct.unpack('>8I', data[off:off + 32])
    if m0 != 0x4B616D65:
        raise ValueError(f'{path}: no Kamek chunk for region {region}')
    pos, end = off + 32 + code, off + sizes[idx]
    cmds = []
    while pos < end:
        word, = struct.unpack('>I', data[pos:pos + 4])
        pos += 4
        kind, addr, absolute = word >> 24, word & 0xFFFFFF, False
        if addr == 0xFFFFFE:
            addr, = struct.unpack('>I', data[pos:pos + 4])
            pos += 4
            absolute = True
        n = NARGS[kind]
        args = struct.unpack(f'>{n}I', data[pos:pos + 4 * n])
        pos += 4 * n
        cmds.append((KINDS[kind], absolute, addr, args))
    return {'code': code, 'bss': bss, 'ctors': (ctor_start, ctor_end),
            'blob': data[off + 32:off + 32 + code], 'cmds': cmds}


class Symbols:
    def __init__(self, map_path, manifest_path):
        pairs = []
        for line in open(map_path, encoding='utf-8', errors='replace'):
            parts = line.rstrip('\n').split(' ', 1)
            if len(parts) == 2:
                try:
                    pairs.append((int(parts[0], 16), parts[1]))
                except ValueError:
                    pass
        pairs.sort()
        self.addrs = [a for a, _ in pairs]
        self.names = [n for _, n in pairs]
        man = json.load(open(manifest_path))
        self.funcs = sorted((f['Start'], f['End']) for f in man['Functions'])
        self.starts = [f[0] for f in self.funcs]
        self.sections = [(s['GuestStart'], s['GuestEnd'], s['Name'], s['Executable']) for s in man['Sections']]

    def function(self, addr):
        i = bisect.bisect_right(self.starts, addr) - 1
        if i >= 0 and self.funcs[i][0] <= addr < self.funcs[i][1]:
            return self.funcs[i][0]
        return None

    def name(self, addr):
        i = bisect.bisect_right(self.addrs, addr) - 1
        if i < 0:
            return f'0x{addr:08x}'
        return self.names[i] if self.addrs[i] == addr else f'{self.names[i]}+0x{addr - self.addrs[i]:x}'

    def section(self, addr):
        for start, end, name, exe in self.sections:
            if start <= addr < end:
                return name, exe
        return None, None


def area(name, addr):
    cls = name.split('::')[0]
    gameplay = {'Item', 'Kart', 'ObjectsMgr', 'Objects', 'Object', 'Effects', 'WheelPhysics',
                'KCLController', 'KCLManager', 'GeoHitTableHolder', 'ObjFlowHolder', 'CourseMgr',
                'RaceBalloons', 'Lakitu', 'AI'}
    menus = {'Pages', 'Page', 'Section', 'SectionMgr', 'SectionParams', 'BMGHolder',
             'FriendsStatusButton', 'FriendStatusButtonRaceAfter', 'FriendButton', 'MenuKartModel',
             'MenuDriverModel', 'MenuModelBRRESHandle', 'BackModelMgr', 'GhostInfoControl',
             'ScnMgrMenu', 'StrapScene'}
    if cls in gameplay or name.startswith(('CtrlRaceItem', 'CtrlRace2DMap')):
        return 'items/kart/objects/effects'
    if cls in menus or name.startswith(('CtrlMenu', 'CtrlRace')):
        return 'menus/UI/HUD'
    if cls == 'RKNet' or name.startswith('DWC'):
        return 'online'
    if cls == 'RKSYS':
        return 'save'
    if cls in ('Audio', 'nw4r'):
        return 'sound'
    if name.startswith('0x'):
        return 'unnamed (StaticR)' if addr >= 0x805103B4 else 'unnamed (DOL)'
    return 'race/scene/core'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--map', required=True)
    ap.add_argument('--manifest', required=True)
    ap.add_argument('--region', default='P')
    ap.add_argument('--out')
    ap.add_argument('packs', nargs='+', help='TAG=path/to/Code.pul (one or two)')
    a = ap.parse_args()
    syms = Symbols(a.map, a.manifest)

    hooks = {}
    for spec in a.packs:
        tag, path = spec.split('=', 1)
        pul = parse_pul(path, a.region)
        rows = []
        for kind, absolute, addr, args in pul['cmds']:
            if absolute:
                func = syms.function(addr)
                name = syms.name(func if func is not None else addr)
                rows.append({'kind': kind, 'addr': addr, 'func': func, 'name': name, 'args': args})
        hooks[tag] = rows
        print(f'{tag}: code 0x{pul["code"]:x}, bss 0x{pul["bss"]:x} (module 0x{pul["code"] + pul["bss"]:x}), '
              f'{len(pul["cmds"])} commands, {len(rows)} on the game, '
              f'{(pul["ctors"][1] - pul["ctors"][0]) // 4} static constructors')
        if a.out:
            os.makedirs(a.out, exist_ok=True)
            with open(os.path.join(a.out, f'{tag}_hooks.txt'), 'w') as f:
                for r in sorted(rows, key=lambda r: r['addr']):
                    sec, _ = syms.section(r['addr'])
                    f.write(f"{r['addr']:08x} {r['kind']:6} {' '.join('%08x' % x for x in r['args']):18} "
                            f"{sec or '?':14} {r['name']}\n")

    if len(hooks) != 2:
        return
    (ta, ra), (tb, rb) = hooks.items()
    by_addr_b = {}
    funcs_b = collections.defaultdict(list)
    for r in rb:
        by_addr_b.setdefault(r['addr'], r)
        funcs_b[r['func']].append(r)

    table = collections.defaultdict(lambda: [0, 0, 0, 0])
    for r in ra:
        t = table[area(r['name'], r['addr'])]
        t[0] += 1
        other = by_addr_b.get(r['addr'])
        if other:
            t[1] += 1
            if r['kind'] in ('B', 'BL') and other['kind'] in ('B', 'BL'):
                t[2] += 1
        elif r['func'] is not None and r['func'] in funcs_b:
            t[3] += 1
    print(f'\n{ta} hooks by area, against {tb}:')
    print(f"  {'area':28} {'hooks':>6} {'same addr':>9} {'both branch':>11} {'same func':>9}")
    for k, v in sorted(table.items(), key=lambda kv: -kv[1][0]):
        print(f'  {k:28} {v[0]:6} {v[1]:9} {v[2]:11} {v[3]:9}')
    tot = [sum(v[i] for v in table.values()) for i in range(4)]
    print(f"  {'total':28} {tot[0]:6} {tot[1]:9} {tot[2]:11} {tot[3]:9}")

    if a.out:
        funcs_a = collections.defaultdict(list)
        for r in ra:
            funcs_a[r['func']].append(r)
        with open(os.path.join(a.out, 'shared.txt'), 'w') as f:
            for func in sorted(k for k in funcs_a if k is not None and k in funcs_b):
                f.write(f'== {syms.name(func)} (0x{func:08x})\n')
                for tag, rows in ((ta, funcs_a[func]), (tb, funcs_b[func])):
                    for r in rows:
                        f.write(f"  {tag} {r['addr']:08x} {r['kind']:6} {' '.join('%08x' % x for x in r['args'])}\n")


if __name__ == '__main__':
    main()
