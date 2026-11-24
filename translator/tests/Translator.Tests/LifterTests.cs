using System.Collections.Generic;
using System.Linq;
using Translator.Core.Disassembly;
using Translator.Core.Ir;
using Translator.Core.Lifting;
using Xunit;

namespace Translator.Tests;

public class LifterTests
{
    [Fact]
    public void LiftsSimpleBlock()
    {
        // Synthetic block: li r3,5; mr r4,r3; blr
        var instructions = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(0x80000000, 0x38600005, "li", new PpcOperand[] { new PpcRegisterOperand("r3", 3), new PpcImmediateOperand(5) }),
            PpcInstruction.Synthetic(0x80000004, 0x7C832378, "mr", new PpcOperand[] { new PpcRegisterOperand("r4", 4), new PpcRegisterOperand("r3", 3), new PpcRegisterOperand("r3", 3) }),
            PpcInstruction.Synthetic(0x80000008, 0x4E800020, "blr", Array.Empty<PpcOperand>())
        };

        var lifter = new PpcLifter();
        var lifted = lifter.Lift(instructions);
        Assert.Equal(instructions, lifted.Select(item => item.Origin));
        Assert.IsType<IrAssign>(lifted[0].Ir.First());
        Assert.IsType<IrAssign>(lifted[1].Ir.First());
        Assert.IsType<IrReturn>(lifted[2].Ir.Single());
    }

    [Fact]
    public void BlOverInlineDataTailCallsItsTarget()
    {
        // bl over; .string "hili"; over: mflr r4; blr (Insane Kart Wii's inline strings)
        var instructions = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(0x81828000, 0x48000009, "bl", Array.Empty<PpcOperand>(), branchTargets: new uint[] { 0x81828008 }, isCall: true),
            PpcInstruction.Synthetic(0x81828004, 0x60000000, "nop", Array.Empty<PpcOperand>()),  // the data
            PpcInstruction.Synthetic(0x81828008, 0x7C8802A6, "mflr", new PpcOperand[] { new PpcRegisterOperand("r4", 4) }),
            PpcInstruction.Synthetic(0x8182800C, 0x4E800020, "blr", Array.Empty<PpcOperand>())
        };

        var previous = PpcLifter.BlOverData;
        try
        {
            var code = new byte[] { 0x48, 0x00, 0x00, 0x09, 0x60, 0x00, 0x00, 0x00, 0x7C, 0x88, 0x02, 0xA6, 0x4E, 0x80, 0x00, 0x20 };
            PpcLifter.BlOverData = PpcLifter.FindBlOverDataSites(code, 0x81828000);
            Assert.Equal(new uint[] { 0x81828000 }, PpcLifter.BlOverDataSites);
            Assert.Equal(0x81828008u, PpcLifter.BlOverData[0x81828000]);

            var lifted = new PpcLifter().Lift(instructions);

            Assert.IsType<IrReturn>(lifted[0].Ir.Last());
            Assert.Contains(lifted[0].Ir, ir => ir is IrCall);
        }
        finally
        {
            PpcLifter.BlOverData = previous;
        }
    }

    [Fact]
    public void CallToAFollowingPrologueStaysACall()
    {
        var instructions = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(0x80010000, 0x48000009, "bl", Array.Empty<PpcOperand>(), branchTargets: new uint[] { 0x80010008 }, isCall: true),
            PpcInstruction.Synthetic(0x80010004, 0x60000000, "nop", Array.Empty<PpcOperand>()),
            PpcInstruction.Synthetic(0x80010008, 0x7C0802A6, "mflr", new PpcOperand[] { new PpcRegisterOperand("r0", 0) }),
            PpcInstruction.Synthetic(0x8001000C, 0x4E800020, "blr", Array.Empty<PpcOperand>())
        };

        var code = new byte[] { 0x48, 0x00, 0x00, 0x09, 0x60, 0x00, 0x00, 0x00, 0x7C, 0x08, 0x02, 0xA6, 0x4E, 0x80, 0x00, 0x20 };
        Assert.Empty(PpcLifter.FindBlOverDataSites(code, 0x80010000));

        var lifted = new PpcLifter().Lift(instructions);

        Assert.DoesNotContain(lifted[0].Ir, ir => ir is IrReturn);
    }
}
