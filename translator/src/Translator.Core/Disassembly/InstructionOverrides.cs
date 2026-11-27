using System.Collections.Frozen;
using System.Globalization;

namespace Translator.Core.Disassembly;

/// <summary>
/// mkw100: guest instruction words replaced before decoding. Used where a 12-player layout is
/// baked into an immediate, such as an allocation size, a field displacement or a stack array's
/// address (projects/mkwii/mkw100/instruction-overrides.txt, named by the project's
/// `translation.instruction_overrides`). Branches cannot be overridden, so control flow is the
/// game's own. Process-wide like the Lifting site lists.
/// </summary>
public sealed class InstructionOverrides
{
    public static InstructionOverrides Current { get; set; } = new(FrozenDictionary<uint, uint>.Empty);

    private readonly FrozenDictionary<uint, uint> _words;

    public InstructionOverrides(IReadOnlyDictionary<uint, uint> words) => _words = words.ToFrozenDictionary();

    public int Count => _words.Count;

    /// <summary>The word to decode at <paramref name="address"/>: the override, else <paramref name="word"/>.</summary>
    public uint Apply(uint address, uint word) => _words.TryGetValue(address, out var replaced) ? replaced : word;

    /// <summary>
    /// One override per line: `0xADDRESS 0xORIGINAL 0xREPLACEMENT`; `#` starts a comment. The
    /// original word is checked against <paramref name="readWord"/> (the program image), so a list
    /// made for another binary fails loudly instead of patching the wrong instruction.
    /// </summary>
    public static InstructionOverrides Load(string path, Func<uint, uint?> readWord)
    {
        var words = new Dictionary<uint, uint>();
        foreach (var (raw, index) in File.ReadLines(path).Select((line, i) => (line, i)))
        {
            var text = raw.Split('#', 2)[0].Trim();
            if (text.Length == 0)
                continue;
            var fields = text.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
            FormatException Bad(string why) => new($"{path}:{index + 1}: {why}: '{text}'");
            if (fields.Length != 3)
                throw Bad("expected address, original word, replacement word");
            var address = Hex(fields[0]) ?? throw Bad("bad address");
            var original = Hex(fields[1]) ?? throw Bad("bad original word");
            var replacement = Hex(fields[2]) ?? throw Bad("bad replacement word");
            if (readWord(address) is { } actual && actual != original)
                throw Bad($"the word at 0x{address:X8} is 0x{actual:X8}, not 0x{original:X8}");
            if (IsBranch(original) || IsBranch(replacement))
                throw Bad("branches cannot be overridden");
            if (!words.TryAdd(address, replacement))
                throw Bad("address listed twice");
        }
        return new InstructionOverrides(words);
    }

    private static bool IsBranch(uint word) => (word >> 26) is 16 or 18 or 19 && ((word >> 26) != 19 || ((word >> 1) & 0x3FF) is 16 or 528);

    private static uint? Hex(string text) =>
        text.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
        && uint.TryParse(text.AsSpan(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var value)
            ? value
            : null;
}
