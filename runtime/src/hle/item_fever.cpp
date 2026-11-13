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
constexpr uint32_t kFeverTimerShadow = 0x82D20000;  // float[12] at 0x818A2D40
constexpr uint32_t kFeverStateShadow = 0x82D20200;  // u8[12] at 0x818A2D70
constexpr uint32_t kFeverShadowRacers = 128 - 12;

// A [12] table and its shadow (racers 12..127; array-sites.txt, profile item-fever).
struct FeverTable { uint32_t table, stride, shadow; };

// Item Fever's resets of these tables run a fixed 12 times and write one value per table; the
// shadow elements get the value element 0 was just given.
void CopyResetToShadow(const FeverTable& t) {
    for (uint32_t i = 0; i < kFeverShadowRacers; ++i) {
        for (uint32_t b = 0; b < t.stride; ++b) {
            Memory::Write8(t.shadow + i * t.stride + b, Memory::Read8(t.table + b));
        }
    }
}

// The struct at 0x818B89F0 (one item's per-racer state, its functions 0x81848790..0x8184A0FC).
// Reset by 0x81848790; the rest of it (0x818B8AE0 [12] x 64 bytes, ...) by 0x818489EC's loop,
// whose bound is a cmpimm site, so it fills the shadows itself.
constexpr FeverTable kResetTables89F0[] = {
    {0x818B89F0, 2, 0x82D21000},
    {0x818B8A08, 4, 0x82D21100},
    {0x818B8A38, 4, 0x82D21300},
    {0x818B8A68, 1, 0x82D21500},
    {0x818B8A74, 1, 0x82D21600},
    {0x818B8A80, 1, 0x82D21700},
    {0x818B8A90, 4, 0x82D21800},
    {0x818B8AC0, 1, 0x82D21A00},
};
// The struct at 0x818B90E0 (the per-kart routine Item Fever hooks into Kart::Sub::Update,
// 0x8184BBC4), reset by 0x8184C828.
constexpr FeverTable kResetTables90E0[] = {
    {0x818B90E0, 1, 0x82D24A00},
    {0x818B90EC, 1, 0x82D24B00},
    {0x818B90F8, 1, 0x82D24C00},
    {0x818B9104, 1, 0x82D24D00},
    {0x818B9110, 1, 0x82D24E00},
    {0x818B911C, 1, 0x82D24F00},
    {0x818B9128, 2, 0x82D25000},
    {0x818B9140, 4, 0x82D25100},
    {0x818B9170, 4, 0x82D25300},
    {0x818B91A0, 1, 0x82D25500},
    {0x818B91AC, 1, 0x82D25600},
};

} // namespace

// Item Fever's race-load hook (node 0x818A2D80): clears its float[12] at 0x818A2D40 and u8[12] at
// 0x818A2D70 (a fixed count of 12, no compare to raise). Their shadows and the flags' are cleared
// with them, so a racer 12+ starts each race like racers 0..11.
extern "C" void ItemFever_ResetRacerTables_81816d88(CpuContext* ctx) {
    MKW_GUEST_NEXT(81816D88)(ctx);
    for (uint32_t i = 0; i < kFeverShadowRacers; ++i) {
        Memory::Write8(kFeverFlagsShadow + i, 0);
    }
    CopyResetToShadow({0x818A2D40, 4, kFeverTimerShadow});
    CopyResetToShadow({0x818A2D70, 1, kFeverStateShadow});
}
REGISTER_NATIVE_WRAPPER_AS(0x81816D88, ItemFever_ResetRacerTables_81816d88, "ItemFever_ResetRacerTables_81816d88");

extern "C" void ItemFever_Reset89F0_81848790(CpuContext* ctx) {
    MKW_GUEST_NEXT(81848790)(ctx);
    for (const FeverTable& t : kResetTables89F0) CopyResetToShadow(t);
}
REGISTER_NATIVE_WRAPPER_AS(0x81848790, ItemFever_Reset89F0_81848790, "ItemFever_Reset89F0_81848790");

extern "C" void ItemFever_Reset90E0_8184c828(CpuContext* ctx) {
    MKW_GUEST_NEXT(8184C828)(ctx);
    for (const FeverTable& t : kResetTables90E0) CopyResetToShadow(t);
}
REGISTER_NATIVE_WRAPPER_AS(0x8184C828, ItemFever_Reset90E0_8184c828, "ItemFever_Reset90E0_8184c828");

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
