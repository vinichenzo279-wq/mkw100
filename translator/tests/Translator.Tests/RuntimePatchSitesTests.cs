using System.Buffers.Binary;
using Translator.Core.Mods;
using Translator.Core.Parsing.Kamek;
using Xunit;

namespace Translator.Tests;

public class RuntimePatchSitesTests
{
    private static KamekCommand Rel(KamekCommandId id, uint offset, uint arg) => new(0, 0, id, false, offset, [arg]);

    private static byte[] Code(params uint[] words)
    {
        var bytes = new byte[words.Length * 4];
        for (var i = 0; i < words.Length; i++)
            BinaryPrimitives.WriteUInt32BigEndian(bytes.AsSpan(i * 4, 4), words[i]);
        return bytes;
    }

    [Fact]
    public void FindsAWrite32CallWithAnAutoMappedAddress()
    {
        var code = Code(
            // 0x00 Write32 helper: stw r4, 0(r3); li r4, 4; bl DCFlushRange; icbi 0, r0; blr
            0x90830000, 0x38800004, 0x48000001, 0x7C0007AC, 0x4E800020,
            // 0x14 caller: lis r3, ha(0x8062C3B0); addi r3, r3, lo(...); lis r4, 0x6000; bl helper; blr
            0x3C600000, 0x38630000, 0x3C806000, 0x4BFFFFE1, 0x4E800020);
        var chunk = new KamekChunk(0, 0, 0, (uint)code.Length, 0, 0, 0, code,
        [
            Rel(KamekCommandId.Rel24, 0x08, 0x801A162C),
            Rel(KamekCommandId.Addr16Ha, 0x16, 0x8062C3B0),
            Rel(KamekCommandId.Addr16Lo, 0x1A, 0x8062C3B0),
        ]);

        var scan = RuntimePatchSites.Scan(chunk, 0x81800000);

        Assert.Equal(new uint[] { 0x81800000 }, scan.Helpers);
        Assert.Equal(1, scan.Calls);
        Assert.Equal(0, scan.UnknownCalls);
        Assert.Equal(new uint[] { 0x60000000 }, scan.Sites[0x8062C3B0]);
    }

    [Fact]
    public void FindsHooksThatReturnToAnAdjustedLr()
    {
        var code = Code(
            0x4E800020,              // 0x00 blr (end of the previous function)
            0x7D8802A6,              // 0x04 mflr r12
            0x398C00A4,              // 0x08 addi r12, r12, 0xA4
            0x7D8803A6,              // 0x0C mtlr r12
            0x4D820020,              // 0x10 beqlr
            0x4E800020,              // 0x14 blr
            0x7C0802A6, 0x4E800020); // 0x18 an ordinary prologue: mflr r0 (no adjust)

        var hooks = LrAdjustingHooks.Scan(code, 0x81800000);

        Assert.Equal(new[] { 0xA4 }, hooks[0x81800004]);
        Assert.Single(hooks);
    }

    [Fact]
    public void FindsNothingWithoutHelpers()
    {
        var code = Code(0x38600000, 0x4E800020);
        var chunk = new KamekChunk(0, 0, 0, (uint)code.Length, 0, 0, 0, code, []);

        var scan = RuntimePatchSites.Scan(chunk, 0x81800000);

        Assert.Empty(scan.Helpers);
        Assert.Empty(scan.Sites);
    }
}
