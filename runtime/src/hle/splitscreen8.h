#pragma once

#include <cstdint>

// mkw100: MKWii 8P Splitscreen (runtime/src/hle/splitscreen8.cpp).
namespace SplitScreen8 {

// Racers of a race with 5-8 local players: 6 or 8 views of a big field outgrow aurora's per-frame
// buffers (8 views: 100 racers used 64% of the 48 MiB storage buffer, 200 overflowed it), so such a
// race keeps at most this many; 1-4 local players keep the product's own cap.
inline constexpr uint32_t kWideMaxRacers = 128;

// Local players of the 5-8 player game the menus set up (the module's menuLocalCount), else 0
// outside the 8 Player Splitscreen build.
uint32_t MenuLocals();

} // namespace SplitScreen8
