#pragma once

#include <cstdint>

// mkw100: 200cc in place of Mirror mode. runtime/src/hle/cc200.cpp.
namespace Cc200 {

// After RaceConfig::initRace (0x805302C4) copied the menu scenario: turns a 150cc Mirror GP/VS
// race into an unmirrored 200cc race and sets the speed globals for it.
void OnInitRace(uint32_t racedata);

// True while the current race is 200cc.
bool Active();

// Mirror is 200cc (the build toggle): menus say "200cc" for Mirror.
bool ReplacesMirror();

} // namespace Cc200
