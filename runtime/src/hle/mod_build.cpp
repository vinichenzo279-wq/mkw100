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
     {0x2C, 0x6C, 0x40, 0xCA, 0x2F, 0x06, 0x05, 0xC1, 0x93, 0x91, 0xCB, 0xFA, 0xFB, 0x97, 0x25, 0x7C, 0x8E, 0x9A, 0xB2, 0x4C},
     "Item Fever 1.1.0"},
};

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
