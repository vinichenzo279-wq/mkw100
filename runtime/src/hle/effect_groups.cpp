// mkw100: effect groups for races of more than 14 players.
//
// Every kart's effects (exhaust, drift sparks, ...) belong to effect group `player index + 2`
// (0x8068F044 sets KartEffect +0x134). The EGG effect manager (global 0x80386E30, r13 - 0x5DD0)
// holds one slot per group: {u32 nw4rGroups; u32 groups; ...; EffectCreator** slots @+0x28}. It is
// created once, by the first Effect::EffectManager::createInstance (0x8067B4C8), with 16 groups,
// and never freed; at that point (boot) no race exists yet. Group i owns the 4 nw4r::ef groups
// i*4..i*4+3 (EffectCreator +0x14), and EGG::EffectManager::CreateMemoryManager (0x80224768) gives
// the nw4r EffectSystem `nw4rGroups` = 64 activity lists. Effect::EffectManager's init (0x8067B580,
// once the race is known) constructs groups 0..15, each registering in its slot. Player 14+ then
// reads its group past the slot table (24 players: a call through a string's bytes from
// EGG::Effect::Create for "rk_gasSmoke").
//
// For more than 14 race players the init first moves the slot table and the EffectSystem's
// activity lists to fixed runtime tables (kNativeTables; the manager outlives every heap a scene
// could give it) sized for the race, then constructs the extra groups the way the game builds its
// 16. Moving the activity lists copies them: an nw4r::ut::List is {head, tail, u16 count, u16
// offset}, with no pointer back to itself. nw4rGroups follows, so a later scene's
// CreateMemoryManager sizes its own lists to match. Up to 14 players nothing changes. A larger
// table stays for the rest of the session.

#include "guest_abi/array_overflow.h"

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"

#include <cstdint>
#include <stdexcept>

namespace {

constexpr uint32_t kEffectRacedataGlobal = 0x809BD728;  // RaceConfig::spInstance
constexpr uint32_t kEffectRaceCount = 0x24;             // u8, race scenario (+0x20) player count
constexpr uint32_t kKartGroupBase = 2;                  // group = player index + 2
constexpr uint32_t kGameGroups = 16;
constexpr uint32_t kMaxGroups = ArrayOverflow::kMaxPlayers + kKartGroupBase;

// EGG::EffectManager
constexpr uint32_t kEggEffectManager = 0x80386E30;
constexpr uint32_t kNw4rGroups = 0x0;                   // u32
constexpr uint32_t kGroupCount = 0x4;                   // u32
constexpr uint32_t kGroupSlots = 0x28;                  // EffectCreator**
constexpr uint32_t kNw4rPerGroup = 4;

// nw4r::ef::EffectSystem (the instance 0x80027130 returns)
constexpr uint32_t kEffectSystem = 0x802AB4F8;
constexpr uint32_t kActivityCount = 0xC014;             // u32 maxGroupID
constexpr uint32_t kActivityLists = 0xC018;             // {ut::List; ut::List; u16}[maxGroupID]
constexpr uint32_t kActivitySize = 0x1C;
constexpr uint32_t kActivityListOffset = 0x14;          // both lists link through +0x14

// Fixed tables in the native span.
constexpr uint32_t kSlotTable = ArrayOverflow::kNativeTables;
constexpr uint32_t kActivityTable = kSlotTable + 0x800;  // room for 512 group slots
static_assert(kMaxGroups * 4 <= kActivityTable - kSlotTable);
static_assert(kActivityTable + kMaxGroups * kNw4rPerGroup * kActivitySize <= ArrayOverflow::kMenuText);

constexpr uint32_t kGroupSize = 0x18;
constexpr uint32_t kNew = 0x80229DCC;                   // operator new(size), the current heap
constexpr uint32_t kGroupCtor = 0x80224348;             // EffectCreator(this, id), registers the slot

void GrowSlots(uint32_t manager, uint32_t groups) {
    const uint32_t current = Memory::Read32(manager + kGroupCount);
    if (groups <= current) {
        return;
    }
    const uint32_t slots = Memory::Read32(manager + kGroupSlots);
    for (uint32_t i = 0; i < groups; ++i) {
        Memory::Write32(kSlotTable + i * 4, i < current ? Memory::Read32(slots + i * 4) : 0);
    }
    Memory::Write32(manager + kGroupSlots, kSlotTable);
    Memory::Write32(manager + kGroupCount, groups);
    Memory::Write32(manager + kNw4rGroups, groups * kNw4rPerGroup);
}

void GrowActivityLists(uint32_t count) {
    const uint32_t current = Memory::Read32(kEffectSystem + kActivityCount);
    if (count <= current) {
        return;
    }
    const uint32_t lists = Memory::Read32(kEffectSystem + kActivityLists);
    for (uint32_t i = 0; i < count * kActivitySize; i += 4) {
        Memory::Write32(kActivityTable + i, i < current * kActivitySize ? Memory::Read32(lists + i) : 0);
    }
    for (uint32_t i = current; i < count; ++i) {
        const uint32_t entry = kActivityTable + i * kActivitySize;
        Memory::Write16(entry + 0xA, kActivityListOffset);
        Memory::Write16(entry + 0xC + 0xA, kActivityListOffset);
    }
    Memory::Write32(kEffectSystem + kActivityLists, kActivityTable);
    Memory::Write32(kEffectSystem + kActivityCount, count);
}

} // namespace

// Effect::EffectManager's init: constructs groups 0..15.
extern "C" void EffectManager_Init_8067b580(CpuContext* ctx) {
    const uint32_t racedata = Memory::Read32(kEffectRacedataGlobal);
    const uint32_t manager = Memory::Read32(kEggEffectManager);
    const uint32_t groups = racedata == 0 ? 0 : Memory::Read8(racedata + kEffectRaceCount) + kKartGroupBase;
    if (manager == 0 || groups <= kGameGroups) {
        MKW_GUEST_NEXT(8067B580)(ctx);
        return;
    }
    if (groups > kMaxGroups) {
        throw std::runtime_error("mkw100 effect groups: more groups than the tables hold");
    }
    GrowSlots(manager, groups);
    GrowActivityLists(groups * kNw4rPerGroup);
    MKW_GUEST_NEXT(8067B580)(ctx);

    const CpuContext saved = *ctx;
    for (uint32_t id = kGameGroups; id < groups; ++id) {
        ctx->gpr[3] = kGroupSize;
        InvokeIndirectCpu(kNew, ctx);
        const uint32_t group = ctx->gpr[3];
        if (group == 0) {
            throw std::runtime_error("mkw100 effect groups: out of memory for an effect group");
        }
        ctx->gpr[4] = id;
        InvokeIndirectCpu(kGroupCtor, ctx);
    }
    *ctx = saved;
}
REGISTER_NATIVE_WRAPPER_AS(0x8067B580, EffectManager_Init_8067b580, "EffectManager_Init_8067b580");
