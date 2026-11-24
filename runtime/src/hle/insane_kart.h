#pragma once

#include <cstdint>

// mkw100: Insane Kart Wii (docs/mkw100/insane-kart.md).
namespace InsaneKart {

// Called once per race frame (Raceinfo::Update): readies the shadows of IKW's per-racer tables
// when a race starts.
void RaceFrame(uint32_t raceinfo);

} // namespace InsaneKart
