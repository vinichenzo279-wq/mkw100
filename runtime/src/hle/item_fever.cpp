// mkw100: Item Fever (Pulsar pack) past 12 racers.
//
// Item Fever keeps some per-racer state in static [12] tables in its own module. The unchecked
// accesses are listed as `index` sites in projects/mkwii/mkw100/array-sites.txt (profile
// item-fever): racers 12+ use a shadow table instead. This file resets those shadows when Item
// Fever resets its own tables. Retro Rewind has no function at these addresses, and the base game
// never calls them.

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"

#include <cstdint>

namespace {

constexpr uint32_t kFeverFlagsShadow = 0x82D00000;  // u8 flags[12] at 0x818A2D34, racers 12..127
constexpr uint32_t kFeverShadowRacers = 128 - 12;

} // namespace

// Item Fever's race-load hook (node 0x818A2D80): clears its float[12] at 0x818A2D40 and u8[12] at
// 0x818A2D70. The flags' shadow is cleared with it, so a racer 12+ starts each race unflagged.
extern "C" void ItemFever_ResetRacerTables_81816d88(CpuContext* ctx) {
    MKW_GUEST_NEXT(81816D88)(ctx);
    for (uint32_t i = 0; i < kFeverShadowRacers; ++i) {
        Memory::Write8(kFeverFlagsShadow + i, 0);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x81816D88, ItemFever_ResetRacerTables_81816d88, "ItemFever_ResetRacerTables_81816d88");
