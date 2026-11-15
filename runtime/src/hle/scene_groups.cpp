// mkw100: scene group capacity for races of more than 12 players.
//
// ScnMgr::CreateScnGroup (0x80562444) builds each of the race scene's model groups with
// nw4r::g3d::ScnGroup::Construct (0x8006E390: allocator, u32* size, max children) and a fixed 768
// children. ScnGroup::Insert (0x8006EB60) returns false once a group holds that many, and the model
// is then never drawn. In a 100-CPU race (course 0x08) both of the race's groups reached 768 and
// at least 400 inserts failed, from ScnMgr::InsertModelDirectorInScn and 0x80563A14: racers were
// invisible. At 12 players none failed.
//
// For more than 12 players the capacity is scaled by players / 12 (6,400 at 100, 4 bytes each).
// Up to 12 players the group is built unchanged.

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"
#include "runtime_log.h"

#include <cstdint>

namespace {

constexpr uint32_t kScnStockPlayers = 12;
constexpr uint32_t kScnStockChildren = 768;      // the constant in 0x80562444
constexpr uint32_t kScnRacedata = 0x809BD728;    // Racedata*, u8 player count at +0x24

bool gInCreateScnGroup = false;  // game thread only

uint32_t ScnPlayerCount() {
    const uint32_t racedata = Memory::Read32(kScnRacedata);
    return racedata == 0 ? 0 : Memory::Read8(racedata + 0x24);
}

} // namespace

extern "C" void ScnMgr_CreateScnGroup_80562444(CpuContext* ctx) {
    gInCreateScnGroup = true;
    MKW_GUEST_NEXT(80562444)(ctx);
    gInCreateScnGroup = false;
}
REGISTER_NATIVE_WRAPPER_AS(0x80562444, ScnMgr_CreateScnGroup_80562444, "ScnMgr_CreateScnGroup_80562444");

extern "C" void ScnGroup_Construct_8006e390(CpuContext* ctx) {
    const uint32_t players = ScnPlayerCount();
    if (gInCreateScnGroup && ctx->gpr[5] == kScnStockChildren && players > kScnStockPlayers) {
        ctx->gpr[5] = kScnStockChildren * players / kScnStockPlayers;
    }
    MKW_GUEST_NEXT(8006E390)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8006E390, ScnGroup_Construct_8006e390, "ScnGroup_Construct_8006e390");

// ScnMgr::InitImpl (0x80562180) builds the scene's root with nw4r::g3d::ScnRoot::Construct
// (0x8006F1A0: allocator, u32* size, max children, max gathered scene objects, lights...) and a
// fixed 768 / 768. The second is the size of the per-frame draw lists (opaque and translucent)
// that ScnRoot::GatherDrawScnObj fills with every visible object; past it, objects are silently
// not drawn. With 100 racers in front of the camera their karts, drivers, wheels and shadows took
// all 768 places and the course models after them were dropped (the track vanished looking
// forward and came back looking backward). Both are scaled by players / 12 like the groups.
namespace {
bool gInScnMgrInit = false;  // game thread only
constexpr uint32_t kScnRootStockObjects = 768;  // the constants in 0x80562180
} // namespace

extern "C" void ScnMgr_InitImpl_80562180(CpuContext* ctx) {
    gInScnMgrInit = true;
    MKW_GUEST_NEXT(80562180)(ctx);
    gInScnMgrInit = false;
}
REGISTER_NATIVE_WRAPPER_AS(0x80562180, ScnMgr_InitImpl_80562180, "ScnMgr_InitImpl_80562180");

extern "C" void ScnRoot_Construct_8006f1a0(CpuContext* ctx) {
    const uint32_t players = ScnPlayerCount();
    if (gInScnMgrInit && players > kScnStockPlayers) {
        if (ctx->gpr[5] == kScnRootStockObjects) {
            ctx->gpr[5] = kScnRootStockObjects * players / kScnStockPlayers;
        }
        if (ctx->gpr[6] == kScnRootStockObjects) {
            ctx->gpr[6] = kScnRootStockObjects * players / kScnStockPlayers;
        }
    }
    MKW_GUEST_NEXT(8006F1A0)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8006F1A0, ScnRoot_Construct_8006f1a0, "ScnRoot_Construct_8006f1a0");

// A model a full group turned away is never drawn (an item on the ground can then be invisible
// yet still collide); counted in the log so a full group shows up.
extern "C" void ScnGroup_Insert_8006eb60(CpuContext* ctx) {
    MKW_GUEST_NEXT(8006EB60)(ctx);
    static uint32_t failed = 0;
    if (ctx->gpr[3] == 0 && (failed++ % 1000) == 0) {
        RT_LOGF("scene", "a model could not join its scene group (group full); %u so far\n", failed);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x8006EB60, ScnGroup_Insert_8006eb60, "ScnGroup_Insert_8006eb60");
