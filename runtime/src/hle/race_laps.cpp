// mkw100: the number of laps of offline GP and VS races ([local_vs] laps, F10 "Laps", --race-laps).
//
// The race scenario has a lap count (RacedataSettings +0x25, 3 from the menu), but the game reads it
// only when the race's mode flags have bit 0x4 (the competition bit; about 50 other functions test
// it: UI, objects, HUD); otherwise it uses a fixed 3. The places that pick between the two:
//  - Raceinfo::__ct (0x805327A0) passes the count to RaceinfoPlayer's constructor (0x80533ED8, r5);
//  - a getter (0x805336A4), used by the HUD, the AI and others;
//  - four functions with the same choice inline (0x80532F88, 0x805342E8, 0x805349B8, 0x80534CCC),
//    whose callees do not test the bit.
// Here the constructor gets the count in r5, the getter returns it, and the four inline ones run
// with the bit set and the count stored, the bit cleared again afterwards. The constructor only
// accepts 1..9, so the setting goes to 9. Base game only (see RaceLaps_Wanted).

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "local_vs.h"
#include "memory.h"
#include "runtime_product.h"
#include "runtime_log.h"

#include <cstdint>

namespace {
constexpr uint32_t kRaceLapsRacedata = 0x809BD728;
constexpr uint32_t kRaceLapsScenario = 0x20;           // race scenario in Racedata
constexpr uint32_t kRaceLapsGameMode = 0xB48 + 0x08;   // 0 GP, 1 VS
constexpr uint32_t kRaceLapsGameType = 0xB48 + 0x0C;   // 0 a race (5: course intro)
constexpr uint32_t kRaceLapsCount = 0xB48 + 0x25;
constexpr uint32_t kRaceLapsModeFlags = 0xB48 + 0x28;
constexpr uint32_t kRaceLapsCompetition = 0x4;

// The laps for this race, or 0 to leave the game alone.
uint32_t RaceLaps_Wanted(uint32_t* race = nullptr) {
    const int n = LocalVs::Laps();
    // Base game only: in Retro Rewind (and Item Fever, the same Pulsar loader) a 2-lap race was won
    // in 0:52 against 2:14 for 3 laps, so Pulsar's own lap code disagrees with these hooks.
    if (n <= 0 || RuntimeProduct::IsRetroRewind()) return 0;
    const uint32_t rd = Memory::Read32(kRaceLapsRacedata);
    if (rd == 0) return 0;
    const uint32_t scenario = rd + kRaceLapsScenario;
    if (Memory::Read32(scenario + kRaceLapsGameMode) > 1 || Memory::Read32(scenario + kRaceLapsGameType) != 0) return 0;
    if (race) *race = scenario;
    return uint32_t(n);
}

// Runs an inline reader with the competition bit set and the count stored.
template <typename Next>
void RaceLaps_WithCount(CpuContext* ctx, Next next) {
    uint32_t race = 0;
    const uint32_t n = RaceLaps_Wanted(&race);
    if (n == 0) {
        next(ctx);
        return;
    }
    const uint32_t flags = Memory::Read32(race + kRaceLapsModeFlags);
    Memory::Write8(race + kRaceLapsCount, uint8_t(n));
    Memory::Write32(race + kRaceLapsModeFlags, flags | kRaceLapsCompetition);
    next(ctx);
    Memory::Write32(race + kRaceLapsModeFlags, flags);
}
}  // namespace

extern "C" void RaceinfoPlayer_ct_80533ed8(CpuContext* ctx) {
    if (const uint32_t n = RaceLaps_Wanted()) {
        static uint32_t logged = 0;
        if (logged != n) {
            logged = n;
            RT_LOGF("race-laps", "%u laps\n", n);
        }
        ctx->gpr[5] = n;
    }
    MKW_GUEST_NEXT(80533ED8)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80533ED8, RaceinfoPlayer_ct_80533ed8, "RaceinfoPlayer_ct_80533ed8");

extern "C" void Raceinfo_GetLapCount_805336a4(CpuContext* ctx) {
    if (const uint32_t n = RaceLaps_Wanted()) {
        ctx->gpr[3] = n;
        return;
    }
    MKW_GUEST_NEXT(805336A4)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x805336A4, Raceinfo_GetLapCount_805336a4, "Raceinfo_GetLapCount_805336a4");

extern "C" void RaceLaps_80532f88(CpuContext* ctx) {
    RaceLaps_WithCount(ctx, MKW_GUEST_NEXT(80532F88));
}
REGISTER_NATIVE_WRAPPER_AS(0x80532F88, RaceLaps_80532f88, "RaceLaps_80532f88");

extern "C" void RaceLaps_805342e8(CpuContext* ctx) {
    RaceLaps_WithCount(ctx, MKW_GUEST_NEXT(805342E8));
}
REGISTER_NATIVE_WRAPPER_AS(0x805342E8, RaceLaps_805342e8, "RaceLaps_805342e8");

extern "C" void RaceLaps_805349b8(CpuContext* ctx) {
    RaceLaps_WithCount(ctx, MKW_GUEST_NEXT(805349B8));
}
REGISTER_NATIVE_WRAPPER_AS(0x805349B8, RaceLaps_805349b8, "RaceLaps_805349b8");

extern "C" void RaceLaps_80534ccc(CpuContext* ctx) {
    RaceLaps_WithCount(ctx, MKW_GUEST_NEXT(80534CCC));
}
REGISTER_NATIVE_WRAPPER_AS(0x80534CCC, RaceLaps_80534ccc, "RaceLaps_80534ccc");
