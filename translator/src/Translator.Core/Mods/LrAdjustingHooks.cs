using System.Buffers.Binary;

namespace Translator.Core.Mods;

/// <summary>
/// mkw100: module hook functions that return to a computed place in the game function that entered
/// them: <c>mflr rX; addi rX, rX, K; mtlr rX; ... blr</c> (Insane Kart Wii's minimap and race-count
/// hooks). On a Wii the blr lands K bytes past the return point of the game function's last call.
/// A recompiled module function returns natively, so the game code around such a hook
/// (FunctionTranslator) reads the LR the hook left and jumps to that place in its own function.
/// </summary>
public static class LrAdjustingHooks
{
    /// <summary>Module function start -> the K offsets it adds to LR. Empty for the base game.</summary>
    public static IReadOnlyDictionary<uint, IReadOnlyList<int>> Current { get; set; } =
        new Dictionary<uint, IReadOnlyList<int>>();

    public static Dictionary<uint, IReadOnlyList<int>> Scan(byte[] code, uint moduleGuestBase)
    {
        uint Word(int offset) => BinaryPrimitives.ReadUInt32BigEndian(code.AsSpan(offset, 4));
        var found = new Dictionary<uint, List<int>>();
        for (var o = 8; o + 4 <= code.Length; o += 4)
        {
            var mtlr = Word(o);
            if ((mtlr & 0xFC1FFFFF) != 0x7C0803A6) continue;
            var rx = (int)((mtlr >> 21) & 31);
            for (var k = 1; k <= 3 && o - 4 * k >= 0; k++)
            {
                var addi = Word(o - 4 * k);
                if (addi >> 26 != 14 || ((addi >> 21) & 31) != rx || ((addi >> 16) & 31) != rx) continue;
                var offset = (int)(short)(addi & 0xFFFF);
                var hasMflr = false;
                for (var j = 1; j <= 4 && o - 4 * k - 4 * j >= 0; j++)
                {
                    var mflr = Word(o - 4 * k - 4 * j);
                    if ((mflr & 0xFC1FFFFF) == 0x7C0802A6 && ((mflr >> 21) & 31) == rx) hasMflr = true;
                }
                if (hasMflr && offset != 0)
                {
                    var start = o;
                    while (start >= 4 && Word(start - 4) != 0x4E800020 && o - start < 0x400) start -= 4;
                    var key = moduleGuestBase + (uint)start;
                    if (!found.TryGetValue(key, out var list)) found[key] = list = new List<int>();
                    if (!list.Contains(offset)) list.Add(offset);
                }
                break;
            }
        }
        return found.ToDictionary(f => f.Key, f => (IReadOnlyList<int>)f.Value);
    }
}
