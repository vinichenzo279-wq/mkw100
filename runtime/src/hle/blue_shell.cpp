// mkw100: the blue shell's target past 12 racers.
//
// Item::ObjKouraTogezo::CalcTargetedPlayerId (0x807AC1A8) picks whom a blue shell flies to. Its
// thrower is the shell's owner (+0x6C), or 12, "nobody" in a 12-racer race, for a shell without one
// (+0x78 bit 0x8000 set). In normal VS (neither battle, 0x809C38BB, nor team VS, 0x809C38BD) it walks the
// ranking (Raceinfo +0x18, player ids by place) and takes the first racer who has not finished
// (RaceinfoPlayer +0x38 bit 0x2; the shell's +0x2ED "retargeted" flag lets finished racers count),
// skipping the thrower unless the thrower leads. Past 12 racers, racer 12 is a real racer, so a
// shell nobody threw skipped racer 12 whenever racer 12 was the best-placed racer still racing but
// not first. Here that case runs natively with no thrower at all; with nobody left racing it takes
// the leader, a real racer (the game's 12 was one past the field). Owned shells, battle, team VS
// and races of 12 or fewer run the game's code.
//
// Item Fever replaces the function's entry (overlay: a branch to 0x8184C65C): its special shells
// (registered at 0x818BC290, the purple shell's targeting) keep a target of their own, and every
// other shell calls the game's function through a trampoline (0x8184C654: the first instruction,
// then a branch to 0x807AC1AC) before Item Fever's item features adjust the result. So under Item
// Fever the entry runs Item Fever's code, and the same fix sits on the trampoline instead. Item
// Fever's own `racer < 12` checks in that code are raised in array-sites.txt (cmpimm).
//
// Not changed: after picking, the shell keeps a u16 mask of the racers it targeted (+0x2F4, 1 << id)
// and, each frame, explodes next to an earlier target that has finished; that check loops over
// racers 0..11 only, so for earlier targets 12+ it never fires, which loses nothing else.

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "hle/mod_build.h"
#include "memory.h"
#include "runtime_log.h"

#include <cstdint>

namespace {

constexpr uint32_t kItemPlayerCount = 0x809C38B8;  // u8
constexpr uint32_t kItemBattle = 0x809C38BB;       // u8: battle (targets the best score)
constexpr uint32_t kItemTeams = 0x809C38BD;        // u8: team VS (skips the thrower's team)
constexpr uint32_t kRaceinfo = 0x809BD730;         // Raceinfo*
constexpr uint32_t kRaceinfoPlayers = 0xC;         // RaceinfoPlayer** by player id
constexpr uint32_t kRaceinfoRanking = 0x18;        // u8* player id by place - 1
constexpr uint32_t kPlayerState = 0x38;            // RaceinfoPlayer: bit 0x2 finished
constexpr uint32_t kShellFlags = 0x78;             // bit 0x8000: no owner
constexpr uint32_t kShellRetargeted = 0x2ED;       // u8

bool InRam(uint32_t a) { return a >= 0x80000000u && a < 0x98000000u; }

// The target of a blue shell nobody threw, in normal VS past 12 racers; false for every other case
// (the game's code decides those).
bool UnownedTarget(uint32_t shell, uint32_t& target) {
    const uint32_t count = Memory::Read8(kItemPlayerCount);
    if (count <= 12 || (Memory::Read32(shell + kShellFlags) & 0x8000) == 0) return false;  // thrown: the game's
    if (Memory::Read8(kItemBattle) != 0 || Memory::Read8(kItemTeams) != 0) return false;
    const uint32_t raceinfo = Memory::Read32(kRaceinfo);
    if (!InRam(raceinfo)) return false;
    const uint32_t players = Memory::Read32(raceinfo + kRaceinfoPlayers);
    const uint32_t ranking = Memory::Read32(raceinfo + kRaceinfoRanking);
    if (!InRam(players) || !InRam(ranking)) return false;
    const bool retargeted = Memory::Read8(shell + kShellRetargeted) != 0;
    target = Memory::Read8(ranking);  // the leader when nobody is left racing
    uint32_t targetPlace = 1;
    for (uint32_t place = 1; place <= count; ++place) {
        const uint32_t id = Memory::Read8(ranking + place - 1);
        const uint32_t player = Memory::Read32(players + 4 * id);
        if (!InRam(player)) continue;
        if ((Memory::Read32(player + kPlayerState) & 0x2) != 0 && !retargeted) continue;
        target = id;
        targetPlace = place;
        break;
    }
    static uint32_t logged = 0;
    if (logged < 8) {
        ++logged;
        RT_LOGF("blue-shell", "a blue shell nobody threw targets racer %u (place %u of %u)\n", target, targetPlace, count);
    }
    return true;
}

} // namespace

extern "C" void ObjKouraTogezo_CalcTargetedPlayerId_807ac1a8(CpuContext* ctx) {
    uint32_t target = 0;
    if (ModBuild::IsKnownItemFever() || !UnownedTarget(ctx->gpr[3], target)) {
        MKW_GUEST_NEXT(807AC1A8)(ctx);
        return;
    }
    ctx->gpr[3] = target;
}
REGISTER_NATIVE_WRAPPER_AS(0x807AC1A8, ObjKouraTogezo_CalcTargetedPlayerId_807ac1a8,
                           "ObjKouraTogezo_CalcTargetedPlayerId_807ac1a8");

// Item Fever's trampoline into the game's CalcTargetedPlayerId (r3: the shell).
extern "C" void ItemFever_CalcTargetedPlayerIdStock_8184c654(CpuContext* ctx) {
    uint32_t target = 0;
    if (!ModBuild::IsKnownItemFever() || !UnownedTarget(ctx->gpr[3], target)) {
        MKW_GUEST_NEXT(8184C654)(ctx);
        return;
    }
    ctx->gpr[3] = target;
}
REGISTER_NATIVE_WRAPPER_AS(0x8184C654, ItemFever_CalcTargetedPlayerIdStock_8184c654,
                           "ItemFever_CalcTargetedPlayerIdStock_8184c654");
