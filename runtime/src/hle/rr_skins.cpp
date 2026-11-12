// mkw100: Retro Rewind kart archives and character skins past 12 racers, and skins on CPU racers.
//
// Retro Rewind ships up to 100 alternative skins per character (Character/AllKart
// "<kart>-<char>-<n>-allkart*.szs", Character/Driver "<char>-<n>.brres"). Its Code.pul scans which
// exist into a [24][101] flag table (0x81877CF8, built at 0x81802628) and lets each of the 4 local
// players pick one in the character menu, a [4][24] byte table (0x81878670).
//
// Its kart archive code gets the racer from an ArchiveMgr element address: (element - ArchiveMgr -
// 8) / 0x1C in the kart path builder 0x818026FC, and the same for the arrays at 0x8 and 0x158 in
// the stubs 0x81832DFC and 0x81832E24 in front of 0x81832CC4. mkw100 moves elements 12+ of those
// arrays out of line (array_overflow.h, class archive_manager), so with 13+ racers that division
// gave a garbage index and 0x81832CC4 read far out of bounds (24 racers crashed loading the karts).
// Here the index comes from ArrayOverflow::ElementIndex instead.
//
// The path builder reads row <racer> of the [4][24] table. A CPU racer never picked a skin, and
// past row 3 that read was outside the table. For every CPU racer, and every racer past 3, row 3
// is filled with a skin per character for the call and restored after it: picked from the skins
// that exist, or the stock look as one more choice, by a hash of the racer index. The pick is
// deterministic, so a race replays the same field. Retro Rewind product only: the base game
// never calls these addresses.
//
// In a race the path builder is not what picks the skin: 0x81832B74 decides one skin per racer into
// a 12-byte table (0x8189F3EC; CPUs get a random existing skin) and the getter 0x818329A0 reads it
// back for 0x81832CC4. The decider walks the 12-slot scenario player array and wrote past the
// table, and the getter sends racers 12+ to a local player's row, so only about 12 racers got
// skins. More than 12 racers: both run natively over a per-racer table here (racers 0-11 also
// written to Retro Rewind's own table); a CPU's pick is PickSkin's.

#include "abi_bridge.h"
#include "guest_abi/array_overflow.h"
#include "guest_abi/player_slots.h"
#include "guest_function_imports.h"
#include "memory.h"

#include <cstdint>

namespace {

// Toggle: off gives CPUs the stock look, as Retro Rewind itself does. The index fixes stay on.
constexpr bool kCpuSkins = true;
constexpr uint32_t kArchiveClass = 9;              // archive_manager in array_overflow.h
constexpr uint32_t kArchiveStride = 0x1C;          // ArchivesHolder
constexpr uint32_t kArchiveModels = 0x8;           // kartModelsHolders[12]
constexpr uint32_t kArchiveModels2 = 0x158;        // kartModelsHolders2[12]
constexpr uint32_t kSkinRacedata = 0x809BD728;     // RaceConfig::spInstance
constexpr uint32_t kSkinPlayerType = 0x38;         // slot type, 1: CPU (see driver_manager.cpp)
constexpr uint32_t kSkinCpuType = 1;
constexpr uint32_t kSkinAvailable = 0x81877CF8;    // u8 [24][101]: skin n of character c exists
constexpr uint32_t kSkinAvailableStride = 101;
constexpr uint32_t kSkinChosen = 0x81878670;       // u8 [4][24]: local player's skin per character
constexpr uint32_t kSkinCharacters = 24;
constexpr uint32_t kSkinMax = 100;
constexpr uint32_t kSkinRows = 4;
constexpr uint32_t kSkinBorrowRow = 3;

uint32_t ArchiveRacer(uint32_t element, uint32_t origin) {
    return ArrayOverflow::ElementIndex(ArrayOverflow::Class(kArchiveClass), element, origin, kArchiveStride);
}

uint32_t SkinHash(uint32_t racer, uint32_t character) {
    uint32_t x = racer * 0x9E3779B1u ^ (character + 1) * 0x85EBCA77u;
    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    x ^= x >> 12;
    return x;
}

uint8_t PickSkin(uint32_t racer, uint32_t character) {
    const uint32_t row = kSkinAvailable + character * kSkinAvailableStride;
    uint32_t count = 0;
    for (uint32_t n = 1; n <= kSkinMax; ++n) count += Memory::Read8(row + n) != 0;
    uint32_t pick = SkinHash(racer, character) % (count + 1);  // 0 keeps the stock look
    if (pick == 0) return 0;
    for (uint32_t n = 1; n <= kSkinMax; ++n) {
        if (Memory::Read8(row + n) != 0 && --pick == 0) return static_cast<uint8_t>(n);
    }
    return 0;
}

constexpr uint32_t kSkinDecided = 0x8189F3EC;     // u8 [12]: Retro Rewind's per-racer race skin
constexpr uint32_t kSkinInline = 12;
constexpr uint32_t kSkinRacersMax = 256;
constexpr uint32_t kSlotCharacter = 0x34;          // Racedata + slot: character id
constexpr uint32_t kSlotLocal = 0x2D;              // Racedata + slot: local player index (s8)
constexpr uint32_t kSkinHumanType = 0;
constexpr uint32_t kSkinNoCpuSkins = 0xB8C;        // Racedata: Retro Rewind's "CPUs stock" flag
uint8_t g_raceSkin[kSkinRacersMax];

uint32_t RacerCount() {
    const uint32_t racedata = Memory::Read32(kSkinRacedata);
    return racedata == 0 ? 0 : Memory::Read8(racedata + 0x24);
}

bool SkinExists(uint32_t character, uint32_t skin) {
    return skin <= kSkinMax && Memory::Read8(kSkinAvailable + character * kSkinAvailableStride + skin) != 0;
}

bool IsCpu(uint32_t racer) {
    const uint32_t racedata = Memory::Read32(kSkinRacedata);
    return racedata != 0 && racer < Memory::Read8(racedata + 0x24) &&
           Memory::Read32(racedata + MKW100_PlayerSlotOffset(racer) + kSkinPlayerType) == kSkinCpuType;
}

// The stubs: r30 ArchiveMgr, r31 the element; r10 the racer for 0x81832CC4.
void ArchiveStub(CpuContext* ctx, uint32_t origin) {
    const uint32_t element = ctx->gpr[31];
    ctx->gpr[11] = element - ctx->gpr[30];
    ctx->gpr[0] = ctx->gpr[11] - origin;
    const uint32_t racer = ArchiveRacer(element, origin);
    ctx->gpr[10] = racer != ArrayOverflow::kNoArray ? racer : ctx->gpr[0] / kArchiveStride;
    InvokeIndirectCpu(0x81832CC4, ctx);  // module code, not in the base map
}

} // namespace

extern "C" void RR_ArchiveModels_81832dfc(CpuContext* ctx) { ArchiveStub(ctx, kArchiveModels); }
REGISTER_NATIVE_REPLACEMENT_AS(0x81832DFC, RR_ArchiveModels_81832dfc, "RR_ArchiveModels_81832dfc");

extern "C" void RR_ArchiveModels2_81832e24(CpuContext* ctx) { ArchiveStub(ctx, kArchiveModels2); }
REGISTER_NATIVE_REPLACEMENT_AS(0x81832E24, RR_ArchiveModels2_81832e24, "RR_ArchiveModels2_81832e24");

// The kart path builder; r7 is the racer's kartModelsHolders element.
extern "C" void RR_KartSkinPath_818026fc(CpuContext* ctx) {
    const uint32_t racer = ArchiveRacer(ctx->gpr[7], kArchiveModels);
    const bool cpu = racer != ArrayOverflow::kNoArray && IsCpu(racer);
    if (racer == ArrayOverflow::kNoArray || (racer < kSkinRows && !cpu)) {
        MKW_GUEST_NEXT(818026FC)(ctx);
        return;
    }
    const uint32_t borrow = kSkinChosen + kSkinBorrowRow * kSkinCharacters;
    uint8_t saved[kSkinCharacters];
    for (uint32_t c = 0; c < kSkinCharacters; ++c) {
        saved[c] = Memory::Read8(borrow + c);
        Memory::Write8(borrow + c, kCpuSkins && cpu ? PickSkin(racer, c) : 0);
    }
    const uint32_t archive = Memory::Read32(ArrayOverflow::Class(kArchiveClass).instanceGlobal);
    ctx->gpr[7] = archive + kArchiveModels + kSkinBorrowRow * kArchiveStride;
    MKW_GUEST_NEXT(818026FC)(ctx);
    for (uint32_t c = 0; c < kSkinCharacters; ++c) Memory::Write8(borrow + c, saved[c]);
}
REGISTER_NATIVE_WRAPPER_AS(0x818026FC, RR_KartSkinPath_818026fc, "RR_KartSkinPath_818026fc");

// 0x81832B74(r3 = race scenario): decides every racer's skin for the race.
extern "C" void RR_DecideRaceSkins_81832b74(CpuContext* ctx) {
    const uint32_t count = RacerCount();
    if (count <= kSkinInline) {
        MKW_GUEST_NEXT(81832B74)(ctx);
        return;
    }
    const uint32_t racedata = Memory::Read32(kSkinRacedata);
    for (uint32_t racer = 0; racer < count && racer < kSkinRacersMax; ++racer) {
        const uint32_t slot = racedata + MKW100_PlayerSlotOffset(racer);
        const uint32_t character = Memory::Read32(slot + kSlotCharacter);
        const uint32_t type = Memory::Read32(slot + kSkinPlayerType);
        uint8_t skin = 0;
        if (character < kSkinCharacters && type == kSkinHumanType) {
            const int32_t local = int8_t(Memory::Read8(slot + kSlotLocal));
            if (local >= 0 && uint32_t(local) < kSkinRows) skin = Memory::Read8(kSkinChosen + local * kSkinCharacters + character);
        } else if (character < kSkinCharacters && type == kSkinCpuType && Memory::Read8(racedata + kSkinNoCpuSkins) == 0) {
            skin = kCpuSkins ? PickSkin(racer, character) : 0;
        }
        g_raceSkin[racer] = skin;
        if (racer < kSkinInline) Memory::Write8(kSkinDecided + racer, skin);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x81832B74, RR_DecideRaceSkins_81832b74, "RR_DecideRaceSkins_81832b74");

// 0x818329A0(racer, character, menu): a racer's skin for character, 0 for the stock look.
extern "C" void RR_RacerSkin_818329a0(CpuContext* ctx) {
    const uint32_t racer = ctx->gpr[3];
    const uint32_t character = ctx->gpr[4];
    if (racer < kSkinInline || RacerCount() <= kSkinInline || ctx->gpr[5] != 0) {
        MKW_GUEST_NEXT(818329A0)(ctx);
        return;
    }
    const uint32_t skin = racer < kSkinRacersMax ? g_raceSkin[racer] : 0;
    ctx->gpr[3] = character < kSkinCharacters && SkinExists(character, skin) ? skin : 0;
}
REGISTER_NATIVE_WRAPPER_AS(0x818329A0, RR_RacerSkin_818329a0, "RR_RacerSkin_818329a0");
