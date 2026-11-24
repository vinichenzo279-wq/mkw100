using Translator.Core.Mods;
using Translator.Core.Parsing.Kamek;
using Xunit;

namespace Translator.Tests;

public class LowMemoryVeneerFolderTests
{
    private const uint ModuleBase = 0x81800000;

    private static KamekCommand Abs(KamekCommandId id, uint address, uint arg) => new(0, 0, id, true, address, [arg]);
    private static KamekCommand Rel(KamekCommandId id, uint offset, uint arg) => new(0, 0, id, false, offset, [arg]);

    [Fact]
    public void FoldsInsaneKartStyleVeneer()
    {
        // Module: 0x10 holds `b <absolute>` (Rel24 to the veneer's blr), 0x14 a plain word.
        var blob = new byte[0x20];
        blob[0x10] = 0x48;
        var chunk = new KamekChunk(0, 0, 0, (uint)blob.Length, 0, 0, 0, blob,
        [
            Abs(KamekCommandId.Branch, 0x800009C8, 0x4),        // veneer: b module+4
            Abs(KamekCommandId.Write32, 0x800009CC, 0x4E800020), // blr after it
            Abs(KamekCommandId.Write32, 0x8062D338, 0x4B9D3690), // b 0x800009C8 from base code
            Rel(KamekCommandId.Rel24, 0x10, 0x800009CC),         // module "returns" through the blr
            Abs(KamekCommandId.Write32, 0x80600000, 0x60000000), // unrelated
        ]);

        var folded = LowMemoryVeneerFolder.Fold(chunk, address => address >= 0x80004000, ModuleBase);

        var veneer = folded.Commands.Single(c => c.AddressIsAbsolute && c.Address == 0x800009C8);
        Assert.Equal(KamekCommandId.Write32, veneer.Id);
        Assert.Equal(KamekPpcEncoding.EncodeBranch(0x800009C8, ModuleBase + 4, link: false), veneer.Arguments[0]);
        var hook = folded.Commands.Single(c => c.AddressIsAbsolute && c.Address == 0x8062D338);
        Assert.Equal(KamekCommandId.Branch, hook.Id);
        Assert.Equal(0x4u, hook.Arguments[0]);
        var ret = folded.Commands.Single(c => c.AddressIsRelative && c.Address == 0x10);
        Assert.Equal(KamekCommandId.Write32, ret.Id);
        Assert.Equal(0x4E800020u, ret.Arguments[0]);
        Assert.Contains(folded.Commands, c => c.AddressIsAbsolute && c.Address == 0x80600000 && c.Id == KamekCommandId.Write32);
    }

    [Fact]
    public void LeavesChunksWithoutVeneersAlone()
    {
        var chunk = new KamekChunk(0, 0, 0, 4, 0, 0, 0, new byte[4],
            [Abs(KamekCommandId.Branch, 0x8053369C, 0x0)]);
        Assert.Same(chunk, LowMemoryVeneerFolder.Fold(chunk, _ => true, ModuleBase));
    }
}
