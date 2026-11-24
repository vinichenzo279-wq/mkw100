using Translator.Core.Parsing.Kamek;

namespace Translator.Core.Mods;

/// <summary>
/// mkw100: a pack may plant a branch veneer in non-executable memory (Insane Kart Wii: `b module`
/// at 0x800009C8 in the OS low-memory globals, `blr` after it) and hook base code with plain
/// `b 0x800009C8` instruction writes, its module code "returning" with `b 0x800009CC`. On a Wii
/// that runs; the recomp only runs translated functions, and there is none in low memory. This
/// folds each such veneer into its users before patch planning:
///   - a base Write32 of `b`/`bl` to the veneer becomes a Branch/BranchLink command to its target;
///   - a module Rel24 to the veneer becomes a relative Branch to its target, and one to a `blr`
///     the pack wrote right after it becomes that `blr` (`nop` for a `bl`);
///   - the veneer's own Branch becomes a Write32 of the same instruction, so memory still matches.
/// </summary>
public static class LowMemoryVeneerFolder
{
    private const uint Blr = 0x4E800020;
    private const uint Nop = 0x60000000;

    public static KamekChunk Fold(KamekChunk chunk, Func<uint, bool> isExecutable, uint moduleGuestBase,
        Action<string>? log = null)
    {
        ArgumentNullException.ThrowIfNull(chunk);
        ArgumentNullException.ThrowIfNull(isExecutable);
        var veneers = chunk.Commands
            .Where(c => c.AddressIsAbsolute && c.Id == KamekCommandId.Branch && c.Arguments.Count == 1 &&
                        !isExecutable(c.Address))
            .ToDictionary(c => c.Address);
        if (veneers.Count == 0)
            return chunk;
        // `blr` words the pack writes right after a veneer.
        var returns = chunk.Commands
            .Where(c => c.AddressIsAbsolute && c.Id == KamekCommandId.Write32 && c.Arguments.Count == 1 &&
                        c.Arguments[0] == Blr && veneers.ContainsKey(c.Address - 4))
            .Select(c => c.Address)
            .ToHashSet();

        var commands = new List<KamekCommand>(chunk.Commands.Count);
        foreach (var command in chunk.Commands)
        {
            if (command.AddressIsAbsolute && veneers.TryGetValue(command.Address, out var own) && ReferenceEquals(own, command))
            {
                var target = KamekAddress.Resolve(command.Arguments[0], moduleGuestBase);
                var offset = unchecked((long)target - command.Address);
                if (offset >= -0x2000000 && offset < 0x2000000)
                {
                    commands.Add(command with
                    {
                        Id = KamekCommandId.Write32,
                        Arguments = [0x48000000u | (uint)(offset & 0x03FFFFFC)]
                    });
                }
                log?.Invoke($"veneer at 0x{command.Address:X8} -> 0x{target:X8} folded into its users");
                continue;
            }

            if (command.AddressIsAbsolute && command.Id == KamekCommandId.Write32 && command.Arguments.Count == 1 &&
                IsRelativeBranch(command.Arguments[0], out var link) &&
                veneers.TryGetValue(BranchTarget(command.Address, command.Arguments[0]), out var veneer))
            {
                commands.Add(command with
                {
                    Id = link ? KamekCommandId.BranchLink : KamekCommandId.Branch,
                    Arguments = [veneer.Arguments[0]]
                });
                log?.Invoke($"0x{command.Address:X8}: branch to veneer 0x{veneer.Address:X8} now goes to its target");
                continue;
            }

            if (command.AddressIsRelative && command.Id == KamekCommandId.Rel24 && command.Arguments.Count == 1 &&
                command.Address + 4 <= chunk.CodeBlob.Length)
            {
                var target = KamekAddress.Resolve(command.Arguments[0], moduleGuestBase);
                var word = (uint)(chunk.CodeBlob[command.Address] << 24 | chunk.CodeBlob[command.Address + 1] << 16 |
                                  chunk.CodeBlob[command.Address + 2] << 8 | chunk.CodeBlob[command.Address + 3]);
                if (veneers.TryGetValue(target, out var called))
                {
                    commands.Add(command with
                    {
                        Id = (word & 1) != 0 ? KamekCommandId.BranchLink : KamekCommandId.Branch,
                        Arguments = [called.Arguments[0]]
                    });
                    log?.Invoke($"module 0x{command.Address:X}: branch to veneer 0x{target:X8} now goes to its target");
                    continue;
                }
                if (returns.Contains(target))
                {
                    commands.Add(command with { Id = KamekCommandId.Write32, Arguments = [(word & 1) != 0 ? Nop : Blr] });
                    log?.Invoke($"module 0x{command.Address:X}: branch to the blr at 0x{target:X8} is now a blr");
                    continue;
                }
            }

            commands.Add(command);
        }

        return new KamekChunk(chunk.Index, chunk.FileOffset, chunk.BssSize, chunk.CodeSize, chunk.CtorStart,
            chunk.CtorEnd, chunk.ChunkSize, chunk.CodeBlob, commands);
    }

    private static bool IsRelativeBranch(uint word, out bool link)
    {
        link = (word & 1) != 0;
        return word >> 26 == 18 && (word & 2) == 0;
    }

    private static uint BranchTarget(uint address, uint word)
    {
        var li = (int)(word & 0x03FFFFFC);
        if ((li & 0x02000000) != 0)
            li -= 0x04000000;
        return unchecked(address + (uint)li);
    }
}
