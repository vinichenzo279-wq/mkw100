// mkw100: Item Fever (Pulsar pack) past 12 racers.
//
// Item Fever keeps some per-racer state in static [12] tables in its own module. The unchecked
// accesses are listed as `index` sites in projects/mkwii/mkw100/array-sites.txt (profile
// item-fever): racers 12+ use a shadow table instead. This file resets those shadows when Item
// Fever resets its own tables, and applies the item reserve Item Rain patches into game code.
// Retro Rewind has no function at 0x81816D88; the Item::Manager wrappers change nothing unpatched.

#include "hle/mod_build.h"
#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"
#include "hle/race_heap.h"
#include "local_vs.h"
#include "recomp_mod_loader.h"
#include "runtime_log.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

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

void LogFeverUnlocks(CpuContext* ctx);  // below, with Item Fever's unlocks

} // namespace

// Item Fever's race-load hook (node 0x818A2D80): clears its float[12] at 0x818A2D40 and u8[12] at
// 0x818A2D70 (a fixed count of 12, no compare to raise). Their shadows and the flags' are cleared
// with them, so a racer 12+ starts each race like racers 0..11.
extern "C" void ItemFever_ResetRacerTables_81816d88(CpuContext* ctx) {
    MKW_GUEST_NEXT(81816D88)(ctx);
    if (!ModBuild::IsKnownItemFever()) return;  // another pack (build) has other code here
    for (uint32_t i = 0; i < kFeverShadowRacers; ++i) {
        Memory::Write8(kFeverFlagsShadow + i, 0);
    }
    CopyResetToShadow({0x818A2D40, 4, kFeverTimerShadow});
    CopyResetToShadow({0x818A2D70, 1, kFeverStateShadow});
    LogFeverUnlocks(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x81816D88, ItemFever_ResetRacerTables_81816d88, "ItemFever_ResetRacerTables_81816d88");

extern "C" void ItemFever_Reset89F0_81848790(CpuContext* ctx) {
    MKW_GUEST_NEXT(81848790)(ctx);
    if (!ModBuild::IsKnownItemFever()) return;
    for (const FeverTable& t : kResetTables89F0) CopyResetToShadow(t);
}
REGISTER_NATIVE_WRAPPER_AS(0x81848790, ItemFever_Reset89F0_81848790, "ItemFever_Reset89F0_81848790");

extern "C" void ItemFever_Reset90E0_8184c828(CpuContext* ctx) {
    MKW_GUEST_NEXT(8184C828)(ctx);
    if (!ModBuild::IsKnownItemFever()) return;
    for (const FeverTable& t : kResetTables90E0) CopyResetToShadow(t);
}
REGISTER_NATIVE_WRAPPER_AS(0x8184C828, ItemFever_Reset90E0_8184c828, "ItemFever_Reset90E0_8184c828");

// Item Fever's unlocks, available from the start while "All characters, vehicles and cups
// available" (local_vs unlock_all, on by default) is on:
//   200cc: Item Fever opens its 200cc class (picked like mirror) for a license once all 8 cups are
//       gold on Very Hard. It keeps that as bit <license> of a flags word in its save (ifever.dat:
//       *(0x818A2D28) +0x40 -> buffer, buffer + *(buffer + 0x14) + 0x30; bit 4+n: "200cc unlocked"
//       shown, 12+n: Meteor Storm shown, 16+n: dark theme shown) and several menu functions test
//       that bit inline, so the bit is set: for every license, whenever Item Fever asks for the
//       current license (0x8180A38C), which every one of those tests does first. Item Fever saves
//       the buffer, so the bit then stays in ifever.dat.
//   Meteor Storm: the secret reward for a bronze or better in all 8 cups of a license's 200cc row,
//       tested by 0x8180B730(license) and nowhere else; it answers yes. Item Fever then shows its
//       one-time "unlocked" notice once, as if the trophies had just been won.
//   Dark theme: the reward for all 8 of those cups gold, tested by 0x8180B854(license); it answers
//       yes too, with the same one-time notice.
// MKW100_TEST_IF_UNLOCKS (tests only) applies them in scripted runs, which otherwise keep the save
// as it is, and logs at race load what Item Fever's own tests answer.
namespace {

constexpr uint32_t kFeverSave = 0x818A2D28;
constexpr uint32_t kFeverUnlockFlags = 0x30;
constexpr uint32_t kFever200ccBits = 0xF;  // bit n: license n

bool FeverUnlocksOn() {
    if (!ModBuild::IsKnownItemFever()) return false;
    static const bool test = std::getenv("MKW100_TEST_IF_UNLOCKS") != nullptr;
    return test || LocalVs::UnlocksEnabled();
}

// The flags word of Item Fever's loaded save, or 0 before the save is loaded.
uint32_t FeverUnlockFlagsAddress() {
    const uint32_t save = Memory::Read32(kFeverSave);
    if (save < 0x80000000u || !Memory::Contains(save + 0x40, 4)) return 0;
    const uint32_t buffer = Memory::Read32(save + 0x40);
    if (buffer < 0x80000000u || !Memory::Contains(buffer + 0x14, 4)) return 0;
    const uint32_t flags = buffer + Memory::Read32(buffer + 0x14) + kFeverUnlockFlags;
    return Memory::Contains(flags, 4) ? flags : 0;
}

} // namespace

// Item Fever: the current license, 0..3 (SaveManager +0x36).
extern "C" void ItemFever_CurrentLicense_8180a38c(CpuContext* ctx) {
    MKW_GUEST_NEXT(8180A38C)(ctx);
    if (!FeverUnlocksOn()) return;
    const uint32_t flags = FeverUnlockFlagsAddress();
    if (flags == 0) return;
    const uint32_t value = Memory::Read32(flags);
    if ((value & kFever200ccBits) == kFever200ccBits) return;
    Memory::Write32(flags, value | kFever200ccBits);
    RT_LOGF("item-fever", "200cc unlocked for every license (flags %08X -> %08X)\n", value, value | kFever200ccBits);
}
REGISTER_NATIVE_WRAPPER_AS(0x8180A38C, ItemFever_CurrentLicense_8180a38c, "ItemFever_CurrentLicense_8180a38c");

// Item Fever: Meteor Storm earned by `license` (r3), 1 or 0.
extern "C" void ItemFever_MeteorStormEarned_8180b730(CpuContext* ctx) {
    if (!FeverUnlocksOn()) { MKW_GUEST_NEXT(8180B730)(ctx); return; }
    static bool logged = false;
    if (!logged) { logged = true; RT_LOGF("item-fever", "Meteor Storm unlocked\n"); }
    ctx->gpr[3] = 1;
}
REGISTER_NATIVE_WRAPPER_AS(0x8180B730, ItemFever_MeteorStormEarned_8180b730, "ItemFever_MeteorStormEarned_8180b730");

// Item Fever: dark theme earned by `license` (r3), 1 or 0.
extern "C" void ItemFever_DarkThemeEarned_8180b854(CpuContext* ctx) {
    if (!FeverUnlocksOn()) { MKW_GUEST_NEXT(8180B854)(ctx); return; }
    static bool logged = false;
    if (!logged) { logged = true; RT_LOGF("item-fever", "dark theme unlocked\n"); }
    ctx->gpr[3] = 1;
}
REGISTER_NATIVE_WRAPPER_AS(0x8180B854, ItemFever_DarkThemeEarned_8180b854, "ItemFever_DarkThemeEarned_8180b854");

namespace {
// MKW100_TEST_IF_UNLOCKS: Item Fever's own answers, logged at race load (no menus run headless).
//   0x8180A3B8(): the GP classes offered, 3 or 4 with 200cc;
//   0x8180B7AC() / 0x8180B8CC(): Meteor Storm / dark theme earned by the current license.
void LogFeverUnlocks(CpuContext* ctx) {
    static const bool test = std::getenv("MKW100_TEST_IF_UNLOCKS") != nullptr;
    if (!test || !ModBuild::IsKnownItemFever()) return;
    const CpuContext saved = *ctx;
    uint32_t answer[3];
    const uint32_t calls[3] = {0x8180A3B8, 0x8180B7AC, 0x8180B8CC};
    for (int i = 0; i < 3; ++i) {
        InvokeIndirectCpu(calls[i], ctx);
        answer[i] = ctx->gpr[3];
        *ctx = saved;
    }
    const uint32_t flags = FeverUnlockFlagsAddress();
    RT_LOGF("item-fever", "test: classes %u, Meteor Storm %u, dark theme %u, flags %08X\n", answer[0], answer[1],
            answer[2], flags ? Memory::Read32(flags) : 0u);
}
} // namespace

// Item Rain's dropped items get models (MKWCI-DROP, 0x81878xxx) only while the scene heap
// (ScnMgr::sInstance[0] +0x10, the race scene's structsMem1) has about 900 KB free (0x8187892C),
// and each extra copy needs about 700 KB of the scene's structsMem2 (0x8187896C). Past 12 racers
// the karts fill structsMem1, and race_heap.cpp places what does not fit there in structsMem2 and
// structsDebug instead, but these checks only saw structsMem1's last bytes: 80 racers, "scene space
// exhausted (768 bytes): drop models stopped at type 0", so the rain's items had no model and lay
// on the track invisible (a Mega Mushroom picked up out of nowhere). The checks now count what the
// fallback still has behind those heaps.
extern "C" void ItemFever_SceneFree_8187892c(CpuContext* ctx) {
    MKW_GUEST_NEXT(8187892C)(ctx);
    if (!ModBuild::IsKnownItemFever() || ctx->gpr[3] == 0xFFFFFFFFu) return;
    const uint32_t scn = Memory::Read32(0x809C1850);  // ScnMgr::sInstance[0]
    const uint32_t egg = scn >= 0x80000000u ? Memory::Read32(scn + 0x10) : 0;
    const uint32_t extra = RaceHeap_FallbackFree(ctx, egg);
    ctx->gpr[3] = uint32_t(std::min<uint64_t>(uint64_t(ctx->gpr[3]) + extra, 0x7FFFFFFFu));
}
REGISTER_NATIVE_WRAPPER_AS(0x8187892C, ItemFever_SceneFree_8187892c, "ItemFever_SceneFree_8187892c");

extern "C" void ItemFever_SceneMem2Free_8187896c(CpuContext* ctx) {
    MKW_GUEST_NEXT(8187896C)(ctx);
    if (!ModBuild::IsKnownItemFever() || ctx->gpr[3] == 0xFFFFFFFFu) return;
    const uint32_t extra = RaceHeap_FallbackFree(ctx, RaceHeap_CurrentSceneHeap(0xC8C));
    ctx->gpr[3] = uint32_t(std::min<uint64_t>(uint64_t(ctx->gpr[3]) + extra, 0x7FFFFFFFu));
}
REGISTER_NATIVE_WRAPPER_AS(0x8187896C, ItemFever_SceneMem2Free_8187896c, "ItemFever_SceneMem2Free_8187896c");

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
