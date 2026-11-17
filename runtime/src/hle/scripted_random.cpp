// mkw100: repeatable Random seeds in scripted test races (--race-cpu / --race-auto).
//
// The game's Random() and Random::Reset() seed from OSGetTick, the host clock in the recomp. The
// game itself seeds its race randomness from Racedata, so the base game repeats exactly, but
// Retro Rewind and Item Fever create clock-seeded Random objects (15 and 14 call sites) for their
// own features, which made two runs of the same Retro Rewind + Item Fever test race diverge
// (docs/mkw100/rr-item-fever.md). In a scripted race those seeds come from a fixed sequence
// instead; normal play keeps clock seeds. Only in the combined build: the other builds keep their
// recorded headless results (the base game repeats anyway; plain Retro Rewind and Item Fever as before).

#include "abi_bridge.h"
#include "boot_race.h"
#include "guest_function_imports.h"
#include "hle/mod_build.h"
#include "memory.h"

#include <cstdint>

namespace {

// The value the game stores is (tick rotated left 16) & 0x0FFFFFFF, at +0xC and again at +0x14.
void ReseedScripted(uint32_t random) {
    // Fixed start, advanced per seeded Random. (No digit separators in this file: the translator's
    // native-source scanner reads a ' as a character literal and would miss the registrations.)
    static uint64_t state = 0x6D6B773130303031ull;
    state += 0x9E3779B97F4A7C15ull;                  // splitmix64
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    const uint32_t seed = static_cast<uint32_t>(z) & 0x0FFFFFFFu;
    Memory::Write32(random + 0x0C, seed);
    Memory::Write32(random + 0x14, seed);
}

bool Reseeding() {
    return BootRace::Scripted() && ModBuild::Current() == ModBuild::Kind::RetroRewindItemFever;
}

} // namespace

// Random::Random()
extern "C" void Random_ct_80555464(CpuContext* ctx) {
    const uint32_t random = ctx->gpr[3];
    MKW_GUEST_NEXT(80555464)(ctx);
    if (Reseeding()) ReseedScripted(random);
}
REGISTER_NATIVE_WRAPPER_AS(0x80555464, Random_ct_80555464, "Random_ct_80555464");

// Random::Reset()
extern "C" void Random_Reset_805554d0(CpuContext* ctx) {
    const uint32_t random = ctx->gpr[3];
    MKW_GUEST_NEXT(805554D0)(ctx);
    if (Reseeding()) ReseedScripted(random);
}
REGISTER_NATIVE_WRAPPER_AS(0x805554D0, Random_Reset_805554d0, "Random_Reset_805554d0");
