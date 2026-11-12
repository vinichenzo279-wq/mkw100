// mkw100: Item Fever (Pulsar pack) past 12 racers.
//
// Item Fever keeps some per-racer state in static [12] tables in its own module. The unchecked
// accesses are listed as `index` sites in projects/mkwii/mkw100/array-sites.txt (profile
// item-fever): racers 12+ use a shadow table instead. This file resets those shadows when Item
// Fever resets its own tables, and applies the item reserve Item Rain patches into game code.
// Retro Rewind has no function at 0x81816D88; the Item::Manager wrappers change nothing unpatched.

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"
#include "recomp_mod_loader.h"

#include <cstdint>

namespace {

constexpr uint32_t kFeverFlagsShadow = 0x82D00000;  // u8 flags[12] at 0x818A2D34, racers 12..127
constexpr uint32_t kFeverShadowRacers = 128 - 12;

} // namespace

// Item Fever's race-load hook (node 0x818A2D80): clears its float[12] at 0x818A2D40 and u8[12] at
// 0x818A2D70. The flags' shadow is cleared with it, so a racer 12+ starts each race unflagged.
extern "C" void ItemFever_ResetRacerTables_81816d88(CpuContext* ctx) {
    MKW_GUEST_NEXT(81816D88)(ctx);
    for (uint32_t i = 0; i < kFeverShadowRacers; ++i) {
        Memory::Write8(kFeverFlagsShadow + i, 0);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x81816D88, ItemFever_ResetRacerTables_81816d88, "ItemFever_ResetRacerTables_81816d88");

// Item::Manager +0x42C is the sum of the item pools' capacities minus 30, set by `addi r0, rN, -30`
// at 0x8079992C (Update) and 0x80799AB0 (UpdateTotalItemCountMinus30). Item Fever rewrites both
// immediates when a race loads: -100 with Item Rain on, -30 off (0x81840B64). Translated code keeps
// -30, so the writes are let through to memory and the wrappers apply the immediate found there.
// Unpatched (the base game, Retro Rewind) the word holds -30 and nothing changes.
namespace {

constexpr uint32_t kItemManagerReserve = 0x42C;
constexpr int32_t kStockReserveImmediate = -30;
constexpr uint32_t kReserveUnset = 0xFEEDF00Du;  // marks "the call did not write the field"

struct AllowReserveWords {
    AllowReserveWords() {
        RecompMod::AllowExecutableWord(0x8079992C, "Item::Manager::Update reserve immediate");
        RecompMod::AllowExecutableWord(0x80799AB0, "Item::Manager::UpdateTotalItemCountMinus30 immediate");
    }
} gAllowReserveWords;

void ApplyReserveImmediate(CpuContext* ctx, uint32_t word, ::GuestFunctions::Entry next) {
    const uint32_t manager = ctx->gpr[3];
    const uint32_t field = manager + kItemManagerReserve;
    const uint32_t before = Memory::Read32(field);
    Memory::Write32(field, kReserveUnset);
    next(ctx);
    const uint32_t written = Memory::Read32(field);
    if (written == kReserveUnset) {
        Memory::Write32(field, before);
        return;
    }
    const int32_t immediate = static_cast<int16_t>(Memory::Read32(word) & 0xFFFF);
    Memory::Write32(field, written + static_cast<uint32_t>(immediate - kStockReserveImmediate));
}

} // namespace

extern "C" void ItemManager_Update_80799850(CpuContext* ctx) {
    ApplyReserveImmediate(ctx, 0x8079992C, MKW_GUEST_NEXT(80799850));
}
REGISTER_NATIVE_WRAPPER_AS(0x80799850, ItemManager_Update_80799850, "ItemManager_Update_80799850");

extern "C" void ItemManager_UpdateTotalItemCountMinus30_80799a50(CpuContext* ctx) {
    ApplyReserveImmediate(ctx, 0x80799AB0, MKW_GUEST_NEXT(80799A50));
}
REGISTER_NATIVE_WRAPPER_AS(0x80799A50, ItemManager_UpdateTotalItemCountMinus30_80799a50,
                           "ItemManager_UpdateTotalItemCountMinus30_80799a50");
