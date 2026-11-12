#pragma once

#include <cstdint>

struct CpuContext;
#include <utility>
#include <vector>

// mkw100 teams for offline Team VS of more than 12 racers (runtime/src/hle/teams.cpp): 2..8 teams
// of near-equal size (differing by at most one), kept for a series, scored by the average points
// per member so uneven sizes stay fair. As Retro Rewind's extended teams do, the race runs with the
// game's red/blue team flag cleared (plain karts, items hit everyone) and team colours tint
// existing UI (name balloons, minimap, result rows); results go to the scrolling standings page
// with team rows on top. Solo VS never has teams; up to 12 racers Team VS is the game's own.
namespace Teams {

inline constexpr uint32_t kMaxTeams = 8;
inline constexpr uint32_t kNoTeam = 0xFF;

struct Colour {
    uint8_t r, g, b;
    const char* name;
};

void Initialize(uint32_t teamsFromConfig);
// Team VS teams past 12 racers, 2..kMaxTeams (Config.toml local_vs.teams; 0 or 1 there means 2).
uint32_t Count();
void SetCount(uint32_t teams);  // F10; writes Config.toml
// Racers per team instead (local_vs.team_size): 0 = use Count(). The teams are then
// racers / size rounded up, 2..kMaxTeams (only 8 colours: past that the teams grow).
uint32_t TeamSize();
void SetTeamSize(uint32_t racers);
// Team score: the members' average (local_vs.team_scoring "average", default) or their total.
bool ScoreByAverage();
void SetScoreByAverage(bool average);
// Teams in play this series (0 when none dealt).
uint32_t InPlay();
// Items (and star/mega/bullet bumps) hit teammates (local_vs.team_hits, default on).
bool TeamHits();
void SetTeamHits(bool on);
// A local player's team colour (player 0-3; split screen): kNoTeam = random, else a colour index
// (local_vs.team_colour, team_colour_p2..p4). Each player with a colour is put on that team.
uint32_t PlayerColour(uint32_t player = 0);
void SetPlayerColour(uint32_t colour, uint32_t player = 0);
// Custom team sizes per colour (local_vs.team_custom, local_vs.team_sizes): every colour with a
// size above 0 is a team; sizes are scaled to the field when they do not add up to it.
bool CustomSizes();
void SetCustomSizes(bool on);
uint32_t ColourSize(uint32_t colour);
void SetColourSize(uint32_t colour, uint32_t size);
// The fixed colour table, for settings UIs.
const Colour& ColourByIndex(uint32_t colour);

// Battle past 12 keeps the game's red (team 0) and blue (1): balloons, items and karts are red/blue
// assets. Red's size (local_vs.battle_red, 0 = even) and the player's team (battle_team: 0 random,
// 1 red, 2 blue) are settable; local_vs.cpp deals them at the first battle of a series.
uint32_t BattleRedSize();
void SetBattleRedSize(uint32_t racers);
uint32_t BattleRedFor(uint32_t racers);  // red's size in a battle of `racers`
uint32_t BattleTeamChoice();
void SetBattleTeamChoice(uint32_t choice);

// After RaceConfig::InitRace (0x805302C4): a Team VS race of more than 12 racers has the race
// scenario's team flag cleared and becomes a teams race; the first race of a series (and a
// headless boot) deals new teams.
void OnInitRace(uint32_t racedata);
// The pre-race team screen of a Team VS series of `racers` (> 12): deals the series' teams now so
// the screen and the race agree; OnInitRace keeps them for that series.
void DealForMenu(uint32_t racers, bool redeal = false);
// The colour index of team `team`'s members count, for UIs.
uint32_t Members(uint32_t team);

// A new series of `racers`: shuffle racers into Count() teams and pick the colour order.
// `repeatable` uses a fixed seed (test modes).
void NewSeries(uint32_t racers, bool repeatable);
// Teams in play for this race: a Team VS race of more than 12 racers (OnInitRace).
bool Active();
// The team of racer `player`, kNoTeam when teams are off. Assigns lazily (headless races start
// without the menus' series setup).
uint32_t TeamOf(uint32_t player);
const Colour& ColourOf(uint32_t team);
// Index of the team's colour in the fixed table (Red, Orange, Yellow, Green, Blue, Purple, Pink, Cyan).
uint32_t ColourIndexOf(uint32_t team);

struct Standing {
    uint32_t team;
    uint32_t members;
    uint32_t total;      // members' points added up
    double average;      // total / members
    uint32_t Score() const;  // the shown and ranked score: average (rounded) or total
};
// Team standings from (racer, points) pairs, best score first.
std::vector<Standing> Rank(const std::vector<std::pair<uint32_t, uint32_t>>& points);
// Last computed series standings (for the F10 table and the results page).
void SetStandings(std::vector<Standing> standings);
std::vector<Standing> LastStandings();
// "[teams] ..." lines: assignment and the standings of `points`.
void Log(const char* what, const std::vector<std::pair<uint32_t, uint32_t>>& points);

// team_ui.cpp: tint a results row (CtrlRaceResult) in the team colour of `player`; does nothing
// when teams are off. Guest calls clobber `ctx`; the caller restores it.
void TintResultRow(CpuContext* ctx, uint32_t control, uint32_t player);

} // namespace Teams
