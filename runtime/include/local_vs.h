#pragma once
#include <cstdint>
struct CpuContext;
namespace LocalVs {
void Initialize();
void DrawSettings();
void DrawHud();
void TestItems(CpuContext* ctx, uint32_t raceinfo);
// F10 "Finish race / battle now" and MKW100_TEST_FINISH, polled every race frame.
void PollFinishNow(CpuContext* ctx);
// Adaptive frame skip past 12 racers: 30 FPS rendering while the race cannot keep 60, every race frame.
void AdaptiveFramerate(CpuContext* ctx);
void PrepareRace(uint32_t racedata);
// After RaceConfig::InitRace: past 12 racers in GP, every racer's start position from the cup standings.
void AfterInitRace(uint32_t racedata);
bool UnlocksEnabled();
// "Impossible CPUs": Hard CPUs get the faster speed and always-on action tables of ai_impossible.cpp.
bool ImpossibleCpus();  // CpuTier() > 0
// Above Hard (ai_impossible.cpp): 0 none, 1 Very Hard, 2 Impossible. The CPU row's top options.
int CpuTier();
void SetCpuTier(int tier);  // --race-very-hard / --race-impossible, for this run only
// Racers for the next single-player VS series, 1..PlayerSlots::kMaxPlayers (F10 and the VS rules page).
int DesiredCount();
// The most racers any product runs (128).
int MaxRacers();
// Local players in the menu scenario (slots 0-3 of type 0), for split screen.
uint32_t LocalPlayers();
// [local_vs] speedometer (F10): the race HUD speedometer (hud_speedometer.cpp; ImGui in mod products).
bool SpeedometerEnabled();
void SetDesiredCount(int count);
// Expanded (13+) offline VS balancing, F10 and Config.toml [local_vs] scoring / item_distribution.
enum class Scoring : uint32_t { ClassicSpread, ClassicTop12, Linear };
enum class ItemDistribution : uint32_t { Even, Generous, Tight, ClassicTop11 };
ItemDistribution CurrentItemDistribution();
// Debug item cycler (runtime/src/hle/item_cycler.cpp), any offline race.
enum class ItemCycler : uint32_t { Off, Debug, Infinite };
ItemCycler CurrentItemCycler();
// Points for finishing `place` (1-based) of `count` racers under the current scoring.
uint32_t PointsFor(uint32_t place, uint32_t count);
// The 0-based ItemSlot.bin row (of `rows`) that race position `position` (0-based) of `count` draws from.
uint32_t ItemRowFor(uint32_t position, uint32_t count, uint32_t rows);
}
