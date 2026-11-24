using System.Buffers.Binary;
using Translator.Core.Parsing.Kamek;

namespace Translator.Core.Mods;

/// <summary>
/// mkw100: game instructions a Kamek module rewrites while running (Insane Kart Wii's
/// KamekRuntimeWrite: Write32/Write16/Write8 store a value, flush the data cache and invalidate the
/// instruction cache; Branch encodes a b/bl and calls Write32). A recompiled game can't take code
/// written at run time, so each such address is translated with every word the module can write
/// there and picks one at run time from guest memory (FunctionTranslator, MKW100_RuntimeWord).
///
/// The scan finds the helpers by their shape and every call to them whose address and value are
/// constants: Kamek's address auto-map makes the patched game address an absolute relocation, and
/// the values are literal instructions or branch targets. Calls with an unknown address or value
/// are counted and skipped (the runtime then keeps refusing those writes).
/// </summary>
public static class RuntimePatchSites
{
    /// <summary>The sites in effect for the current translation, by word address. Empty for the base game.</summary>
    public static IReadOnlyDictionary<uint, IReadOnlyList<uint>> Current { get; set; } =
        new Dictionary<uint, IReadOnlyList<uint>>();

    private const uint DcFlushRange = 0x801A162C;

    public sealed record ScanResult(
        IReadOnlyDictionary<uint, IReadOnlyList<uint>> Sites,
        IReadOnlyList<uint> Helpers,
        int Calls,
        int UnknownCalls,
        IReadOnlySet<uint>? ImmediateSites = null);

    /// <summary>
    /// Sites the module writes a computed value to, where the game's instruction is one whose low
    /// half is an immediate (li/addi, cmpwi/cmplwi, ori): translated with the immediate read from
    /// guest memory at run time (IKW's battle time, coin and balloon counts). Empty for the base game.
    /// </summary>
    public static IReadOnlySet<uint> Immediates { get; set; } = new HashSet<uint>();

    /// <summary>Primary opcodes whose low 16 bits are an immediate the translator can read at run time.</summary>
    public static bool IsImmediateForm(uint word) => (word >> 26) is 14 or 10 or 11 or 24;

    private enum HelperKind { Write32, Write16, Write8, Branch, CondWrite32, CondWrite16, CondWrite8 }

    /// <param name="baseWord">The game's word at an address (for 16- and 8-bit writes, merged into
    /// it); null when unknown, in which case only 32-bit and branch variants are recorded.</param>
    public static ScanResult Scan(KamekChunk chunk, uint moduleGuestBase, Func<uint, uint?>? baseWord = null)
    {
        var code = chunk.CodeBlob;
        uint Word(int offset) => BinaryPrimitives.ReadUInt32BigEndian(code.AsSpan(offset, 4));

        // Relative relocation commands by the module offset they patch.
        var rel24 = new Dictionary<int, uint>();
        var lo16 = new Dictionary<int, uint>();
        var ha16 = new Dictionary<int, uint>();
        foreach (var command in chunk.Commands)
        {
            if (!command.AddressIsRelative || command.Arguments.Count == 0) continue;
            var target = KamekAddress.Resolve(command.Arguments[0], moduleGuestBase);
            switch (command.Id)
            {
                case KamekCommandId.Rel24: rel24[(int)command.Address] = target; break;
                case KamekCommandId.Addr16Lo: lo16[(int)command.Address & ~3] = target; break;
                case KamekCommandId.Addr16Ha:
                case KamekCommandId.Addr16Hi: ha16[(int)command.Address & ~3] = target; break;
            }
        }

        // bl, or b (a tail call into the helper, as IKW's boot hooks make).
        uint? CallTarget(int offset)
        {
            var w = Word(offset);
            if (w >> 26 != 18 || (w & 2) != 0) return null;
            if (rel24.TryGetValue(offset, out var t)) return t;
            var d = (int)(w & 0x03FFFFFC);
            if ((d & 0x02000000) != 0) d -= 0x04000000;
            return moduleGuestBase + (uint)(offset + d);
        }

        int FunctionStart(int offset)
        {
            for (var o = offset - 4; o >= 0 && offset - o <= 0x4000; o -= 4)
                if (Word(o) == 0x4E800020) return o + 4;
            return Math.Max(0, offset - 0x4000) & ~3;
        }

        // The store helpers: stX r4, 0(r3), then a bl DCFlushRange within a few words, and an icbi.
        var helpers = new Dictionary<uint, HelperKind>();
        for (var o = 0; o + 4 <= code.Length; o += 4)
        {
            var w = Word(o);
            HelperKind? kind = w switch
            {
                0x90830000 => HelperKind.Write32,
                0xB0830000 => HelperKind.Write16,
                0x98830000 => HelperKind.Write8,
                _ => null
            };
            if (kind is null) continue;
            var flush = false;
            var icbi = false;
            for (var k = 1; k <= 12 && o + 4 * k + 4 <= code.Length; k++)
            {
                if (CallTarget(o + 4 * k) == DcFlushRange) flush = true;
                if ((Word(o + 4 * k) & 0xFC0007FE) == 0x7C0007AC) icbi = true;  // icbi
            }
            if (flush && icbi) helpers[moduleGuestBase + (uint)FunctionStart(o)] = kind.Value;
        }
        var write32 = helpers.Where(h => h.Value == HelperKind.Write32).Select(h => h.Key).ToHashSet();
        // The branch helper encodes 0x48000000 | delta (oris rX, rY, 0x4800) and calls Write32.
        for (var o = 0; o + 4 <= code.Length; o += 4)
        {
            var w = Word(o);
            if (w >> 26 != 25 || (w & 0xFFFF) != 0x4800) continue;
            for (var k = 1; k <= 6 && o + 4 * k + 4 <= code.Length; k++)
            {
                if (CallTarget(o + 4 * k) is { } t && write32.Contains(t))
                {
                    helpers.TryAdd(moduleGuestBase + (uint)FunctionStart(o), HelperKind.Branch);
                    break;
                }
            }
        }

        // The conditional helpers: compare the old value with r4, then mr r4, r5 and call a store helper.
        for (var o = 4; o + 4 <= code.Length; o += 4)
        {
            if (Word(o - 4) != 0x7CA42B78 || CallTarget(o) is not { } store || !helpers.TryGetValue(store, out var storeKind))
                continue;
            HelperKind? cond = storeKind switch
            {
                HelperKind.Write32 => HelperKind.CondWrite32,
                HelperKind.Write16 => HelperKind.CondWrite16,
                HelperKind.Write8 => HelperKind.CondWrite8,
                _ => null
            };
            if (cond is not null) helpers.TryAdd(moduleGuestBase + (uint)FunctionStart(o), cond.Value);
        }

        var sites = new Dictionary<uint, HashSet<uint>>();
        var calls = 0;
        var unknown = 0;
        var immediates = new HashSet<uint>();
        var helperStarts = helpers.Keys.ToHashSet();
        for (var o = 0; o + 4 <= code.Length; o += 4)
        {
            if (CallTarget(o) is not { } target || !helpers.TryGetValue(target, out var kind)) continue;
            // Calls inside a helper (the branch helper's Write32) are not patches of their own.
            var caller = moduleGuestBase + (uint)FunctionStart(o);
            if (helperStarts.Contains(caller)) continue;
            calls++;
            var regs = ConstantsAt(code, FunctionStart(o), o, lo16, ha16, CallTarget);
            var valueRegister = kind is HelperKind.CondWrite32 or HelperKind.CondWrite16 or HelperKind.CondWrite8 ? 5 : 4;
            if (!regs.TryGetValue(3, out var address) || address < 0x80000000 || address >= moduleGuestBase)
            {
                unknown++;
                continue;
            }
            if (!regs.TryGetValue(valueRegister, out var value))
            {
                // A computed 32-bit value at an instruction with an immediate: the immediate is the
                // variable part (kmRuntimeWrite32A(addr, 0x38000000 + time)).
                // Without the game's words (the base build only needs the addresses) every such
                // address counts.
                if (kind is HelperKind.Write32 or HelperKind.CondWrite32 && (address & 3) == 0 &&
                    (baseWord is null || baseWord(address) is { } original && IsImmediateForm(original)))
                {
                    immediates.Add(address);
                }
                else
                {
                    unknown++;
                }
                continue;
            }
            var wordAddress = address & ~3u;
            uint? variant = kind switch
            {
                HelperKind.Write32 or HelperKind.CondWrite32 => value,
                HelperKind.Branch => EncodeBranch(address, value, regs.TryGetValue(5, out var link) && link != 0),
                HelperKind.Write16 or HelperKind.CondWrite16 => baseWord?.Invoke(wordAddress) is { } w16
                    ? (address & 2) == 0 ? (w16 & 0x0000FFFF) | ((value & 0xFFFF) << 16) : (w16 & 0xFFFF0000) | (value & 0xFFFF)
                    : null,
                HelperKind.Write8 or HelperKind.CondWrite8 => baseWord?.Invoke(wordAddress) is { } w8
                    ? (w8 & ~(0xFFu << (int)(8 * (3 - (address & 3))))) | ((value & 0xFF) << (int)(8 * (3 - (address & 3))))
                    : null,
                _ => null
            };
            if (variant is null)
            {
                unknown++;
                continue;
            }
            if (!sites.TryGetValue(wordAddress, out var set)) sites[wordAddress] = set = new HashSet<uint>();
            set.Add(variant.Value);
        }

        return new ScanResult(
            sites.ToDictionary(s => s.Key, s => (IReadOnlyList<uint>)s.Value.OrderBy(v => v).ToArray()),
            helpers.Keys.OrderBy(h => h).ToArray(),
            calls,
            unknown,
            immediates);
    }

    private static uint? EncodeBranch(uint from, uint to, bool link)
    {
        var delta = (long)to - from;
        if ((delta & 3) != 0 || delta < -0x02000000 || delta > 0x01FFFFFC) return null;
        return 0x48000000u | ((uint)delta & 0x03FFFFFCu) | (link ? 1u : 0u);
    }

    /// <summary>
    /// Constant register values at <paramref name="end"/>, simulating the function linearly from
    /// <paramref name="start"/> (branches are ignored: values set before a region check still
    /// count). li/lis/addi/addis/ori/oris/mr and Kamek's Ha16/Lo16 relocations give constants; a call
    /// clobbers r0 and r3..r12; any other write of a register forgets it.
    /// </summary>
    private static Dictionary<int, uint> ConstantsAt(byte[] code, int start, int end,
        Dictionary<int, uint> lo16, Dictionary<int, uint> ha16, Func<int, uint?> callTarget)
    {
        var regs = new Dictionary<int, uint>();
        for (var o = start; o < end; o += 4)
        {
            var w = BinaryPrimitives.ReadUInt32BigEndian(code.AsSpan(o, 4));
            var op = (int)(w >> 26);
            var rd = (int)((w >> 21) & 31);
            var ra = (int)((w >> 16) & 31);
            var imm = w & 0xFFFF;
            var simm = (int)(short)imm;
            switch (op)
            {
                case 14: // addi / li
                    if (lo16.TryGetValue(o, out var lo)) regs[rd] = lo;
                    else if (ra == 0) regs[rd] = (uint)simm;
                    else if (regs.TryGetValue(ra, out var b14)) regs[rd] = unchecked(b14 + (uint)simm);
                    else regs.Remove(rd);
                    continue;
                case 15: // addis / lis
                    if (ha16.TryGetValue(o, out var hi)) regs[rd] = hi;  // its Lo16 pair sets the full address
                    else if (ra == 0) regs[rd] = imm << 16;
                    else if (regs.TryGetValue(ra, out var b15)) regs[rd] = unchecked(b15 + (imm << 16));
                    else regs.Remove(rd);
                    continue;
                case 24: // ori rA, rS
                    if (regs.TryGetValue(rd, out var s24)) regs[ra] = s24 | imm; else regs.Remove(ra);
                    continue;
                case 25: // oris
                    if (regs.TryGetValue(rd, out var s25)) regs[ra] = s25 | (imm << 16); else regs.Remove(ra);
                    continue;
                case 18:
                    if ((w & 1) != 0 && callTarget(o) is not null)
                    {
                        regs.Remove(0);
                        for (var r = 3; r <= 12; r++) regs.Remove(r);
                    }
                    continue;
                case 31:
                {
                    var xo = (int)((w >> 1) & 0x3FF);
                    if (xo == 444 && rd == (int)((w >> 11) & 31))  // mr rA, rS
                    {
                        if (regs.TryGetValue(rd, out var s31)) regs[ra] = s31; else regs.Remove(ra);
                        continue;
                    }
                    // Logical and shift forms write rA; stores, compares, cache ops and sync write nothing.
                    if (xo is 28 or 60 or 124 or 284 or 316 or 412 or 444 or 476 or 954 or 922 or 26 or 24 or 536 or 792 or 824)
                        regs.Remove(ra);
                    else if (xo is not (0 or 32 or 151 or 183 or 215 or 247 or 407 or 439 or 662 or 918 or 663 or 695
                             or 727 or 759 or 983 or 54 or 86 or 470 or 982 or 278 or 246 or 1014 or 598 or 854 or 150 or 467 or 146))
                        regs.Remove(rd);
                    continue;
                }
                case 20: case 21: case 23: case 26: case 27: case 28: case 29:
                    regs.Remove(ra);  // rlw*, xori, xoris, andi., andis.
                    continue;
                case 32: case 33: case 34: case 35: case 40: case 41: case 42: case 43: case 46:
                case 7: case 8: case 12: case 13:
                    regs.Remove(rd);  // loads, mulli, subfic, addic
                    continue;
            }
        }
        return regs;
    }
}
