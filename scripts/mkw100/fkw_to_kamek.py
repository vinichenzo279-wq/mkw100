#!/usr/bin/env python3
"""Turn a Formula Kart Wii payload (fkw/fkw/FormulaKartWii<R>.bin) into a Kamek chunk.

FKW is not a Pulsar pack. Riivolution loads a small bootstrap (fkw/Loader.bin) into .init, which
reads the payload to a fixed address (0x808DD400 in every region) after StaticR has loaded and calls
the payload's loadCodes. loadCodes patches the game once with plain stores (directWrite8/16/32,
branches, memcpy) whose values depend only on the Riivolution setting bytes at 0x80004030..0x8000406F.

A static recompiler cannot follow code that patches itself at run time, so this script runs
loadCodes once in a small PowerPC interpreter against the base game images, records every byte it
stores outside the stack, and writes the result as a raw Kamek v3 chunk the translator's mod pipeline
already reads (translate-mod --code-pul <chunk> --module-guest-base 0x808DD400):

  * the payload image (with any bytes loadCodes wrote into the payload itself folded in) is the
    module code, linked absolute at 0x808DD400, so it needs no relocations;
  * a word in base code that becomes a branch into the payload -> absolute Branch/BranchLink;
  * a word in base data that becomes a pointer into the payload -> absolute WritePointer (Addr32);
  * everything else -> absolute Write32/Write16/Write8 with the final value.

The setting bytes themselves live in .init's TRK vector area, which has no translated function, so
they are not emitted here; the runtime writes them (runtime/src/hle/formula_kart.cpp).

Only the user's own download is read; no FKW code is copied into MKW100.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

PAYLOAD_BASE = 0x808DD400
SETTINGS_START = 0x80004030
SETTINGS_END = 0x80004070
# MKW100's array shadow window (runtime/include/guest_abi/array_overflow.h, 0x82000000..0x83000000);
# packs use the 0x82D00000 span (Item Fever up to 0x82D30200, Retro Rewind at 0x82D80000).
AMOUNTS_SHADOW = 0x82D60000
INIT_SCRATCH_START = 0x80004000
INIT_SCRATCH_END = 0x80005F34  # first translated .init function (memcpy)
STACK_TOP = 0x81700000
STACK_BOTTOM = STACK_TOP - 0x10000
RETURN_SENTINEL = 0xDEADBEE0

MASK32 = 0xFFFFFFFF


def s16(v: int) -> int:
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def s32(v: int) -> int:
    v &= MASK32
    return v - 0x100000000 if v & 0x80000000 else v


def rotl(v: int, n: int) -> int:
    n &= 31
    v &= MASK32
    return ((v << n) | (v >> (32 - n))) & MASK32 if n else v


def mask(mb: int, me: int) -> int:
    m = 0
    i = mb
    while True:
        m |= 1 << (31 - i)
        if i == me:
            break
        i = (i + 1) & 31
    return m


class Memory:
    def __init__(self) -> None:
        self.images: list[tuple[int, bytes]] = []
        self.overlay: dict[int, int] = {}
        self.writes: dict[int, int] = {}  # final byte per address, excluding the stack
        self.write_order: list[int] = []
        self.unmapped_reads: set[int] = set()

    def add_image(self, base: int, data: bytes) -> None:
        self.images.append((base, data))

    def read8(self, a: int) -> int:
        a &= MASK32
        if a in self.overlay:
            return self.overlay[a]
        for base, data in self.images:
            if base <= a < base + len(data):
                return data[a - base]
        self.unmapped_reads.add(a & ~0xF)
        return 0

    def write8(self, a: int, v: int) -> None:
        a &= MASK32
        v &= 0xFF
        self.overlay[a] = v
        if not (STACK_BOTTOM <= a < STACK_TOP):
            if a not in self.writes:
                self.write_order.append(a)
            self.writes[a] = v

    def read(self, a: int, n: int) -> int:
        v = 0
        for i in range(n):
            v = (v << 8) | self.read8(a + i)
        return v

    def write(self, a: int, v: int, n: int) -> None:
        for i in range(n):
            self.write8(a + i, (v >> (8 * (n - 1 - i))) & 0xFF)


class Cpu:
    def __init__(self, mem: Memory, natives: dict[int, str]) -> None:
        self.m = mem
        self.r = [0] * 32
        self.lr = 0
        self.ctr = 0
        self.cr = 0
        self.xer_ca = 0
        self.pc = 0
        self.natives = natives
        self.steps = 0
        self.copies: list[tuple[int, int, int]] = []  # memcpy (dst, src, n)

    # condition register helpers
    def set_cr_field(self, field: int, lt: bool, gt: bool, eq: bool) -> None:
        bits = (8 if lt else 0) | (4 if gt else 0) | (2 if eq else 0)
        shift = 28 - 4 * field
        self.cr = (self.cr & ~(0xF << shift) & MASK32) | (bits << shift)

    def cr_bit(self, bit: int) -> int:
        return (self.cr >> (31 - bit)) & 1

    def set_cr_bit(self, bit: int, value: int) -> None:
        if value:
            self.cr |= 1 << (31 - bit)
        else:
            self.cr &= ~(1 << (31 - bit)) & MASK32

    def rec(self, v: int) -> None:
        sv = s32(v)
        self.set_cr_field(0, sv < 0, sv > 0, sv == 0)

    def ea(self, ra: int, d: int) -> int:
        return ((self.r[ra] if ra else 0) + d) & MASK32

    def branch_ok(self, bo: int, bi: int) -> bool:
        if not (bo & 0x4):
            self.ctr = (self.ctr - 1) & MASK32
        ctr_ok = (bo & 0x4) or ((self.ctr != 0) ^ bool(bo & 0x2))
        cond_ok = (bo & 0x10) or (self.cr_bit(bi) == ((bo >> 3) & 1))
        return bool(ctr_ok and cond_ok)

    def call_native(self, target: int) -> None:
        name = self.natives[target]
        if name == "memcpy":
            dst, src, n = self.r[3], self.r[4], self.r[5]
            self.copies.append((dst, src, n))
            for i in range(n):
                self.m.write8(dst + i, self.m.read8(src + i))
        elif name == "nop":
            pass
        else:
            raise RuntimeError(f"native {name} not implemented")
        self.pc = self.lr

    def run(self, entry: int, max_steps: int = 5_000_000) -> None:
        self.pc = entry
        self.lr = RETURN_SENTINEL
        self.r[1] = STACK_TOP - 0x100
        while self.pc != RETURN_SENTINEL:
            if self.pc in self.natives:
                self.call_native(self.pc)
                continue
            self.steps += 1
            if self.steps > max_steps:
                raise RuntimeError("step limit hit")
            insn = self.m.read(self.pc, 4)
            if not (PAYLOAD_BASE <= self.pc < PAYLOAD_BASE + 0x100000):
                raise RuntimeError(f"execution left the payload at 0x{self.pc:08X} (insn 0x{insn:08X})")
            self.step(insn)

    def step(self, i: int) -> None:
        pc = self.pc
        nxt = (pc + 4) & MASK32
        op = i >> 26
        rd = (i >> 21) & 31
        ra = (i >> 16) & 31
        rb = (i >> 11) & 31
        imm = i & 0xFFFF
        r = self.r
        m = self.m

        if op == 14:  # addi
            r[rd] = ((r[ra] if ra else 0) + s16(imm)) & MASK32
        elif op == 15:  # addis
            r[rd] = ((r[ra] if ra else 0) + (s16(imm) << 16)) & MASK32
        elif op == 12 or op == 13:  # addic / addic.
            res = r[ra] + (s16(imm) & MASK32)
            self.xer_ca = 1 if res > MASK32 else 0
            r[rd] = res & MASK32
            if op == 13:
                self.rec(r[rd])
        elif op == 8:  # subfic
            res = (s16(imm) & MASK32) + ((~r[ra]) & MASK32) + 1
            self.xer_ca = 1 if res > MASK32 else 0
            r[rd] = res & MASK32
        elif op == 7:  # mulli
            r[rd] = (s32(r[ra]) * s16(imm)) & MASK32
        elif op == 24:
            r[ra] = r[rd] | imm
        elif op == 25:
            r[ra] = r[rd] | (imm << 16)
        elif op == 26:
            r[ra] = r[rd] ^ imm
        elif op == 27:
            r[ra] = r[rd] ^ (imm << 16)
        elif op == 28:
            r[ra] = r[rd] & imm
            self.rec(r[ra])
        elif op == 29:
            r[ra] = r[rd] & (imm << 16)
            self.rec(r[ra])
        elif op == 10:  # cmpli
            a, b = r[ra], imm
            self.set_cr_field(rd >> 2, a < b, a > b, a == b)
        elif op == 11:  # cmpi
            a, b = s32(r[ra]), s16(imm)
            self.set_cr_field(rd >> 2, a < b, a > b, a == b)
        elif op == 18:  # b
            li = i & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            target = (li if (i & 2) else pc + li) & MASK32
            if i & 1:
                self.lr = nxt
            self.pc = target
            return
        elif op == 16:  # bc
            bd = i & 0xFFFC
            if bd & 0x8000:
                bd -= 0x10000
            if i & 1:
                self.lr = nxt
            if self.branch_ok(rd, ra):
                self.pc = (bd if (i & 2) else pc + bd) & MASK32
                return
        elif op == 19:
            xo = (i >> 1) & 0x3FF
            if xo == 16:  # bclr
                target = self.lr
                if i & 1:
                    self.lr = nxt
                if self.branch_ok(rd, ra):
                    self.pc = target & ~3 & MASK32
                    return
            elif xo == 528:  # bcctr
                if i & 1:
                    self.lr = nxt
                if self.branch_ok(rd | 0x4, ra):
                    self.pc = self.ctr & ~3 & MASK32
                    return
            elif xo == 150:  # isync
                pass
            elif xo in (193, 289, 257, 449, 33, 129, 225, 417):  # cr logic
                a, b = self.cr_bit(ra), self.cr_bit(rb)
                v = {193: a ^ b, 289: 1 - (a ^ b), 257: a & b, 449: a | b, 33: 1 - (a | b),
                     129: a & (1 - b), 225: 1 - (a & b), 417: a | (1 - b)}[xo]
                self.set_cr_bit(rd, v)
            else:
                raise RuntimeError(f"op19 xo {xo} at 0x{pc:08X}")
        elif op in (20, 21, 23):
            sh = rb if op != 23 else (r[rb] & 31)
            mb = (i >> 6) & 31
            me = (i >> 1) & 31
            mk = mask(mb, me)
            rot = rotl(r[rd], sh)
            if op == 20:
                r[ra] = (rot & mk) | (r[ra] & ~mk & MASK32)
            else:
                r[ra] = rot & mk
            if i & 1:
                self.rec(r[ra])
        elif op in (32, 33, 34, 35, 40, 41, 42, 43):
            a = self.ea(ra, s16(imm))
            n = {32: 4, 33: 4, 34: 1, 35: 1, 40: 2, 41: 2, 42: 2, 43: 2}[op]
            v = m.read(a, n)
            if op in (42, 43):
                v = s16(v) & MASK32
            r[rd] = v
            if op in (33, 35, 41, 43):
                r[ra] = a
        elif op in (36, 37, 38, 39, 44, 45):
            a = self.ea(ra, s16(imm))
            n = {36: 4, 37: 4, 38: 1, 39: 1, 44: 2, 45: 2}[op]
            m.write(a, r[rd], n)
            if op in (37, 39, 45):
                r[ra] = a
        elif op == 46:  # lmw
            a = self.ea(ra, s16(imm))
            for k in range(rd, 32):
                r[k] = m.read(a, 4)
                a += 4
        elif op == 47:  # stmw
            a = self.ea(ra, s16(imm))
            for k in range(rd, 32):
                m.write(a, r[k], 4)
                a += 4
        elif op == 31:
            self.op31(i, rd, ra, rb)
        else:
            raise RuntimeError(f"unsupported opcode {op} (0x{i:08X}) at 0x{pc:08X}")
        self.pc = nxt

    def op31(self, i: int, rd: int, ra: int, rb: int) -> None:
        r = self.r
        m = self.m
        xo = (i >> 1) & 0x3FF
        rc = i & 1
        xo9 = xo & 0x1FF
        res_reg = None
        if xo9 == 266:
            r[rd] = (r[ra] + r[rb]) & MASK32; res_reg = rd
        elif xo9 == 40:
            r[rd] = (r[rb] - r[ra]) & MASK32; res_reg = rd
        elif xo9 == 104:
            r[rd] = (-r[ra]) & MASK32; res_reg = rd
        elif xo9 == 235:
            r[rd] = (s32(r[ra]) * s32(r[rb])) & MASK32; res_reg = rd
        elif xo9 == 459:
            r[rd] = (r[ra] // r[rb]) if r[rb] else 0; res_reg = rd
        elif xo9 == 491:
            a, b = s32(r[ra]), s32(r[rb])
            r[rd] = (int(a / b) & MASK32) if b else 0; res_reg = rd
        elif xo9 == 10:
            res = r[ra] + r[rb]; self.xer_ca = res >> 32; r[rd] = res & MASK32; res_reg = rd
        elif xo9 == 8:
            res = (~r[ra] & MASK32) + r[rb] + 1; self.xer_ca = res >> 32; r[rd] = res & MASK32; res_reg = rd
        elif xo9 == 138:
            res = r[ra] + r[rb] + self.xer_ca; self.xer_ca = res >> 32; r[rd] = res & MASK32; res_reg = rd
        elif xo9 == 136:
            res = (~r[ra] & MASK32) + r[rb] + self.xer_ca; self.xer_ca = res >> 32; r[rd] = res & MASK32; res_reg = rd
        elif xo9 == 202:
            res = r[ra] + self.xer_ca; self.xer_ca = res >> 32; r[rd] = res & MASK32; res_reg = rd
        elif xo == 444:
            r[ra] = r[rd] | r[rb]; res_reg = ra
        elif xo == 28:
            r[ra] = r[rd] & r[rb]; res_reg = ra
        elif xo == 316:
            r[ra] = r[rd] ^ r[rb]; res_reg = ra
        elif xo == 124:
            r[ra] = ~(r[rd] | r[rb]) & MASK32; res_reg = ra
        elif xo == 60:
            r[ra] = r[rd] & ~r[rb] & MASK32; res_reg = ra
        elif xo == 412:
            r[ra] = (r[rd] | (~r[rb] & MASK32)) & MASK32; res_reg = ra
        elif xo == 24:
            n = r[rb] & 63; r[ra] = (r[rd] << n) & MASK32 if n < 32 else 0; res_reg = ra
        elif xo == 536:
            n = r[rb] & 63; r[ra] = (r[rd] >> n) if n < 32 else 0; res_reg = ra
        elif xo == 824:
            n = rb; v = s32(r[rd]); r[ra] = (v >> n) & MASK32
            self.xer_ca = 1 if v < 0 and (r[rd] & ((1 << n) - 1)) else 0; res_reg = ra
        elif xo == 792:
            n = r[rb] & 63; v = s32(r[rd]); r[ra] = (v >> min(n, 31)) & MASK32; res_reg = ra
        elif xo == 954:
            v = r[rd] & 0xFF; r[ra] = (v - 0x100 if v & 0x80 else v) & MASK32; res_reg = ra
        elif xo == 922:
            r[ra] = s16(r[rd]) & MASK32; res_reg = ra
        elif xo == 26:
            v = r[rd]; n = 0
            while n < 32 and not (v & (0x80000000 >> n)):
                n += 1
            r[ra] = n; res_reg = ra
        elif xo == 0:  # cmp
            a, b = s32(r[ra]), s32(r[rb]); self.set_cr_field(rd >> 2, a < b, a > b, a == b)
        elif xo == 32:  # cmpl
            a, b = r[ra], r[rb]; self.set_cr_field(rd >> 2, a < b, a > b, a == b)
        elif xo == 339:  # mfspr
            spr = ((i >> 16) & 31) | (((i >> 11) & 31) << 5)
            r[rd] = {8: self.lr, 9: self.ctr, 1: self.xer_ca << 29}.get(spr, 0)
        elif xo == 467:  # mtspr
            spr = ((i >> 16) & 31) | (((i >> 11) & 31) << 5)
            if spr == 8:
                self.lr = r[rd]
            elif spr == 9:
                self.ctr = r[rd]
            elif spr == 1:
                self.xer_ca = (r[rd] >> 29) & 1
        elif xo == 19:
            r[rd] = self.cr
        elif xo == 144:  # mtcrf
            self.cr = r[rd]
        elif xo in (86, 982, 598, 54, 278, 246, 1014, 470, 854):  # cache / sync
            pass
        elif xo in (23, 55, 87, 119, 279, 311, 343):
            a = ((r[ra] if ra else 0) + r[rb]) & MASK32
            n = {23: 4, 55: 4, 87: 1, 119: 1, 279: 2, 311: 2, 343: 2}[xo]
            v = m.read(a, n)
            if xo == 343:
                v = s16(v) & MASK32
            r[rd] = v
            if xo in (55, 119, 311):
                r[ra] = a
        elif xo in (151, 183, 215, 247, 407, 439):
            a = ((r[ra] if ra else 0) + r[rb]) & MASK32
            n = {151: 4, 183: 4, 215: 1, 247: 1, 407: 2, 439: 2}[xo]
            m.write(a, r[rd], n)
            if xo in (183, 247, 439):
                r[ra] = a
        else:
            raise RuntimeError(f"unsupported op31 xo {xo} (0x{i:08X}) at 0x{self.pc:08X}")
        if rc and res_reg is not None:
            self.rec(r[res_reg])


def default_settings(xml_path: Path, choices: dict[str, int]) -> dict[int, int]:
    """Setting bytes the Riivolution defaults (or explicit choices) select."""
    root = ET.parse(xml_path).getroot()
    patches: dict[str, list[tuple[int, int]]] = {}
    for p in root.iter("patch"):
        mems = []
        for mem in p.findall("memory"):
            off = int(mem.get("offset", "0"), 16)
            val = mem.get("value")
            if val and SETTINGS_START <= off < SETTINGS_END:
                mems.append((off, int(val, 16)))
        patches[p.get("id")] = mems
    out: dict[int, int] = {}
    for opt in root.iter("option"):
        name = opt.get("name")
        choice = choices.get(name, int(opt.get("default", "0")))
        opts = opt.findall("choice")
        if 1 <= choice <= len(opts):
            for pref in opts[choice - 1].findall("patch"):
                for off, val in patches.get(pref.get("id"), []):
                    out[off] = val
    return out


def section_map(dol: bytes, rel: bytes, rel_load: int) -> list[dict]:
    """Executable/data ranges of main.dol and StaticR.rel as the base manifest places them."""
    out = []
    for k in range(18):
        off, addr, size = (struct.unpack_from(">I", dol, base + 4 * k)[0] for base in (0x0, 0x48, 0x90))
        if size:
            out.append({"Name": f"dol{k}", "GuestStart": addr, "GuestEnd": addr + size, "Executable": k < 7})
    bss_addr, bss_size = struct.unpack_from(">II", dol, 0xD8)
    out.append({"Name": "dol-bss", "GuestStart": bss_addr, "GuestEnd": bss_addr + bss_size, "Executable": False})
    count, table = struct.unpack_from(">II", rel, 0x0C)
    for k in range(count):
        off, size = struct.unpack_from(">II", rel, table + 8 * k)
        if off & ~1 and size:
            start = rel_load + (off & ~1)
            out.append({"Name": f"StaticR.rel:{k}", "GuestStart": start, "GuestEnd": start + size,
                        "Executable": bool(off & 1)})
    out.append({"Name": "lowmem", "GuestStart": 0x80000000, "GuestEnd": 0x80004000, "Executable": False})
    return out


def find_section(sections: list[dict], a: int) -> dict | None:
    for s in sections:
        if s["GuestStart"] <= a < s["GuestEnd"]:
            return s
    return None


def kamek_chunk(code: bytes, commands: list[bytes]) -> bytes:
    body = b"".join(commands)
    size = 0x20 + len(code) + len(body)
    header = struct.pack(">IIIIIIII", 0x4B616D65, 0x6B000003, 0, len(code), 0, 0, size, 0)
    return header + code + body


def cmd_abs(cid: int, addr: int, *args: int) -> bytes:
    return struct.pack(">II", (cid << 24) | 0x00FFFFFE, addr) + b"".join(struct.pack(">I", a) for a in args)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--payload", required=True, type=Path, help="fkw/fkw/FormulaKartWiiP.bin")
    ap.add_argument("--xml", required=True, type=Path, help="riivolution/fkw.xml (for the default settings)")
    ap.add_argument("--dol", required=True, type=Path, help="Assets/main.dol (PAL)")
    ap.add_argument("--rel", required=True, type=Path, help="Assets/StaticR.rel (PAL)")
    ap.add_argument("--rel-load", default="0x805102E0", help="StaticR load address (recomp.yml)")
    ap.add_argument("--out", required=True, type=Path, help="output Kamek chunk (Code.pul stand-in)")
    ap.add_argument("--report", type=Path, help="JSON report of every patch and the settings bytes")
    ap.add_argument("--choice", action="append", default=[], help="'Option Name=N' (Riivolution 1-based choice)")
    ap.add_argument("--max-racers", type=int, default=255,
                    help="12 keeps FKW's own racer tables; more moves them to MKW100's shadow window")
    args = ap.parse_args()

    payload = args.payload.read_bytes()
    entry, end = struct.unpack(">II", payload[:8])
    if not (PAYLOAD_BASE <= entry < end) or end - PAYLOAD_BASE != len(payload):
        print(f"payload header 0x{entry:08X}/0x{end:08X} does not match a {len(payload):#x}-byte image at "
              f"0x{PAYLOAD_BASE:08X}", file=sys.stderr)
        return 1

    sections = section_map(args.dol.read_bytes(), args.rel.read_bytes(), int(args.rel_load, 0))
    # loadCodes reads nothing but its own image and the setting bytes; any other read is reported.
    mem = Memory()
    mem.add_image(PAYLOAD_BASE, payload)

    choices = {}
    for c in args.choice:
        k, v = c.rsplit("=", 1)
        choices[k] = int(v)
    settings = default_settings(args.xml, choices)
    for a in range(SETTINGS_START, SETTINGS_END):
        mem.overlay[a] = settings.get(a, 0)

    # memcpy as FKW links it (directWriteArray); the address is the base game's memcpy.
    natives = {0x80005F34: "memcpy"}
    cpu = Cpu(mem, natives)
    cpu.run(entry)

    if mem.unmapped_reads:
        print("warning: reads from memory outside the base images: " +
              ", ".join(f"0x{a:08X}" for a in sorted(mem.unmapped_reads)[:16]), file=sys.stderr)

    code = bytearray(payload)
    commands: list[bytes] = []
    report = []
    payload_end = PAYLOAD_BASE + len(payload)
    in_payload = lambda a: PAYLOAD_BASE <= a < payload_end

    # Code the payload only carries to copy into base-game code (WiiLink's per-region text arrays)
    # is position-dependent: it never runs where it sits, but the translator's function discovery
    # would find blocks in it and try to translate them. Blank each such memcpy source, and the
    # same-sized blocks next to it with the same instruction shapes (the other regions' copies).
    def shape(a: int, n: int) -> list[int]:
        return [struct.unpack_from(">I", payload, a - PAYLOAD_BASE + k)[0] >> 26 for k in range(0, n - 3, 4)]

    def same_shape(x: list[int], y: list[int]) -> bool:
        return len(x) == len(y) and sum(p == q for p, q in zip(x, y)) >= 0.75 * len(x)

    blanked: list[tuple[int, int]] = []
    for dst, src, n in cpu.copies:
        sec = find_section(sections, dst)
        if not (sec and sec["Executable"] and in_payload(src) and in_payload(src + n - 1) and n >= 8):
            continue
        ref = shape(src, n)
        blocks = [src]
        # Only long blocks get siblings: a 2-4 instruction snippet's shape matches real code too.
        for step in ((-n, n) if n >= 0x40 else ()):
            a = src + step
            while in_payload(a) and in_payload(a + n - 1) and same_shape(shape(a, n), ref):
                blocks.append(a)
                a += step
        for a in blocks:
            code[a - PAYLOAD_BASE:a - PAYLOAD_BASE + n] = bytes(n)
            blanked.append((a, n))

    # `bl over; .string "..."; over: mflr rN` takes the address of inline data. A static
    # recompiler treats the bl as a call whose return address is data, and crashes returning there
    # (FKW's lap counter pane names). Copy the data into the blanked space and rewrite the bl slot
    # and the first data words as `lis rN, hi; addi rN, rN, lo; b over+4`: rN gets the same
    # address of the same bytes and control flow is plain. (LR is not set; every site calls
    # something right after, which sets it anyway.)
    free = [[a, a + n] for a, n in sorted(blanked) if n >= 0x40]  # dead space, by address

    def alloc(size: int, what: str) -> int:
        for block in free:
            if block[0] + size <= block[1]:
                block[0] += (size + 3) & ~3
                return block[0] - ((size + 3) & ~3)
        raise RuntimeError(f"no blanked space left for {what}")

    tricks = []

    def word(a: int) -> int:
        return struct.unpack_from(">I", code, a - PAYLOAD_BASE)[0]

    def put(a: int, w: int) -> None:
        struct.pack_into(">I", code, a - PAYLOAD_BASE, w & MASK32)

    for a in range(PAYLOAD_BASE + 8, payload_end, 4):
        i = word(a)
        if i >> 26 != 18 or (i & 3) != 1:
            continue
        li = i & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        t = a + li
        gap = t - (a + 4)
        if not (in_payload(t) and 8 <= gap <= 0x400 and (word(t) & 0xFC1FFFFF) == 0x7C0802A6):
            continue
        rn = (word(t) >> 21) & 31
        dst = alloc(gap, f"the inline data at 0x{a + 4:08X}")
        code[dst - PAYLOAD_BASE:dst - PAYLOAD_BASE + gap] = code[a + 4 - PAYLOAD_BASE:t - PAYLOAD_BASE]
        hi = ((dst + 0x8000) >> 16) & 0xFFFF
        lo = dst & 0xFFFF
        put(a, (15 << 26) | (rn << 21) | hi)                  # lis rN, hi
        put(a + 4, (14 << 26) | (rn << 21) | (rn << 16) | lo)  # addi rN, rN, lo
        put(a + 8, (18 << 26) | ((t + 4 - (a + 8)) & 0x03FFFFFC))  # b over+4
        tricks.append((a, dst, gap))

    # Past 12 racers: the roulette amounts, u8 ItemAmounts[2][12] at 0x800001B8 (low memory; rows
    # 12 bytes apart), are indexed by player id with no bound in the roulette hooks, so racers 12+
    # write over FKW's other low-memory globals and then the OS's. Move the table to MKW100's
    # shadow window with rows 0x100 apart (255 racers). Every word is checked first, so another
    # FKW build stops here instead of being patched blindly.
    if args.max_racers > 12:
        rows = AMOUNTS_SHADOW
        hi = rows >> 16
        sites = [
            # (address, original word, new word)
            (0x808E195C, 0x3D608000, 0x3D600000 | hi), (0x808E1960, 0x616B01C4, 0x616B0100),  # amounts[1]
            (0x808E1A20, 0x3C808000, 0x3C800000 | hi), (0x808E1A24, 0x608401B8, 0x60840000),  # Update1
            (0x808E1A34, 0x88A4000C, 0x88A40100), (0x808E1A3C, 0x9864000C, 0x98640100),
            (0x808E1A54, 0x3C808000, 0x3C800000 | hi), (0x808E1A58, 0x608401B8, 0x60840000),  # Update2
            (0x808E1A68, 0x88A4000C, 0x88A40100), (0x808E1A70, 0x9864000C, 0x98640100),
            (0x808E1A7C, 0x3CA08000, 0x3CA00000 | hi), (0x808E1A80, 0x60A501B8, 0x60A50000),  # Update3
            (0x808E1A94, 0x3CA08000, 0x3CA00000 | hi), (0x808E1A98, 0x60A501C4, 0x60A50100),  # Update4
            (0x808E205C, 0x3D208000, 0x3D200000 | hi), (0x808E2060, 0x392901B8, 0x39290000),  # AmountRandomizer
            (0x808E2068, 0x890A000C, 0x890A0100), (0x808E2070, 0x986A000C, 0x986A0100),
            (0x808E22DC, 0x3D208000, 0x3D200000 | hi), (0x808E22E0, 0x392901B8, 0x39290000),  # (old variant)
            (0x808E22E8, 0x890A000C, 0x890A0100), (0x808E22F0, 0x986A000C, 0x986A0100),
        ]
        for a, old, new in sites:
            if word(a) != old:
                raise RuntimeError(f"ItemAmounts site 0x{a:08X} holds 0x{word(a):08X}, expected 0x{old:08X}: "
                                   "not the FKW build these sites were measured on")
            put(a, new)
        print(f"moved ItemAmounts[2][12] (0x800001B8) to 0x{rows:08X}, rows 0x100 apart ({len(sites)} words)")

    # Addresses only reached indirectly must be registered as functions, but the translator finds
    # module entry points from relocations and prologues, and this module has no relocations:
    #  - switch jump tables (a run of words next to a bctr, all pointing near it): case labels;
    #  - payload addresses loadCodes stores into the game (AllItemsCanLand's +8/+16/+24 entries
    #    have no prologue).
    # Each gets a module-relative Branch command at a dead word of the blanked space; discovery
    # registers every Branch target, and the written `b` there is never executed.
    seeds: set[int] = set()
    for a in range(PAYLOAD_BASE, payload_end, 4):
        if word(a) != 0x4E800420:  # bctr
            continue
        near = lambda v: a - 0x400 <= v < a + 0x400 and not v & 3
        o = PAYLOAD_BASE
        while o < payload_end:
            run = o
            while run < payload_end and near(word(run)):
                run += 4
            if run - o >= 8:
                seeds.update(word(x) for x in range(o, run, 4))
            o = run + 4 if run == o else run
    for w in sorted({a & ~3 for a in mem.writes}):
        if all(w + k in mem.writes for k in range(4)) and not in_payload(w):
            v = mem.read(w, 4)
            sec = find_section(sections, w)
            if in_payload(v) and not (sec and sec["Executable"]):
                seeds.add(v)
    for target in sorted(seeds):
        at = alloc(4, "function seeds")
        put(at, (18 << 26) | ((target - at) & 0x03FFFFFC))
        commands.append(struct.pack(">II", (64 << 24) | (at - PAYLOAD_BASE), target))
    print(f"seeded {len(seeds)} indirect-only entry point(s): " + ", ".join(f"0x{s:08X}" for s in sorted(seeds)))

    written = sorted(mem.writes)
    dropped: list[int] = []
    words: dict[int, set[int]] = {}
    for a in written:
        if in_payload(a):
            code[a - PAYLOAD_BASE] = mem.writes[a]
            continue
        if SETTINGS_START <= a < SETTINGS_END:
            continue
        if INIT_SCRATCH_START <= a < INIT_SCRATCH_END:
            # .init's TRK vector area (FKW's bootstrap + settings live there): no translated
            # function covers it, so the translator cannot overlay it. Only an online-only word
            # (Anti_Wiimmfi_3) lands here.
            dropped.append(a)
            continue
        words.setdefault(a & ~3, set()).add(a & 3)

    counts = {"branch": 0, "pointer": 0, "write32": 0, "write16": 0, "write8": 0, "unknown-section": 0}
    for w in sorted(words):
        sec = find_section(sections, w)
        if sec is None:
            counts["unknown-section"] += 1
        lanes = words[w]
        executable = bool(sec and sec["Executable"])
        if lanes == {0, 1, 2, 3}:
            v = mem.read(w, 4)
            kind = "write32"
            if executable and (v >> 26) == 18 and not (v & 2):
                li = v & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                target = (w + li) & MASK32
                if in_payload(target):
                    kind = "branch"
                    commands.append(cmd_abs(65 if v & 1 else 64, w, target))
            elif not executable and in_payload(v):
                kind = "pointer"
                commands.append(cmd_abs(1, w, v))
            if kind == "write32":
                commands.append(cmd_abs(32, w, v))
            counts[kind] += 1
            report.append({"addr": f"0x{w:08X}", "kind": kind, "value": f"0x{v:08X}",
                           "section": sec["Name"] if sec else None})
            continue
        # partial word: emit halfwords where both bytes of an aligned half changed, else bytes
        for half in (0, 2):
            if {half, half + 1} <= lanes:
                v = mem.read(w + half, 2)
                commands.append(cmd_abs(33, w + half, v))
                counts["write16"] += 1
                report.append({"addr": f"0x{w + half:08X}", "kind": "write16", "value": f"0x{v:04X}",
                               "section": sec["Name"] if sec else None})
            else:
                for b in (half, half + 1):
                    if b in lanes:
                        v = mem.read8(w + b)
                        commands.append(cmd_abs(34, w + b, v))
                        counts["write8"] += 1
                        report.append({"addr": f"0x{w + b:08X}", "kind": "write8", "value": f"0x{v:02X}",
                                       "section": sec["Name"] if sec else None})

    chunk = kamek_chunk(bytes(code), commands)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(chunk)
    print(f"loadCodes ran {cpu.steps} instructions; {len(commands)} patch commands {counts}")
    for a, dst, n in tricks:
        print(f"rewrote bl-over-data at 0x{a:08X}: {n} inline bytes moved to 0x{dst:08X}")
    for a, n in sorted(blanked):
        print(f"blanked 0x{a:08X}..0x{a + n:08X}: code the payload copies into the base game, not run in place")
    if dropped:
        print(f"dropped {len(dropped)} byte(s) written into .init's TRK area: 0x{dropped[0] & ~3:08X}..")
    print(f"payload sha1 {hashlib.sha1(payload).hexdigest()}  code sha1 {hashlib.sha1(bytes(code)).hexdigest()}")
    print(f"wrote {args.out} ({len(chunk)} bytes)")
    if args.report:
        args.report.write_text(json.dumps({
            "payload_sha1": hashlib.sha1(payload).hexdigest(),
            "code_sha1": hashlib.sha1(bytes(code)).hexdigest(),
            "settings": {f"0x{a:08X}": v for a, v in sorted(settings.items())},
            "counts": counts,
            "patches": report,
        }, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
