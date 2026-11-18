// mkw100: Formula Kart Wii (github.com/CLF78/FormulaKartWii) as the mod product.
//
// FKW is a Riivolution patch, not a Pulsar pack. Riivolution writes its option bytes to
// 0x80004030..0x8000406F (.init's TRK vector area) and a bootstrap that loads the payload; the
// payload's loadCodes then patches the game once. Build-MKW100.ps1 -FormulaKart runs loadCodes at
// build time (scripts/mkw100/fkw_to_kamek.py) with the XML's default choices, so the options that
// only decide which patches apply are baked in at those defaults.
//
// Seven options are read by the payload's own code during races, so they work from [formula_kart]
// in Config.toml and the F10 "Formula Kart" menu, applied at once:
//   green_shell             what the Newbie Helper hands out in place of a green shell (blue)
//   better_items            Newbie Helper's better item pool for players far behind (true)
//   bullet_thunderclouds    a thundercloud makes players far behind a Bullet Bill, not Mega (true)
//   feather_cut_indicators  markers on the feather shortcuts (false)
//   drift_bikes / drift_karts  force inside/outside drift ("default")
//   race_music              "normal" or "accelerating" (faster each lap)
// The rest keep fkw.xml's defaults (kBakedSettings; keep in step with fkw_to_kamek.py's "settings"
// in build-logs/fkw_patches.json).

#include "hle/formula_kart.h"

#include "abi_bridge.h"
#include "boot_race.h"
#include "guest_function_imports.h"
#include "hle/mod_build.h"
#include "memory.h"
#include "recomp_mod_loader.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <imgui.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>

namespace {

constexpr uint32_t kSettingsStart = 0x80004030;
constexpr uint32_t kSettingsEnd = 0x80004070;
constexpr uint32_t kFeatherCuts = 0x80004030;
constexpr uint32_t kDriftBikes = 0x80004031;
constexpr uint32_t kDriftKarts = 0x80004032;
constexpr uint32_t kBetterItems = 0x80004040;
constexpr uint32_t kGreenShell = 0x80004041;
constexpr uint32_t kBulletClouds = 0x80004042;
constexpr uint32_t kRaceMusic = 0x80004061;

struct FkwSetting { uint32_t address; uint8_t value; };

// fkw.xml defaults (Riivolution default="N" choices) of the options baked in at build time.
constexpr FkwSetting kBakedSettings[] = {
    {0x80004043, 1},  // Instant Respawn
    {0x80004044, 1},  // Automatic Respawn Boost
    {0x80004045, 1},  // Instant Slipstream
    {0x80004055, 1},  // Nametags Distance: Normal
    {0x80004057, 1},  // Bloom and Depth of Field: Normal
    {0x80004058, 1},  // Lightning Flash
    {0x80004060, 1},  // Character Voices
    {0x80004062, 1},  // Music: Transformation
    {0x80004063, 1},  // Music: Distortion
    {0x80004064, 1},  // Automatic BRSAR Patching
};

struct Choice { const char* key; const char* label; uint8_t value; };

// FKW's item ids, as fkw.xml's choices write them.
constexpr Choice kGreenShellChoices[] = {
    {"green", "Green Shell (off)", 0x00}, {"red", "Red Shell", 0x01},  {"banana", "Banana", 0x02},
    {"fib", "Fake Item Box", 0x03},       {"bomb", "Bob-omb", 0x06},   {"blue", "Blue Shell", 0x07},
    {"feather", "Feather", 0x0C},         {"pow", "POW Block", 0x0D},
};
constexpr Choice kDriftBikeChoices[] = {
    {"default", "Bike's own", 0}, {"outside", "Outside drift", 1}, {"inside", "Inside drift", 2},
};
constexpr Choice kDriftKartChoices[] = {
    {"default", "Kart's own", 0}, {"inside", "Inside drift", 1},
};
// 0 (no race music) is a baked patch, so only the two values the payload reads are offered.
constexpr Choice kRaceMusicChoices[] = {
    {"normal", "Normal", 1}, {"accelerating", "Faster each lap", 2},
};

template <size_t N>
int ChoiceIndex(const Choice (&choices)[N], const std::optional<std::string>& key, int fallback) {
    if (key) {
        for (size_t i = 0; i < N; ++i) {
            if (*key == choices[i].key) return int(i);
        }
    }
    return fallback;
}

std::atomic<int> g_greenShell{5};   // blue
std::atomic<bool> g_betterItems{true};
std::atomic<bool> g_bulletClouds{true};
std::atomic<bool> g_featherCuts{false};
std::atomic<int> g_driftBikes{0};
std::atomic<int> g_driftKarts{0};
std::atomic<int> g_raceMusic{0};    // normal

// .init is an executable range, so go around the guest write guard: no translated code runs from
// these bytes (the TRK vector table FKW reuses as storage). The payload reads them as it needs them.
void Poke(uint32_t address, uint8_t value) { *Memory::GetPointer(address, 1) = value; }

void ApplyRuntimeSettings() {
    Poke(kGreenShell, kGreenShellChoices[g_greenShell.load()].value);
    Poke(kBetterItems, g_betterItems.load() ? 1 : 0);
    Poke(kBulletClouds, g_bulletClouds.load() ? 1 : 0);
    Poke(kFeatherCuts, g_featherCuts.load() ? 1 : 0);
    Poke(kDriftBikes, kDriftBikeChoices[g_driftBikes.load()].value);
    Poke(kDriftKarts, kDriftKartChoices[g_driftKarts.load()].value);
    Poke(kRaceMusic, kRaceMusicChoices[g_raceMusic.load()].value);
}

void FormulaKart_WriteSettings() {
    if (!ModBuild::IsKnownFormulaKart()) return;
    const auto& c = RuntimeConfigFile::Get();
    g_greenShell = ChoiceIndex(kGreenShellChoices, c.formulaKartGreenShell, 5);
    g_betterItems = c.formulaKartBetterItems.value_or(true);
    g_bulletClouds = c.formulaKartBulletClouds.value_or(true);
    g_featherCuts = c.formulaKartFeatherCuts.value_or(false);
    g_driftBikes = ChoiceIndex(kDriftBikeChoices, c.formulaKartDriftBikes, 0);
    g_driftKarts = ChoiceIndex(kDriftKartChoices, c.formulaKartDriftKarts, 0);
    g_raceMusic = ChoiceIndex(kRaceMusicChoices, c.formulaKartRaceMusic, 0);
    uint8_t* bytes = Memory::GetPointer(kSettingsStart, kSettingsEnd - kSettingsStart);
    std::memset(bytes, 0, kSettingsEnd - kSettingsStart);
    for (const FkwSetting& s : kBakedSettings) bytes[s.address - kSettingsStart] = s.value;
    ApplyRuntimeSettings();
    RT_LOGF("formula-kart", "options: green shell -> %s, better items %d, bullet clouds %d, feather cuts %d, "
            "drift bikes %s, drift karts %s, race music %s\n",
            kGreenShellChoices[g_greenShell.load()].key, int(g_betterItems.load()), int(g_bulletClouds.load()),
            int(g_featherCuts.load()), kDriftBikeChoices[g_driftBikes.load()].key,
            kDriftKartChoices[g_driftKarts.load()].key, kRaceMusicChoices[g_raceMusic.load()].key);
}

const bool kRegistered = [] {
    RecompMod::RegisterPostRelInitializer(&FormulaKart_WriteSettings);
    return true;
}();

template <size_t N>
void Combo(const char* label, const Choice (&choices)[N], std::atomic<int>& index, const char* key) {
    int current = index.load();
    ImGui::SetNextItemWidth(240);
    if (ImGui::BeginCombo(label, choices[current].label)) {
        for (int i = 0; i < int(N); ++i) {
            if (ImGui::Selectable(choices[i].label, i == current) && i != current) {
                index = i;
                RuntimeConfigFile::WriteSetting("formula_kart", key, RuntimeConfigFile::FormatString(choices[i].key));
                ApplyRuntimeSettings();
            }
        }
        ImGui::EndCombo();
    }
}

void Check(const char* label, std::atomic<bool>& value, const char* key) {
    bool on = value.load();
    if (ImGui::Checkbox(label, &on)) {
        value = on;
        RuntimeConfigFile::WriteSetting("formula_kart", key, on ? "true" : "false");
        ApplyRuntimeSettings();
    }
}

// FKW's payload, where its item randomizer calls come from (the converted module's range).
constexpr uint32_t kPayloadStart = 0x808DD400;
constexpr uint32_t kPayloadEnd = 0x808E35A0;

}  // namespace

namespace FormulaKart {

void DrawSettings() {
    Combo("Green shell becomes", kGreenShellChoices, g_greenShell, "green_shell");
    Check("Better items when far behind (Newbie Helper)", g_betterItems, "better_items");
    Check("Thundercloud gives a Bullet Bill when far behind (not Mega)", g_bulletClouds, "bullet_thunderclouds");
    Check("Feather cut indicators", g_featherCuts, "feather_cut_indicators");
    Combo("Bike drift", kDriftBikeChoices, g_driftBikes, "drift_bikes");
    Combo("Kart drift", kDriftKartChoices, g_driftKarts, "drift_karts");
    Combo("Race music", kRaceMusicChoices, g_raceMusic, "race_music");
    ImGui::TextUnformatted("Applies at once. FKW's other options stay at its defaults.");
}

}  // namespace FormulaKart

// nw4r::snd::detail::Util::CalcRandom(): the sound library's LCG (state in .sdata), which FKW's
// Ultimate Item Randomizer and amount randomizer use for every roll. The audio thread draws from
// the same state, so how many draws come between two item rolls depends on host timing, and two
// runs of a scripted test race diverge. In a scripted race, calls from FKW's payload get their own
// LCG (same multiplier and increment, fixed start); sound keeps the shared one, and normal play is
// unchanged.
extern "C" void FormulaKart_CalcRandom_800ad050(CpuContext* ctx) {
    const uint32_t lr = ctx->lr;
    if (lr >= kPayloadStart && lr < kPayloadEnd && BootRace::Scripted() && ModBuild::IsKnownFormulaKart()) {
        static uint32_t state = 0x464B5731u;  // fixed start
        state = state * 0x0019660Du + 0x3C6EF35Fu;
        ctx->gpr[3] = state >> 16;
        return;
    }
    MKW_GUEST_NEXT(800AD050)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x800AD050, FormulaKart_CalcRandom_800ad050, "FormulaKart_CalcRandom_800ad050");
