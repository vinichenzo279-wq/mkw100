// mkw100: item limits for races of more than 12 players.
//
// Each item object kind has a limit, the most of it that may be in play at once
// (Item::ObjProperties[15] at 0x809C2F48, stride 0x74, u32 limit at +0x4). ComputeCapacities
// (0x80790FB8) turns the limits into pool capacities (+0xC, +0x10), and Item::Manager's
// ObjHolders allocate their pools from them when the manager is constructed (0x807992D8). An item
// pulled with no room left in its pool is replaced by a mushroom (Item::Manager::
// IsThereCapacityForItem 0x80799BE8). The stock limits are sized for 12 players, so with 100
// most pulls of stars, megas and shells came out as mushrooms.
//
// For more than 12 players each limit is scaled by players / 12, rounded up, before the
// capacities are computed from it. Items that hit the whole field (blue shell,
// lightning, blooper, POW) keep their stock limit, and the cooldown that blocks pulling them
// again after one was pulled (300 frames, Item::ItemSlotData::CanItemNotBeObtained 0x807BB380)
// is scaled by the same factor: at 100 players 2,500 frames, about 42 seconds. A gameplay choice:
// the game has no data for 13+ players. Up to 12 players everything runs unchanged.

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"
#include "recomp_mod_loader.h"
#include "runtime_log.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace {

// Toggle for the whole change; off runs the game's own limits and cooldown at every player count.
constexpr bool kScaleItemLimits = true;
// Toggle: past 12 players a pulled field-hitting item (blue shell, lightning, blooper, POW) is kept
// with probability sqrt(12 / players), otherwise the roll is redone (up to 4 times). Every one of
// them hits most of a 100-kart field, so their per-pull rate drops too, not only their cooldown.
// 100-racer oracle race: 14 of 648 pulls (2.2%) -> 8 of 662 (1.2%).
constexpr bool kRarerFieldItems = true;
constexpr uint32_t kItemStockPlayers = 12;
constexpr uint32_t kItemRacedata = 0x809BD728;      // Racedata*, u8 player count at +0x24
constexpr uint32_t kItemObjProperties = 0x809C2F48; // Item::ObjProperties[15]
constexpr uint32_t kItemObjStride = 0x74;
constexpr uint32_t kItemObjLimit = 0x4;
constexpr uint32_t kItemObjKinds = 15;
constexpr uint32_t kItemCooldownFrames = 300;       // the game's own constant in 0x807BB380

// ItemObjId: 5 blue shell, 6 lightning, 10 blooper, 11 POW. Their cooldown timers are
// ItemSlotData +0x34 +0x38 +0x3C +0x40, the order 0x807BB380 reads them in.
constexpr bool IsFieldItem(uint32_t id) { return id == 5 || id == 6 || id == 10 || id == 11; }

// Item Fever reworks the Thunder Cloud (ItemObjId 14) with its own per-cloud data, made for the
// stock limit: a second cloud object read a garbage list in Item::ObjKumo::InitSelf and a later
// cloud spawned with no model (100 racers). Under that pack the cloud keeps its stock limit.
bool ItemFeverPack() {
    static const bool itemFever = RecompMod::RiivolutionXml().find("ItemFever") != std::string::npos;
    return itemFever;
}
bool KeepsStockLimit(uint32_t id) { return IsFieldItem(id) || (id == 14 && ItemFeverPack()); }

uint32_t ItemPlayerCount() {
    const uint32_t racedata = Memory::Read32(kItemRacedata);
    return racedata == 0 ? 0 : Memory::Read8(racedata + 0x24);
}

} // namespace

// Item::ObjProperties::ComputeCapacities. The Item::Manager constructor (0x807992D8) copies every
// race's properties fresh from StaticR (0x80790AFC) and then calls this, so the limits read here
// are always the stock ones.
extern "C" void ItemObjProperties_ComputeCapacities_80790fb8(CpuContext* ctx) {
    const uint32_t players = ItemPlayerCount();
    if (kScaleItemLimits && players > kItemStockPlayers) {
        for (uint32_t i = 0; i < kItemObjKinds; ++i) {
            const uint32_t address = kItemObjProperties + i * kItemObjStride + kItemObjLimit;
            const uint32_t stock = Memory::Read32(address);
            if (!KeepsStockLimit(i)) {
                Memory::Write32(address, (stock * players + kItemStockPlayers - 1) / kItemStockPlayers);
            }
            RT_LOGF("item-limits", "%u players: item object %u limit %u -> %u\n", players, i, stock, Memory::Read32(address));
        }
    }
    MKW_GUEST_NEXT(80790FB8)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80790FB8, ItemObjProperties_ComputeCapacities_80790fb8, "ItemObjProperties_ComputeCapacities_80790fb8");

// bool ItemSlotData::CanItemNotBeObtained(this, ItemObjId id, bool useCooldown): true while the
// item's timer is below the cooldown. Only the cooldown length changes past 12 players.
extern "C" void ItemSlot_CanItemNotBeObtained_807bb380(CpuContext* ctx) {
    const uint32_t players = ItemPlayerCount();
    const uint32_t id = ctx->gpr[4];
    if (!kScaleItemLimits || players <= kItemStockPlayers || (ctx->gpr[5] & 0xFF) == 0 || !IsFieldItem(id)) {
        MKW_GUEST_NEXT(807BB380)(ctx);
        return;
    }
    const uint32_t timer = id == 6 ? 0x34 : id == 5 ? 0x38 : id == 10 ? 0x3C : 0x40;
    const int32_t frames = static_cast<int32_t>(Memory::Read32(ctx->gpr[3] + timer));
    const int32_t cooldown = static_cast<int32_t>(kItemCooldownFrames * players / kItemStockPlayers);
    ctx->gpr[3] = frames < cooldown ? 1 : 0;
}
REGISTER_NATIVE_WRAPPER_AS(0x807BB380, ItemSlot_CanItemNotBeObtained_807bb380, "ItemSlot_CanItemNotBeObtained_807bb380");

namespace {
// ItemId (not ItemObjId): 7 blue shell, 8 lightning, 12 blooper, 13 POW.
constexpr bool IsFieldItemId(uint32_t id) { return id == 7 || id == 8 || id == 12 || id == 13; }
uint32_t gFieldRollState = 0x9E3779B9u;  // fixed seed: the oracle race stays reproducible
uint32_t NextFieldRoll() {
    gFieldRollState ^= gFieldRollState << 13;
    gFieldRollState ^= gFieldRollState >> 17;
    gFieldRollState ^= gFieldRollState << 5;
    return gFieldRollState;
}
} // namespace

// ItemSlotData::DecideItem(this, boxType, position, isHuman, hasTriple, player) -> ItemId.
extern "C" void ItemSlot_DecideItem_807bb42c(CpuContext* ctx) {
    const uint32_t players = ItemPlayerCount();
    const CpuContext entry = *ctx;
    MKW_GUEST_NEXT(807BB42C)(ctx);
    if (!kRarerFieldItems || players <= kItemStockPlayers) return;
    const double keep = std::sqrt(double(kItemStockPlayers) / double(players));
    for (int retry = 0; retry < 4 && IsFieldItemId(ctx->gpr[3]); ++retry) {
        if (double(NextFieldRoll()) / 4294967296.0 < keep) return;
        *ctx = entry;
        MKW_GUEST_NEXT(807BB42C)(ctx);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x807BB42C, ItemSlot_DecideItem_807bb42c, "ItemSlot_DecideItem_807bb42c");
