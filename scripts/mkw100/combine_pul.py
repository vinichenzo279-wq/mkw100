#!/usr/bin/env python3
"""Combine Retro Rewind's and Item Fever's Kamek modules into one (docs/mkw100/rr-item-fever.md).

    python -I scripts/mkw100/combine_pul.py --rr <RR Code.pul> --if <IF Code.pul> \
        --plan projects/mkwii/mkw100/rr-item-fever.txt --out <combined Code.pul>

The result is one raw Kamek v3 chunk (PAL) that translate-mod handles like any pack:

    0x000000  Retro Rewind's code, then room for its bss (zeros)
    ctor      one constructor table listing both packs' constructors
    tramps    PowerPC trampolines for the `chain` sites of the plan
    +0xAFF00  marker: "MKW100 RR+IF v1", then both packs' Kamek code SHA-1 (runtime mod_build.cpp)
    IF_OFF    Item Fever's code; the chunk's bss is Item Fever's bss

Every Retro Rewind command is kept. Item Fever's module-relative commands are moved by IF_OFF, so
its code works unchanged. Of Item Fever's patches to the game, only those the plan names are kept
(`keep`, `keep-func`); a kept patch replaces Retro Rewind's at the same address. A `chain` site
gets a trampoline that calls Retro Rewind's target, then Item Fever's, passing r3-r5 to both.

Plan file, one entry per line (# comments):
    if-offset 0xC0000
    chain 0xADDRESS            # both packs branch here: call RR's target, then IF's
    keep 0xADDRESS             # keep IF's patch at this address (code or data)
    keep-range 0xSTART 0xEND   # keep IF's patches in [START, END)
    keep-func 0xADDRESS        # keep all IF patches inside the base function starting here
    own-func 0xADDRESS         # the same, and drop all of RR's patches inside that function
    rename-string OLD NEW      # replace a NUL-terminated string in IF's module (same length)
    stub-rr 0xADDRESS          # make the RR function at this guest address return at once (blr)
    drop-rr 0xADDRESS          # leave out RR's patch at this game address (the game's code runs)
    write32 0xADDRESS 0xVALUE  # an extra patch of MKW100's own: write VALUE at a game address
    nest-bl 0xIF_OFF 0xRR_OFF  # IF's `bl <game function>` at IF module offset IF_OFF calls RR's wrapper
                               # of that function (RR module offset RR_OFF) instead: both wrappers run
    nest-ctr 0xIF_LIS 0xIF_BCTRL 0xRR_NE 0xRR_EQ
                               # the same for an IF call through `lis; addi r12; mtctr r12 ... bctrl`
                               # to a game address: it calls RR's wrapper RR_NE when r4 != 0, RR_EQ
                               # when r4 == 0 (one IF wrapper hooked at two game sites that pass r4)
"""
import argparse
import hashlib
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pul_hooks import parse_pul, KINDS  # noqa: E402

IDS = {v: k for k, v in KINDS.items()}
ADDRESS_ARG = {'Addr32', 'Lo16', 'Hi16', 'Ha16', 'Rel24', 'B', 'BL', 'CWPtr'}  # arg0 is an address
LINK_BASE = 0x803992E0  # where the Pulsar loader puts Code.pul on a PAL Wii (recomp.yml module_link_base)


def read_plan(path):
    plan = {'if-offset': 0xC0000, 'chain': [], 'keep': set(), 'keep-func': set(), 'own-func': set(),
            'rename-string': [], 'stub-rr': [], 'keep-range': [], 'drop-rr': set(), 'write32': [],
            'nest-bl': [], 'nest-ctr': []}
    for raw in open(path, encoding='utf-8'):
        line = raw.split('#', 1)[0].split()
        if not line:
            continue
        if line[0] == 'rename-string':
            if len(line) != 3 or len(line[1]) != len(line[2]):
                raise SystemExit(f'{path}: rename-string needs two strings of the same length')
            plan['rename-string'].append((line[1].encode(), line[2].encode()))
            continue
        if line[0] == 'write32':
            plan['write32'].append((int(line[1], 16), int(line[2], 16)))
            continue
        if line[0] in ('nest-bl', 'nest-ctr'):
            plan[line[0]].append(tuple(int(x, 16) for x in line[1:]))
            if len(plan[line[0]][-1]) != (2 if line[0] == 'nest-bl' else 4):
                raise SystemExit(f'{path}: {line[0]} has the wrong number of values')
            continue
        if line[0] == 'keep-range':
            plan['keep-range'].append((int(line[1], 16), int(line[2], 16)))
            continue
        key, value = line[0], int(line[1], 16)
        if key == 'if-offset':
            plan[key] = value
        elif key in ('chain', 'stub-rr'):
            plan[key].append(value)
        elif key in ('keep', 'keep-func', 'own-func', 'drop-rr'):
            plan[key].add(value)
        else:
            raise SystemExit(f'{path}: unknown entry {key}')
    return plan


def function_starts(manifest):
    import json
    import bisect
    funcs = sorted((f['Start'], f['End']) for f in json.load(open(manifest))['Functions'])
    starts = [f[0] for f in funcs]

    def of(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return funcs[i][0] if i >= 0 and funcs[i][0] <= addr < funcs[i][1] else None
    return of


def shift(kind, args, delta):
    if kind in ADDRESS_ARG and args[0] < 0x80000000:
        return (args[0] + delta,) + tuple(args[1:])
    return tuple(args)


def ppc_b(src, dst, link):
    delta = dst - src
    assert -0x2000000 <= delta < 0x2000000 and delta % 4 == 0
    return 0x48000000 | (delta & 0x03FFFFFC) | (1 if link else 0)


def trampoline(at, rr_target, if_target):
    """Call rr_target then if_target with the caller's r3-r5, return to the caller's LR."""
    words = [
        0x9421FFE0,  # stwu r1, -0x20(r1)
        0x7C0802A6,  # mflr r0
        0x90010024,  # stw  r0, 0x24(r1)
        0x90610008,  # stw  r3, 0x8(r1)
        0x9081000C,  # stw  r4, 0xc(r1)
        0x90A10010,  # stw  r5, 0x10(r1)
        None,        # bl   rr_target
        0x80610008,  # lwz  r3, 0x8(r1)
        0x8081000C,  # lwz  r4, 0xc(r1)
        0x80A10010,  # lwz  r5, 0x10(r1)
        None,        # bl   if_target
        0x80010024,  # lwz  r0, 0x24(r1)
        0x7C0803A6,  # mtlr r0
        0x38210020,  # addi r1, r1, 0x20
        0x4E800020,  # blr
    ]
    words[6] = ppc_b(at + 24, rr_target, True)
    words[10] = ppc_b(at + 40, if_target, True)
    return b''.join(struct.pack('>I', w) for w in words)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--rr', required=True)
    ap.add_argument('--if', dest='if_', required=True)
    ap.add_argument('--plan', required=True)
    ap.add_argument('--manifest', help='base manifest, needed for keep-func')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    plan = read_plan(a.plan)
    rr, iff = parse_pul(a.rr), parse_pul(a.if_)
    # The plan and MKW100's site lists hold addresses inside one build of each pack.
    digests = {'retro-rewind': hashlib.sha1(rr['blob']).hexdigest(), 'item-fever': hashlib.sha1(iff['blob']).hexdigest()}
    builds = os.path.join(os.path.dirname(os.path.abspath(a.plan)), 'mod-builds.txt')
    known = {}
    for raw in open(builds, encoding='utf-8'):
        fields = raw.split('#', 1)[0].split()
        if len(fields) >= 2:
            known.setdefault(fields[0], set()).add(fields[1].lower())
    for profile, digest in digests.items():
        if digest not in known.get(profile, set()):
            raise SystemExit(f'{profile} Code.pul (Kamek code SHA-1 {digest}) is not a build listed in {builds}; '
                             'the plan holds addresses of the listed one (docs/mkw100/UPDATING.md)')
    off = plan['if-offset']
    rr_end = rr['code'] + rr['bss']
    if off < rr_end + 0x1000 or off % 0x10000:
        raise SystemExit(f'if-offset 0x{off:x} must be 64 KiB aligned and leave room after RR (ends 0x{rr_end:x})')

    def ctor_targets(mod, delta):
        cs, ce = mod['ctors']
        by_addr = {a: args for k, ab, a, args in mod['cmds'] if not ab and k == 'Addr32'}
        out = []
        for slot in range(cs, ce, 4):
            if slot not in by_addr:
                raise SystemExit(f'constructor slot 0x{slot:x} has no pointer relocation')
            out.append(shift('Addr32', by_addr[slot], delta)[0])
        return out

    ctors = ctor_targets(rr, 0) + ctor_targets(iff, off)
    ctor_at = (rr_end + 0xFF) & ~0xFF
    tramp_at = (ctor_at + 4 * len(ctors) + 0xFF) & ~0xFF

    marker_at = 0xAFF00
    stub_at = tramp_at + 0x40 * len(plan['chain'])
    if stub_at + 0x10 * len(plan['nest-ctr']) > marker_at or marker_at + 56 > off:
        raise SystemExit('no room for the trampolines and the marker below if-offset')
    image = bytearray(off + iff['code'])
    image[marker_at:marker_at + 56] = (b'MKW100 RR+IF v1' + bytes(1) + bytes.fromhex(digests['retro-rewind']) +
                                       bytes.fromhex(digests['item-fever']))
    image[:rr['code']] = rr['blob']
    image[off:off + iff['code']] = iff['blob']

    # Item Fever's un-relocated references to its own linked image (lis rX, hi(LINK_BASE + o)):
    # inside the combined image they must name LINK_BASE + IF_OFF + o, which the translator's
    # link-base remap then turns into the guest address. IF_OFF keeps the low halves unchanged.
    relocated = {a & ~3 for k, ab, a, args in iff['cmds'] if not ab}
    link_end = LINK_BASE + iff['code']
    moved = 0
    for o in range(0, iff['code'], 4):
        w, = struct.unpack('>I', iff['blob'][o:o + 4])
        if w >> 26 == 15 and (w >> 16) & 31 == 0 and o not in relocated:  # lis rD, imm
            hi = (w & 0xFFFF) << 16
            if LINK_BASE - 0x10000 <= hi < link_end:
                if off & 0xFFFF:
                    raise SystemExit('if-offset must keep low halves (multiple of 0x10000)')
                struct.pack_into('>I', image, off + o, (w & 0xFFFF0000) | (((hi + off) >> 16) & 0xFFFF))
                moved += 1

    # Nested wrappers: IF's wrapper of a game function calls RR's wrapper of it, which calls the game.
    rr_code = rr['code']
    nested = set()
    for if_off, rr_off in plan['nest-bl']:
        hits = [c for c in iff['cmds'] if not c[1] and c[0] == 'Rel24' and c[2] & ~3 == if_off]
        if len(hits) != 1 or struct.unpack('>I', iff['blob'][if_off:if_off + 4])[0] >> 26 != 18:
            raise SystemExit(f'nest-bl 0x{if_off:x}: not a relocated bl in IF')
        if not 0 <= rr_off < rr_code:
            raise SystemExit(f'nest-bl 0x{rr_off:x}: not RR code')
        struct.pack_into('>I', image, off + if_off, ppc_b(off + if_off, rr_off, True))
        nested.add(if_off)
    for n, (lis_at, bctrl_at, rr_ne, rr_eq) in enumerate(plan['nest-ctr']):
        w = [struct.unpack('>I', iff['blob'][o:o + 4])[0] for o in (lis_at, lis_at + 4, lis_at + 8, bctrl_at)]
        if (w[0] >> 26 != 15 or w[1] >> 26 != 14 or (w[1] >> 21) & 31 != 12 or w[2] != 0x7D8903A6
                or w[3] != 0x4E800421 or not 0 <= rr_ne < rr_code or not 0 <= rr_eq < rr_code):
            raise SystemExit(f'nest-ctr 0x{lis_at:x}: expected lis; addi r12; mtctr r12 ... bctrl')
        at = stub_at + 0x10 * n
        stub = [0x2C040000, 0x41820008, ppc_b(at + 8, rr_ne, False), ppc_b(at + 12, rr_eq, False)]  # cmpwi r4,0; beq +8
        image[at:at + 16] = b''.join(struct.pack('>I', x) for x in stub)
        for o in (lis_at, lis_at + 4, lis_at + 8):
            struct.pack_into('>I', image, off + o, 0x60000000)  # nop (r3 is set again before the call)
        struct.pack_into('>I', image, off + bctrl_at, ppc_b(off + bctrl_at, at, True))

    for old, new in plan['rename-string']:
        hits = [i for i in range(off, off + iff['code']) if image[i:i + len(old) + 1] == old + bytes(1)
                and (i == off or image[i - 1] == 0)]
        if len(hits) != 1:
            raise SystemExit(f'rename-string {old.decode()}: found {len(hits)} times in IF, need exactly 1')
        image[hits[0]:hits[0] + len(new)] = new

    rr_relocated = {a & ~3 for k, ab, a, args in rr['cmds'] if not ab}
    for addr in plan['stub-rr']:
        o = addr - 0x81800000
        if not 0 <= o < rr['code'] or o in rr_relocated:
            raise SystemExit(f'stub-rr 0x{addr:08x}: not an unrelocated word of RR code')
        struct.pack_into('>I', image, o, 0x4E800020)  # blr

    cmds = []  # (kind, absolute, addr, args)
    for k, ab, addr, args in rr['cmds']:
        cmds.append((k, ab, addr, tuple(args)))
    for i, target in enumerate(ctors):
        cmds.append(('Addr32', False, ctor_at + 4 * i, (target,)))

    if (plan['keep-func'] or plan['own-func']) and not a.manifest:
        raise SystemExit('keep-func/own-func need --manifest')
    func_of = function_starts(a.manifest) if a.manifest else (lambda _a: None)
    rr_abs = {}
    for idx, (k, ab, addr, args) in enumerate(cmds):
        if ab:
            rr_abs.setdefault(addr, []).append(idx)
    if_abs = {}
    for k, ab, addr, args in iff['cmds']:
        if ab:
            if_abs.setdefault(addr, []).append((k, shift(k, args, off)))

    drop = set()
    for n, site in enumerate(plan['chain']):
        rr_cmd = [cmds[i] for i in rr_abs.get(site, []) if cmds[i][0] in ('B', 'BL')]
        if_cmd = [c for c in if_abs.get(site, []) if c[0] in ('B', 'BL')]
        if len(rr_cmd) != 1 or len(if_cmd) != 1 or rr_cmd[0][0] != if_cmd[0][0]:
            raise SystemExit(f'chain 0x{site:08x}: needs one branch of the same kind from each pack')
        kind = rr_cmd[0][0]
        at = tramp_at + 0x40 * n
        rr_target = rr_cmd[0][3][0]
        if_target = if_cmd[0][1][0]
        if rr_target >= 0x80000000 or if_target >= 0x80000000:
            raise SystemExit(f'chain 0x{site:08x}: targets must be module code')
        image[at:at + 60] = trampoline(at, rr_target, if_target)
        drop.update(rr_abs[site])
        cmds.append((kind, True, site, (at,)))
    kept = 0
    for site, idxs in rr_abs.items():
        if (func_of(site) in plan['own-func'] or site in plan['drop-rr']) and site not in plan['chain']:
            drop.update(idxs)
    missing = sorted(a for a in plan['drop-rr'] if a not in rr_abs)
    if missing:
        raise SystemExit('drop-rr: RR has no patch at ' + ', '.join(f'0x{a:08x}' for a in missing))
    for site, entries in sorted(if_abs.items()):
        if site in plan['chain']:
            continue
        if (site in plan['keep'] or func_of(site) in plan['keep-func'] | plan['own-func'] or
                any(lo <= site < hi for lo, hi in plan['keep-range'])):
            drop.update(rr_abs.get(site, []))
            for k, args in entries:
                cmds.append((k, True, site, args))
                kept += 1
    for addr, value in plan['write32']:
        if addr in rr_abs or any(c[2] == addr and c[1] for c in cmds[len(rr['cmds']) + len(ctors):]):
            raise SystemExit(f'write32 0x{addr:08x}: a kept pack patch writes this address already')
        cmds.append(('W32', True, addr, (value,)))
    cmds = [c for i, c in enumerate(cmds) if i not in drop]
    # Item Fever's module-relative commands last, so nothing of Retro Rewind's overwrites them.
    for k, ab, addr, args in iff['cmds']:
        if not ab and not (k == 'Rel24' and addr & ~3 in nested):
            cmds.append((k, False, addr + off, shift(k, args, off)))

    body = bytearray()
    for k, ab, addr, args in cmds:
        if ab:
            body += struct.pack('>II', (IDS[k] << 24) | 0xFFFFFE, addr)
        else:
            assert addr < 0xFFFFFE
            body += struct.pack('>I', (IDS[k] << 24) | addr)
        body += b''.join(struct.pack('>I', x) for x in args)
    header = struct.pack('>8I', 0x4B616D65, 0x6B000003, iff['bss'], len(image), ctor_at, ctor_at + 4 * len(ctors),
                         0x20 + len(image) + len(body), 0)
    with open(a.out, 'wb') as f:
        f.write(header + bytes(image) + bytes(body))
    print(f'combined: code 0x{len(image):x} + bss 0x{iff["bss"]:x}, IF at +0x{off:x}, '
          f'{len(ctors)} constructors at +0x{ctor_at:x}, {len(plan["chain"])} chained sites, '
          f'{kept} IF game patches kept, {len(drop)} RR commands replaced, {moved} IF self-references moved, '
          f'{len(plan["stub-rr"])} RR functions stubbed, {len(nested) + len(plan["nest-ctr"])} wrappers nested')
    print(f'Kamek code SHA-1 {hashlib.sha1(bytes(image)).hexdigest()}')


if __name__ == '__main__':
    main()
