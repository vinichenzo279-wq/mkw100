// mkw100: GMData::HandlePositionTracking (0x805336D8) for races of more than 12 players.
//
// Each frame Raceinfo::Update ranks the racers here: every racer gets a sort key, placed at its
// previous rank, the keys are insertion-sorted (higher first, stable), and the result is written
// back as each racer's rank (+0x20 of its GM player) and the rank -> racer table. The game keeps
// the keys and indices in stack arrays of 12 (u8 at r1+8, f64 at r1+0x18 of a 0xC0 frame), so
// with more racers it writes over its caller's frame (24 racers: return to pc 0).
//
// Up to 12 racers the game's function runs unchanged. Above, this reimplements it with the same
// keys and the same double arithmetic step by step (constants read from the game's data):
//   finished (Raceinfo player +0x38 bit 1): key = i * k1 + (k2 - time_ms), the finish time clamped
//       to 99:59.999 as the game does;
//   racing: key = the float at GM player +0xC (race progress).

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

#pragma clang fp contract(off)  // the game rounds after each fmul/fsub/fadd

namespace {

constexpr uint32_t kPositionsRacedataGlobal = 0x809BD728;  // RaceConfig::spInstance
constexpr uint32_t kPositionsRaceinfoGlobal = 0x809BD730;
constexpr uint32_t kRacePlayerCount = 0x24;       // u8, race scenario settings
constexpr uint32_t kPositionsStackSlots = 12;          // the stack arrays' size
constexpr uint32_t kPositionsMaxRacers = 255;  // PlayerSlots::kMaxPlayers
// The function's double constants (lfd 0x160/0x168/0x170 off 0x80890000).
constexpr uint32_t kTieBreak = 0x80890160;   // k1
constexpr uint32_t kTimeBase = 0x80890168;   // k2
constexpr uint32_t kIntBias = 0x80890170;    // 2^52, the u32 -> double conversion bias

double ReadDouble(uint32_t address) { return std::bit_cast<double>(Memory::Read64(address)); }

// (double)value the way the game converts an unsigned word: (2^52 + value) - 2^52.
double FromUnsigned(uint32_t value, double bias) {
    return std::bit_cast<double>((uint64_t{0x43300000} << 32) | value) - bias;
}

} // namespace

extern "C" void GMData_HandlePositionTracking_805336d8(CpuContext* ctx) {
    const uint32_t count = Memory::Read8(Memory::Read32(kPositionsRacedataGlobal) + kRacePlayerCount);
    if (count <= kPositionsStackSlots) {
        MKW_GUEST_NEXT(805336D8)(ctx);
        return;
    }
    if (count > kPositionsMaxRacers) {
        throw std::runtime_error("mkw100 race positions: more racers than slots");
    }
    const uint32_t self = ctx->gpr[3];
    const uint32_t raceinfoPlayers = Memory::Read32(Memory::Read32(kPositionsRaceinfoGlobal) + 0xC);
    const double k1 = ReadDouble(kTieBreak);
    const double k2 = ReadDouble(kTimeBase);
    const double bias = ReadDouble(kIntBias);

    std::array<double, kPositionsMaxRacers> keys{};
    std::array<uint8_t, kPositionsMaxRacers> racers{};
    std::array<bool, kPositionsMaxRacers> placed{};
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t player = Memory::Read32(raceinfoPlayers + i * 4);
        const uint32_t gm = Memory::Read32(Memory::Read32(Memory::Read32(self + 4) + 0xC) + i * 4);
        const int32_t slot = int32_t(Memory::Read8(gm + 0x20)) - 1;  // u8 rank, 0 = none (128 racers: rank 128 is past int8)
        if (slot < 0 || static_cast<uint32_t>(slot) >= count || placed[slot]) {
            char message[96];
            std::snprintf(message, sizeof message, "mkw100 race positions: racer %u has rank %d of %u", i, slot + 1, count);
            throw std::runtime_error(message);
        }
        placed[slot] = true;
        if (Memory::Read32(player + 0x38) & 2) {
            const uint32_t timer = Memory::Read32(player + 0x40);
            const uint32_t minutes = Memory::Read16(timer + 4);
            const bool over = minutes > 99;
            const uint32_t m = over ? 99 : minutes & 0xFF;
            const uint32_t s = over ? 59 : Memory::Read8(timer + 6);
            const uint32_t ms = over ? 999 : Memory::Read16(timer + 8);
            const double tie = k1 * FromUnsigned(i & 0xFF, bias);
            const double time = FromUnsigned((s + m * 60) * 1000 + ms, bias);
            const double remaining = k2 - time;
            keys[slot] = tie + remaining;
        } else {
            keys[slot] = static_cast<double>(std::bit_cast<float>(Memory::Read32(gm + 0xC)));
        }
        racers[slot] = static_cast<uint8_t>(i);
    }

    // Insertion sort, higher keys first; equal keys keep their order (the game's `fcmpo; blt`).
    for (uint32_t k = 1; k < count; ++k) {
        const double key = keys[k];
        const uint8_t racer = racers[k];
        uint32_t j = k;
        while (j > 0 && keys[j - 1] < key) {
            keys[j] = keys[j - 1];
            racers[j] = racers[j - 1];
            --j;
        }
        keys[j] = key;
        racers[j] = racer;
    }

    const uint32_t gmData = Memory::Read32(self + 4);
    for (uint32_t rank = 0; rank < count; ++rank) {
        const uint8_t racer = racers[rank];
        const uint32_t gm = Memory::Read32(Memory::Read32(gmData + 0xC) + racer * 4u);
        Memory::Write8(gm + 0x20, static_cast<uint8_t>(rank + 1));
        Memory::Write8(Memory::Read32(gmData + 0x18) + rank, racer);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x805336D8, GMData_HandlePositionTracking_805336d8, "GMData_HandlePositionTracking_805336d8");
