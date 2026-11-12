// mkw100 overflow player slots: the offset helper behind the translator's rewritten 0xF0 sites,
// and the RaceConfig functions that walk a fixed 12 players and must carry players 12+ along.
// Layout and rationale: runtime/include/guest_abi/player_slots.h.

#include "guest_abi/player_slots.h"

#include "abi_bridge.h"
#include "hle/cc200.h"
#include "hle/teams.h"
#include "guest_function_imports.h"
#include "memory.h"
#include "local_vs.h"
#include "boot_race.h"

#include <array>
#include <cstdio>
#include <stdexcept>

namespace PlayerSlots {

uint32_t PlayerAddress(uint32_t racedata, uint32_t scenario, uint32_t index) {
    const uint32_t inline_ = scenario + kPlayersOffset + index * kStride;
    if (index < kInlineSlots) {
        return inline_;
    }
    return kOverflowBase + (index - kInlineSlots) * kWindow + (scenario - racedata) + kPlayersOffset;
}

} // namespace PlayerSlots

using namespace PlayerSlots;

extern "C" uint32_t MKW100_PlayerSlotOffset(uint32_t index) {
    // Out-of-range indices keep the original arithmetic, whatever they meant.
    if (index < kInlineSlots || index >= kMaxPlayers) {
        return index * kStride;
    }
    const uint32_t racedata = Memory::Read32(kRacedataGlobal);
    if (racedata == 0) {
        return index * kStride;
    }
    // Racedata + delta + disp = kOverflowBase + (index - 12) * kWindow + (disp relative to Racedata)
    return kOverflowBase - racedata + (index - kInlineSlots) * kWindow;
}

extern "C" uint32_t MKW100_PlayerSlotAddress(uint32_t slot, uint32_t displacement) {
    const uint32_t racedata = Memory::Read32(kRacedataGlobal);
    // Player i and the base the game added its product to (Racedata or a scenario, as an offset
    // from Racedata). Inline: slot = Racedata + base + i * 0xF0, and the four bases differ mod
    // 0xF0 (0, 0x20, 0xC10 = 0xD0, 0x1800 = 0x90). Overflow: slot = window(i) + base.
    uint32_t index = kMaxPlayers;
    uint32_t base = 0;
    if (slot - kOverflowBase < kOverflowSize) {
        index = kInlineSlots + (slot - kOverflowBase) / kWindow;
        base = (slot - kOverflowBase) % kWindow;
    } else {
        const uint32_t relative = slot - racedata;
        constexpr uint32_t kBases[] = {0, kScenarioOffsets[0], kScenarioOffsets[1], kScenarioOffsets[2]};
        for (const uint32_t candidate : kBases) {
            if (relative >= candidate && (relative - candidate) % kStride == 0 &&
                (relative - candidate) / kStride < kInlineSlots) {
                index = (relative - candidate) / kStride;
                base = candidate;
                break;
            }
        }
    }
    // The field as if player i sat in slot 0: which scenario's array, slot k, offset in the slot.
    const uint32_t field = base + displacement;
    for (const uint32_t scenario : kScenarioOffsets) {
        const uint32_t first = scenario + kPlayersOffset;
        if (index < kMaxPlayers && field - first < kInlineSlots * kStride) {
            const uint32_t player = index + (field - first) / kStride;
            if (player >= kMaxPlayers) {
                break;
            }
            return PlayerAddress(racedata, racedata + scenario, player) + (field - first) % kStride;
        }
    }
    char message[96];
    std::snprintf(message, sizeof message, "mkw100 player slots: 0x%08X%+d is not a player slot field", slot,
                  static_cast<int32_t>(displacement));
    throw std::runtime_error(message);
}

namespace {

constexpr uint32_t kPlayerTypeOffset = 0x10;
constexpr uint32_t kLocalPlayerNumOffset = 0x4;
constexpr uint32_t kPlayerInputIdxOffset = 0x5;
constexpr uint32_t kSettingsGameModeOffset = 0xB48 + 0x8;
enum : uint32_t { kTypeRealLocal = 0, kTypeNone = 5 };
constexpr uint32_t kGameModeAwards = 11;

uint32_t ScenarioRacedata(uint32_t scenario) {
    const uint32_t racedata = Memory::Read32(kRacedataGlobal);
    for (const uint32_t offset : kScenarioOffsets) {
        if (racedata + offset == scenario) {
            return racedata;
        }
    }
    throw std::runtime_error("mkw100 player slots: scenario is not inside Racedata");
}

void CopyPlayer(uint32_t to, uint32_t from) {
    // Everything but the vtable pointer, as RaceConfig::Player's assignment does.
    for (uint32_t offset = 4; offset < kStride; offset += 4) {
        Memory::Write32(to + offset, Memory::Read32(from + offset));
    }
}

struct SavedArgs {
    std::array<uint32_t, 32> gpr;
    explicit SavedArgs(const CpuContext* ctx) { std::copy(std::begin(ctx->gpr), std::end(ctx->gpr), gpr.begin()); }
};

} // namespace

// RaceConfig::createInstance: construct the overflow players of all three scenarios when it
// creates the instance. (Hooked here rather than on the RaceConfig ctor at 0x8053015C, which a
// wrapper never saw called.)
extern "C" void RaceConfig_CreateInstance_8052fe58(CpuContext* ctx) {
    const uint32_t before = Memory::Read32(kRacedataGlobal);
    MKW_GUEST_NEXT(8052FE58)(ctx);
    const uint32_t racedata = Memory::Read32(kRacedataGlobal);
    if (before != 0 || racedata == 0) {
        return;
    }
    const SavedArgs saved(ctx);
    for (const uint32_t offset : kScenarioOffsets) {
        for (uint32_t i = kInlineSlots; i < kMaxPlayers; ++i) {
            const uint32_t player = PlayerAddress(racedata, racedata + offset, i);
            ctx->gpr[3] = player;
            MKW_GUEST_FUNC(8052D96C)(ctx);  // RaceConfig::Player::Player
            // The ctor makes a local player; slots 0..11 always hold a racer, overflow slots
            // hold one only when something asks for it.
            Memory::Write32(player + kPlayerTypeOffset, kTypeNone);
        }
    }
    std::copy(saved.gpr.begin(), saved.gpr.end(), std::begin(ctx->gpr));
}
REGISTER_NATIVE_WRAPPER_AS(0x8052FE58, RaceConfig_CreateInstance_8052fe58, "RaceConfig_CreateInstance_8052fe58");

// RaceConfig::Scenario::copy (operator=) copies 12 players; carry the overflow ones too.
extern "C" void RaceConfigScenario_Copy_805305ac(CpuContext* ctx) {
    const uint32_t to = ctx->gpr[3];
    const uint32_t from = ctx->gpr[4];
    MKW_GUEST_NEXT(805305AC)(ctx);
    const uint32_t racedata = ScenarioRacedata(to);
    for (uint32_t i = kInlineSlots; i < kMaxPlayers; ++i) {
        CopyPlayer(PlayerAddress(racedata, to, i), PlayerAddress(racedata, from, i));
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x805305AC, RaceConfigScenario_Copy_805305ac, "RaceConfigScenario_Copy_805305ac");

// RaceConfig::Scenario::computePlayerCounts counts 12 slots; add the overflow players. They are
// never local (one HUD per local player, at most 4), and awards keep their 3/6 cap.
extern "C" void RaceConfigScenario_ComputePlayerCounts_8052f788(CpuContext* ctx) {
    const uint32_t scenario = ctx->gpr[3];
    const uint32_t playerCountOut = ctx->gpr[4];
    MKW_GUEST_NEXT(8052F788)(ctx);
    if (Memory::Read32(scenario + kSettingsGameModeOffset) == kGameModeAwards) {
        return;
    }
    const uint32_t racedata = ScenarioRacedata(scenario);
    uint32_t count = Memory::Read8(playerCountOut);
    for (uint32_t i = kInlineSlots; i < kMaxPlayers; ++i) {
        const uint32_t type = Memory::Read32(PlayerAddress(racedata, scenario, i) + kPlayerTypeOffset);
        if (type == kTypeNone) {
            continue;
        }
        if (type == kTypeRealLocal) {
            throw std::runtime_error("mkw100 player slots: local players must use slots 0..11");
        }
        ++count;
    }
    Memory::Write8(playerCountOut, static_cast<uint8_t>(count));
}
REGISTER_NATIVE_WRAPPER_AS(0x8052F788, RaceConfigScenario_ComputePlayerCounts_8052f788,
                           "RaceConfigScenario_ComputePlayerCounts_8052f788");

// RaceConfig::Scenario::initRace clears the controller binding of 12 players first.
extern "C" void RaceConfigScenario_InitRace_8052fb90(CpuContext* ctx) {
    const uint32_t scenario = ctx->gpr[3];
    const uint32_t racedata = ScenarioRacedata(scenario);
    for (uint32_t i = kInlineSlots; i < kMaxPlayers; ++i) {
        const uint32_t player = PlayerAddress(racedata, scenario, i);
        Memory::Write8(player + kLocalPlayerNumOffset, 0xFF);
        Memory::Write8(player + kPlayerInputIdxOffset, 0xFF);
    }
    MKW_GUEST_NEXT(8052FB90)(ctx);
    // The vanilla previous-race update only normalizes twelve starting ranks.
    // Expanded interactive VS keeps the complete finishing order and score.
    if (!BootRace::CpuDriven() && scenario == racedata + kScenarioOffsets[1] &&
        Memory::Read32(scenario + kSettingsGameModeOffset) == 1 &&
        Memory::Read8(scenario + 4) > kInlineSlots && Memory::Read8(scenario + 0xB6C) != 0) {
        const uint32_t count = std::min<uint32_t>(Memory::Read8(scenario + 4), kMaxPlayers);
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t player = PlayerAddress(racedata, scenario, i);
            Memory::Write16(player + 0xD8, Memory::Read16(player + 0xDA));
            Memory::Write8(player + 0xE1, Memory::Read8(player + 0xE2));
        }
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x8052FB90, RaceConfigScenario_InitRace_8052fb90, "RaceConfigScenario_InitRace_8052fb90");

// RaceConfig::initRace inits the menu scenario, then copies its players into the race scenario
// field by field with a pointer walk over the 12 inline slots (0x80530314..0x805304E0; nothing
// after it reads players). Without this the race scenario's overflow players keep stale fields,
// among them the start rank at +0xE1 that Raceinfo's ctor indexes its position table with.
extern "C" void RaceConfig_InitRace_805302c4(CpuContext* ctx) {
    const uint32_t racedata = ctx->gpr[3];
    LocalVs::PrepareRace(racedata);
    MKW_GUEST_NEXT(805302C4)(ctx);
    Cc200::OnInitRace(racedata);
    for (uint32_t i = kInlineSlots; i < kMaxPlayers; ++i) {
        CopyPlayer(PlayerAddress(racedata, racedata + kScenarioOffsets[0], i),
                   PlayerAddress(racedata, racedata + kScenarioOffsets[1], i));
    }
    // After the overflow players are copied, so their race scenario fields stay as set here.
    Teams::OnInitRace(racedata);
    LocalVs::AfterInitRace(racedata);
}
REGISTER_NATIVE_WRAPPER_AS(0x805302C4, RaceConfig_InitRace_805302c4, "RaceConfig_InitRace_805302c4");
