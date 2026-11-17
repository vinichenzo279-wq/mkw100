using System;
using System.Collections.Generic;
using System.IO;
using Translator.Core.Disassembly;
using Translator.Core.Ir;
using Translator.Core.Lifting;
using Xunit;

namespace Translator.Tests;

/// <summary>mkw100: listed deref sites take their effective address from the slot address helper.</summary>
// The site lists are process-wide: the suites that swap them must not run at the same time.
[Collection("mkw100 site lists")]
public class PlayerSlotDerefSitesTests
{
    // Far from any address other suites assemble at: Current is process-wide.
    private const uint Site = 0x80F10000;

    private static PpcInstruction DForm(uint address, int op, string mnemonic, int rd, int ra, short displacement) =>
        PpcInstruction.Synthetic(address, ((uint)op << 26) | ((uint)rd << 21) | ((uint)ra << 16) | (ushort)displacement,
            mnemonic, new PpcOperand[]
            {
                new PpcRegisterOperand($"r{rd}", rd), new PpcDisplacementOperand(displacement, $"r{ra}", ra)
            });

    private static IReadOnlyList<IrInstruction> Lift(PpcInstruction ins, IEnumerable<uint> derefs)
    {
        var previous = PlayerSlotStrideSites.Current;
        PlayerSlotStrideSites.Current = new PlayerSlotStrideSites(Array.Empty<uint>(), derefs);
        try
        {
            var blr = PpcInstruction.Synthetic(ins.Address + 4, 0x4E800020, "blr", Array.Empty<PpcOperand>());
            return new PpcLifter().Lift(new List<PpcInstruction> { ins, blr })[0].Ir;
        }
        finally { PlayerSlotStrideSites.Current = previous; }
    }

    [Fact]
    public void ListedStoreAddressesThroughHelper()
    {
        var ir = Lift(DForm(Site, 38, "stb", 6, 12, 0x1D9), new[] { Site });
        Assert.Equal(2, ir.Count);
        var call = Assert.IsType<IrCall>(ir[0]);
        Assert.Equal(PlayerSlotStrideSites.AddressHelper, call.Target);
        Assert.Equal("r12", call.Arguments[0].RegisterName);
        Assert.Equal(0x1D9, call.Arguments[1].Constant);
        var store = Assert.IsType<IrStore>(ir[1]);
        Assert.Equal(new IrAddress(call.Destination, 0), store.Address);
        Assert.Equal("r6", store.Source.RegisterName);
    }

    [Fact]
    public void ListedLoadAddressesThroughHelper()
    {
        var ir = Lift(DForm(Site, 40, "lhz", 0, 4, 0x1D2), new[] { Site });
        var call = Assert.IsType<IrCall>(ir[0]);
        var load = Assert.IsType<IrLoad>(ir[1]);
        Assert.Equal("r0", load.Destination);
        Assert.Equal(new IrAddress(call.Destination, 0), load.Address);
    }

    [Fact]
    public void UnlistedStoreIsUnchanged()
    {
        var store = Assert.IsType<IrStore>(Assert.Single(Lift(DForm(Site + 0x100, 38, "stb", 6, 12, 0x1D9), new[] { Site })));
        Assert.Equal(new IrAddress("r12", 0x1D9), store.Address);
    }

    [Fact]
    public void UpdateFormIsRejected()
    {
        Assert.ThrowsAny<Exception>(() => Lift(DForm(Site, 39, "stbu", 6, 12, 0x1D9), new[] { Site }));
    }

    [Fact]
    public void LoadsDerefLines()
    {
        var path = Path.GetTempFileName();
        try
        {
            File.WriteAllLines(path, new[] { "0x8052FD04  # scenario", "0x8052FD1C deref  # of 0x8052FD04" });
            var sites = PlayerSlotStrideSites.Load(path);
            Assert.Equal(1, sites.Count);
            Assert.Equal(1, sites.DerefCount);
            Assert.True(sites.Contains(0x8052FD04));
            Assert.False(sites.Contains(0x8052FD1C));
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void LoadRejectsUnknownKind()
    {
        var path = Path.GetTempFileName();
        try
        {
            File.WriteAllLines(path, new[] { "0x8052FD1C step" });
            Assert.Throws<FormatException>(() => PlayerSlotStrideSites.Load(path));
        }
        finally { File.Delete(path); }
    }
}
