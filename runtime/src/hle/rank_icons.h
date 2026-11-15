#pragma once

#include <cstdint>

struct CpuContext;

// mkw100 position icons for 13th..128th (runtime/src/hle/rank_icons.cpp).
namespace RankIcons {

// rank_icons/rankNNN.tpl for `place` is present and loaded (13..128).
bool Available(uint32_t place);

// The race HUD position counter `control` (CtrlRaceRankNum) shows `place`: for 13+ its texture
// pattern's 12th entry is the place's icon, for 1..12 the stock texture. False when 13+ has no
// icon. Allocates in the race scene's heap through `ctx` and restores it.
bool Show(CpuContext* ctx, uint32_t control, uint32_t place);

// A race is being built (Raceinfo's constructor): forget the last race's buffers and counters,
// which lived in its scene heaps.
void NewRace();

} // namespace RankIcons
