#include "boot_race.h"

#include "abi_bridge.h"
#include "guest_abi/access_trace.h"
#include "guest_abi/player_slots.h"
#include "guest_abi/guest_region.h"
#include "guest_function_imports.h"
#include "local_vs.h"
#include "memory.h"
#include "runtime_log.h"
#include <dolphin/pad.h> // R25_GAMEPAD_AUTOBIND

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

constexpr std::string_view kOption = "--race";
constexpr uint32_t kLuigiCircuit = 0x08;
constexpr uint32_t kLastRaceCourse = 0x1F;

// Racedata (menu scenario, copied into the race scenario by Racedata::InitRace)
constexpr uint32_t kRaceScenarioOffset = 0x20;
constexpr uint32_t kMenuScenarioOffset = 0xC10;
constexpr uint32_t kAwardScenarioEnd = 0x1800 + 0xBF0;
constexpr uint32_t kPlayersOffset = 0x8;
constexpr uint32_t kPlayerSize = 0xF0;
constexpr uint32_t kPlayerKartOffset = 0x8;
constexpr uint32_t kPlayerCharacterOffset = 0xC;
constexpr uint32_t kPlayerTypeOffset = 0x10;
constexpr uint32_t kPlayerControllerChannelOffset = 0x6;
constexpr uint32_t kPlayerTeamOffset = 0xCC;         // 0 red, 1 blue
constexpr uint32_t kSettingsModeFlags = 0x28;       // bit 0x2: team mode
constexpr uint32_t kPlayerCount = 12;
constexpr uint32_t kSettingsOffset = 0xB48;

constexpr uint32_t kSectionPadOffset = 0x34;
constexpr uint32_t kFirstGcnControllerId = 0x124;
constexpr uint32_t kSectionP1Vs = 0x20;
constexpr uint32_t kSectionP1TeamVs = 0x24;  // --race-teams: the Team VS flow and its team results

enum : uint32_t { kPlayerRealLocal = 0, kPlayerCpu = 1, kPlayerNone = 5 };
enum : uint32_t { kGameModeVs = 1, kCc150 = 2, kCpuHard = 2, kItemsBalanced = 0 };

struct Racer {
    uint32_t character;
    uint32_t kart;
};

// Standard kart matching each character's weight class.
constexpr std::array<Racer, 12> kRacers{{
    {0x00, 0x01}, // Mario
    {0x07, 0x01}, // Luigi
    {0x10, 0x01}, // Peach
    {0x0A, 0x01}, // Yoshi
    {0x0F, 0x01}, // Daisy
    {0x08, 0x00}, // Toad
    {0x06, 0x00}, // Baby Mario
    {0x0E, 0x00}, // Koopa Troopa
    {0x03, 0x02}, // Bowser
    {0x09, 0x02}, // Donkey Kong
    {0x0B, 0x02}, // Wario
    {0x16, 0x02}, // Funky Kong
}};

// MKW100 R24: opt-in visual roster variety. OFF by default to preserve
// the official fixed-seed 12/24/100 oracle baselines exactly.
// Character weight IDs and vehicle IDs taken from Pulsar GameSource's
// MarioKartWii/System/Identifiers.hpp and UI/Page/Menu/KartSelect.hpp.
// Avoid Mii/alternate costume IDs in this first test.
struct DiverseRacer {
    uint32_t character;
    uint32_t weight; // 0 = small, 1 = medium, 2 = large
};
constexpr std::array<DiverseRacer, 24> kDiverseCharacters{{
    {0x00, 1}, {0x01, 0}, {0x02, 2}, // Mario, Baby Peach, Waluigi
    {0x07, 1}, {0x04, 0}, {0x03, 2}, // Luigi, Baby Daisy, Bowser
    {0x0A, 1}, {0x05, 0}, {0x09, 2}, // Yoshi, Dry Bones, Donkey Kong
    {0x0F, 1}, {0x06, 0}, {0x0B, 2}, // Daisy, Baby Mario, Wario
    {0x10, 1}, {0x08, 0}, {0x13, 2}, // Peach, Toad, King Boo
    {0x11, 1}, {0x0C, 0}, {0x15, 2}, // Birdo, Baby Luigi, Dry Bowser
    {0x12, 1}, {0x0D, 0}, {0x16, 2}, // Diddy Kong, Toadette, Funky Kong
    {0x14, 1}, {0x0E, 0}, {0x17, 2}, // Bowser Jr, Koopa, Rosalina
}};
// Each column is a legitimate vehicle for that weight class; karts + bikes.
constexpr std::array<std::array<uint32_t, 12>, 3> kDiverseVehicles{{
    {{0x00, 0x03, 0x06, 0x09, 0x0C, 0x0F, 0x12, 0x15, 0x18, 0x1B, 0x1E, 0x21}},
    {{0x01, 0x04, 0x07, 0x0A, 0x0D, 0x10, 0x13, 0x16, 0x19, 0x1C, 0x1F, 0x22}},
    {{0x02, 0x05, 0x08, 0x0B, 0x0E, 0x11, 0x14, 0x17, 0x1A, 0x1D, 0x20, 0x23}},
}};
bool g_diverseRoster = false;
Racer SelectDiverseRacer(uint32_t index) {
    const auto& racer = kDiverseCharacters[index % kDiverseCharacters.size()];
    // Advances the kart/bike every three racers; repeated characters in a
    // 100-racer field also change vehicle on later 24-racer cycles.
    return Racer{racer.character, kDiverseVehicles[racer.weight][(index / 3u) % 12u]};
}

std::optional<uint32_t> g_course;
// Set when ArgumentParser::parse picked the race section, for Pulsar's boot hook below.
bool g_raceSectionPicked = false;
uint32_t g_players = kPlayerCount;
bool g_cpuDriven = false;
bool g_teams = false;  // --race-teams: the game's own red/blue team mode
bool g_mirror = false; // --race-mirror: the Mirror flag, a 200cc race with cc200.cpp's toggle on
// --race-battle[=coin]: a balloon (or coin) battle on Block Plaza (or the --race=0x20..0x29 arena);
// battles are always red / blue teams.
bool g_battle = false;
uint32_t g_battleType = 0;  // 0 balloon, 1 coin
constexpr uint32_t kSectionP1Battle = 0x28;
// --race-gp: a Grand Prix race (gameMode 0, the 1P GP section) on the given course.
bool g_gp = false;
constexpr uint32_t kSectionP1Gp = 0x1E;
constexpr uint32_t kGameModeGp = 0;
constexpr uint32_t kBlockPlaza = 0x21;
constexpr uint32_t kLastArena = 0x29;
constexpr uint32_t kGameModeBattle = 3;
bool g_autoDriven = false;
bool g_lockstep = false;
bool g_heapCheck = false;

constexpr std::string_view kPlayersOption = "--race-players=";

uint32_t ParsePlayers(std::string_view text) {
    uint32_t players = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), players);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size() || players < 1 ||
        players > PlayerSlots::kMaxPlayers) {
        throw std::invalid_argument("--race-players expects 1 to " + std::to_string(PlayerSlots::kMaxPlayers));
    }
    return players;
}

uint32_t ParseCourse(std::string_view text) {
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
        base = 16;
    }
    uint32_t course = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), course, base);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size() || course > kLastArena) {
        throw std::invalid_argument("--race expects a course id from 0 to 0x1F, or an arena 0x20 to 0x29 with --race-battle");
    }
    return course;
}

void SetUpMenuScenario(uint32_t course) {
    if (g_battle && course <= kLastRaceCourse) course = kBlockPlaza;  // --race-cpu may have picked Luigi Circuit
    const uint32_t racedata = Memory::Read32(MKW_GADDR(809BD728));
    if (racedata == 0) {
        throw std::runtime_error("--race: Racedata does not exist yet at SectionMgr::Init");
    }
    const uint32_t scenario = racedata + kMenuScenarioOffset;
    // Players 12+ live in the overflow slots (guest_abi/player_slots.h); the roster repeats.
    for (uint32_t i = 0; i < std::max(g_players, kPlayerCount); ++i) {
        const uint32_t player = PlayerSlots::PlayerAddress(racedata, scenario, i);
        const Racer racer = g_diverseRoster ? SelectDiverseRacer(i) : kRacers[i % kRacers.size()];
        Memory::Write32(player + kPlayerKartOffset, racer.kart);
        Memory::Write32(player + kPlayerCharacterOffset, racer.character);
        Memory::Write32(player + kPlayerTypeOffset,
                        i >= g_players ? kPlayerNone : i == 0 ? kPlayerRealLocal : kPlayerCpu);
        Memory::Write32(player + kPlayerTeamOffset, g_teams || g_battle ? i % 2 : 0);
    }
    const uint32_t settings = scenario + kSettingsOffset;
    Memory::Write32(settings + 0x00, course);
    Memory::Write32(settings + 0x04, g_battle ? 0 : kCc150);  // battle sets 50cc, which battle code ignores
    Memory::Write32(settings + 0x08, g_battle ? kGameModeBattle : g_gp ? kGameModeGp : kGameModeVs);
    Memory::Write32(settings + 0x0C, 0);
    Memory::Write32(settings + 0x10, g_battleType);
    Memory::Write32(settings + 0x14, kCpuHard);
    Memory::Write32(settings + 0x18, kItemsBalanced);
    Memory::Write8(settings + 0x24, 0);
    Memory::Write8(settings + 0x25, 3);
    Memory::Write32(settings + kSettingsModeFlags, (g_teams || g_battle ? 2 : 0) | (g_mirror ? 1 : 0));
}

} // namespace

namespace BootRace {

bool ParseArgument(std::string_view arg) {
    if (arg == "--race-roster=diverse") {
        g_diverseRoster = true;
        return true;
    }
    if (arg == "--race-roster=standard") {
        g_diverseRoster = false;
        return true;
    }
    if (arg.starts_with(kPlayersOption)) {
        g_players = ParsePlayers(arg.substr(kPlayersOption.size()));
        return true;
    }
    if (arg == "--lockstep") {
        g_lockstep = true;
        return true;
    }
    if (arg == "--race-impossible") {
        LocalVs::SetCpuTier(2);
        return true;
    }
    if (arg == "--race-very-hard") {
        LocalVs::SetCpuTier(1);
        return true;
    }
    if (arg == "--race-mirror") {
        g_mirror = true;
        return true;
    }
    if (arg == "--race-battle" || arg == "--race-battle=coin") {
        g_battle = true;
        g_battleType = arg.ends_with("coin") ? 1 : 0;
        if (!g_course) g_course = kBlockPlaza;
        return true;
    }
    if (arg == "--race-gp") {
        g_gp = true;
        return true;
    }
    if (arg == "--race-teams") {
        g_teams = true;
        return true;
    }
    if (arg == "--heap-check") {
        g_heapCheck = true;
        return true;
    }
    if (arg == "--race-auto") {
        g_autoDriven = true;
        if (!g_course) {
            g_course = kLuigiCircuit;
        }
        return true;
    }
    if (arg == "--race-cpu") {
        g_cpuDriven = true;
        if (!g_course) {
            g_course = kLuigiCircuit;
        }
        return true;
    }
    if (!arg.starts_with(kOption)) {
        return false;
    }
    const std::string_view rest = arg.substr(kOption.size());
    if (rest.empty()) {
        g_course = kLuigiCircuit;
    } else if (rest.front() == '=') {
        g_course = ParseCourse(rest.substr(1));
    } else {
        return false;
    }
    return true;
}

bool Requested() {
    return g_course.has_value();
}

bool CpuDriven() {
    return g_cpuDriven;
}

bool AutoDriven() {
    return g_autoDriven && !g_cpuDriven;
}

bool Scripted() {
    return g_cpuDriven || g_autoDriven;
}

bool Lockstep() {
    return g_lockstep;
}

bool HeapCheck() {
    return g_heapCheck;
}

} // namespace BootRace

// ArgumentParser::parse, picks the boot section in SectionMgr::Init.
extern "C" void ArgumentParser_Parse_805b9344(CpuContext* ctx) {
    static bool consumed = false;
    if (!g_course || consumed) {
        MKW_GUEST_FUNC(805B9344)(ctx);
        return;
    }
    consumed = true;
    SetUpMenuScenario(*g_course);
    // Analysis builds (MKW100_ACCESS_TRACE): watch the race, menu and award scenarios.
    if (AccessTrace::kEnabled) {
        const uint32_t racedata = Memory::Read32(MKW_GADDR(809BD728));
        AccessTrace::SetRange(racedata + kRaceScenarioOffset, racedata + kAwardScenarioEnd);
    }

    if (!g_cpuDriven) {
        // R25_GAMEPAD_AUTOBIND: WiiCompiled uses GameCube PAD for SDL/XInput devices.
        // If the host did not assign any device to P1 yet, bind controller 0.
        // Never steal a currently assigned P1 device or affect CPU-oracle runs.
        if (PADGetIndexForPort(0) < 0 && PADCount() != 0) {
            PADSetPortForIndex(0, 0);
            RT_LOGF("boot-race", "R25 auto-bound first detected gamepad to GC port 1\n");
        }
        const uint32_t sectionMgr = Memory::Read32(MKW_GADDR(809C1E38));
        if (sectionMgr == 0) {
            throw std::runtime_error("--race: SectionMgr instance is not set at SectionMgr::Init");
        }
        RT_LOGF("r26-input", "human boot: sectionMgr=0x%08X PADCount=%u port0_index=%d requestedGcnId=0x%X\n",
                sectionMgr, PADCount(), PADGetIndexForPort(0), kFirstGcnControllerId);
        ctx->gpr[3] = sectionMgr + kSectionPadOffset;
        ctx->gpr[4] = 0;
        ctx->gpr[5] = kFirstGcnControllerId;
        MKW_GUEST_FUNC(8061B490)(ctx);
        // ActivatePad only records the expected ID. The normal menu flow also
        // calls 8061B5A4 to bind that ID to InputManager's local input holder.
        // Without it SectionPad sees an expected GC pad but an unbound holder.
        ctx->gpr[3] = sectionMgr + kSectionPadOffset;
        MKW_GUEST_FUNC(8061B5A4)(ctx);
        const uint32_t inputMgr = Memory::Read32(MKW_GADDR(809BD70C));
        RT_LOGF("r27-input", "connected P1: expected=0x%X holder=0x%08X\n",
                Memory::Read32(sectionMgr + kSectionPadOffset + 4),
                inputMgr ? Memory::Read32(inputMgr + 8) : 0);
    }

    RT_LOGF("boot-race", "booting into 1P %s on course 0x%02X with %u racers%s\n", g_battle ? "battle" : g_gp ? "GP" : "VS",
            g_battle && *g_course <= kLastRaceCourse ? kBlockPlaza : *g_course, g_players,
            g_cpuDriven ? " (CPU driven)" : g_autoDriven ? " (scripted human)" : "");
    if (g_teams) RT_LOGF("boot-race", "team mode: racers alternate red / blue\n");
    g_raceSectionPicked = true;
    ctx->gpr[3] = g_battle ? kSectionP1Battle : g_gp ? kSectionP1Gp : g_teams ? kSectionP1TeamVs : kSectionP1Vs;
}
REGISTER_NATIVE_FUNCTION_AS(0x805B9344, ArgumentParser_Parse_805b9344, "ArgumentParser_Parse_805b9344");

// Retro Rewind (Pulsar) replaces SectionMgr::Init's parse call with 0x8184EC74, which calls
// ArgumentParser::parse but returns its own boot section (-1, or 0x55 for a pending license),
// so --race booted to the title screen. Keep the race section when parse picked it.
// The base game never calls 0x8184EC74.
extern "C" void Pulsar_BootSection_8184ec74(CpuContext* ctx) {
    MKW_GUEST_NEXT(8184EC74)(ctx);
    if (g_raceSectionPicked) {
        g_raceSectionPicked = false;
        ctx->gpr[3] = g_battle ? kSectionP1Battle : g_gp ? kSectionP1Gp : g_teams ? kSectionP1TeamVs : kSectionP1Vs;
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x8184EC74, Pulsar_BootSection_8184ec74, "Pulsar_BootSection_8184ec74");

// Item Fever (Pulsar) hooks the same SectionMgr::Init call (0x80634F20) with its own boot-section
// function at 0x8180D038. Retro Rewind has no function there, so this never runs under it.
extern "C" void ItemFever_BootSection_8180d038(CpuContext* ctx) {
    MKW_GUEST_NEXT(8180D038)(ctx);
    if (g_raceSectionPicked) {
        g_raceSectionPicked = false;
        ctx->gpr[3] = g_battle ? kSectionP1Battle : g_gp ? kSectionP1Gp : g_teams ? kSectionP1TeamVs : kSectionP1Vs;
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x8180D038, ItemFever_BootSection_8180d038, "ItemFever_BootSection_8180d038");

// --race-cpu hands the local racer to the CPU driver while it keeps its camera and HUD.

// RacedataScenario::InitScreens; local racers keep no real controller channel.
extern "C" void RacedataScenario_InitScreens_8052f4e8(CpuContext* ctx) {
    if (!g_cpuDriven) {
        const uint32_t scenario = ctx->gpr[3];
        if (g_course) {
            const uint32_t sectionMgr = Memory::Read32(MKW_GADDR(809C1E38));
            if (sectionMgr && Memory::Read32(sectionMgr + kSectionPadOffset + 4) == kFirstGcnControllerId) {
                // InputManager is reset during the transition into a race.
                // Restore the selected binding before InitScreens reads its type.
                const CpuContext saved = *ctx;
                ctx->gpr[3] = sectionMgr + kSectionPadOffset;
                MKW_GUEST_FUNC(8061B5A4)(ctx);
                *ctx = saved;
            }
        }
        MKW_GUEST_NEXT(8052F4E8)(ctx);
        if (g_course) {
            RT_LOGF("r27-input", "InitScreens: P1 channel=%u controllerType=%d\n",
                    unsigned(Memory::Read8(scenario + kPlayersOffset + kPlayerControllerChannelOffset)),
                    int32_t(Memory::Read32(scenario + kPlayersOffset + 0xD0)));
        }
        return;
    }
    const uint32_t players = ctx->gpr[3] + kPlayersOffset;
    std::array<uint8_t, kPlayerCount> channels{};
    for (uint32_t i = 0; i < kPlayerCount; ++i) {
        channels[i] = Memory::Read8(players + i * kPlayerSize + kPlayerControllerChannelOffset);
    }
    MKW_GUEST_NEXT(8052F4E8)(ctx);
    for (uint32_t i = 0; i < kPlayerCount; ++i) {
        const uint32_t player = players + i * kPlayerSize;
        if (Memory::Read32(player + kPlayerTypeOffset) == kPlayerRealLocal) {
            Memory::Write8(player + kPlayerControllerChannelOffset, channels[i]);
        }
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x8052F4E8, RacedataScenario_InitScreens_8052f4e8, "RacedataScenario_InitScreens_8052f4e8");

// KartAIController::IsCPU
extern "C" void KartAIController_IsCPU_8072624c(CpuContext* ctx) {
    MKW_GUEST_FUNC(8072624C)(ctx);
    if (g_cpuDriven) {
        ctx->gpr[3] = 1;
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x8072624C, KartAIController_IsCPU_8072624c, "KartAIController_IsCPU_8072624c");

// Kart::Link::IsCPU; bit 0 marks a CPU racer, bit 1 the local one.
extern "C" void KartLink_IsCPU_80590664(CpuContext* ctx) {
    MKW_GUEST_FUNC(80590664)(ctx);
    if (g_cpuDriven) {
        ctx->gpr[3] = ctx->gpr[0] & 3;
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x80590664, KartLink_IsCPU_80590664, "KartLink_IsCPU_80590664");

// RacedataScenario::InitRNG; fixed seeds keep --race-cpu runs repeatable for benchmarking.
extern "C" void RacedataScenario_InitRng_8052f924(CpuContext* ctx) {
    const uint32_t scenario = ctx->gpr[3];
    MKW_GUEST_FUNC(8052F924)(ctx);
    if (g_cpuDriven || g_autoDriven) {
        Memory::Write32(scenario + kSettingsOffset + 0x2C, 0x74A1B095u);
        Memory::Write32(scenario + kSettingsOffset + 0x30, 0x1234567u);
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x8052F924, RacedataScenario_InitRng_8052f924, "RacedataScenario_InitRng_8052f924");

// SectionPad::GetFirstDeactivatedPad; a CPU driven racer needs no controller.
extern "C" void SectionPad_GetFirstDeactivatedPad_8061b9bc(CpuContext* ctx) {
    const uint32_t sectionPad = ctx->gpr[3];
    MKW_GUEST_FUNC(8061B9BC)(ctx);
    if (g_course && !g_cpuDriven) {
        static uint32_t calls = 0;
        static uint32_t previous = 0xFFFFFFFEu;
        const uint32_t result = ctx->gpr[3];
        if (++calls <= 3000 && (calls == 1 || result != previous)) {
            RT_LOGF("r27-input", "connection check: disconnected=%d expected=0x%X actual=0x%X mode=%u mask=0x%X\n",
                    int32_t(result), Memory::Read32(sectionPad + 4),
                    Memory::Read32(sectionPad + 8), Memory::Read32(sectionPad + 12),
                    Memory::Read32(sectionPad + 88));
        }
        previous = result;
    }
    if (g_cpuDriven) {
        ctx->gpr[3] = static_cast<uint32_t>(-1);
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x8061B9BC, SectionPad_GetFirstDeactivatedPad_8061b9bc,
                            "SectionPad_GetFirstDeactivatedPad_8061b9bc");
