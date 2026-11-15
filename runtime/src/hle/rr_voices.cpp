// mkw100: Retro Rewind's custom skin voices past 12 racers.
//
// A Retro Rewind skin can bring its own voice clips (E. Gadd over Baby Luigi, Toadsworth over Toad).
// When a race loads, its LoadCharacterVoices (0x81833C98, run from RaceScene::CreateAndInitInstances)
// gives each racer with such a skin an alias: the voice group of a character nobody in the race
// uses, which it then fills with the custom clips, preferring a character whose voice layout
// matches. Its per-racer tables hold 12 racers (0x8189F3F8: aliases +0x00, voice sources, slots
// +0x34, has-custom-voice +0x40, silent +0x4C, source suffixes [12][0x20] +0x58), but its loops run
// to the race's racer count. Past 12, racer 16's skin slot landed on player 0's has-custom-voice
// flag, racer 12's flag on player 0's silent flag, and the suffix strings ran up to 3 KB into the
// data after them: a player's custom voice went quiet or fell back to the stock voice, at a field
// size that depended on the characters. Its voice lookups only ever serve racers 0..11 anyway.
//
// Here LoadCharacterVoices runs with the race's racer count held at 12, so it writes only its own
// tables, and as at most 12 characters are taken among racers 0..11, a local player always gets an
// alias, the best match among the rest. A CPU from 12 on may race that alias character; its voice
// group now holds the custom clips, so that CPU is muted (CharacterActor +0x6FF, as Retro Rewind
// mutes its silent skins) instead of speaking with the player's voice. Known Retro Rewind build
// only (mod_build.h).

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "hle/mod_build.h"
#include "memory.h"
#include "runtime_log.h"

#include <cstdint>

namespace {

constexpr uint32_t kRacedata = 0x809BD728;        // RaceConfig*
constexpr uint32_t kRacerCount = 0x24;            // races scenario player count (u8)
constexpr uint32_t kInlineRacers = 12;
constexpr uint32_t kVoiceAliases = 0x8189F3F8;    // s8[12]
constexpr uint32_t kVoiceCustom = 0x8189F438;     // bool[12]
constexpr uint32_t kActorPlayer = 0x6EC;          // CharacterActor: u8 player id
constexpr uint32_t kActorCharacter = 0x9C;        // u16
constexpr uint32_t kActorVoiceOn = 0x6FF;         // bool
constexpr uint32_t kCharacters = 24;

bool g_aliasTaken[kCharacters] = {};
uint32_t g_muted = 0;

} // namespace

// Retro Rewind LoadCharacterVoices()
extern "C" void RR_LoadCharacterVoices_81833c98(CpuContext* ctx) {
    const uint32_t racedata = Memory::Read32(kRacedata);
    const uint32_t count = racedata ? Memory::Read8(racedata + kRacerCount) : 0;
    if (!ModBuild::IsKnownRetroRewind() || count <= kInlineRacers) {
        MKW_GUEST_NEXT(81833C98)(ctx);
        return;
    }
    Memory::Write8(racedata + kRacerCount, kInlineRacers);
    MKW_GUEST_NEXT(81833C98)(ctx);
    Memory::Write8(racedata + kRacerCount, count);
    for (uint32_t c = 0; c < kCharacters; ++c) g_aliasTaken[c] = false;
    g_muted = 0;
    for (uint32_t p = 0; p < kInlineRacers; ++p) {
        const int32_t alias = int8_t(Memory::Read8(kVoiceAliases + p));
        if (Memory::Read8(kVoiceCustom + p) == 0 || alias < 0 || uint32_t(alias) >= kCharacters) continue;
        g_aliasTaken[alias] = true;
        RT_LOGF("rr-voices", "racer %u: custom voice through character %d (%u racers)\n", p, alias, count);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x81833C98, RR_LoadCharacterVoices_81833c98, "RR_LoadCharacterVoices_81833c98");

// Retro Rewind InitCharacterVoiceRanges(CharacterActor*), at every racer's voice set-up.
extern "C" void RR_InitCharacterVoiceRanges_81834588(CpuContext* ctx) {
    const uint32_t actor = ctx->gpr[3];
    MKW_GUEST_NEXT(81834588)(ctx);
    if (!ModBuild::IsKnownRetroRewind()) return;
    const uint32_t player = Memory::Read8(actor + kActorPlayer);
    const uint32_t character = Memory::Read16(actor + kActorCharacter);
    if (player < kInlineRacers || character >= kCharacters || !g_aliasTaken[character]) return;
    Memory::Write8(actor + kActorVoiceOn, 0);
    if (g_muted++ == 0) RT_LOGF("rr-voices", "CPUs racing a custom voice's alias character are muted (first: racer %u)\n", player);
}
REGISTER_NATIVE_WRAPPER_AS(0x81834588, RR_InitCharacterVoiceRanges_81834588, "RR_InitCharacterVoiceRanges_81834588");
