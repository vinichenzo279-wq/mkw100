// mkw100: Insane Kart Wii (Pulsar pack, docs/mkw100/insane-kart.md).
//
// IKW hooks the game's strlen (0x80021254) through a snippet it writes into low memory
// (0x800018A8): a pointer without the top bit set reads as an empty string instead of crashing.
// The recomp cannot run code in low memory, so the hook is dropped at translate time (recomp.yml
// drop_commands) and the same guard lives here, for the IKW build only.

#include "hle/insane_kart.h"
#include "hle/mod_build.h"
#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"
#include "runtime_log.h"
#include "recomp_mod_loader.h"

#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <unordered_set>

// IKW's per-racer [12] tables in its module, found by scripts/mkw100/pack_racer_tables.py (the
// item-expansion code's per-racer state): every indexed access is an index site in
// array-sites.txt (profile insane-kart), so racers 12..254 use a shadow at 0x82E00000 (the window
// Item Fever's tables use in its own build; one pack per executable). When a race starts, a shadow
// gets the value all 12 elements share, else zeroes.
namespace {

struct RacerTable { uint32_t table, size, shadow; };
constexpr RacerTable kRacerTables[] = {
#include "insane_kart_racer_tables.inc"
    // Added by hand (array-sites.txt): the per-racer u8[12] its Kart::Manager::Update hook clears.
    {0x81872384, 1, 0x82E04000},
};
constexpr uint32_t kShadowRacers = 255 - 12;

void ReadyRacerTables() {
    for (const RacerTable& t : kRacerTables) {
        bool uniform = true;
        for (uint32_t i = 1; i < 12 && uniform; ++i)
            for (uint32_t b = 0; b < t.size; ++b)
                if (Memory::Read8(t.table + i * t.size + b) != Memory::Read8(t.table + b)) { uniform = false; break; }
        for (uint32_t i = 0; i < kShadowRacers; ++i)
            for (uint32_t b = 0; b < t.size; ++b)
                Memory::Write8(t.shadow + i * t.size + b, uniform ? Memory::Read8(t.table + b) : 0);
    }
}

} // namespace

void InsaneKart::RaceFrame(uint32_t raceinfo) {
    if (!ModBuild::IsKnownInsaneKart()) return;
    static uint32_t lastRaceinfo = 0, lastFrame = 0;
    const uint32_t frame = Memory::Read32(raceinfo + 0x20);
    if (raceinfo != lastRaceinfo || frame < lastFrame) {
        ReadyRacerTables();
        static bool logged = false;
        if (!logged) {
            logged = true;
            RT_LOGF("insane-kart", "%zu per-racer tables shadowed for racers 12+\n", std::size(kRacerTables));
        }
    }
    lastRaceinfo = raceinfo;
    lastFrame = frame;
}

extern "C" void InsaneKart_Strlen_80021254(CpuContext* ctx) {
    if ((ctx->gpr[3] & 0x80000000u) == 0 && ModBuild::IsKnownInsaneKart()) {
        ctx->gpr[3] = 0;
        return;
    }
    MKW_GUEST_NEXT(80021254)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80021254, InsaneKart_Strlen_80021254, "InsaneKart_Strlen_80021254");

// IKW patches game code while running: its settings and modes switch about 120 instructions between
// the game's word and IKW's (a branch into its module, a nop, an immediate, a branch flip), all
// through its KamekRuntimeWrite helpers (module 0x818001CC word, 0x81800210 halfword, 0x8180025C
// byte in 3.4.1; the branch helper at 0x8180035C ends in the word one). translate-mod finds every
// such call with a constant address and value and compiles each site with all its words
// (Mods.RuntimePatchSites); the natives below store the word, and the site's code runs the
// matching variant. A write the translator didn't see (unknown address or value) is skipped and
// logged once, so that feature keeps the game's code. Writes outside translated code (IKW's thunks
// in low memory) just happen. (RuntimePatch is below the section traces.)
// MKW100_TRACE_SECTIONS=1 (IKW build): logs every scene change request and who asked for it
// (MKW100_TEST_RESULTS logs the next sections and pages, race_oracle.cpp).
static bool TraceSections() {
    static const bool on = std::getenv("MKW100_TRACE_SECTIONS") != nullptr;
    return on && ModBuild::IsKnownInsaneKart();
}
extern "C" void InsaneKart_SectionMgrInit_80634e44(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3];
    MKW_GUEST_NEXT(80634E44)(ctx);
    if (TraceSections())
        RT_LOGF("insane-kart", "SectionMgr::Init: boot section 0x%X, +8 0x%X, +C 0x%X\n", Memory::Read32(self + 4),
                Memory::Read32(self + 8), Memory::Read32(self + 12));
}
REGISTER_NATIVE_WRAPPER_AS(0x80634E44, InsaneKart_SectionMgrInit_80634e44, "InsaneKart_SectionMgrInit_80634e44");

extern "C" void InsaneKart_RequestSceneChange_80635ac8(CpuContext* ctx) {
    if (TraceSections()) RT_LOGF("insane-kart", "RequestSceneChange(%d) from 0x%08X\n", int(ctx->gpr[4]), ctx->lr);
    MKW_GUEST_NEXT(80635AC8)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80635AC8, InsaneKart_RequestSceneChange_80635ac8, "InsaneKart_RequestSceneChange_80635ac8");

// The 16- and 8-bit helpers (0x81800210, 0x8180025C) are handled the same way.
static void RuntimePatch(CpuContext* ctx, uint32_t width) {
    const uint32_t address = ctx->gpr[3], value = ctx->gpr[4];
    // A run-time patch site the translator compiled with this word (Mods.RuntimePatchSites): the
    // write goes to guest memory, where the translated code at the site reads which variant to run.
    const uint32_t wordAddress = address & ~3u;
    const uint32_t shift = width == 4 ? 0 : 8 * (4 - width - (address & 3));
    const uint32_t mask = width == 4 ? 0xFFFFFFFFu : ((1u << (width * 8)) - 1) << shift;
    const uint32_t newWord = (Memory::Read32(wordAddress) & ~mask) | ((value << shift) & mask);
    const bool site = RecompMod::IsRuntimePatchWord(wordAddress, newWord);
    if (site || !RecompMod::IsExecutableAddress(address)) {
        if (width == 4) Memory::Write32(address, value);
        else if (width == 2) Memory::Write16(address, static_cast<uint16_t>(value));
        else Memory::Write8(address, static_cast<uint8_t>(value));
        return;
    }
    static std::mutex lock;
    static std::unordered_set<uint32_t> logged;
    std::lock_guard<std::mutex> guard(lock);
    if (logged.insert(address).second) {
        RT_LOGF("insane-kart", "run-time code patch skipped: 0x%08X <- 0x%0*X (from 0x%08X)\n", address,
                int(width * 2), value & (width == 4 ? 0xFFFFFFFFu : (1u << (width * 8)) - 1), ctx->lr);
    }
}

extern "C" void InsaneKart_RuntimePatch32_818001cc(CpuContext* ctx) {
    if (!ModBuild::IsKnownInsaneKart()) { MKW_GUEST_NEXT(818001CC)(ctx); return; }
    RuntimePatch(ctx, 4);
}
REGISTER_NATIVE_WRAPPER_AS(0x818001CC, InsaneKart_RuntimePatch32_818001cc, "InsaneKart_RuntimePatch32_818001cc");

extern "C" void InsaneKart_RuntimePatch16_81800210(CpuContext* ctx) {
    if (!ModBuild::IsKnownInsaneKart()) { MKW_GUEST_NEXT(81800210)(ctx); return; }
    RuntimePatch(ctx, 2);
}
REGISTER_NATIVE_WRAPPER_AS(0x81800210, InsaneKart_RuntimePatch16_81800210, "InsaneKart_RuntimePatch16_81800210");

extern "C" void InsaneKart_RuntimePatch8_8180025c(CpuContext* ctx) {
    if (!ModBuild::IsKnownInsaneKart()) { MKW_GUEST_NEXT(8180025C)(ctx); return; }
    RuntimePatch(ctx, 1);
}
REGISTER_NATIVE_WRAPPER_AS(0x8180025C, InsaneKart_RuntimePatch8_8180025c, "InsaneKart_RuntimePatch8_8180025c");
