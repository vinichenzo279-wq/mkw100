using System.Globalization;

namespace Translator.Core.Lifting;

/// <summary>
/// mkw100: a translate-mod profile whose Kamek module combines other profiles' modules
/// (Retro Rewind + Item Fever's items, docs/mkw100/rr-item-fever.md). It takes the `profile=`
/// sites of each part, with the part's module addresses moved to where the part sits in the
/// combined module.
///
/// File `profile-merges.txt` beside array-sites.txt, one part per line:
/// `PROFILE PART 0xDELTA  # comment`. Every site value inside the Kamek overlay
/// (0x81800000..0x81A00000: addresses, index-table addresses) moves by DELTA; other values
/// (shadow tables, offsets, bounds) stay.
/// </summary>
public static class ProfileMerges
{
    public const uint OverlayStart = 0x81800000;
    public const uint OverlayEnd = 0x81A00000;

    public sealed record Part(string Profile, uint Delta);

    public static IReadOnlyList<Part> Load(string path, string profile)
    {
        var parts = new List<Part>();
        if (!File.Exists(path))
            return parts;
        foreach (var (raw, index) in File.ReadLines(path).Select((line, i) => (line, i)))
        {
            var text = raw.Split('#', 2)[0].Trim();
            if (text.Length == 0)
                continue;
            var fields = text.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
            if (fields.Length != 3 || !fields[2].StartsWith("0x", StringComparison.OrdinalIgnoreCase) ||
                !uint.TryParse(fields[2].AsSpan(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var delta))
                throw new FormatException($"{path}:{index + 1}: expected `PROFILE PART 0xDELTA`, got '{text}'");
            if (string.Equals(fields[0], profile, StringComparison.OrdinalIgnoreCase))
                parts.Add(new Part(fields[1], delta));
        }
        return parts;
    }

    public static uint Move(uint value, uint delta) =>
        value >= OverlayStart && value < OverlayEnd ? checked(value + delta) : value;

    /// <summary>The array sites for <paramref name="profile"/> made of <paramref name="parts"/>.</summary>
    public static ArrayOverflowSites Merge(ArrayOverflowSites all, string profile, IReadOnlyList<Part> parts)
    {
        var sites = all.All.Where(site => site.Profile is null).ToList();
        foreach (var part in parts)
        {
            foreach (var site in all.All.Where(site => string.Equals(site.Profile, part.Profile, StringComparison.OrdinalIgnoreCase)))
            {
                sites.Add(site with
                {
                    Address = Move(site.Address, part.Delta),
                    Offset = unchecked((int)Move(unchecked((uint)site.Offset), part.Delta)),
                    Profile = profile
                });
            }
        }
        return all.With(sites);
    }

    /// <summary>The player-slot sites for <paramref name="profile"/> made of <paramref name="parts"/>.</summary>
    public static PlayerSlotStrideSites MergeSlots(string path, IReadOnlyList<Part> parts)
    {
        var common = PlayerSlotStrideSites.Load(path, null);
        var sites = common.Sites.ToList();
        var derefs = common.Derefs.ToList();
        foreach (var part in parts)
        {
            var withPart = PlayerSlotStrideSites.Load(path, part.Profile);
            sites.AddRange(withPart.Sites.Except(common.Sites).Select(a => Move(a, part.Delta)));
            derefs.AddRange(withPart.Derefs.Except(common.Derefs).Select(a => Move(a, part.Delta)));
        }
        return new PlayerSlotStrideSites(sites.Distinct(), derefs.Distinct());
    }
}
