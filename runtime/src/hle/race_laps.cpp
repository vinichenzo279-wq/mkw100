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
// with the bit set and the count stored, the bit cleared again afterwards. The constructor takes
// any count (it allocates a split time per lap), so the setting goes to 99. Base game only (see
// RaceLaps_Wanted).

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "local_vs.h"
#include "rr_ko_modes.h"
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
    } else if (RuntimeProduct::IsRetroRewind()) {
        if (const uint32_t laps = RrKoModes::LapKoLaps(ctx)) ctx->gpr[5] = laps;
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

// CtrlRaceLap (the HUD lap counter, Init 0x807EF6E8, OnUpdate 0x807EF810) sets the frames of its two
// number animations (this lap, and the race's laps) through 0x8063CB20; the frame is the number. The
// game's lap_number has the digits 0..9 only, so past 9 laps a frame past them read whatever followed
// (garbage texture: an unmapped read, then a panic). Retro Rewind's two-digit RRp_number (chosen past
// 9 laps, the name at 0x808A9CC7) goes to 50. The frame is kept within the counter's numbers: past 9
// laps the game's counter shows 9.
// Called by the one wrapper of 0x8063CB20 (team_ui.cpp) before the game's function.
void RaceLaps_ClampLapCounterFrame(CpuContext* ctx) {
    const uint32_t lr = ctx->lr;
    if (lr == 0x807EF768 || lr == 0x807EF7A0 || lr == 0x807EF8B8) {
        // 'R': Retro Rewind's RRp_number; 'f': Formula Kart Wii's fap_number (it writes the 'f'
        // over lap_number's 'l' and draws two digits itself, up to its 50 laps).
        const uint8_t counter = Memory::Read8(0x808A9CC7);
        const bool twoDigit = counter == 'R' || counter == 'f';
        const double most = twoDigit ? 50.0 : 9.0;
        if (ctx->fpr[1].d > most) ctx->fpr[1].d = most;
    }
}
