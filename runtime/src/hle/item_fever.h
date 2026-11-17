#pragma once

#include <cstdint>

struct CpuContext;

namespace ItemFever {

// Called once per race frame (Raceinfo::Update): readies the shadows of Item Fever's per-racer
// tables when a race starts, and runs the MKW100_TEST_GIVE_ITEM / MKW100_TEST_RACER_TABLES tests.
void RaceFrame(CpuContext* ctx, uint32_t raceinfo);

} // namespace ItemFever
