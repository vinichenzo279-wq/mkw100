// mkw100: "Impossible" CPUs, a difficulty above Hard (F10 > Local VS, or --race-impossible).
//
// AI::Params::Load (0x8073ACA8) picks the CPU parameters from two files in Race/Common.szs by
// the race's cpuMode (settings +0x14, 0 Easy, 1 Normal, 2 Hard; 3 is CPU_NONE, so no 4th mode):
//  - kartAISpdParam.bin, 3 floats per entry, copied to *(params +4). GP: course * 4 + mode (the
//    4th column is Mirror); VS: 0x80 + engine * 96 + course * 3 + mode. The first float steps up
//    with difficulty (VS 150cc: -3 / 2 / 5, GP: -18 / -8 / 0 / Mirror 2); the second (1.2 / 1.1
//    / 1.0) and third (per engine, 30 / 40 / 50) are left alone.
//  - kartAIActionParam.bin, 3 modes x 12 race positions x 8 chances (0..100: drift, mini-turbo,
//    trick, wheelie, ...), stored by AI::Params::LoadActions (0x8073B020) as bytes at
//    *(params +8). Hard 1st place: 100 0 100 100 90 75 70 70; 12th: 10 0 20 50 40 0 0 0.
//
// With the setting on and the race on Hard, after the game's Load the first speed float gets
// kSpeedStep added and every non-zero chance becomes 100 (zero stays zero: those actions are off
// for Hard too). Measured, headless all-CPU 12-racer VS race (oracle seed): Very Hard (+10) 1:27.464,
// Impossible (+40) 1:09.099,
// against 1:38.440 on Hard, race over at frame 5,530 against 6,255.

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "local_vs.h"
#include "memory.h"

#include <cstdint>
#include <cstdlib>

namespace {

constexpr uint32_t kImpossibleRacedata = 0x809BD728;  // RaceConfig::spInstance
constexpr uint32_t kImpossibleCpuMode = 0x20 + 0xB48 + 0x14;  // races scenario settings cpuMode
constexpr uint32_t kImpossibleHard = 2;
// Hard - Normal is 3 to 10 across the tables. Very Hard is 10 above Hard; Impossible, MKW100_TEST_CPU_STEP
// for tuning, is kImpossibleSpeedStep above.
constexpr float kVeryHardSpeedStep = 10.0f;
constexpr float kImpossibleSpeedStep = 40.0f;
constexpr uint32_t kImpossibleActionBytes = 12 * 8;

} // namespace

extern "C" void AIParams_Load_8073aca8(CpuContext* ctx) {
    const uint32_t params = ctx->gpr[3];
    MKW_GUEST_NEXT(8073ACA8)(ctx);
    if (!LocalVs::ImpossibleCpus() || params == 0) return;
    const uint32_t racedata = Memory::Read32(kImpossibleRacedata);
    if (racedata == 0 || Memory::Read32(racedata + kImpossibleCpuMode) != kImpossibleHard) return;
    static const char* const tuned = std::getenv("MKW100_TEST_CPU_STEP");
    const float step = LocalVs::CpuTier() >= 2 ? (tuned ? float(std::atof(tuned)) : kImpossibleSpeedStep) : kVeryHardSpeedStep;
    const uint32_t speed = Memory::Read32(params + 0x4);
    if (speed != 0) Memory::WriteFloat32(speed, Memory::ReadFloat32(speed) + step);
    const uint32_t actions = Memory::Read32(params + 0x8);
    if (actions == 0) return;
    for (uint32_t i = 0; i < kImpossibleActionBytes; ++i) {
        if (Memory::Read8(actions + i) != 0) Memory::Write8(actions + i, 100);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x8073ACA8, AIParams_Load_8073aca8, "AIParams_Load_8073aca8");
