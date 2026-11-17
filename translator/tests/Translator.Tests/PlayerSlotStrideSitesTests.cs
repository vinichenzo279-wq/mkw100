using System.Collections.Generic;
using System.IO;
using System.Linq;
using Translator.Core.Disassembly;
using Translator.Core.Ir;
using Translator.Core.Lifting;
using Xunit;

namespace Translator.Tests;

/// <summary>mkw100: listed `mulli rD, rA, 0xF0` sites lift to the overflow slot helper.</summary>
// The site lists are process-wide: the suites that swap them must not run at the same time.
[Collection("mkw100 site lists")]
public class PlayerSlotStrideSitesTests
{
    // Far from any address other suites assemble at: Current is process-wide.
    private const uint Site = 0x80F00000;

    private static IReadOnlyList<IrInstruction> LiftMulli(uint address, short multiplier)
    {
        var instructions = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(address, 0x1C040000u | (ushort)multiplier, "mulli", new PpcOperand[]
            {
                new PpcRegisterOperand("r0", 0), new PpcRegisterOperand("r4", 4), new PpcImmediateOperand(multiplier)
            }),
            PpcInstruction.Synthetic(address + 4, 0x4E800020, "blr", System.Array.Empty<PpcOperand>())
        };
        return new PpcLifter().Lift(instructions)[0].Ir;
    }

    private static T WithSites<T>(IEnumerable<uint> sites, System.Func<T> body)
    {
        var previous = PlayerSlotStrideSites.Current;
        PlayerSlotStrideSites.Current = new PlayerSlotStrideSites(sites);
        try { return body(); }
        finally { PlayerSlotStrideSites.Current = previous; }
    }

    [Fact]
    public void ListedSiteBecomesHelperCall()
    {
        var ir = WithSites(new[] { Site }, () => LiftMulli(Site, 0xF0));
        var call = Assert.IsType<IrCall>(Assert.Single(ir));
        Assert.Equal("r0", call.Destination);
        Assert.Equal(PlayerSlotStrideSites.Helper, call.Target);
        Assert.Equal("r4", Assert.Single(call.Arguments).RegisterName);
    }

    [Fact]
    public void UnlistedSiteStaysAMultiply()
    {
        var ir = WithSites(new[] { Site }, () => LiftMulli(Site + 0x100, 0xF0));
        var binary = Assert.IsType<IrBinary>(Assert.Single(ir));
        Assert.Equal("mul", binary.Op);
    }

    [Fact]
    public void ListedSiteWithOtherStrideIsRejected()
    {
        Assert.ThrowsAny<System.Exception>(() => WithSites(new[] { Site }, () => LiftMulli(Site, 0x54)));
    }

    [Fact]
    public void LoadsAddressesAndComments()
    {
        var path = Path.GetTempFileName();
        try
        {
            File.WriteAllLines(path, new[] { "# header", "", "0x8072626C  # race", "0X80531F18" });
            var sites = PlayerSlotStrideSites.Load(path);
            Assert.Equal(2, sites.Count);
            Assert.True(sites.Contains(0x8072626C));
            Assert.True(sites.Contains(0x80531F18));
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void LoadRejectsNonAddresses()
    {
        var path = Path.GetTempFileName();
        try
        {
            File.WriteAllLines(path, new[] { "8072626C" });
            Assert.Throws<FormatException>(() => PlayerSlotStrideSites.Load(path));
        }
        finally { File.Delete(path); }
    }
}
