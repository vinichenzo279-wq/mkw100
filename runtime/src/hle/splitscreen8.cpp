// mkw100: MKWii 8P Splitscreen (SplitScreen8, github.com/therenow9/mkwii-8p-splitscreen, GPLv3) in
// MKW100.
//
// The 8 Player Splitscreen build translates the release's "Mario Kart Wii" module (payload/mkw/Code.pul
// of 1.2.0, rr-pulsar's SplitScreen8 built with SS8_VANILLA). It was written for the game's 12 racers;
// with MKW100's CPUs past 12 two of its tables are too short. The addresses below are inside that one
// build (ModBuild::Kind::SplitScreen8, Kamek code SHA-1 bd6f6a10...), whose module sits at 0x81800000.
//
// - Locals.cpp's SortHumansWide (0x81808B28) ranks the humans of a race with more than 4 of them into
//   AI::Manager's ranked-human entries, but only ranks 1-12: a human placed 13th or lower dropped out of
//   the order and the CPUs rubber-banded against a stale "last human". Replaced here by the same
//   ordering over any rank.
// - ScreenGuards.cpp's stand-in for Effects::Mgr's per-screen visibility on screens 5-8 points at two
//   12-entry arrays (distance u8[12], on screen s32[12]) indexed by racer, so racers 12+ read whatever
//   followed them. They are repointed once the module is in place to 255-entry copies in MKW100's mod
//   shadow span (0x82DA0000, next to the other packs' shadows at 0x82D00000..0x82D80000).
// - Players 5-8 without a controller (a headless --race-locals boot skips the join page that binds
//   one) take Wii Remote channels 1-4 instead of updating through a null controller, and the module's
//   reconnect check (Join.cpp FirstDeactivatedPad, 0x81807E94) does not ask them to join.

#include "abi_bridge.h"
#include "boot_race.h"
#include "guest_function_imports.h"
#include "hle/mod_build.h"
#include "hle/splitscreen8.h"
#include "memory.h"
#include "recomp_mod_loader.h"
#include "runtime_log.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

// A named namespace: hle/ sources build as unity batches, where file-local names collide.
namespace splitscreen8_detail {

// Module data (Locals.cpp, Screens.cpp, ScreenGuards.cpp).
constexpr uint32_t kRaceScreenCount = 0x8181376C;  // u8 raceScreenCount
constexpr uint32_t kExtHumans = 0x81813368;        // Human extHumans[4] {u8 *ai, u8 *ranked}
constexpr uint32_t kExtHumanCount = 0x81813388;    // u32 extHumanCount
constexpr uint32_t kSub9d8Visible = 0x81812140;    // {const u8 *distance, 0, 0, const s32 *onScreen}
constexpr uint32_t kVisibleDistance = 0x81812100;  // u8[12], all 0
constexpr uint32_t kVisibleOnScreen = 0x81812110;  // s32[12], all 1
constexpr uint32_t kExtHumanSlots = 4;

// AI::Manager+0x84's object: 4 human entries {AI object, i-th ranked} at +0x148, the count at +0x17C.
constexpr uint32_t kHumanEntries = 0x148;
constexpr uint32_t kHumanCount = 0x17C;
constexpr uint32_t kHumanRank = 0x14;  // s32, 1-based race position of a human's AI object

constexpr uint32_t kWideDistance = 0x82DA0000;  // u8[256], 0 (near)
constexpr uint32_t kWideOnScreen = 0x82DA0100;  // s32[256], 1 (on screen)
constexpr uint32_t kWideEntries = 256;

// Input::Manager: holders 0-3 at +4 (0xEC each); the module's holders 4-7 follow its 0x415C bytes
// (holder id at +4 + id * 0xEC + 0x3DA8); Wii controllers 0-3 at +0x1720 (0x920 each).
constexpr uint32_t kInputManager = 0x809BD70C;
constexpr uint32_t kHolderSize = 0xEC;
constexpr uint32_t kExtHolderGap = 0x3DA8;
constexpr uint32_t kWiiControllers = 0x1720;
constexpr uint32_t kWiiControllerSize = 0x920;
constexpr uint32_t kHolderController = 4;  // ControllerHolder: Controller* curController

bool InGuestRam(uint32_t address) {
    return (address >= 0x80000000u && address < 0x81800000u) || (address >= 0x90000000u && address < 0x98000000u);
}

void WidenVisibility() {
    if (!ModBuild::IsKnownSplitScreen8()) return;
    if (Memory::Read32(kSub9d8Visible) != kVisibleDistance || Memory::Read32(kSub9d8Visible + 12) != kVisibleOnScreen) {
        RT_LOGF("splitscreen8", "WARNING: the module's screen 5-8 visibility table is not where it was measured; left at 12 racers\n");
        return;
    }
    for (uint32_t i = 0; i < kWideEntries; ++i) {
        Memory::Write8(kWideDistance + i, 0);
        Memory::Write32(kWideOnScreen + i * 4, 1);
    }
    // Module data can sit in a range the guest write guard treats as code; nothing runs from these words.
    uint8_t* table = Memory::GetPointer(kSub9d8Visible, 16);
    const auto put = [&](uint32_t offset, uint32_t value) {
        table[offset] = uint8_t(value >> 24);
        table[offset + 1] = uint8_t(value >> 16);
        table[offset + 2] = uint8_t(value >> 8);
        table[offset + 3] = uint8_t(value);
    };
    put(0, kWideDistance);
    put(12, kWideOnScreen);
    RT_LOGF("splitscreen8", "screens 5-8 see racers 0-%u (visibility tables widened)\n", kWideEntries - 1);
}

const bool kRegistered = [] {
    RecompMod::RegisterPostRelInitializer(&WidenVisibility);
    return true;
}();

// SplitScreen8::SortHumansWide(AI object holder) (Locals.cpp), called in place of the game's
// 0x807426E4 from AI::Manager's update (0x80741708).
void SortHumansWide(CpuContext* ctx) {
    if (!ModBuild::IsKnownSplitScreen8()) {
        MKW_GUEST_NEXT(81808B28)(ctx);
        return;
    }
    const uint32_t ai = ctx->gpr[3];
    const uint32_t extCount = std::min<uint32_t>(Memory::Read32(kExtHumanCount), kExtHumanSlots);
    if (Memory::Read8(kRaceScreenCount) == 0 || extCount == 0) {
        MKW_GUEST_NEXT(81808B28)(ctx);  // the module calls the game's own then
        return;
    }
    const uint32_t gameCount = std::min<uint32_t>(Memory::Read32(ai + kHumanCount), 4);
    const auto aiSlot = [&](uint32_t i) {
        return i < 4 ? ai + kHumanEntries + i * 8 : kExtHumans + (i - 4) * 8;
    };
    std::array<std::pair<int32_t, uint32_t>, 8> ranked{};
    uint32_t n = 0;
    for (uint32_t i = 0; i < gameCount + extCount && i < 4 + kExtHumanSlots; ++i) {
        const uint32_t human = Memory::Read32(aiSlot(i));
        if (!InGuestRam(human)) continue;
        const int32_t rank = int32_t(Memory::Read32(human + kHumanRank));
        if (rank >= 1) ranked[n++] = {rank, human};
    }
    std::stable_sort(ranked.begin(), ranked.begin() + n, [](const auto& a, const auto& b) { return a.first < b.first; });
    for (uint32_t k = 0; k < n; ++k) Memory::Write32(aiSlot(k) + 4, ranked[k].second);
}

// Input::RealControllerHolder::Update(holder, paused).
void HolderUpdate(CpuContext* ctx) {
    const uint32_t holder = ctx->gpr[3];
    if (ModBuild::IsKnownSplitScreen8() && Memory::Read32(holder + kHolderController) == 0) {
        const uint32_t input = Memory::Read32(kInputManager);
        for (uint32_t id = 4; input != 0 && id < 8; ++id) {
            if (holder != input + 4 + id * kHolderSize + kExtHolderGap) continue;
            const CpuContext saved = *ctx;
            ctx->gpr[3] = holder;
            ctx->gpr[4] = input + kWiiControllers + (id - 4) * kWiiControllerSize;
            ctx->gpr[5] = 0;
            MKW_GUEST_FUNC(80521554)(ctx);  // ControllerHolder::SetController(controller, nullptr)
            *ctx = saved;
            RT_LOGF("splitscreen8", "player %u had no controller: bound to Wii Remote channel %u\n", id + 1, id - 3);
            break;
        }
        if (Memory::Read32(holder + kHolderController) == 0) return;  // keeps its last (empty) input
    }
    MKW_GUEST_NEXT(80521768)(ctx);
}

} // namespace splitscreen8_detail

extern "C" void SplitScreen8_SortHumansWide_81808b28(CpuContext* ctx) {
    splitscreen8_detail::SortHumansWide(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x81808B28, SplitScreen8_SortHumansWide_81808b28, "SplitScreen8_SortHumansWide_81808b28");

extern "C" void SplitScreen8_RealControllerHolderUpdate_80521768(CpuContext* ctx) {
    splitscreen8_detail::HolderUpdate(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80521768, SplitScreen8_RealControllerHolderUpdate_80521768,
                           "SplitScreen8_RealControllerHolderUpdate_80521768");

// SplitScreen8::FirstDeactivatedPad(SectionPad*) (Join.cpp), in place of SectionPad::GetFirstDeactivatedPad
// at the reconnect box's three call sites: in a 5-8 player game it also returns a player 5-8 who has not
// joined. A --race boot has no join page, so its players 5-8 never join and the race sat paused on the box.
extern "C" void SplitScreen8_FirstDeactivatedPad_81807e94(CpuContext* ctx) {
    MKW_GUEST_NEXT(81807E94)(ctx);
    if (ModBuild::IsKnownSplitScreen8() && BootRace::Requested() && int32_t(ctx->gpr[3]) >= 4) {
        ctx->gpr[3] = uint32_t(-1);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x81807E94, SplitScreen8_FirstDeactivatedPad_81807e94, "SplitScreen8_FirstDeactivatedPad_81807e94");


uint32_t SplitScreen8::MenuLocals() {
    constexpr uint32_t kMenuLocalCount = 0x8181312E;  // u8 menuLocalCount (Entry.cpp)
    return ModBuild::IsKnownSplitScreen8() ? Memory::Read8(kMenuLocalCount) : 0;
}
