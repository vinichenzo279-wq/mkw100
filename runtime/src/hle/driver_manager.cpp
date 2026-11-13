// mkw100: Race::DriverMgr (class driver_manager, guest_abi/array_overflow.h) for races of more
// than 12 players. Layout from Pulsar's GameSource/MarioKartWii/Driver/DriverManager.hpp.
//
// The per-player arrays live in shadow windows for players 12+; this file covers what the site
// rewrite cannot:
//   - CreateInstance (0x8078C9E8): sets up the windows, then gives every player 12+ the finish
//     type the ctor gives players 0..11 (4, not finished; CalcFinishType computes one only then).
//   - Update (0x8078D824): its first loop walks a fixed 12 models (frame counters, targeting
//     positions); the same body runs here for players 12..N-1. Its second loop stores each
//     player's position in u8 racePositions[12] at 0x161, indexed as `this + i` (stride 1, no
//     scale for array_sites to rewrite), so players 12+ write over the fields after it (24: up to
//     FinishType[0] at 0x178). With more than 12 players those fields are saved before Update and
//     restored after it; positions of players 12+ are then not kept. Only CalcFinishType reads
//     them, and only online, so online players 12+ get their finish type from the Raceinfo
//     position alone (online past 12 is untested).
//   - ~DriverMgr (0x8078CE24): releases models 0..11 only; models 12+ are released here first.
//   - CalcFinishType (0x8078CFA4): in a race (game modes 0, 1, 7, 8) a finisher who is not 1st
//     gets GOOD up to the position in a u32 table [player count - 1] (0x808D1820; 12 players:
//     5) and BAD below it. With more than 12 players the table is read past its 12 rows (24: a
//     relocated vtable pointer, negative to the signed compare, so every finisher from 2nd is
//     BAD). The finish type picks the fanfare, the driver's finish animation and the kart's
//     driving after the line. Past 12 players the 12-player share is kept: GOOD up to
//     ceil(table[11] * count / 12) (24: 10th). With more than 12 players the whole function
//     runs here: it adds one scaled index to both finishType (0x178) and Raceinfo's player array
//     (0x8078CFB0), so the window rewrite cannot serve it and array_sites leaves it unproven.
//   - __ct (0x8078CA5C): its own position loop also runs over every race player before it sets
//     the fields after the array, which it rewrites, except 0x174 (never initialized by the game)
//     and Timer top5[5] at 0x1B0, built before the loop. From 80 players the loop reaches them
//     (100: up to 0x1C4, top5[0] and [1] with their vtables). Their bytes are saved when the ctor
//     calls SetParams (0x807BD1D0), between building them and the loop, and the ones the loop
//     overwrote are put back after it, except the record fields the ctor sets after the loop
//     (100 players, M5 Pro: all five timers keep vtable 0x808B2D44 after the ctor).
// Up to 12 players nothing here changes the game's behaviour.

#include "guest_abi/array_overflow.h"
#include "guest_abi/player_slots.h"

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"

#include <array>
#include <cstdint>

namespace {

constexpr uint32_t kDriverClass = 8;
constexpr uint32_t kDriverRacedataGlobal = 0x809BD728;  // RaceConfig::spInstance
constexpr uint32_t kDriverRaceCount = 0x24;             // u8, race scenario player count

// DriverMgr fields
constexpr uint32_t kModels = 0x10;          // DriverController*[12]
constexpr uint32_t kTargeting = 0xD0;       // Vec3*[12]
constexpr uint32_t kFrameCounter = 0x100;   // u32[12]
constexpr uint32_t kCountdown = 0x130;      // u32[12]
constexpr uint32_t kFinishType = 0x178;     // FinishType[12]
constexpr uint32_t kNotFinished = 4;
constexpr uint32_t kPositionsEnd = 0x16D;   // end of u8 racePositions[12] at 0x161
constexpr uint32_t kDriverMgrSize = 0x1F0;
constexpr uint32_t kRacePositionsBegin = 0x161;  // u8 racePositions[12]
constexpr uint32_t kTop5 = 0x1B0;                // Timer top5[5], 0xC each
constexpr uint32_t kTop5End = 0x1EC;
constexpr uint32_t kSavedEnd = kDriverMgrSize;   // saved with top5: the object's last word too
// From 144 racers the position loops (ctor and Update, `this + 0x161 + i`) run past the object's
// 0x1F0 bytes into the next heap block (200 racers: its header, a broken free list). The object
// is then allocated with room for them behind it; nothing reads that tail.
constexpr uint32_t kPositionsTail = 0x161 + ArrayOverflow::kMaxPlayers > kDriverMgrSize
                                        ? ((0x161 + ArrayOverflow::kMaxPlayers - kDriverMgrSize + 3) & ~3u)
                                        : 0;
constexpr uint32_t kDriverNew = 0x80229DCC;      // operator new(size), the current heap
constexpr uint32_t kDriverCtor = 0x8078CA5C;
constexpr uint32_t kTimerSize = 0xC;
constexpr uint32_t kTop5HasRecord = 0x1A8;       // bool[5], the ctor fills the timer when set

// DriverController fields read by Update's first loop
constexpr uint32_t kModelState = 0x2BC;     // 2: following a targeting position; 0: kModelFlag unset
constexpr uint32_t kModelFlag = 0x2B4;      // u8, resets the frame counter
constexpr uint32_t kModelTarget = 0x2A8;    // Vec3

constexpr uint32_t kReleaseModel = 0x807D2784;  // what ~DriverMgr calls on each model

// CalcFinishType (0x8078CFA4..0x8078D19C), replicated for races of more than 12 players
constexpr uint32_t kFinishFirst = 0;
constexpr uint32_t kFinishGood = 1;
constexpr uint32_t kFinishBad = 2;
constexpr uint32_t kFinishThresholds = 0x808D1820;  // s32[12], indexed by player count - 1
constexpr uint32_t kFinishCpuDefault = 0x808D1818;  // the type a non-CPU gets in those modes
constexpr uint32_t kFinishKartManager = 0x809C18F8;  // Kart::Manager*, u8 player count at +0x24
constexpr uint32_t kFinishKartCount = 0x24;
constexpr uint32_t kFinishRaceinfo = 0x809BD730;     // Raceinfo*, RaceinfoPlayer*[] at +0xC
constexpr uint32_t kFinishRaceinfoPlayers = 0xC;
constexpr uint32_t kFinishRaceinfoPosition = 0x20;  // u8 race position of a RaceinfoPlayer
constexpr uint32_t kFinishOnline = 0x809C38B9;      // bool, online race
constexpr uint32_t kRacePositions = 0x161;          // u8 racePositions[12], online only
constexpr uint32_t kFinishMode = 0xB70;             // RaceConfig race scenario game mode
constexpr uint32_t kFinishFlags = 0xB90;            // bit 0x4: every mode takes the CPU path
constexpr uint32_t kFinishPlayerType = 0x38;        // RaceConfig player slot type (1: CPU)
constexpr uint32_t kFinishCpuType = 1;
constexpr uint32_t kKartPlayer = 0x80590100;        // Kart::Manager::GetKartPlayer(manager, i)
constexpr uint32_t kKartIsGhost = 0x80590678;       // Kart link IsGhost: nonzero gives FIRST
constexpr uint32_t kFinishOtherModes = 0x8078D4CC;  // the game's own rule for the other modes
constexpr uint32_t kOnlineCheck = 0x80536230;       // (Raceinfo, 4): 0 lets racePositions count

uint32_t DriverRaceCount() {
    const uint32_t racedata = Memory::Read32(kDriverRacedataGlobal);
    return racedata == 0 ? 0 : Memory::Read8(racedata + kDriverRaceCount);
}

uint32_t g_constructing = 0;  // the DriverMgr whose ctor is running
bool g_top5Saved = false;
std::array<uint8_t, kSavedEnd - kTop5> g_top5{};

// Timer bytes the ctor writes after its position loop for a top-5 slot with a record: min (u16),
// sec (u8), ms (u16), active (bool) at +4, +6, +8, +0xA.
bool WrittenAfterLoop(uint32_t self, uint32_t offset) {
    if (offset >= kTop5End) {
        return false;
    }
    const uint32_t slot = (offset - kTop5) / kTimerSize;
    const uint32_t field = (offset - kTop5) % kTimerSize;
    return Memory::Read8(self + kTop5HasRecord + slot) != 0 &&
           (field == 4 || field == 5 || field == 6 || field == 8 || field == 9 || field == 0xA);
}

} // namespace

extern "C" void DriverMgr_Ctor_8078ca5c(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3];
    g_constructing = self;
    g_top5Saved = false;
    MKW_GUEST_NEXT(8078CA5C)(ctx);
    g_constructing = 0;
    const uint32_t loopEnd = kRacePositionsBegin + DriverRaceCount();
    if (!g_top5Saved || loopEnd <= kTop5) {
        return;
    }
    for (uint32_t offset = kTop5; offset < loopEnd && offset < kSavedEnd; ++offset) {
        if (!WrittenAfterLoop(self, offset)) {
            Memory::Write8(self + offset, g_top5[offset - kTop5]);
        }
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x8078CA5C, DriverMgr_Ctor_8078ca5c, "DriverMgr_Ctor_8078ca5c");

// DriverMgr::SetParams: the ctor calls it after building top5 and before its position loop.
extern "C" void DriverMgr_SetParams_807bd1d0(CpuContext* ctx) {
    if (g_constructing != 0) {
        for (uint32_t offset = kTop5; offset < kSavedEnd; ++offset) {
            g_top5[offset - kTop5] = Memory::Read8(g_constructing + offset);
        }
        g_top5Saved = true;
    }
    MKW_GUEST_NEXT(807BD1D0)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x807BD1D0, DriverMgr_SetParams_807bd1d0, "DriverMgr_SetParams_807bd1d0");

extern "C" void DriverMgr_CreateInstance_8078c9e8(CpuContext* ctx) {
    const ArrayOverflow::ArrayClass& c = ArrayOverflow::Class(kDriverClass);
    const uint32_t before = Memory::Read32(c.instanceGlobal);
    if (before == 0 && kRacePositionsBegin + DriverRaceCount() > kDriverMgrSize) {
        // The game's CreateInstance with a longer allocation: new, __ct, store.
        ctx->gpr[3] = kDriverMgrSize + kPositionsTail;
        InvokeIndirectCpu(kDriverNew, ctx);
        if (ctx->gpr[3] != 0) {
            InvokeIndirectCpu(kDriverCtor, ctx);
        }
        Memory::Write32(c.instanceGlobal, ctx->gpr[3]);
    } else {
        MKW_GUEST_NEXT(8078C9E8)(ctx);
    }
    if (before != 0 || Memory::Read32(c.instanceGlobal) == 0) {
        return;
    }
    const CpuContext saved = *ctx;
    ArrayOverflow::InitWindows(c, ctx);
    *ctx = saved;
    for (uint32_t i = ArrayOverflow::kInlineSlots; i < ArrayOverflow::kMaxPlayers; ++i) {
        Memory::Write32(ArrayOverflow::Window(c, i) + kFinishType, kNotFinished);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x8078C9E8, DriverMgr_CreateInstance_8078c9e8, "DriverMgr_CreateInstance_8078c9e8");

extern "C" void DriverMgr_Update_8078d824(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3];
    const uint32_t players = DriverRaceCount();
    const ArrayOverflow::ArrayClass& c = ArrayOverflow::Class(kDriverClass);
    if (players <= ArrayOverflow::kInlineSlots || self != ArrayOverflow::Instance(c)) {
        MKW_GUEST_NEXT(8078D824)(ctx);
        return;
    }
    std::array<uint32_t, (kDriverMgrSize - kPositionsEnd + 3) / 4> fields{};
    const uint32_t first = kPositionsEnd & ~3u;  // 0x16C: whole words, racePositions[11] put back
    for (uint32_t i = 0; first + i * 4 < kDriverMgrSize; ++i) {
        fields[i] = Memory::Read32(self + first + i * 4);
    }
    MKW_GUEST_NEXT(8078D824)(ctx);
    const uint8_t newLastPosition = Memory::Read8(self + kPositionsEnd - 1);
    for (uint32_t i = 0; first + i * 4 < kDriverMgrSize; ++i) {
        Memory::Write32(self + first + i * 4, fields[i]);
    }
    Memory::Write8(self + kPositionsEnd - 1, newLastPosition);

    // Update's first loop (0x8078D848..0x8078D8FC) for models 12+.
    for (uint32_t i = ArrayOverflow::kInlineSlots; i < players && i < ArrayOverflow::kMaxPlayers; ++i) {
        const uint32_t e = ArrayOverflow::Window(c, i);
        const uint32_t model = Memory::Read32(e + kModels);
        if (model == 0) {
            continue;
        }
        const bool reset = Memory::Read32(model + kModelState) != 0 && Memory::Read8(model + kModelFlag) != 0;
        Memory::Write32(e + kFrameCounter, reset ? 0 : Memory::Read32(e + kFrameCounter) + 1);
        const int32_t countdown = static_cast<int32_t>(Memory::Read32(e + kCountdown));
        Memory::Write32(e + kCountdown, static_cast<uint32_t>(countdown - 1));
        if (countdown > 0) {
            if (Memory::Read32(model + kModelState) == 2) {
                const uint32_t target = Memory::Read32(e + kTargeting);
                for (uint32_t k = 0; k < 12; k += 4) {
                    Memory::Write32(model + kModelTarget + k, Memory::Read32(target + k));
                }
            }
        } else if (Memory::Read32(e + kTargeting) != 0) {
            Memory::Write32(e + kTargeting, 0);
            if (Memory::Read32(model + kModelState) == 2) {
                Memory::Write32(model + kModelState, 0);
            }
        }
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x8078D824, DriverMgr_Update_8078d824, "DriverMgr_Update_8078d824");

namespace {

uint32_t DriverCallGuest(CpuContext* ctx, uint32_t function, uint32_t r3, uint32_t r4) {
    ctx->gpr[3] = r3;
    ctx->gpr[4] = r4;
    InvokeIndirectCpu(function, ctx);
    return ctx->gpr[3];
}

// Modes 2 and 5, or flag 0x4: FIRST for the kart the game singles out, else the slot stays
// unfinished for a CPU and gets the game's constant for anyone else.
uint32_t DriverFinishTypeOfKart(CpuContext* ctx, uint32_t racedata, uint32_t player) {
    const uint32_t kart = DriverCallGuest(ctx, kKartPlayer, Memory::Read32(kFinishKartManager), player);
    if (DriverCallGuest(ctx, kKartIsGhost, kart, 0) != 0) {
        return kFinishFirst;
    }
    const uint32_t type = Memory::Read32(racedata + MKW100_PlayerSlotOffset(player) + kFinishPlayerType);
    return type == kFinishCpuType ? kNotFinished : Memory::Read32(kFinishCpuDefault);
}

uint32_t DriverFinishTypeByPosition(CpuContext* ctx, uint32_t self, uint32_t player, uint32_t count) {
    const uint32_t raceinfo = Memory::Read32(kFinishRaceinfo);
    const uint32_t entry = Memory::Read32(Memory::Read32(raceinfo + kFinishRaceinfoPlayers) + player * 4);
    uint32_t position = Memory::Read8(entry + kFinishRaceinfoPosition);
    // Online the game takes the larger of it and racePositions[player], which Update keeps for
    // players 0..11 only (see the header).
    if (Memory::Read8(kFinishOnline) != 0 && DriverCallGuest(ctx, kOnlineCheck, raceinfo, 4) == 0 &&
        player < ArrayOverflow::kInlineSlots) {
        const uint32_t online = Memory::Read8(self + kRacePositions + player);
        position = online > position ? online : position;
    }
    if (position == 1) {
        return kFinishFirst;
    }
    // ceil(table[11] * count / 12): the 12-player share of GOOD finishers (24: 10th).
    const int32_t last = static_cast<int32_t>(Memory::Read32(kFinishThresholds + (ArrayOverflow::kInlineSlots - 1) * 4));
    const int32_t threshold = (last * static_cast<int32_t>(count) + ArrayOverflow::kInlineSlots - 1) /
                              static_cast<int32_t>(ArrayOverflow::kInlineSlots);
    return static_cast<int32_t>(position) > threshold ? kFinishBad : kFinishGood;
}

} // namespace

extern "C" void DriverMgr_CalcFinishType_8078cfa4(CpuContext* ctx) {
    const uint32_t player = ctx->gpr[3] & 0xFF;
    const ArrayOverflow::ArrayClass& c = ArrayOverflow::Class(kDriverClass);
    const uint32_t self = ArrayOverflow::Instance(c);
    const uint32_t kartManager = Memory::Read32(kFinishKartManager);
    const uint32_t racedata = Memory::Read32(kDriverRacedataGlobal);
    const uint32_t count = kartManager == 0 ? 0 : Memory::Read8(kartManager + kFinishKartCount);
    if (count <= ArrayOverflow::kInlineSlots || self == 0 || racedata == 0 || player >= count ||
        player >= ArrayOverflow::kMaxPlayers) {
        MKW_GUEST_NEXT(8078CFA4)(ctx);
        return;
    }
    const uint32_t slot = player < ArrayOverflow::kInlineSlots ? self + kFinishType + player * 4
                                                                : ArrayOverflow::Window(c, player) + kFinishType;
    uint32_t type = Memory::Read32(slot);
    if (type == kNotFinished) {
        const CpuContext saved = *ctx;
        const int32_t mode = static_cast<int32_t>(Memory::Read32(racedata + kFinishMode));
        if (Memory::Read32(racedata + kFinishFlags) & 4 || mode == 2 || mode == 5) {
            type = DriverFinishTypeOfKart(ctx, racedata, player);
        } else if (mode == 0 || mode == 1 || mode == 7 || mode == 8) {
            type = DriverFinishTypeByPosition(ctx, self, player, count);
        } else if (mode == 4) {
            type = kFinishFirst;
        } else {
            type = DriverCallGuest(ctx, kFinishOtherModes, saved.gpr[3], saved.gpr[4]);
        }
        *ctx = saved;
        Memory::Write32(slot, type);
    }
    ctx->gpr[3] = type;
}
REGISTER_NATIVE_WRAPPER_AS(0x8078CFA4, DriverMgr_CalcFinishType_8078cfa4, "DriverMgr_CalcFinishType_8078cfa4");

extern "C" void DriverMgr_Dtor_8078ce24(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3];
    const ArrayOverflow::ArrayClass& c = ArrayOverflow::Class(kDriverClass);
    // DestroyInstance (0x8078CA38) clears the global before it calls this, so `this` is the
    // instance; there is one DriverMgr at a time.
    if (self != 0) {
        const CpuContext saved = *ctx;
        for (uint32_t i = ArrayOverflow::kInlineSlots; i < ArrayOverflow::kMaxPlayers; ++i) {
            const uint32_t slot = ArrayOverflow::Window(c, i) + kModels;
            const uint32_t model = Memory::Read32(slot);
            if (model != 0) {
                ctx->gpr[3] = model;
                InvokeIndirectCpu(kReleaseModel, ctx);
                Memory::Write32(slot, 0);
            }
        }
        *ctx = saved;
    }
    MKW_GUEST_NEXT(8078CE24)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8078CE24, DriverMgr_Dtor_8078ce24, "DriverMgr_Dtor_8078ce24");
