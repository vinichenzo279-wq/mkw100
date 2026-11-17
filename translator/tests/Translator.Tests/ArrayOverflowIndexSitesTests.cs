using System.Collections.Generic;
using System.IO;
using System.Linq;
using Translator.Core.Disassembly;
using Translator.Core.Ir;
using Translator.Core.Lifting;
using Xunit;

namespace Translator.Tests;

/// <summary>mkw100: index, indexb and indexadd sites (a pack's static [12] tables, pack_racer_tables.py).</summary>
// The site lists are process-wide: the suites that swap them must not run at the same time.
[Collection("mkw100 site lists")]
public class ArrayOverflowIndexSitesTests
{
    // Far from any address other suites assemble at: Current is process-wide.
    private const uint Site = 0x80F30000;
    private const string Table = "0x818AC454";
    private const string Shadow = "0x82E1E170";

    private static PpcInstruction X(uint address, uint word, string mnemonic, int rs, int ra, int rb) =>
        PpcInstruction.Synthetic(address, word, mnemonic, new PpcOperand[]
        {
            new PpcRegisterOperand($"r{rs}", rs), new PpcRegisterOperand($"r{ra}", ra), new PpcRegisterOperand($"r{rb}", rb)
        });

    private static uint XWord(int rs, int ra, int rb, int xo) => (31u << 26) | ((uint)rs << 21) | ((uint)ra << 16) | ((uint)rb << 11) | ((uint)xo << 1);

    private static IReadOnlyList<IrInstruction> Lift(PpcInstruction ins, string kind, int size)
    {
        var path = Path.GetTempFileName();
        File.WriteAllText(path, $"0x{Site:X8}  0  {kind}  {size}  {Table}  {Shadow}\n");
        var previous = ArrayOverflowSites.Current;
        ArrayOverflowSites.Current = ArrayOverflowSites.Load(path);
        try
        {
            var blr = PpcInstruction.Synthetic(ins.Address + 4, 0x4E800020, "blr", System.Array.Empty<PpcOperand>());
            return new PpcLifter().Lift(new List<PpcInstruction> { ins, blr })[0].Ir;
        }
        finally
        {
            ArrayOverflowSites.Current = previous;
            File.Delete(path);
        }
    }

    [Fact]
    public void IndexAddBecomesTheAddHelper()
    {
        // add r28, r0, r3: the table in r0 is a real register for add.
        var ir = Lift(X(Site, XWord(28, 0, 3, 266), "add", 28, 0, 3), "indexadd", 12);
        var call = Assert.IsType<IrCall>(Assert.Single(ir));
        Assert.Equal("r28", call.Destination);
        Assert.Equal(ArrayOverflowSites.IndexedAddHelper, call.Target);
        Assert.Equal(new[] { "r0", "r3" }, call.Arguments.Take(2).Select(a => a.RegisterName));
    }

    [Fact]
    public void IndexBMovesTheSecondRegister()
    {
        // stbx r4, r3, r0: offset in r3, table in r0.
        var ir = Lift(X(Site, XWord(4, 3, 0, 215), "stbx", 4, 3, 0), "indexb", 92);
        var first = Assert.IsType<IrCall>(ir.First());
        var last = Assert.IsType<IrCall>(ir.Last());
        Assert.Equal(ArrayOverflowSites.IndexedBaseHelper, first.Target);
        Assert.Equal("r0", first.Destination);
        Assert.Equal(ArrayOverflowSites.IndexedRestoreHelper, last.Target);
        Assert.Equal("r0", last.Destination);
    }

    [Fact]
    public void LoadIntoTheBaseNeedsNoRestore()
    {
        // lwzx r3, r3, r0: the load replaces the (moved) base.
        var ir = Lift(X(Site, XWord(3, 3, 0, 23), "lwzx", 3, 3, 0), "index", 4);
        Assert.Equal(ArrayOverflowSites.IndexedBaseHelper, Assert.IsType<IrCall>(ir.First()).Target);
        Assert.DoesNotContain(ir, i => i is IrCall c && c.Target == ArrayOverflowSites.IndexedRestoreHelper);
    }

    [Fact]
    public void IndexWithBaseZeroIsRejected()
    {
        // lbzx r5, 0, r3: rA = 0 reads as 0, not r0.
        Assert.ThrowsAny<System.Exception>(() => Lift(X(Site, XWord(5, 0, 3, 87), "lbzx", 5, 0, 3), "index", 1));
    }
}
