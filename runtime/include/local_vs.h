#pragma once
#include <cstdint>
struct CpuContext;
namespace LocalVs {
void Initialize();
void DrawSettings();
void DrawHud();
void TestItems(CpuContext* ctx, uint32_t raceinfo);
void PrepareRace(uint32_t racedata);
// After RaceConfig::InitRace: past 12 racers in GP, every racer's start position from the cup standings.
void AfterInitRace(uint32_t racedata);
bool UnlocksEnabled();
// "Impossible CPUs": Hard CPUs get the faster speed and always-on action tables of ai_impossible.cpp.
bool ImpossibleCpus();
void SetImpossibleCpus(bool on);  // --race-impossible, for this run only
// Racers for the next single-player VS series, 1..PlayerSlots::kMaxPlayers (F10 and the VS rules page).
int DesiredCount();
// The most racers this product runs (128; Retro Rewind 100 for now).
int MaxRacers();
// Local players in the menu scenario (slots 0-3 of type 0), for split screen.
uint32_t LocalPlayers();
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
