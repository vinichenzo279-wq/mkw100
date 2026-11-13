// mkw100 race oracle: the regression instrument for the N-player engine rewrite.
//
// Active only with `--race-cpu`, whose racers are all CPU driven with fixed RNG seeds
// (boot_race.cpp), so one build and course must give one result every run. After every
// Raceinfo::Update it folds each racer's position, lap, race completion and state flags into
// a running FNV-1a hash (the trajectory), printed every kCheckpointFrames so a divergence can
// be bisected to a frame window. When the race reaches RACESTAGE_FINISHED it prints every
// racer's place and finish time, a hash of those (the result) and the final trajectory hash,
// then exits 0. A race that never finishes, or that restarts without a finish, exits 2.
//
// Layouts from Pulsar's GameSource (Raceinfo.hpp, Racedata.hpp, Timer.hpp), PAL addresses.

#include "abi_bridge.h"
#include "boot_race.h"
#include "guest_abi/guest_region.h"
#include "guest_function_imports.h"
#include "hle_stubs.h"
#include "memory.h"
#include "runtime_exit.h"
#include "runtime_log.h"
#include "local_vs.h"
#include "hle/teams.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

void TestProfiler_Start();  // test_profiler.cpp
void TestProfiler_Dump();

namespace {

// Raceinfo (sInstance 809bd730), size 0x4c
constexpr uint32_t kRaceinfoPlayers = 0x0C;   // RaceinfoPlayer** indexed by player id
constexpr uint32_t kRaceinfoFrames = 0x20;    // u32 race frames
constexpr uint32_t kRaceinfoStage = 0x28;     // RaceStage
constexpr uint32_t kStageRace = 2;
constexpr uint32_t kStageFinished = 4;

// RaceinfoPlayer, size 0x54
constexpr uint32_t kPlayerCompletion = 0x0C;  // f32 race completion
constexpr uint32_t kPlayerPosition = 0x20;    // u8, 1 is first
constexpr uint32_t kPlayerLap = 0x24;         // u16
constexpr uint32_t kPlayerStateFlags = 0x38;  // u32
constexpr uint32_t kPlayerFinishTime = 0x40;  // Timer*

// Timer, size 0xc
constexpr uint32_t kTimerMinutes = 0x4;       // u16
constexpr uint32_t kTimerSeconds = 0x6;       // u8
constexpr uint32_t kTimerMilliseconds = 0x8;  // u16
constexpr uint32_t kTimerActive = 0xA;        // bool

// Racedata (sInstance 809bd728): race scenario at +0x20, its player count at +0x4.
constexpr uint32_t kRacedataRaceScenarioPlayerCount = 0x20 + 0x4;

// 600 frames is 10 s of race: fine enough to bisect, few enough lines to read.
constexpr uint32_t kCheckpointFrames = 600;
// Diagnostic: print every racer's raw state for the first kTraceFrames race frames, to find
// the first frame two runs disagree on. 0 in normal use.
constexpr uint32_t kTraceFrames = 0;
// Diagnostic: sleep this long in every race frame to simulate a host too slow for 60 fps, the
// situation 100 karts will create. At 40000 both hashes matched the baseline with and without
// --lockstep (10.11.2026). 0 in normal use.
constexpr uint32_t kHostDelayMicros = 0;

// VI retrace counter (vi.cpp kViRetraceCountAddr). Without --lockstep guest time is wall-clock
// driven, so a race frame the host produced late spans 2+ retraces: a lag frame, logged and
// counted. Lag does not change this race: with kHostDelayMicros 40000 and no lockstep, 6,015 of
// 6,255 race frames lagged (mostly 3 retraces each) and both hashes matched the baseline
// (M5 Pro, 10.11.2026). The race simulation steps once per frame, not once per retrace. The
// count stays as a check on --lockstep, which must report 0. (An earlier divergence blamed on
// lag was a missing save file: a fresh NAND has no rksys.dat and the boot race goes elsewhere.)
const uint32_t kRetraceCountAddr = MKW_GADDR(80386BE4);
// Luigi Circuit at 150cc finishes in about 2 min; 10 min means the race is stuck.
constexpr uint32_t kFrameLimit = 60 * 60 * 10;

constexpr uint64_t kFnvOffset = 0xCBF29CE484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001B3ull;

void Fold(uint64_t& hash, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        hash ^= (value >> shift) & 0xFF;
        hash *= kFnvPrime;
    }
}

uint32_t FloatBits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

uint64_t g_trajectory = kFnvOffset;
uint32_t g_framesSeen = 0;
uint32_t g_lastRetrace = 0;
uint32_t g_lagFrames = 0;
uint32_t g_raceinfo = 0;
bool g_done = false;

uint32_t PlayerCount() {
    const uint32_t racedata = Memory::Read32(MKW_GADDR(809BD728));
    return racedata ? Memory::Read8(racedata + kRacedataRaceScenarioPlayerCount) : 0;
}

void Finish(uint32_t raceinfo, uint32_t playerCount) {
    const uint32_t players = Memory::Read32(raceinfo + kRaceinfoPlayers);
    uint64_t result = kFnvOffset;
    Fold(result, playerCount);
    std::printf("[race-oracle] finished at race frame %u, %u racers\n",
                Memory::Read32(raceinfo + kRaceinfoFrames), playerCount);
    for (uint32_t id = 0; id < playerCount; ++id) {
        const uint32_t player = Memory::Read32(players + id * 4);
        const uint32_t position = Memory::Read8(player + kPlayerPosition);
        const uint32_t timer = Memory::Read32(player + kPlayerFinishTime);
        const bool timed = timer && Memory::Read8(timer + kTimerActive);
        const uint32_t minutes = timed ? Memory::Read16(timer + kTimerMinutes) : 0;
        const uint32_t seconds = timed ? Memory::Read8(timer + kTimerSeconds) : 0;
        const uint32_t milliseconds = timed ? Memory::Read16(timer + kTimerMilliseconds) : 0;
        Fold(result, id);
        Fold(result, position);
        Fold(result, timed ? (minutes * 60000 + seconds * 1000 + milliseconds) : 0xFFFFFFFFu);
        if (timed) {
            std::printf("[race-oracle] racer %2u place %2u time %u:%02u.%03u\n", id, position, minutes, seconds,
                        milliseconds);
        } else {
            std::printf("[race-oracle] racer %2u place %2u time none\n", id, position);
        }
    }
    if (Teams::Active()) {
        std::vector<std::pair<uint32_t, uint32_t>> points;
        for (uint32_t id = 0; id < playerCount; ++id) {
            const uint32_t position = Memory::Read8(Memory::Read32(players + id * 4) + kPlayerPosition);
            points.emplace_back(id, LocalVs::PointsFor(position, playerCount));
        }
        Teams::Log("race", points);
    }
    std::printf("[race-oracle] lag frames %u of %u\n", g_lagFrames, g_framesSeen);
    std::printf("[race-oracle] result %016llx trajectory %016llx\n", static_cast<unsigned long long>(result),
                static_cast<unsigned long long>(g_trajectory));
    std::fflush(stdout);
    std::fflush(stderr);
    // MKW100_TEST_RESULTS=<seconds>: keep running past the finish so the results and standings
    // pages are exercised too (the scripted driver taps A through them); a crash there fails the
    // case. Then exit as usual.
    if (const char* stay = std::getenv("MKW100_TEST_RESULTS"); stay && std::atoi(stay) > 0) {
        const int seconds = std::atoi(stay);
        std::printf("[race-oracle] staying %d s for the results pages\n", seconds);
        std::fflush(stdout);
        std::thread([seconds] {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            std::printf("[race-oracle] results pages survived %d s\n", seconds);
            RuntimeFlushAndExit(EXIT_SUCCESS);
        }).detach();
        return;
    }
    // Skips teardown of the audio and GX worker threads, which the oracle does not need.
    RuntimeFlushAndExit(EXIT_SUCCESS);
}

} // namespace

// Raceinfo::Update
extern "C" void Raceinfo_Update_805331b4(CpuContext* ctx) {
    const uint32_t raceinfo = ctx->gpr[3];
    MKW_GUEST_NEXT(805331B4)(ctx);
    LocalVs::TestItems(ctx, raceinfo);
    LocalVs::PollFinishNow(ctx);
    LocalVs::AdaptiveFramerate(ctx);
    if (!BootRace::Scripted() || g_done) {
        return;
    }
    // Boot and loading keep wall-clock retraces (they wait on retraces before any frame is
    // presented); lockstep takes over from the race scene's first update, before the countdown.
    if (BootRace::Lockstep()) {
        VI_HLE_EnableLockstep();
    }
    const uint32_t stage = Memory::Read32(raceinfo + kRaceinfoStage);
    // A second race, or this one rewinding, means the finish was never observed: with lag the
    // game was seen to leave the race scene and start another without Update reporting stage 4.
    if (g_framesSeen > 0 && (raceinfo != g_raceinfo || stage < kStageRace)) {
        g_done = true;
        std::printf("[race-oracle] FAIL: race restarted at race frame %u without a finish, lag %u\n",
                    g_framesSeen, g_lagFrames);
        RuntimeFlushAndExit(2);
    }
    if (stage < kStageRace) {
        return;
    }
    g_raceinfo = raceinfo;
    static const bool performanceTest = std::getenv("MKW100_TEST_PERF") != nullptr;
    static const auto performanceStart = std::chrono::steady_clock::now();
    const uint32_t retrace = Memory::Read32(kRetraceCountAddr);
    if (g_framesSeen > 0 && retrace - g_lastRetrace != 1) {
        ++g_lagFrames;
        std::printf("[race-oracle] lag at race frame %u stage %u: %u retraces\n", g_framesSeen, stage,
                    retrace - g_lastRetrace);
    }
    g_lastRetrace = retrace;
    const uint32_t playerCount = PlayerCount();
    const uint32_t players = Memory::Read32(raceinfo + kRaceinfoPlayers);
    for (uint32_t id = 0; id < playerCount; ++id) {
        const uint32_t player = Memory::Read32(players + id * 4);
        Fold(g_trajectory, Memory::Read8(player + kPlayerPosition));
        Fold(g_trajectory, Memory::Read16(player + kPlayerLap));
        Fold(g_trajectory, FloatBits(Memory::ReadFloat32(player + kPlayerCompletion)));
        Fold(g_trajectory, Memory::Read32(player + kPlayerStateFlags));
        if (g_framesSeen < kTraceFrames) {
            std::printf("[race-trace] f%u r%u pos %u lap %u comp %08x flags %08x\n", g_framesSeen, id,
                        Memory::Read8(player + kPlayerPosition), Memory::Read16(player + kPlayerLap),
                        FloatBits(Memory::ReadFloat32(player + kPlayerCompletion)),
                        Memory::Read32(player + kPlayerStateFlags));
        }
    }
    if (g_framesSeen < kTraceFrames) {
        std::printf("[race-trace] f%u raceFrames %u stage %u\n", g_framesSeen,
                    Memory::Read32(raceinfo + kRaceinfoFrames), stage);
    }
    ++g_framesSeen;
    if (performanceTest && g_framesSeen == 120) TestProfiler_Start();  // past the race start
    if (performanceTest && g_framesSeen==1200) {
        TestProfiler_Dump();
        const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-performanceStart).count();
        std::printf("[race-perf] frames=%u seconds=%.3f simulation_hz=%.2f trajectory=%016llx\n",
            g_framesSeen,seconds,g_framesSeen/seconds,static_cast<unsigned long long>(g_trajectory));
        RuntimeFlushAndExit(0);
    }
    if (kHostDelayMicros != 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(kHostDelayMicros));
    }
    if (g_framesSeen % kCheckpointFrames == 0) {
        std::printf("[race-oracle] frame %u trajectory %016llx lag %u\n", g_framesSeen,
                    static_cast<unsigned long long>(g_trajectory), g_lagFrames);
        std::fflush(stdout);
    }
    if (stage >= kStageFinished) {
        g_done = true;
        Finish(raceinfo, playerCount);
    }
    if (g_framesSeen >= kFrameLimit) {
        g_done = true;
        std::printf("[race-oracle] FAIL: no finish after %u frames, stage %u\n", g_framesSeen, stage);
        std::fflush(stdout);
        RuntimeFlushAndExit(2);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x805331B4, Raceinfo_Update_805331b4, "Raceinfo_Update_805331b4");

// MKW100_TEST_RESULTS: log every page activation and section change, to see which screens a
// headless race reaches after its finish.
namespace {
bool ResultsTrace() {
    static const bool on = std::getenv("MKW100_TEST_RESULTS") != nullptr;
    return on;
}
} // namespace

// Page::Activate(this). A page that cannot be activated (null, or its vtable or input manager's not
// code) is skipped and the caller logged. (The 120-racer battle crash here was a clobbered r29 from
// the vanilla UpdatePoints' stack array, fixed in local_vs.cpp; this check catches such a page only
// when it is already broken on entry.)
namespace {
bool GuestCode(uint32_t a) { return a >= 0x80000000 && a < 0x81800000; }
bool GuestData(uint32_t a) {
    return (a >= 0x80000000 && a < 0x81800000) || (a >= 0x90000000 && a < 0x98000000);
}
// A page Activate can run: its vtable and its input manager's vtable (called at +0x28) are code.
bool ActivatablePage(uint32_t page) {
    if (!GuestData(page) || !GuestCode(Memory::Read32(page))) return false;
    const uint32_t input = Memory::Read32(page + 0x38);
    if (input == 0) return true;
    if (!GuestData(input)) return false;
    const uint32_t vt = Memory::Read32(input);
    return GuestCode(vt) && GuestCode(Memory::Read32(vt + 0x28));
}
} // namespace

extern "C" void RaceOracle_PageActivate_80601aec(CpuContext* ctx) {
    if (!ActivatablePage(ctx->gpr[3])) {
        static uint32_t logged = 0;
        const uint32_t page = ctx->gpr[3];
        if (logged++ < 8) {
            RT_LOGF("pages", "skipped activating a broken page 0x%08X (id 0x%X, input 0x%08X, lr=0x%08X)\n", page,
                    GuestData(page) ? Memory::Read32(page + 4) : 0, GuestData(page) ? Memory::Read32(page + 0x38) : 0,
                    ctx->lr);
        }
        return;
    }
    if (ResultsTrace()) {
        std::printf("[race-oracle] page 0x%02X activated (0x%08X, input 0x%08X, lr 0x%08X)\n",
                    Memory::Read32(ctx->gpr[3] + 4), ctx->gpr[3], Memory::Read32(ctx->gpr[3] + 0x38), ctx->lr);
        std::fflush(stdout);
    } else if (const uint32_t rd = Memory::Read32(0x809BD728); rd != 0 && Memory::Read8(rd + 0x24) > 12) {
        // Past 12 racers every page change is logged, to place a stuck screen (battle results).
        RT_LOGF("pages", "page 0x%02X activated\n", Memory::Read32(ctx->gpr[3] + 4));
    }
    MKW_GUEST_NEXT(80601AEC)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80601AEC, RaceOracle_PageActivate_80601aec, "RaceOracle_PageActivate_80601aec");

// SectionMgr::SetNextSection(this, id, animDirection)
extern "C" void RaceOracle_SetNextSection_80635a3c(CpuContext* ctx) {
    if (ResultsTrace()) std::printf("[race-oracle] next section 0x%02X\n", ctx->gpr[4]);
    MKW_GUEST_NEXT(80635A3C)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80635A3C, RaceOracle_SetNextSection_80635a3c, "RaceOracle_SetNextSection_80635a3c");
