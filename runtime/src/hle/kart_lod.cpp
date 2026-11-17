// mkw100: CPU racers with the game's low-detail kart and driver models.
//
// Every kart+driver archive exists in three detail levels: Race/Kart/<kart>-<char>.szs, _2.szs
// and _4.szs (about half the size). In a _4 archive the driver is one static model ("model_cpu",
// no animations) and the kart one merged model ("model_lod", no separate wheels or arms); the full
// archive has the animated driver ("model" plus "model_lod") and the kart in parts. The game uses
// the _4 set in split screen (two or more screens) for every racer that is not a local player:
//  - RaceScene::OnEnter (0x80553C50) passes detail level 2 to ArchiveMgr::LoadKartArchive
//    (0x80540E3C, r8: 0, 1, 2 for "", "_2", "_4");
//  - Kart::BRRESHandle's constructor (0x80576B3C) sets +0x11, which builds the merged kart;
//  - DriverController::Load (0x807C8758) sets +0xA8, which CreateModelDirectors (0x807D21F8) reads
//    to build "model_cpu" (falling back to "model").
// All three read the screen count, so in a one-screen race nobody gets them. Past 12 racers the CPU
// racers here get all three. Base game and Item Fever; Retro Rewind loads its own skin archives,
// which have the full set only.
//
// Windowed at 128 racers this took the frame rate from 30-36 to 48-49 fps. Video settings, "Low-detail
// CPU racers" ([video] low_detail_cpus, default on), turns it off; MKW100_TEST_KART_LOD=0 / 1 forces
// it off / on, at any racer count (for measuring).
//
// A race with it differs from one without (same seed, different result): the CPU drivers' models
// are not animated, so the race takes another course from the start.

#include "abi_bridge.h"
#include "runtime_config.h"
#include "guest_abi/player_slots.h"
#include "guest_function_imports.h"
#include "hle/mod_build.h"
#include "memory.h"
#include "runtime_log.h"

#include <cstdint>
#include <cstdlib>

extern "C" uint32_t MKW100_PlayerSlotOffset(uint32_t index);

namespace {

constexpr uint32_t KartLod_Racedata = 0x809BD728;     // RaceConfig::spInstance
constexpr uint32_t KartLod_RacerCount = 0x24;         // u8
constexpr uint32_t KartLod_PlayerType = 0x38;         // per scenario player: 0 local human, 1 CPU, 4 online
constexpr uint32_t KartLod_LowDetail = 2;             // "_4"
constexpr uint32_t KartLod_HandleMergedKart = 0x11;   // Kart::BRRESHandle: u8, build the merged kart
constexpr uint32_t KartLod_HandleSplitLocal = 0x12;   // Kart::BRRESHandle: u8, a split-screen local player
constexpr uint32_t KartLod_HandleRacer = 0x13;        // Kart::BRRESHandle: u8 racer
constexpr uint32_t KartLod_DriverCpuModel = 0xA8;     // DriverController: u8, build "model_cpu"
constexpr uint32_t KartLod_Racers = 256;

bool g_lowDetail[KartLod_Racers];  // racers whose archive was loaded at level 2 here

int KartLod_ForcedMode() {
    static const int mode = [] {
        const char* v = std::getenv("MKW100_TEST_KART_LOD");
        return v == nullptr ? -1 : (v[0] == '0' ? 0 : 1);
    }();
    return mode;
}

bool KartLod_UseLowDetail(uint32_t racer) {
    const int forced = KartLod_ForcedMode();
    if (forced == 0) return false;
    const ModBuild::Kind kind = ModBuild::Current();
    if (kind != ModBuild::Kind::None && kind != ModBuild::Kind::ItemFever && kind != ModBuild::Kind::FormulaKart) return false;
    const uint32_t rd = Memory::Read32(KartLod_Racedata);
    if (rd < 0x80000000u || rd >= 0x98000000u) return false;
    if (forced < 0 && (!RuntimeConfigFile::LowDetailCpus(true) || Memory::Read8(rd + KartLod_RacerCount) <= 12)) {
        return false;
    }
    return Memory::Read32(rd + MKW100_PlayerSlotOffset(racer) + KartLod_PlayerType) == 1;  // CPU
}

} // namespace

// Called from the one wrapper on 0x80540E3C (local_vs.cpp, LocalVs_BattleKartArchive_80540e3c) before
// the game's code: r3 ArchiveMgr, r4 racer, r5 kart, r6 character, r7 colour, r8 detail level.
void KartLod_OnLoadKartArchive(CpuContext* ctx) {
    const uint32_t racer = ctx->gpr[4];
    if (racer >= KartLod_Racers) return;
    g_lowDetail[racer] = false;
    if (ctx->gpr[8] == 0 && KartLod_UseLowDetail(racer)) {
        static uint32_t logged = 0;
        if (logged++ == 0) RT_LOGF("kart-lod", "CPU racers use the low-detail kart and driver models\n");
        ctx->gpr[8] = KartLod_LowDetail;
        g_lowDetail[racer] = true;
    }
}

// Kart::BRRESHandle::__ct(this, ?, ghost, racer): the merged kart for a low-detail racer.
extern "C" void KartBRRESHandle_ct_80576b3c(CpuContext* ctx) {
    const uint32_t handle = ctx->gpr[3];
    const uint32_t racer = ctx->gpr[6] & 0xFF;
    MKW_GUEST_NEXT(80576B3C)(ctx);
    if (racer < KartLod_Racers && g_lowDetail[racer]) {
        Memory::Write8(handle + KartLod_HandleMergedKart, 1);
        Memory::Write8(handle + KartLod_HandleSplitLocal, 0);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x80576B3C, KartBRRESHandle_ct_80576b3c, "KartBRRESHandle_ct_80576b3c");

// The course: ArchiveMgr::LoadCourseArchive(this, course, heap, splitScreen) (0x80540760) loads
// Race/Course/<name>_d.szs when its last argument is set and that file exists (else <name>.szs).
// The _d archive holds the same collision and objects with a lighter model (course_d_model.brres,
// which CourseMgr::CreateInstance takes whenever the archive has it). RaceScene::OnEnter sets the
// argument for split screen only; with Video settings, "Low-detail course" ([video] low_detail_course,
// default off: about 1 fps at 128 racers on course 0x08) it is set past 12 racers too.
// MKW100_TEST_COURSE_LOD=0 / 1 forces it off / on.
extern "C" void ArchiveMgr_LoadCourseArchive_80540760(CpuContext* ctx) {
    static const int forced = [] {
        const char* v = std::getenv("MKW100_TEST_COURSE_LOD");
        return v == nullptr ? -1 : (v[0] == '0' ? 0 : 1);
    }();
    if (ctx->gpr[6] == 0 && forced != 0) {
        bool use = forced == 1;
        if (forced < 0 && RuntimeConfigFile::LowDetailCourse(false)) {
            const uint32_t rd = Memory::Read32(KartLod_Racedata);
            use = rd >= 0x80000000u && rd < 0x98000000u && Memory::Read8(rd + KartLod_RacerCount) > 12;
        }
        if (use) {
            static uint32_t logged = 0;
            if (logged++ == 0) RT_LOGF("kart-lod", "the course loads its split-screen model when it has one\n");
            ctx->gpr[6] = 1;
        }
    }
    MKW_GUEST_NEXT(80540760)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80540760, ArchiveMgr_LoadCourseArchive_80540760, "ArchiveMgr_LoadCourseArchive_80540760");

// DriverController::CreateModelDirectors(this, ..., handle r6, ghost r7): "model_cpu" for a
// low-detail racer.
extern "C" void DriverController_CreateModelDirectors_807d21f8(CpuContext* ctx) {
    const uint32_t handle = ctx->gpr[6];
    if (handle >= 0x80000000u && handle < 0x98000000u) {
        const uint32_t racer = Memory::Read8(handle + KartLod_HandleRacer);
        if (racer < KartLod_Racers && g_lowDetail[racer]) Memory::Write8(ctx->gpr[3] + KartLod_DriverCpuModel, 1);
    }
    MKW_GUEST_NEXT(807D21F8)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x807D21F8, DriverController_CreateModelDirectors_807d21f8,
                           "DriverController_CreateModelDirectors_807d21f8");
