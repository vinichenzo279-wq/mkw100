#!/usr/bin/env python3
"""Show what one function of a Pulsar pack's Kamek module does at a glance (no disassembler needed).

    python -I scripts/mkw100/pul_func.py <Code.pul> <module offset> [--map projects/mkwii/MAP.txt]
    python -I scripts/mkw100/pul_func.py <Code.pul> --string "text"   (functions that use a string)

For the function containing the offset: its range, the game functions and module functions it
calls, the module strings it loads (lis/addi pairs, also through a base register + offset), and
immediate compares. Used for docs/mkw100/rr-item-fever.md (which game calls Item Fever's code
relies on).
"""
import argparse
import bisect
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pul_hooks import parse_pul  # noqa: E402


def load_map(path):
    syms = []
    if not path or not os.path.exists(path):
        return syms
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.match(r'\s*([0-9A-Fa-f]{8})\s+[0-9A-Fa-f]+\s+([0-9A-Fa-f]{8})\s+\S+\s+(\S+)', line) or \
            re.match(r'\s*(?:0x)?([0-9A-Fa-f]{8})\s+(\S+)$', line)
        if m and len(m.groups()) == 3:
            syms.append((int(m.group(2), 16), m.group(3)))
        elif m:
            syms.append((int(m.group(1), 16), m.group(2)))
    syms.sort()
    return syms


def name_of(syms, addr):
    if not syms:
        return ''
    i = bisect.bisect_right([a for a, _ in syms], addr) - 1
    if i < 0:
        return ''
    a, n = syms[i]
    return n if a == addr else f'{n}+0x{addr - a:X}'


def word(blob, o):
    return struct.unpack('>I', blob[o:o + 4])[0]


def func_range(blob, off):
    o = off & ~3
    while o > 0 and (word(blob, o) >> 16) != 0x9421:  # stwu r1, -x(r1)
        o -= 4
    e = off & ~3
    while e < len(blob) - 4 and word(blob, e) != 0x4E800020:  # blr (first one after the offset)
        e += 4
    # keep going past early returns until the next prologue
    n = e + 4
    while n < len(blob) - 4 and (word(blob, n) >> 16) != 0x9421:
        n += 4
    return o, n


def cstring(blob, o):
    if not (0 <= o < len(blob)):
        return None
    end = blob.find(b'\0', o)
    s = blob[o:end]
    if 3 <= len(s) < 200 and all(32 <= c < 127 or c in (9, 10) for c in s):
        return s.decode().replace('\n', '\\n')
    return None


def describe(pul, start, end, syms):
    blob = pul['blob']
    relocs = {}
    for kind, absolute, addr, args in pul['cmds']:
        if not absolute and start <= addr < end:
            relocs[addr & ~3] = (kind, args[0])
    hi = {}
    out = []
    for o in range(start, end, 4):
        w = word(blob, o)
        op = w >> 26
        rd, ra, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        r = relocs.get(o)
        if op == 18:  # b / bl
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            link = w & 1
            if r and r[0] == 'Rel24':
                tgt = r[1]
                out.append(f'{o:06x} {"bl" if link else "b "} game 0x{tgt:08X} {name_of(syms, tgt)}')
            elif not (w & 2):
                out.append(f'{o:06x} {"bl" if link else "b "} module 0x{o + li:x}')
        elif op == 15:  # lis
            hi[rd] = (r[1] if r else (imm << 16), bool(r))
        elif op == 14 and ra in hi:  # addi from a lis
            base, isrel = hi[ra]
            val = r[1] if r else (base + simm) & 0xFFFFFFFF
            if r or isrel:
                s = cstring(blob, val)
                out.append(f'{o:06x} r{rd} = module 0x{val:x}' + (f'  "{s}"' if s else ''))
                hi.setdefault(('base', rd), val)
            else:
                out.append(f'{o:06x} r{rd} = 0x{val:08X} {name_of(syms, val)}')
        elif op == 14 and ('base', ra) in hi:  # addi from a string-pool base
            val = hi[('base', ra)] + simm
            s = cstring(blob, val)
            if s:
                out.append(f'{o:06x} r{rd} = module 0x{val:x}  "{s}"')
        elif op == 11 and imm < 0x400:  # cmpwi
            out.append(f'{o:06x} cmpwi r{ra}, 0x{imm:x}')
        elif op == 19 and (w & 0x7FE) == (528 << 1) and (w & 1):
            out.append(f'{o:06x} bctrl')
    return out


DFORM = {32: 'lwz', 33: 'lwzu', 34: 'lbz', 35: 'lbzu', 36: 'stw', 37: 'stwu', 38: 'stb', 39: 'stbu',
         40: 'lhz', 42: 'lha', 44: 'sth', 48: 'lfs', 50: 'lfd', 52: 'stfs', 54: 'stfd'}
XFORM = {23: 'lwzx', 87: 'lbzx', 151: 'stwx', 215: 'stbx', 279: 'lhzx', 343: 'lhax', 407: 'sthx',
         535: 'lfsx', 599: 'lfdx', 663: 'stfsx', 727: 'stfdx', 266: 'add', 40: 'subf', 444: 'or',
         235: 'mullw', 0: 'cmpw', 32: 'cmplw'}


def raw(pul, start, end, syms):
    """A plain listing of the common instructions (loads, stores, adds, compares, branches)."""
    blob = pul['blob']
    relocs = {}
    for kind, absolute, addr, args in pul['cmds']:
        if not absolute and start <= addr < end:
            relocs[addr & ~3] = (kind, args[0])
    out = []
    for o in range(start, end, 4):
        w = word(blob, o)
        op = w >> 26
        rd, ra, rb, imm = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, w & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        r = relocs.get(o)
        rel = f'  [{r[0]} 0x{r[1]:x}]' if r else ''
        if op in DFORM:
            text = f'{DFORM[op]} {"f" if op >= 48 else "r"}{rd}, {simm:#x}(r{ra})'
        elif op == 31 and ((w >> 1) & 0x3FF) in XFORM:
            name = XFORM[(w >> 1) & 0x3FF]
            if name in ('cmpw', 'cmplw'):
                text = f'{name} cr{rd >> 2}, r{ra}, r{rb}'
            elif name == 'or' and ra != 0:
                text = f'mr r{ra}, r{rd}' if rd == rb else f'or r{ra}, r{rd}, r{rb}'
            else:
                text = f'{name} {"f" if name.startswith(("lf", "stf")) else "r"}{rd}, r{ra}, r{rb}'
        elif op == 14:
            text = f'li r{rd}, {simm:#x}' if ra == 0 else f'addi r{rd}, r{ra}, {simm:#x}'
        elif op == 15:
            text = f'lis r{rd}, {imm:#x}' if ra == 0 else f'addis r{rd}, r{ra}, {imm:#x}'
        elif op == 7:
            text = f'mulli r{rd}, r{ra}, {simm:#x}'
        elif op == 24:
            text = f'ori r{ra}, r{rd}, {imm:#x}'
        elif op in (10, 11):
            text = f'{"cmplwi" if op == 10 else "cmpwi"} cr{rd >> 2}, r{ra}, {simm:#x}'
        elif op == 21:
            sh, mb, me = rb, (w >> 6) & 31, (w >> 1) & 31
            text = f'rlwinm r{ra}, r{rd}, {sh}, {mb}, {me}'
        elif op == 18:
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            tgt = r[1] if r and r[0] == 'Rel24' else o + li
            where = f'game 0x{tgt:08X} {name_of(syms, tgt)}' if r and r[0] == 'Rel24' else f'0x{tgt:x}'
            text = f'{"bl" if w & 1 else "b"} {where}'
            rel = ''
        elif op == 16:
            bd = w & 0xFFFC
            if bd & 0x8000:
                bd -= 0x10000
            text = f'bc {rd},{ra} 0x{o + bd:x}'
        elif op == 19 and (w & 0x7FE) == (528 << 1):
            text = 'bctrl' if w & 1 else 'bctr'
        elif w == 0x4E800020:
            text = 'blr'
        else:
            text = f'.word 0x{w:08X}'
        out.append(f'{o:06x}  {text}{rel}')
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pul')
    ap.add_argument('offset', nargs='?')
    ap.add_argument('--string')
    ap.add_argument('--map', default='projects/mkwii/MAP.txt')
    ap.add_argument('--raw', action='store_true', help='plain listing of the whole function')
    a = ap.parse_args()
    pul = parse_pul(a.pul)
    syms = load_map(a.map)
    blob = pul['blob']
    if a.string:
        i = blob.find(a.string.encode())
        s = i
        while s > 0 and 32 <= blob[s - 1] < 127:
            s -= 1
        print(f'string at 0x{s:x}: {cstring(blob, s)}')
        seen = set()
        for off in range(0, pul['code'] - 4, 4):
            pass
        for kind, absolute, addr, args in pul['cmds']:
            if not absolute and kind in ('Lo16', 'Ha16') and s - 0x400 <= args[0] <= s:
                f = func_range(blob, addr)
                if f in seen:
                    continue
                seen.add(f)
                lines = describe(pul, f[0], f[1], syms)
                if any(cstring(blob, s) and cstring(blob, s)[:30] in l for l in lines):
                    print(f'function 0x{f[0]:x}-0x{f[1]:x}')
        return
    off = int(a.offset, 16)
    start, end = func_range(blob, off)
    print(f'function 0x{start:x}-0x{end:x}')
    for line in (raw if a.raw else describe)(pul, start, end, syms):
        print(' ', line)


if __name__ == '__main__':
    main()
