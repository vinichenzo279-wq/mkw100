using System.IO;
using System.Linq;
using Translator.Core.Lifting;
using Xunit;

namespace Translator.Tests;

/// <summary>
/// mkw100: a profile whose Kamek module combines two packs takes each pack's profile sites, with
/// module addresses moved to where combine_pul.py put the pack (docs/mkw100/rr-item-fever.md).
/// </summary>
public class ProfileMergesTests
{
    private static string WriteTemp(string text)
    {
        var path = Path.GetTempFileName();
        File.WriteAllText(path, text);
        return path;
    }

    [Fact]
    public void LoadsOnlyTheRequestedProfile()
    {
        var path = WriteTemp("""
            # comment
            combo  first   0x0
            combo  second  0xC0000   # moved
            other  first   0x10000
            """);
        try
        {
            var parts = ProfileMerges.Load(path, "combo");
            Assert.Equal(new[] { ("first", 0u), ("second", 0xC0000u) }, parts.Select(p => (p.Profile, p.Delta)));
            Assert.Empty(ProfileMerges.Load(path, "missing"));
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void MissingFileMeansNoParts()
    {
        Assert.Empty(ProfileMerges.Load(Path.Combine(Path.GetTempPath(), "no-such-profile-merges.txt"), "combo"));
    }

    [Fact]
    public void MergeMovesOverlayValuesAndKeepsShadowsAndBaseSites()
    {
        var all = new ArrayOverflowSites(new[]
        {
            new ArrayOverflowSites.Site(0x80500000, 1, ArrayOverflowSites.SiteKind.Scale, 4, 0x10),
            new ArrayOverflowSites.Site(0x81816D50, 0, ArrayOverflowSites.SiteKind.Index, 1, unchecked((int)0x818A2D34),
                0x82D00000, "second"),
            new ArrayOverflowSites.Site(0x81837BC8, 10, ArrayOverflowSites.SiteKind.Scale, 56, 0, 0, "first"),
            new ArrayOverflowSites.Site(0x81840000, 0, ArrayOverflowSites.SiteKind.CmpImm, 128, 0, 0, "unrelated"),
        });
        var merged = ProfileMerges.Merge(all, "combo",
            new[] { new ProfileMerges.Part("first", 0), new ProfileMerges.Part("second", 0xC0000) });

        Assert.Equal(3, merged.Count);
        Assert.True(merged.TryGet(0x80500000, out var baseSite));
        Assert.Null(baseSite.Profile);
        Assert.True(merged.TryGet(0x81837BC8, out var first));
        Assert.Equal("combo", first.Profile);
        Assert.True(merged.TryGet(0x818D6D50, out var second));
        Assert.Equal(unchecked((int)0x81962D34), second.Offset);  // index table moved with the code
        Assert.Equal(0x82D00000u, second.Shadow);                   // shadow window stays
        Assert.Equal("combo", second.Profile);
        Assert.False(merged.TryGet(0x81840000, out _));             // another profile's site is left out
    }

    [Fact]
    public void MergeSlotsMovesEachPartsSites()
    {
        var path = WriteTemp("""
            0x80600000
            0x81801000  profile=first
            0x81802000 deref  profile=second
            0x81803000  profile=unrelated
            """);
        try
        {
            var merged = ProfileMerges.MergeSlots(path,
                new[] { new ProfileMerges.Part("first", 0), new ProfileMerges.Part("second", 0xC0000) });
            Assert.Equal(new[] { 0x80600000u, 0x81801000u }, merged.Sites.OrderBy(a => a));
            Assert.Equal(new[] { 0x818C2000u }, merged.Derefs);
        }
        finally { File.Delete(path); }
    }
}
