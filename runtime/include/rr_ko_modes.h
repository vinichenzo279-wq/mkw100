#pragma once

#include <cstdint>
#include <vector>

struct CpuContext;

// Retro Rewind's KO, Lap KO and KO Royale past 12 racers (runtime/src/hle/rr_ko_modes.cpp).
namespace RrKoModes {

struct Standing {
    uint32_t id;     // player id this race
    uint32_t place;  // 1-based place this race
    uint32_t total;  // series total after this race
};

// The series standings page of an offline VS race past 12 racers (LocalVs_TotalRows, which takes
// the place of GPVSLeaderboardTotal::FillRows and with it Retro Rewind's KO::Mgr::ProcessKOs).
void OnSeriesStandings(CpuContext* ctx, const std::vector<Standing>& standings);

// The balloons every racer starts a KO Royale race with past 12 racers (balloon_battle.cpp hands
// them out), or 0 when the race is not KO Royale.
uint32_t RoyaleStartingBalloons(CpuContext* ctx);

// The laps of an offline Lap KO race past 12 racers (N knocked out per lap), or 0 to keep Retro
// Rewind's; RaceinfoPlayer's constructor (race_laps.cpp) asks.
uint32_t LapKoLaps(CpuContext* ctx);

} // namespace RrKoModes
