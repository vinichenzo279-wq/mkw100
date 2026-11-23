#!/usr/bin/env python3
"""Give a Pulsar pack's per-racer tables sized for 12 racers room for 255, as translator sites.

    python -I scripts/mkw100/pack_racer_tables.py <Code.pul> --profile item-fever \\
        --sites projects/mkwii/mkw100/array-sites.txt --shadow 0x82E00000 \\
        [--write-sites] [--write-tables runtime/src/hle/item_fever_racer_tables.inc] [--report FILE]

Item Fever was written for 12 racers: its effects keep to racers 0..11 (`cmpwi/cmplwi rX, 12`)
and their state lives in static tables of 12 elements. This finds those tables and gives each a
shadow for elements 12..254 the way array-sites.txt does for the tables listed there by hand:

1. Tables. Per function, registers are tracked through straight-line code: a module address
   (lis/addi with a relocation, plus addi offsets), a racer index (a register the function compares
   with 12, or a copy) and a scaled index (rlwinm/slwi or mulli of one). A module address used as
   the base of an indexed load/store or an add with a scaled racer index is a table of 12 elements
   of that size. Tables already shadowed by a site list, tables inside the module's initialised
   data and tables whose 12 elements overlap another's are left out (reported).
2. Sites. Every indexed access to a table, in any function, becomes an `index` (table in rA),
   `indexb` (table in rB) or `indexadd` (add) site: the runtime helpers only act when the base is
   the table and the offset reaches element 12, so the sites change nothing for racers 0..11.
3. Guards. A compare with 12 becomes a `cmpimm 255` site when the compared register (or a copy, or
   a value scaled from it) reaches only shadowed tables and game functions that take any racer
   (Kart::Manager::GetKartPlayer, ...) and is not the counter of a loop that also steps a pointer
   through a table (which would walk past it). Every other compare stays and is reported.

--write-sites replaces the generated block in the site list (between the BEGIN/END markers);
--write-tables writes the table list the runtime uses to reset the shadows at race start.
"""
import argparse
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pul_hooks import parse_pul  # noqa: E402

MODULE_BASE = 0x81800000
INLINE = 12
BIG_TABLE = 24  # a left-out table holding this many racers or more bounds a check instead of blocking it

# Indexed accesses checked by hand: not racer-indexed, or a game array the game bounds itself; ignored
# as untracked-base uses (Item
# Fever 1.1.2 module offsets; another build's offsets match nothing here):
NOT_RACER_SITES = {
    # 0x59d4c (a kart's model visibility, called by the Boo Mushroom's ghost 0x5821c via 0x5e3ec):
    # r3 is a model object, r29 / r30 loop counters capped at 4 over its arrays. The caller's racer
    # number only looks passed in because r3 held it earlier in that caller.
    0x5a32c, 0x5a408,
    # 0x5f268 (the Black Bob-omb's slots, called by 0x57bbc with an object in r3): r29 is a slot
    # number from 0x5ee30 into 8-slot tables at 0x818BE3A8.
    0x5f2d4, 0x5f2dc, 0x5f2ec, 0x5f2f0, 0x5f3fc, 0x5f404,
    0x60fd0,  # 0x60f38, same caller and slots
}
# Item Fever functions whose parameter registers are not racer numbers, checked by hand (a caller's
# racer only looks passed in because the register held it earlier in the caller).
NOT_RACER_PARAMS = {
    0x5f268: set(range(3, 11)), 0x60f38: set(range(3, 11)),  # the Black Bob-omb's slots: 0x57bbc passes an item object
}
# Racer-indexed accesses to game arrays MKW100 sizes for every racer, each checked by hand; they count
# as a racer use that is safe past 12, like a call to a racer function (RACER_TAKERS).
GAME_ARRAY_SITES = {
    # 0x5a4c8 (a racer's effects on or off, same caller): the game's effect manager (0x809C21D0)
    # array of Effects::Player at +0x68, read only below that manager's own count (+0x0).
    0x5a510,
    # The Boo Mushroom's steal (0x58074, 0x57d98, 0x5e710): Item::Manager's Item::Players (+0x14, 0x248 each),
    # which MKW100 sizes for every racer, indexed only below the manager's count (+0x10).
    0x58128, 0x57e14, 0x57e24, 0x5e76c, 0x5e7f0, 0x5e80c,
    0x58e7c, 0x58ed8,  # 0x58e40 / 0x58e9c: a racer's held item / its count
    0x3c38c,  # 0x3c2a4 (the same Item::Players, below the count)
    # 0x58af0: Racedata's player entries (0xF0 each); the `mulli 0xF0` at 0x58b14 is a player-slot site
    # (player-slot-sites.txt), so racers 12+ read their own entry in the overflow region.
    0x58b18, 0x47900,  # 0x478dc too: its `mulli 0xF0` at 0x478fc is a player-slot site
}
RACERS = 255
XLOAD = {23, 87, 279, 343, 535, 599}          # lwzx lbzx lhzx lhax lfsx lfdx
XSTORE = {151, 215, 407, 663, 727}            # stwx stbx sthx stfsx stfdx
XFORM = XLOAD | XSTORE
XFLOAT = {535, 599, 663, 727}
ADD = 266
# X-form memory ops the sites do not rewrite (update forms, byte-reversed, string, cache ops):
# a racer-derived register in one makes its compare unsafe to raise.
XOTHER = {55, 119, 183, 247, 311, 375, 439, 567, 631, 695, 759, 534, 662, 790, 918, 597, 725,
          86, 246, 278, 470, 982, 1014}
# Game functions that take a racer index, and in which register.
RACER_TAKERS = {
    0x80590100: (4,),  # Kart::Manager::GetKartPlayer(this, idx)
    0x805900F4: (4,),  # Kart::Manager::GetKartPlayerById (same table)
    0x807BC908: (),    # (Item::PlayerInventory helper): not a racer taker; listed to be explicit
}
BEGIN = '# BEGIN generated by scripts/mkw100/pack_racer_tables.py (profile {profile}): do not edit by hand'
END = '# END generated by scripts/mkw100/pack_racer_tables.py (profile {profile})'


def word(blob, o):
    return struct.unpack('>I', blob[o:o + 4])[0]


def functions(blob, code, extra=()):
    """Function ranges: every stack-frame prologue and every call target inside the module (leaf
    functions have no prologue), each up to the next start."""
    starts = {o for o in range(0, code - 4, 4) if (word(blob, o) >> 16) == 0x9421}
    starts |= {e for e in extra if 0 <= e < code}
    starts = sorted(starts)
    for i, s in enumerate(starts):
        yield s, starts[i + 1] if i + 1 < len(starts) else code


def equality_test(blob, o, end, field):
    """True when the compare at o is only read as equal / not equal (its branch tests the EQ bit)."""
    for q in range(o + 4, min(end, o + 32), 4):
        w = word(blob, q)
        op = w >> 26
        if op == 16 or (op == 19 and ((w >> 1) & 0x3FF) in (16, 528)):  # bc, bclr, bcctr
            bo, bi = (w >> 21) & 31, (w >> 16) & 31
            if bo & 0x10:          # branch always: not a test of this compare
                continue
            if bi >> 2 == field:
                return (bi & 3) == 2
        if op in (10, 11) and ((w >> 23) & 7) == field:
            return False          # the field is set again before any branch reads it
    return False


def copied_from(blob, start, o, reg):
    """The register `reg` was copied, masked (clrlwi) or sign-extended from just before o, if any."""
    for q in range(o - 4, max(start, o - 24) - 4, -4):
        w = word(blob, q)
        op = w >> 26
        rd, ra, rb = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        if op == 21 and ra == reg:                                  # rlwinm reg, rS, 0, mb, 31
            return rd if rb == 0 and ((w >> 1) & 31) == 31 else None
        if op == 31 and ra == reg and ((w >> 1) & 0x3FF) in (444, 954, 922):  # mr / extsb / extsh
            return rd if ((w >> 1) & 0x3FF) != 444 or rd == rb else None
        if op == 31 and rd == reg and ((w >> 1) & 0x3FF) not in (151, 215, 407, 663, 727):
            return None
        if op in (14, 15, 7, 32, 34, 40, 42) and rd == reg:
            return None
    return None


class Func:
    """One linear pass over a function; records indexed accesses and compares with 12."""

    def __init__(self, blob, relocs, calls, start, end, seeds=()):
        self.start, self.end = start, end
        self.seeds = set(seeds)  # parameter registers callers pass a racer in
        self.module_calls = collections.defaultdict(list)  # racer reg -> (callee, arg reg)
        self.accesses = []     # (site, kind, base reg, idx reg, table addr or None, stride, racer src or None)
        self.guards = []       # (site, reg)
        self.unknown = collections.defaultdict(list)  # racer reg -> sites using it on an untracked base
        self.taken = collections.defaultdict(list)    # racer reg -> calls that take it as a racer
        self.steps = set()     # module addresses stepped by `addi rP, rP, k` (pointer loops)
        self.counters = set()  # registers incremented by `addi r, r, 1`
        self.addrs = set()     # module addresses named by relocations here
        self.members = set()   # base + constant: the other variables of a source file's block
        compared = {}
        alias = {}  # register -> guard register it is the same racer number as
        for o in range(start, end, 4):
            w = word(blob, o)
            if (w >> 26) in (10, 11) and (w & 0xFFFF) == INLINE and not ((w >> 21) & 1):
                reg = (w >> 16) & 31
                if equality_test(blob, o, end, (w >> 23) & 7):
                    continue  # `== 12` / `!= 12`: an item id or a count, not a bound
                self.guards.append((o, reg))
                compared.setdefault(reg, o)
                src = copied_from(blob, start, o, reg)
                if src is not None and src != reg:
                    alias.setdefault(src, reg)
                    compared.setdefault(src, o)
        for reg in self.seeds:
            compared.setdefault(reg, start)
        self.alias = alias
        # Code inside a loop: between a backward branch and its target. `addi rP, rP, k` there steps
        # a pointer; outside a loop it just names base + k (0x81856564: `addi r31, r31, 0x20`).
        loops = []
        for o in range(start, end, 4):
            w = word(blob, o)
            if (w >> 26) == 18 and not (w & 2):
                d = w & 0x03FFFFFC
                d = d - 0x04000000 if d & 0x02000000 else d
            elif (w >> 26) == 16 and not (w & 2):
                d = w & 0xFFFC
                d = d - 0x10000 if d & 0x8000 else d
            else:
                continue
            if d <= 0 and start <= o + d:
                loops.append((o + d, o))
        in_loop = lambda o: any(t <= o <= b for t, b in loops)
        origin = set()  # registers holding a relocation's address itself
        mod = {}      # reg -> table address (module offset)
        racer = {}    # reg -> guard register it came from
        scaled = {}   # reg -> (guard register, stride)
        hi = {}       # reg -> relocation target of a lis
        for o in range(start, end, 4):
            w = word(blob, o)
            op = w >> 26
            rd, ra, rb, imm = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, w & 0xFFFF
            simm = imm - 0x10000 if imm & 0x8000 else imm
            r = relocs.get(o)

            def kill(reg):
                origin.discard(reg)
                mod.pop(reg, None)
                racer.pop(reg, None)
                scaled.pop(reg, None)
                hi.pop(reg, None)

            if op == 15 and ra == 0:                       # lis
                kill(rd)
                if r:
                    hi[rd] = r
            elif op == 14:                                 # addi / li
                if ra != 0 and ra in hi and r:
                    target = r
                    kill(rd)
                    mod[rd] = target
                    origin.add(rd)
                    self.addrs.add(target)
                elif ra != 0 and ra in mod:
                    stepping = rd == ra and in_loop(o)
                    if stepping and simm > 0:
                        self.steps.add(mod[ra])
                    target = mod[ra] + simm
                    from_origin = ra in origin
                    kill(rd)
                    if from_origin:
                        self.members.add(target)  # a variable named as base + constant
                    if not stepping:
                        mod[rd] = target
                elif ra != 0 and rd == ra and simm == 1:
                    self.counters.add(rd)
                    kill(rd)
                else:
                    kill(rd)
            elif op == 21:                                 # rlwinm
                sh, mb, me = rb, (w >> 6) & 31, (w >> 1) & 31
                src = racer.get(rd, rd if rd in compared else None)
                kill(ra)
                if src is not None and sh and me <= 31 - sh:
                    scaled[ra] = (src, 1 << sh)
                elif src is not None and sh == 0:
                    racer[ra] = src                        # a mask (clrlwi) keeps the index
            elif op == 7:                                  # mulli
                src = racer.get(ra, ra if ra in compared else None)
                kill(rd)
                if src is not None and simm > 0:
                    scaled[rd] = (src, simm)
            elif op == 31:
                xo = (w >> 1) & 0x3FF
                if xo == 444 and rd == rb:                 # mr ra, rs
                    if ra != rd:
                        m, rc, sc, h = mod.get(rd), racer.get(rd), scaled.get(rd), hi.get(rd)
                        if rd in compared and rc is None:
                            rc = rd
                        kill(ra)
                        if m is not None:
                            mod[ra] = m
                        if rc is not None:
                            racer[ra] = rc
                        if sc is not None:
                            scaled[ra] = sc
                        if h is not None:
                            hi[ra] = h
                elif xo in XOTHER:
                    for reg in (ra, rb):
                        if reg in scaled:
                            self.unknown[self.canon(scaled[reg][0])].append(o)
                        elif reg in racer or reg in compared:
                            self.unknown[self.canon(racer.get(reg, reg))].append(o)
                    if ra in mod and xo in (55, 119, 183, 247, 311, 375, 439, 567, 631, 695, 759):
                        self.steps.add(mod[ra])  # an update form walks a pointer through a table
                    kill(ra)
                    kill(rd)
                elif xo in XFORM or xo == ADD:
                    self._access(o, xo, rd, ra, rb, mod, racer, scaled, compared)
                    if xo in XLOAD and not (xo in XFLOAT):
                        kill(rd)
                    elif xo == ADD:
                        kill(rd)
                elif xo == 954 and rb == 0:                # extsb ra, rs
                    src = racer.get(rd, rd if rd in compared else None)
                    kill(ra)
                    if src is not None:
                        racer[ra] = src
                elif xo == 467:                            # mtspr: no GPR written (rA/rB hold the SPR)
                    pass
                elif xo in (339, 371):                     # mfspr, mftb: only rD
                    kill(rd)
                else:
                    # other X-form arithmetic/logic writes rA (logic) or rD (arith); drop both
                    kill(ra)
                    kill(rd)
            elif op in (32, 33, 34, 35, 40, 41, 42, 43):  # integer D-form loads
                kill(rd)
            elif op == 18 and (w & 1):                     # bl
                target = calls.get(o)
                if target is None and not (w & 2):          # a call inside the module
                    li = w & 0x03FFFFFC
                    if li & 0x02000000:
                        li -= 0x04000000
                    callee = o + li
                    for reg in range(3, 11):
                        src = racer.get(reg, reg if reg in compared and reg not in mod else None)
                        if src is not None:
                            self.module_calls[self.canon(src)].append((callee, reg))
                if target is not None and target in RACER_TAKERS:
                    for reg in RACER_TAKERS[target]:
                        src = racer.get(reg, reg if reg in compared else None)
                        if src is not None:
                            self.taken[self.canon(src)].append(o)
                for reg in list(set(mod) | set(racer) | set(scaled) | set(hi)):
                    if reg <= 12:
                        kill(reg)
            elif op in (24, 25, 26, 27, 28, 29):           # ori oris xori xoris andi. andis.
                kill(ra)
            elif op in (12, 13, 8):                        # addic addic. subfic
                kill(rd)

    def canon(self, reg):
        return self.alias.get(reg, reg) if reg is not None else None

    def _access(self, o, xo, rd, ra, rb, mod, racer, scaled, compared):
        table_reg = idx_reg = None
        if (ra != 0 or xo == ADD) and ra in mod:
            table_reg, idx_reg = ra, rb
        elif rb in mod:
            table_reg, idx_reg = rb, ra
        src, stride = None, None
        if idx_reg is not None and o not in NOT_RACER_SITES:
            if idx_reg in scaled:
                src, stride = scaled[idx_reg]
            elif idx_reg in racer or idx_reg in compared:
                src, stride = racer.get(idx_reg, idx_reg), 1
        kind = 'add' if xo == ADD else ('xa' if table_reg == ra else 'xb')
        if table_reg is not None and (idx_reg == table_reg or (kind != 'add' and ra == 0)):
            table_reg = None  # base and index the same register, or rA = 0 (the value 0 in a load or store)
        if table_reg is not None:
            self.accesses.append((o, kind, table_reg, idx_reg, mod[table_reg], stride, self.canon(src)))
        else:
            # racer-derived offset on a base we cannot name
            if o in GAME_ARRAY_SITES:
                for reg in (ra, rb):
                    if reg in scaled:
                        self.taken[self.canon(scaled[reg][0])].append(o)
                    elif reg in racer or reg in compared:
                        self.taken[self.canon(racer.get(reg, reg))].append(o)
                return
            for reg in (() if o in NOT_RACER_SITES else (ra, rb)):
                if reg in scaled:
                    self.unknown[self.canon(scaled[reg][0])].append(o)
                elif reg in racer or reg in compared:
                    self.unknown[self.canon(racer.get(reg, reg))].append(o)


def analyse(pul, known):
    blob, code = pul['blob'], pul['code']
    relocs, calls = {}, {}
    for kind, absolute, addr, args in pul['cmds']:
        if absolute:
            continue
        if kind in ('Ha16', 'Lo16', 'Hi16'):
            if kind != 'Lo16':
                relocs.setdefault(addr & ~3, args[0])
            else:
                relocs[addr & ~3] = args[0]
        elif kind == 'Rel24':
            calls[addr & ~3] = args[0]
    targets = set()
    for o in range(0, code - 4, 4):
        w = word(blob, o)
        if (w >> 26) == 18 and (w & 1) and not (w & 2) and o not in calls:
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            targets.add(o + li)
    spans_all = list(functions(blob, code, targets))
    funcs = [Func(blob, relocs, calls, s, e) for s, e in spans_all]
    for _ in range(3):  # racer parameters: what callers pass a racer in
        seeds = collections.defaultdict(set)
        for f in funcs:
            for src, lst in f.module_calls.items():
                for callee, reg in lst:
                    seeds[callee].add(reg)
        funcs = [Func(blob, relocs, calls, s, e, set(seeds.get(s, ())) - NOT_RACER_PARAMS.get(s, set()))
                 for s, e in spans_all]
    by_start = {f.start: f for f in funcs}
    # 1. tables: a racer-indexed access names the table and its element size
    strides = collections.defaultdict(collections.Counter)
    for f in funcs:
        for o, kind, treg, ireg, table, stride, src in f.accesses:
            if src is not None and stride:
                strides[table][stride] += 1
    # variable boundaries: what relocations name, and the other tables' starts (fields of element 0
    # and loop pointers inside a table are not boundaries)
    bounds = sorted(set(relocs.values()) | set(strides) | {m for f in funcs for m in f.members})

    def next_bound(t):
        import bisect
        i = bisect.bisect_right(bounds, t)
        return bounds[i] if i < len(bounds) else None

    tables, left_out = {}, {}
    capacity = {}  # left-out tables with more than 12 elements: how many they hold
    for table, c in strides.items():
        stride = c.most_common(1)[0][0]
        if len(c) > 1:
            left_out[table] = f'several element sizes {dict(c)}'
        elif MODULE_BASE + table in known:
            left_out[table] = 'already shadowed by a site list'
        elif table < code:
            left_out[table] = 'initialised data'
        else:
            # a [12] table ends where the next variable starts (give or take alignment)
            nb = next_bound(table)
            size = INLINE * stride
            if nb is not None and nb - table < size:
                left_out[table] = f'next variable at +0x{nb - table:x}, inside 12 elements (0x{size:x})'
            elif nb is not None and nb - table > ((size + 7) & ~7) + 8:
                left_out[table] = f'next variable at +0x{nb - table:x}: more than 12 elements (0x{size:x})'
                capacity[table] = min((nb - table) // stride, RACERS)
            else:
                tables[table] = stride
    spans = sorted((t, t + INLINE * s) for t, s in tables.items())
    for (a0, a1), (b0, b1) in zip(spans, spans[1:]):
        if b0 < a1:
            left_out[a0] = left_out.get(a0, f'overlaps 0x{b0:x}')
            left_out[b0] = left_out.get(b0, f'overlaps 0x{a0:x}')
    for t in list(tables):
        if t in left_out:
            del tables[t]
    # 2. sites: every indexed access to a table (the helpers check base and offset at run time)
    sites = {}
    for f in funcs:
        for o, kind, treg, ireg, table, stride, src in f.accesses:
            if table in tables and (stride is None or stride == tables[table]):
                sites[o] = (kind, table)
    # 3. guards
    shadowed = set(tables) | {k - MODULE_BASE for k in known}
    memo = {}

    def why_unsafe(f, reg, depth=0):
        """(None, limit) when racers 12+ reaching `reg` in f only touch shadowed tables, or tables that
        already hold `limit` racers (Item Fever's 64-racer tables); else (the reason, 0)."""
        key = (f.start, reg)
        if key in memo:
            return memo[key]
        memo[key] = (None, RACERS)  # a cycle: assume safe while it is being decided
        reason, limit = None, RACERS
        uses = [a for a in f.accesses if a[6] == reg]
        big = {a[4] for a in uses if a[4] not in shadowed and capacity.get(a[4], 0) >= BIG_TABLE}
        off = sorted({hex(a[4]) for a in uses if a[4] not in shadowed and a[4] not in big})
        if big:
            limit = min(capacity[t] for t in big)
        if off:
            reason = 'indexes tables not shadowed: ' + ','.join(off)
        elif f.unknown.get(reg):
            reason = 'racer offset on an untracked base at ' + ','.join(hex(u) for u in f.unknown[reg][:3])
        elif depth < 8:
            for callee, arg in f.module_calls.get(reg, []):
                c = by_start.get(callee)
                if c is None:
                    reason = f'passed to 0x{callee:x}, not a known function start'
                    break
                own_guard = [o for o, r in c.guards if r == arg]
                if own_guard and any(o not in widen_set for o in own_guard):
                    continue  # the callee keeps racers 12+ out itself
                sub, sub_limit = why_unsafe(c, arg, depth + 1)
                if sub:
                    reason = f'via 0x{callee:x}: {sub}'
                    break
                limit = min(limit, sub_limit)
        memo[key] = (reason, 0 if reason else limit)
        return memo[key]

    widen_set = set()
    widen, keep = {}, []
    for f in funcs:
        pointer_loop = bool(f.steps & set(tables)) or any(
            t <= s < t + INLINE * tables[t] for s in f.steps for t in tables)
        for o, reg in f.guards:
            uses = [a for a in f.accesses if a[6] == reg]
            taken = f.taken.get(reg, [])
            passed = f.module_calls.get(reg, [])
            if not uses and not taken and not passed:
                keep.append((o, f.start, 'no racer use found'))
                continue
            if pointer_loop and reg in f.counters:
                keep.append((o, f.start, 'loop counter of a pointer-stepping loop'))
                continue
            reason, limit = why_unsafe(f, reg)
            if not reason and not uses and not taken:
                reason = 'only passed on to Item Fever functions (no table or racer function of its own)'
            if reason:
                keep.append((o, f.start, reason))
            else:
                widen[o] = (f.start, len(uses), len(taken), limit)
                widen_set.add(o)
    return tables, left_out, sites, widen, keep


def known_tables(path, profile):
    """Tables the site list already shadows by hand (the generated block is not counted)."""
    known = set()
    inside = False
    for line in open(path, encoding='utf-8'):
        if line.startswith('# BEGIN generated by scripts/mkw100/pack_racer_tables.py'):
            inside = True
        elif line.startswith('# END generated by scripts/mkw100/pack_racer_tables.py'):
            inside = False
        if inside:
            continue
        f = line.split('#')[0].split()
        if len(f) >= 6 and f[2] == 'index' and f'profile={profile}' in line:
            known.add(int(f[4], 16))
    return known


def known_addresses(path, profile):
    out = set()
    inside = False
    for line in open(path, encoding='utf-8'):
        if line.startswith('# BEGIN generated by scripts/mkw100/pack_racer_tables.py'):
            inside = True
        elif line.startswith('# END generated by scripts/mkw100/pack_racer_tables.py'):
            inside = False
        elif not inside:
            f = line.split('#')[0].split()
            if f and (f'profile={profile}' in line or not any(x.startswith('profile=') for x in f)):
                out.add(int(f[0], 16))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pul')
    ap.add_argument('--profile', default='item-fever')
    ap.add_argument('--sites', default='projects/mkwii/mkw100/array-sites.txt')
    ap.add_argument('--shadow', default='0x82E00000')
    ap.add_argument('--shadow-size', default='0x100000')
    ap.add_argument('--write-sites', action='store_true')
    ap.add_argument('--write-tables')
    ap.add_argument('--report')
    ap.add_argument('--guards', help='N:M: raise only these raised compares (sorted by address); for bisecting')
    a = ap.parse_args()
    pul = parse_pul(a.pul)
    tables, left_out, sites, widen, keep = analyse(pul, known_tables(a.sites, a.profile))
    taken_sites = known_addresses(a.sites, a.profile)
    # shadows: elements 12..254 of each table, 4-byte aligned, in table order
    shadow, limit = int(a.shadow, 16), int(a.shadow, 16) + int(a.shadow_size, 16)
    shadows = {}
    for t in sorted(tables):
        shadows[t] = shadow
        shadow += ((RACERS - INLINE) * tables[t] + 3) & ~3
    if shadow > limit:
        sys.exit(f'shadows need 0x{shadow - int(a.shadow, 16):x} bytes, more than {a.shadow_size}')
    lines = []
    clash = 0
    for o in sorted(sites):
        kind, t = sites[o]
        if MODULE_BASE + o in taken_sites:
            clash += 1
            continue
        k = {'xa': 'index', 'xb': 'indexb', 'add': 'indexadd'}[kind]
        lines.append(f'0x{MODULE_BASE + o:08X}  0  {k}  {tables[t]}  0x{MODULE_BASE + t:08X}  '
                     f'0x{shadows[t]:08X}  profile={a.profile}')
    raised = sorted(widen)
    if a.guards:
        n, m = (int(x) if x else None for x in a.guards.split(':'))
        raised = raised[n:m]
    for o in raised:
        if MODULE_BASE + o in taken_sites:
            continue
        limit = widen[o][3]
        note = '' if limit >= RACERS else f' (its tables hold {limit} racers)'
        lines.append(f'0x{MODULE_BASE + o:08X}  0  cmpimm  {limit}  0x0  profile={a.profile}  '
                     f'# racer < 12 in 0x{MODULE_BASE + widen[o][0]:08X}{note}')
    report = [
        f'{len(tables)} tables shadowed ({shadow - int(a.shadow, 16):#x} bytes from {a.shadow}), '
        f'{len(left_out)} left out, {len(sites)} access sites ({clash} already listed), '
        f'{len(widen)} compares raised ({sum(1 for v in widen.values() if v[3] < RACERS)} of them below 255), {len(keep)} kept at 12',
    ]
    report += [f'table 0x{MODULE_BASE + t:08X} x{tables[t]} shadow 0x{shadows[t]:08X}' for t in sorted(tables)]
    report += [f'left out 0x{MODULE_BASE + t:08X}: {why}' for t, why in sorted(left_out.items())]
    report += [f'kept 0x{MODULE_BASE + o:08X} (in 0x{MODULE_BASE + f:08X}): {why}' for o, f, why in sorted(keep)]
    print(report[0])
    if a.report:
        open(a.report, 'w', encoding='utf-8').write('\n'.join(report) + '\n')
    if a.write_sites:
        text = open(a.sites, encoding='utf-8').read()
        begin, end = BEGIN.format(profile=a.profile), END.format(profile=a.profile)
        block = '\n'.join([begin] + lines + [end]) + '\n'
        if begin in text:
            pre, rest = text.split(begin, 1)
            post = rest.split(end, 1)[1].lstrip('\n')
            text = pre + block + post
        else:
            text = text.rstrip('\n') + '\n\n' + block
        open(a.sites, 'w', encoding='utf-8', newline='\n').write(text)
    if a.write_tables:
        out = [f'// Generated by scripts/mkw100/pack_racer_tables.py (profile {a.profile}): do not edit.',
               '// { table, element size, shadow of elements 12..254 }']
        out += [f'{{0x{MODULE_BASE + t:08X}, {tables[t]}, 0x{shadows[t]:08X}}},' for t in sorted(tables)]
        open(a.write_tables, 'w', encoding='utf-8', newline='\n').write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
