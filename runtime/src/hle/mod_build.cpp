// mkw100: see mod_build.h. Keep the digests in step with projects/mkwii/mkw100/mod-builds.txt.

#include "hle/mod_build.h"

#include "memory.h"
#include "runtime_log.h"

#include <atomic>
#include <cstdint>

namespace {

constexpr uint32_t kKamekCodeSha1 = 0x800017B0;  // 20 bytes, written by the mod loader at boot

struct KnownBuild { ModBuild::Kind kind; uint8_t sha1[20]; const char* name; };
constexpr KnownBuild kKnownBuilds[] = {
    {ModBuild::Kind::RetroRewind,
     {0x50, 0x61, 0x37, 0x4C, 0x93, 0x7B, 0x8E, 0xBE, 0x70, 0xFA, 0x67, 0xBE, 0xF7, 0xF3, 0x5B, 0x16, 0xF6, 0xE9, 0x22, 0x8B},
     "WheelWizard RetroRewind6"},
    {ModBuild::Kind::ItemFever,
     {0x47, 0x81, 0x63, 0x37, 0xC3, 0x8D, 0xFD, 0x92, 0xFD, 0x0F, 0xA1, 0x9E, 0x82, 0xFE, 0x72, 0xB3, 0xAF, 0x3F, 0x62, 0x3C},
     "Item Fever 1.1.1"},
    {ModBuild::Kind::FormulaKart,
     // The converted image (fkw_to_kamek.py blanks the code it only copies into the game and
     // rewrites bl-over-data, seeds indirect entries, moves ItemAmounts to the shadow window), from
     // FormulaKartWiiP.bin SHA-1 ea5f4b57966ff7366487047ce78edd4def4853d7. fkw_to_kamek.py prints
     // this as "code sha1".
     {0x9F, 0x9A, 0xE3, 0x9A, 0xC1, 0xD2, 0x6A, 0x6F, 0xB6, 0x2E, 0x0C, 0x1F, 0xD6, 0x49, 0x47, 0x4C, 0xCD, 0x72, 0x72, 0xD4},
     "Formula Kart Wii (FormulaKartWiiP.bin)"},
};

// The combined Retro Rewind + Item Fever module carries this marker and its two source digests
// (combine_pul.py writes them; its own digest changes with every change of the combining plan).
constexpr uint32_t kCombinedMarker = 0x818AFF00;
constexpr char kCombinedMagic[16] = "MKW100 RR+IF v1";

bool CombinedFromKnownBuilds() {
    if (!Memory::Contains(kCombinedMarker, 56)) return false;
    for (uint32_t i = 0; i < 16; ++i)
        if (Memory::Read8(kCombinedMarker + i) != uint8_t(kCombinedMagic[i])) return false;
    for (uint32_t part = 0; part < 2; ++part) {
        const ModBuild::Kind want = part == 0 ? ModBuild::Kind::RetroRewind : ModBuild::Kind::ItemFever;
        bool found = false;
        for (const KnownBuild& b : kKnownBuilds) {
            if (b.kind != want) continue;
            bool same = true;
            for (uint32_t i = 0; i < 20; ++i) same &= Memory::Read8(kCombinedMarker + 16 + part * 20 + i) == b.sha1[i];
            found |= same;
        }
        if (!found) return false;
    }
    return true;
}

std::atomic<int> g_kind{-1};  // decided once the digest is present

} // namespace

namespace ModBuild {

Kind Current() {
    if (const int k = g_kind.load(std::memory_order_acquire); k >= 0) return Kind(k);
    // Settings load (LocalVs::MaxRacers) runs before guest memory is mapped.
    if (!Memory::Contains(kKamekCodeSha1, 20)) return Kind::None;
    uint8_t digest[20];
    bool any = false;
    for (uint32_t i = 0; i < 20; ++i) any |= (digest[i] = Memory::Read8(kKamekCodeSha1 + i)) != 0;
    if (!any) return Kind::None;  // base game, or not loaded yet: decide later
    Kind kind = Kind::Unknown;
    const char* name = nullptr;
    for (const KnownBuild& b : kKnownBuilds) {
        bool same = true;
        for (uint32_t i = 0; i < 20; ++i) same &= digest[i] == b.sha1[i];
        if (same) { kind = b.kind; name = b.name; break; }
    }
    if (kind == Kind::Unknown && CombinedFromKnownBuilds()) {
        kind = Kind::RetroRewindItemFever;
        name = "Retro Rewind + Item Fever items (combined from WheelWizard RetroRewind6 and Item Fever 1.1.1)";
    }
    if (kind == Kind::Unknown) {
        RT_LOGF("mod-build", "WARNING: this Code.pul is not a build MKW100's pack fixes were measured on "
                "(mod-builds.txt); they are off and races are capped at 12 racers\n");
    } else {
        RT_LOGF("mod-build", "%s: MKW100 pack fixes on\n", name);
    }
    g_kind.store(int(kind), std::memory_order_release);
    return kind;
}

} // namespace ModBuild
