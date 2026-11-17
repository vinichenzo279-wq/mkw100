// mkw100: Formula Kart Wii (github.com/CLF78/FKW) as the mod product.
//
// FKW is a Riivolution patch, not a Pulsar pack. Riivolution writes its option bytes to
// 0x80004030..0x8000406F (.init's TRK vector area) and a bootstrap that loads the payload; the
// payload's loadCodes then patches the game once. Build-MKW100.ps1 -FormulaKart runs loadCodes at
// build time (scripts/mkw100/fkw_to_kamek.py) with the XML's default choices, so every patch is
// already baked in. The payload's own code still reads some option bytes during races (Newbie
// Helper, Better Item Pool, the green shell replacement, Bullet Bill thunderclouds, respawn and
// slipstream helpers, music options), so they are written here with the same defaults.
//
// Keep kSettings in step with fkw_to_kamek.py's output ("settings" in build-logs/fkw_patches.json).

#include "hle/mod_build.h"
#include "memory.h"
#include "recomp_mod_loader.h"
#include "runtime_log.h"

#include <cstdint>

namespace {

struct FkwSetting { uint32_t address; uint8_t value; };

// fkw.xml defaults (Riivolution default="N" choices); everything else is 0.
constexpr FkwSetting kSettings[] = {
    {0x80004040, 1},  // Better Item Pool
    {0x80004041, 7},  // Green Shell Replacement: Blue Shell
    {0x80004042, 1},  // Bullet Bill Thunderclouds
    {0x80004043, 1},  // Instant Respawn
    {0x80004044, 1},  // Automatic Respawn Boost
    {0x80004045, 1},  // Instant Slipstream
    {0x80004055, 1},  // Nametags Distance: Normal
    {0x80004057, 1},  // Bloom and Depth of Field: Normal
    {0x80004058, 1},  // Lightning Flash
    {0x80004060, 1},  // Character Voices
    {0x80004061, 1},  // Music: Race
    {0x80004062, 1},  // Music: Transformation
    {0x80004063, 1},  // Music: Distortion
    {0x80004064, 1},  // Automatic BRSAR Patching
};
constexpr uint32_t kSettingsStart = 0x80004030;
constexpr uint32_t kSettingsEnd = 0x80004070;

void FormulaKart_WriteSettings() {
    if (!ModBuild::IsKnownFormulaKart()) return;
    // .init is an executable range, so go around the guest write guard: no translated code
    // runs from these bytes (they are the TRK vector table FKW reuses as storage).
    uint8_t* bytes = Memory::GetPointer(kSettingsStart, kSettingsEnd - kSettingsStart);
    for (uint32_t a = kSettingsStart; a < kSettingsEnd; ++a) bytes[a - kSettingsStart] = 0;
    for (const FkwSetting& s : kSettings) bytes[s.address - kSettingsStart] = s.value;
    RT_LOGF("formula-kart", "option bytes written (fkw.xml defaults)\n");
}

const bool kRegistered = [] {
    RecompMod::RegisterPostRelInitializer(&FormulaKart_WriteSettings);
    return true;
}();

}  // namespace
