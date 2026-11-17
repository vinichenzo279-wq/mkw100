// mkw100: less per-frame work for CPU racers far from the camera, in races of more than 12.
//
// Two parts, both from the camera distance the game already computes every frame for each kart
// (ClipInfoMgr::Update, 0x80787774: the kart's ClipInfo, Kart::Player +0x6C, holds the squared
// distance to each screen's camera at +0xC + 4 * screen):
//  - sound: Audio::KartActor::Update (0x807076D0), the per-kart engine, road, drift and collision
//    sound logic, runs only for the nearest kKeepSound CPU racers (and every human). The others'
//    held sounds (engine, road) stop by themselves once nobody holds them; one-shot sounds already
//    playing finish. At 128 racers the race sound logic plus the sound thread were about 11% of the
//    game thread.
//  - drawing: a CPU kart's ClipInfo far distance (+0x8, squared; ClipInfo::Init 0x80787C24) is
//    lowered to the configured distance, so the game's own clipping (the same flags that hide karts
//    behind the camera) stops drawing it beyond that.
// Neither changes the simulation: a race takes the same course with it on or off.
//
// [video] far_cpu_distance: 0 off; the draw distance in course units otherwise. Default 0 (off).
// MKW100_TEST_FAR_CPU=<distance> overrides it, MKW100_TEST_FAR_PARTS=sound / draw limits it to one
// part, MKW100_TEST_FAR_LOG prints the CPU camera distances every 10 s.

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" uint32_t MKW100_PlayerSlotOffset(uint32_t index);

namespace {

constexpr uint32_t FarCpu_Racedata = 0x809BD728;      // RaceConfig::spInstance
constexpr uint32_t FarCpu_RacerCount = 0x24;          // u8
constexpr uint32_t FarCpu_PlayerType = 0x38;          // per scenario player: 1 CPU
constexpr uint32_t FarCpu_KartManager = 0x809C18F8;   // Kart::Manager*: Kart::Player** at +0x20, u8 count at +0x24
constexpr uint32_t FarCpu_PlayerClip = 0x6C;          // Kart::Player: ClipInfo*
constexpr uint32_t FarCpu_ClipFar = 0x8;              // ClipInfo: squared far distance
constexpr uint32_t FarCpu_ClipDistance = 0xC;         // ClipInfo: squared camera distance, screen 0
constexpr uint32_t FarCpu_ActorLink = 0xDC;           // Audio::KartActor: Kart::Link (accessor at +0)
constexpr uint32_t FarCpu_Racers = 256;
constexpr uint32_t FarCpu_KeepSound = 12;

bool FarCpu_InRam(uint32_t a) { return (a >= 0x80000000u && a < 0x81800000u) || (a >= 0x90000000u && a < 0x94000000u); }

struct FarCpuConfig {
    float distance = 0;
    bool sound = false, draw = false, log = false;
};

const FarCpuConfig& FarCpu_Settings() {
    static const FarCpuConfig c = [] {
        FarCpuConfig c;
        const char* forced = std::getenv("MKW100_TEST_FAR_CPU");
        c.distance = forced ? float(std::atof(forced)) : float(RuntimeConfigFile::FarCpuDistance(0));
        const char* parts = std::getenv("MKW100_TEST_FAR_PARTS");
        c.sound = c.distance > 0 && (parts == nullptr || std::strstr(parts, "sound") != nullptr);
        c.draw = c.distance > 0 && (parts == nullptr || std::strstr(parts, "draw") != nullptr);
        c.log = std::getenv("MKW100_TEST_FAR_LOG") != nullptr;
        return c;
    }();
    return c;
}

bool g_farCpuSoundOn[FarCpu_Racers];   // this frame: whether the racer's KartActor runs
uint32_t g_farCpuFrame = 0;

}  // namespace

// Kart::Manager::Update (0x8058FFE8), once a frame before the karts: which CPU racers keep their
// sound logic, and the far distance of their clipping.
extern "C" void FarCpu_KartManagerUpdate_8058ffe8(CpuContext* ctx) {
    const FarCpuConfig& c = FarCpu_Settings();
    const uint32_t mgr = ctx->gpr[3];
    const uint32_t rd = Memory::Read32(FarCpu_Racedata);
    if ((c.distance > 0 || c.log) && FarCpu_InRam(mgr) && FarCpu_InRam(rd) && Memory::Read8(rd + FarCpu_RacerCount) > 12) {
        const uint32_t players = Memory::Read32(mgr + 0x20);
        const uint32_t count = std::min<uint32_t>(Memory::Read8(mgr + 0x24), FarCpu_Racers);
        float d2[FarCpu_Racers];
        uint32_t cpus = 0;
        float sorted[FarCpu_Racers];
        for (uint32_t i = 0; i < count; ++i) {
            g_farCpuSoundOn[i] = true;
            d2[i] = -1;
            const uint32_t player = Memory::Read32(players + 4 * i);
            const uint32_t clip = FarCpu_InRam(player) ? Memory::Read32(player + FarCpu_PlayerClip) : 0;
            if (!FarCpu_InRam(clip) || Memory::Read32(rd + MKW100_PlayerSlotOffset(i) + FarCpu_PlayerType) != 1) continue;
            d2[i] = Memory::ReadFloat32(clip + FarCpu_ClipDistance);
            sorted[cpus++] = d2[i];
            if (c.draw) Memory::WriteFloat32(clip + FarCpu_ClipFar, c.distance * c.distance);
        }
        if (c.sound && cpus > FarCpu_KeepSound) {
            std::nth_element(sorted, sorted + FarCpu_KeepSound - 1, sorted + cpus);
            const float limit = sorted[FarCpu_KeepSound - 1];
            for (uint32_t i = 0; i < count; ++i) g_farCpuSoundOn[i] = d2[i] < 0 || d2[i] <= limit;
        }
        if (c.log && cpus > 0 && ++g_farCpuFrame % 600 == 0) {
            std::sort(sorted, sorted + cpus);
            RT_LOGF("far-cpu", "CPU camera distances: min %.0f p25 %.0f median %.0f p75 %.0f max %.0f (%u CPUs)\n",
                    std::sqrt(sorted[0]), std::sqrt(sorted[cpus / 4]), std::sqrt(sorted[cpus / 2]),
                    std::sqrt(sorted[cpus * 3 / 4]), std::sqrt(sorted[cpus - 1]), cpus);
        }
    } else {
        std::fill(std::begin(g_farCpuSoundOn), std::end(g_farCpuSoundOn), true);
    }
    MKW_GUEST_NEXT(8058FFE8)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8058FFE8, FarCpu_KartManagerUpdate_8058ffe8, "FarCpu_KartManagerUpdate_8058ffe8");

// Audio::KartActor::Update (this): the racer is Link accessor -> settings +0x10.
extern "C" void FarCpu_KartActorUpdate_807076d0(CpuContext* ctx) {
    if (FarCpu_Settings().sound) {
        const uint32_t accessor = Memory::Read32(ctx->gpr[3] + FarCpu_ActorLink);
        const uint32_t settings = FarCpu_InRam(accessor) ? Memory::Read32(accessor) : 0;
        if (FarCpu_InRam(settings)) {
            const uint32_t racer = Memory::Read8(settings + 0x10);
            if (racer < FarCpu_Racers && !g_farCpuSoundOn[racer]) return;
        }
    }
    MKW_GUEST_NEXT(807076D0)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x807076D0, FarCpu_KartActorUpdate_807076d0, "FarCpu_KartActorUpdate_807076d0");
