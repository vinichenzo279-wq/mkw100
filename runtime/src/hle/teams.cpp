// mkw100 teams for offline VS of more than 12 racers. See teams.h.
//
// Modelled on Retro Rewind's extended teams (rr-pulsar PulsarEngine/UI/ExtendedTeamSelect, GPL-3,
// with Pulsar and MKW-SP under MIT): racers shuffled into teams at the start of a series, a colour
// per team chosen from a fixed table and randomised per series, the game's red/blue team mode left
// off so the race plays as normal VS, team identity shown by tinting UI that already exists. RR is
// limited to 12 players and 6 colours; this keeps one team per racer index up to
// PlayerSlots::kMaxPlayers, 8 colours, and scores a team by its members' average, not their sum,
// so a team one racer short is not behind.

#include "teams.h"

#include "abi_bridge.h"
#include "boot_race.h"
#include "guest_function_imports.h"
#include "local_vs.h"
#include "guest_abi/player_slots.h"
#include "runtime_config.h"
#include "memory.h"
#include "runtime_log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <numeric>
#include <random>
#include <cstdlib>
#include <string>

namespace {

// RR's six (red, orange, yellow, green, blue, purple), then pink and cyan.
constexpr std::array<Teams::Colour, Teams::kMaxTeams> kColours{{
    {255, 40, 40, "Red"},
    {255, 150, 0, "Orange"},
    {255, 225, 0, "Yellow"},
    {40, 200, 40, "Green"},
    {30, 90, 255, "Blue"},
    {160, 60, 220, "Purple"},
    {255, 110, 180, "Pink"},
    {0, 210, 220, "Cyan"},
}};
constexpr Teams::Colour kNoColour{255, 255, 255, "None"};

std::atomic<uint32_t> g_count{2};
std::atomic<uint32_t> g_teamSize{0};
std::atomic<bool> g_byAverage{true};
std::atomic<uint32_t> g_inPlay{0};
std::atomic<bool> g_teamHits{true};
std::atomic<uint32_t> g_playerColour{Teams::kNoTeam};
std::atomic<bool> g_custom{false};
std::array<std::atomic<uint32_t>, Teams::kMaxTeams> g_colourSize{};

const char* const kTeamsColourKeys[Teams::kMaxTeams] = {"red", "orange", "yellow", "green", "blue", "purple", "pink", "cyan"};

void TeamsSaveColourSizes() {
    std::string text;
    for (uint32_t c = 0; c < Teams::kMaxTeams; ++c) text += (c ? "," : "") + std::to_string(g_colourSize[c].load());
    RuntimeConfigFile::WriteSetting("local_vs", "team_sizes", RuntimeConfigFile::FormatString(text));
}

// Sizes for `teams` teams from weights adding up to anything, at least 1 each (racers > teams):
// largest remainder.
std::vector<uint32_t> TeamsScaleSizes(const std::vector<uint32_t>& weights, uint32_t racers) {
    // Proportional shares with the largest remainders rounded up, so sizes that already add up to
    // the field are kept exactly; then every team gets at least one racer (racers > teams).
    const uint32_t n = uint32_t(weights.size());
    uint64_t sum = 0;
    for (uint32_t w : weights) sum += w;
    std::vector<uint32_t> sizes(n, 0);
    std::vector<std::pair<uint64_t, uint32_t>> remainders;
    uint32_t given = 0;
    for (uint32_t k = 0; k < n; ++k) {
        const uint64_t share = uint64_t(racers) * weights[k];
        sizes[k] = uint32_t(share / sum);
        given += sizes[k];
        remainders.emplace_back(share % sum, k);
    }
    std::stable_sort(remainders.begin(), remainders.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (uint32_t i = 0; given < racers; ++i, ++given) sizes[remainders[i % n].second] += 1;
    for (uint32_t k = 0; k < n; ++k) {
        while (sizes[k] == 0) {
            const uint32_t from = uint32_t(std::max_element(sizes.begin(), sizes.end()) - sizes.begin());
            --sizes[from];
            ++sizes[k];
        }
    }
    return sizes;
}
std::atomic<bool> g_teamVsRace{false};  // OnInitRace: this race is a Team VS race past 12
std::atomic<bool> g_menuDealt{false};   // DealForMenu dealt the coming series
std::mutex g_lock;
std::vector<uint8_t> g_teamOf;                  // per racer
std::array<uint8_t, Teams::kMaxTeams> g_colour{};  // team -> colour index
std::vector<Teams::Standing> g_standings;

} // namespace

extern "C" uint32_t MKW100_PlayerSlotOffset(uint32_t index);

namespace local_vs_detail {
uint32_t RaceCount();  // local_vs.cpp: the race scenario's player count
}

namespace {

uint32_t Clamp(uint32_t teams) { return std::clamp(teams, 2u, Teams::kMaxTeams); }

constexpr uint32_t kTeamsRacedata = 0x809BD728;     // RaceConfig::spInstance
constexpr uint32_t kTeamsRaceModeFlags = 0x20 + 0xB70;  // race scenario settings modeFlags
constexpr uint32_t kTeamsRaceNumber = 0x20 + 0xB6C;     // race scenario: race of the series (u8)
constexpr uint32_t kTeamsFlag = 2;
constexpr uint32_t kTeamsRaceGameMode = 0x20 + 0xB50;  // race scenario settings gameMode
constexpr uint32_t kTeamsModeVs = 1;
// Race scenario player team (+0x28 + 0xCC), compared for equality by the hit checks below; 2 there
// means no team, so team t is stored as t + kTeamsFieldBase.
constexpr uint32_t kTeamsRaceTeam = 0x28 + 0xCC;
constexpr uint32_t kTeamsFieldBase = 3;

} // namespace

namespace Teams {

void Initialize(uint32_t teamsFromConfig) {
    const auto& c = RuntimeConfigFile::Get();
    g_count = Clamp(teamsFromConfig);
    g_teamSize = std::min<uint32_t>(c.localVsTeamSize.value_or(0), PlayerSlots::kMaxPlayers);
    g_byAverage = c.localVsTeamScoring.value_or("average") != "total";
    g_teamHits = c.localVsTeamHits.value_or(true);
    g_playerColour = kNoTeam;
    for (uint32_t k = 0; k < kMaxTeams; ++k)
        if (c.localVsTeamColour.value_or("random") == kTeamsColourKeys[k]) g_playerColour = k;
    g_custom = c.localVsTeamCustom.value_or(false);
    const std::string sizes = c.localVsTeamSizes.value_or("50,0,0,0,50,0,0,0");
    size_t at = 0;
    for (uint32_t k = 0; k < kMaxTeams; ++k) {
        const size_t comma = sizes.find(',', at);
        const std::string field = sizes.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        g_colourSize[k] = std::min<uint32_t>(uint32_t(std::strtoul(field.c_str(), nullptr, 10)), PlayerSlots::kMaxPlayers);
        if (comma == std::string::npos) { for (uint32_t r = k + 1; r < kMaxTeams; ++r) g_colourSize[r] = 0; break; }
        at = comma + 1;
    }
}

uint32_t PlayerColour() { return g_playerColour.load(); }
void SetPlayerColour(uint32_t colour) {
    g_playerColour = colour < kMaxTeams ? colour : kNoTeam;
    RuntimeConfigFile::WriteSetting("local_vs", "team_colour",
                                    RuntimeConfigFile::FormatString(colour < kMaxTeams ? kTeamsColourKeys[colour] : "random"));
}
bool CustomSizes() { return g_custom.load(); }
void SetCustomSizes(bool on) {
    g_custom = on;
    RuntimeConfigFile::WriteSetting("local_vs", "team_custom", on ? "true" : "false");
}
uint32_t ColourSize(uint32_t colour) { return colour < kMaxTeams ? g_colourSize[colour].load() : 0; }
void SetColourSize(uint32_t colour, uint32_t size) {
    if (colour >= kMaxTeams) return;
    g_colourSize[colour] = std::min<uint32_t>(size, PlayerSlots::kMaxPlayers);
    TeamsSaveColourSizes();
}
const Colour& ColourByIndex(uint32_t colour) { return colour < kMaxTeams ? kColours[colour] : kNoColour; }

bool TeamHits() { return g_teamHits.load(); }
void SetTeamHits(bool on) {
    g_teamHits = on;
    RuntimeConfigFile::WriteSetting("local_vs", "team_hits", on ? "true" : "false");
}

uint32_t TeamSize() { return g_teamSize.load(); }
void SetTeamSize(uint32_t racers) {
    g_teamSize = std::min<uint32_t>(racers, PlayerSlots::kMaxPlayers);
    RuntimeConfigFile::WriteSetting("local_vs", "team_size", std::to_string(g_teamSize.load()));
}
bool ScoreByAverage() { return g_byAverage.load(); }
void SetScoreByAverage(bool average) {
    g_byAverage = average;
    RuntimeConfigFile::WriteSetting("local_vs", "team_scoring", RuntimeConfigFile::FormatString(average ? "average" : "total"));
}
uint32_t InPlay() { return g_inPlay.load(); }
uint32_t Standing::Score() const { return g_byAverage.load() ? uint32_t(average + 0.5) : total; }

void OnInitRace(uint32_t racedata) {
    g_teamVsRace = false;
    if (racedata == 0) return;
    const uint32_t flags = Memory::Read32(racedata + kTeamsRaceModeFlags);
    // VS only: battles always run with the team flag and keep the game's red/blue.
    if (Memory::Read32(racedata + kTeamsRaceGameMode) != kTeamsModeVs) return;
    if ((flags & kTeamsFlag) == 0 || local_vs_detail::RaceCount() <= 12) return;
    // Plain VS from here (karts, HUD, item hits); team identity is ours. The menu scenario keeps
    // the flag, so every race of the series comes back through here.
    Memory::Write32(racedata + kTeamsRaceModeFlags, flags & ~kTeamsFlag);
    g_teamVsRace = true;
    bool fresh = Memory::Read8(racedata + kTeamsRaceNumber) == 0;
    {
        std::lock_guard guard(g_lock);
        const bool sized = g_teamOf.size() == local_vs_detail::RaceCount();
        if (fresh && sized && g_menuDealt.exchange(false)) fresh = false;  // shown on the team screen
        fresh = fresh || !sized;
    }
    if (fresh) {
        NewSeries(local_vs_detail::RaceCount(), BootRace::Scripted());
        Log("series", {});
    }
    for (uint32_t i = 0; i < local_vs_detail::RaceCount(); ++i) {
        const uint32_t team = TeamOf(i);
        Memory::Write32(racedata + MKW100_PlayerSlotOffset(i) + kTeamsRaceTeam, team < kMaxTeams ? team + kTeamsFieldBase : 2);
    }
}

uint32_t Count() { return g_count.load(); }

void DealForMenu(uint32_t racers, bool redeal) {
    g_teamVsRace = false;  // at the menus now; the last race's flag must not drive TeamOf
    bool sized;
    {
        std::lock_guard guard(g_lock);
        sized = g_teamOf.size() == racers;
    }
    if (!redeal && g_menuDealt.load() && sized) return;  // already shown for this series
    NewSeries(racers, BootRace::Scripted());
    g_menuDealt = true;
    Log("team screen", {});
}

uint32_t Members(uint32_t team) {
    std::lock_guard guard(g_lock);
    uint32_t n = 0;
    for (uint8_t t : g_teamOf) n += t == team;
    return n;
}

void SetCount(uint32_t teams) {
    g_count = Clamp(teams);
    RuntimeConfigFile::WriteSetting("local_vs", "teams", std::to_string(g_count.load()));
}

void NewSeries(uint32_t racers, bool repeatable) {
    // Custom: one team per colour with a size; else Count() teams, or teams of TeamSize().
    std::vector<uint32_t> customColours, weights;
    if (CustomSizes()) {
        for (uint32_t k = 0; k < kMaxTeams; ++k)
            if (ColourSize(k) > 0) { customColours.push_back(k); weights.push_back(ColourSize(k)); }
        if (customColours.size() < 2) customColours.clear();
    }
    const uint32_t size = TeamSize();
    const uint32_t teams = !customColours.empty() ? uint32_t(customColours.size())
                         : size ? Clamp((racers + size - 1) / size) : Count();
    g_inPlay = racers > teams ? teams : 0;
    std::lock_guard guard(g_lock);
    g_standings.clear();
    g_teamOf.assign(racers, uint8_t(kNoTeam));
    if (racers <= teams) return;
    std::mt19937 random(repeatable ? 0x74A1B095u
                                   : uint32_t(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::vector<uint32_t> order(racers);
    std::iota(order.begin(), order.end(), 0u);
    std::shuffle(order.begin(), order.end(), random);
    std::iota(g_colour.begin(), g_colour.end(), uint8_t(0));
    if (!customColours.empty()) {
        const auto sizes = TeamsScaleSizes(weights, racers);
        uint32_t next = 0;
        for (uint32_t k = 0; k < teams; ++k) {
            g_colour[k] = uint8_t(customColours[k]);
            for (uint32_t m = 0; m < sizes[k]; ++m) g_teamOf[order[next++]] = uint8_t(k);
        }
    } else {
        for (uint32_t i = 0; i < racers; ++i) g_teamOf[order[i]] = uint8_t(i % teams);  // sizes differ by <= 1
        if (!repeatable) std::shuffle(g_colour.begin(), g_colour.end(), random);  // any colours
    }
    // Your colour: put racer 0 on that colour's team (custom), or give racer 0's team that colour.
    const uint32_t mine = PlayerColour();
    if (mine < kMaxTeams) {
        uint32_t want = kNoTeam;
        for (uint32_t k = 0; k < teams; ++k) if (g_colour[k] == mine) want = k;
        if (want == kNoTeam) {
            // Not among this series' colours: racer 0's team takes it.
            g_colour[g_teamOf[0]] = uint8_t(mine);
        } else if (!customColours.empty()) {
            if (g_teamOf[0] != want) {  // swap places with a member of that team
                for (uint32_t i = 1; i < racers; ++i)
                    if (g_teamOf[i] == want) { g_teamOf[i] = g_teamOf[0]; g_teamOf[0] = uint8_t(want); break; }
            }
        } else {
            std::swap(g_colour[want], g_colour[g_teamOf[0]]);
        }
    }
}

bool Active() {
    if (!g_teamVsRace.load() || local_vs_detail::RaceCount() <= 12) return false;
    std::lock_guard guard(g_lock);
    return g_inPlay.load() >= 2 && g_teamOf.size() > g_inPlay.load();
}

uint32_t TeamOf(uint32_t player) {
    const uint32_t racers = local_vs_detail::RaceCount();
    bool assign = false;
    {
        std::lock_guard guard(g_lock);
        // Lazily (headless boots skip the menus), but only for a race: at the menus the race
        // scenario still holds the last race's count, and the team screen's teams must stand.
        assign = g_teamVsRace.load() && g_teamOf.size() != racers;
    }
    if (assign) {
        NewSeries(racers, BootRace::Scripted());
        std::vector<std::pair<uint32_t, uint32_t>> none;
        Log("assigned", none);
    }
    std::lock_guard guard(g_lock);
    return player < g_teamOf.size() ? g_teamOf[player] : kNoTeam;
}

const Colour& ColourOf(uint32_t team) {
    if (team >= kMaxTeams) return kNoColour;
    std::lock_guard guard(g_lock);
    return kColours[g_colour[team]];
}

uint32_t ColourIndexOf(uint32_t team) {
    if (team >= kMaxTeams) return kNoTeam;
    std::lock_guard guard(g_lock);
    return g_colour[team];
}

std::vector<Standing> Rank(const std::vector<std::pair<uint32_t, uint32_t>>& points) {
    std::array<Standing, kMaxTeams> sums{};
    for (uint32_t t = 0; t < kMaxTeams; ++t) sums[t].team = t;
    for (const auto& [player, earned] : points) {
        const uint32_t team = TeamOf(player);
        if (team >= kMaxTeams) continue;
        sums[team].members += 1;
        sums[team].total += earned;
    }
    std::vector<Standing> result;
    for (auto& s : sums) {
        if (s.members == 0) continue;
        s.average = double(s.total) / double(s.members);
        result.push_back(s);
    }
    const bool byAverage = g_byAverage.load();
    std::stable_sort(result.begin(), result.end(), [byAverage](const Standing& a, const Standing& b) {
        return byAverage ? a.average > b.average : a.total > b.total;
    });
    return result;
}

void SetStandings(std::vector<Standing> standings) {
    std::lock_guard guard(g_lock);
    g_standings = std::move(standings);
}

std::vector<Standing> LastStandings() {
    std::lock_guard guard(g_lock);
    return g_standings;
}

void Log(const char* what, const std::vector<std::pair<uint32_t, uint32_t>>& points) {
    if (!g_teamVsRace.load() && !g_menuDealt.load()) return;
    if (points.empty()) {
        std::array<uint32_t, kMaxTeams> sizes{};
        uint32_t racers = 0;
        {
            std::lock_guard guard(g_lock);
            racers = uint32_t(g_teamOf.size());
            for (uint8_t t : g_teamOf) if (t < kMaxTeams) ++sizes[t];
        }
        std::string line;
        for (uint32_t t = 0; t < InPlay(); ++t) line += std::string(" ") + ColourOf(t).name + "=" + std::to_string(sizes[t]);
        RT_LOGF("teams", "%s %u racers in %u teams:%s (racer 0 is %s)\n", what, racers, InPlay(), line.c_str(),
                ColourOf(TeamOf(0)).name);
        return;
    }
    for (const auto& s : Rank(points)) {
        RT_LOGF("teams", "%s %s: %u members, %u points, average %.2f\n", what, ColourOf(s.team).name, s.members, s.total,
                s.average);
    }
}

} // namespace Teams

// Team hits off: the game's own hit checks skip teammates when the race scenario's team flag is set
// (Kart::Collision::CheckItemCollision 0x8057257C, HandleFibCollision 0x8057325C,
// CheckKartCollision 0x8056F7F0: flag, then race team +0xF4 compared for equality, 2 = none).
// OnInitRace cleared that flag for the race and stored our teams there, so with team hits off the
// flag is set again for the length of each check only. Nothing they call reads the flag.
namespace {

struct TeamsHitScope {
    uint32_t flagsAddr = 0;
    uint32_t saved = 0;
    TeamsHitScope() {
        if (g_teamHits.load() || !Teams::Active()) return;
        const uint32_t rd = Memory::Read32(kTeamsRacedata);
        if (rd == 0) return;
        flagsAddr = rd + kTeamsRaceModeFlags;
        saved = Memory::Read32(flagsAddr);
        Memory::Write32(flagsAddr, saved | kTeamsFlag);
    }
    ~TeamsHitScope() {
        if (flagsAddr) Memory::Write32(flagsAddr, saved);
    }
};

} // namespace

extern "C" void Teams_ItemCollision_8057257c(CpuContext* ctx) {
    TeamsHitScope scope;
    MKW_GUEST_NEXT(8057257C)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8057257C, Teams_ItemCollision_8057257c, "Teams_ItemCollision_8057257c");

extern "C" void Teams_FibCollision_8057325c(CpuContext* ctx) {
    TeamsHitScope scope;
    MKW_GUEST_NEXT(8057325C)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8057325C, Teams_FibCollision_8057325c, "Teams_FibCollision_8057325c");

extern "C" void Teams_KartCollision_8056f7f0(CpuContext* ctx) {
    TeamsHitScope scope;
    MKW_GUEST_NEXT(8056F7F0)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8056F7F0, Teams_KartCollision_8056f7f0, "Teams_KartCollision_8056f7f0");
