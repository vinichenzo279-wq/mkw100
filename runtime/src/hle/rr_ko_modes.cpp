// mkw100: Retro Rewind's KO, Lap KO and KO Royale stay off past 12 racers.
//
// The three modes (rr-pulsar PulsarEngine/Gamemodes/KO, LapKO, BattleRoyale) keep their racers in
// [12] tables. KO ranks every racer into a PlayerPosition[12] on the stack after each race and
// compacts the menu scenario's 12 RacedataPlayers by player id, so with more racers it writes past
// both. Lap KO and KO Royale (built on Lap KO's manager) treat racers 12+ as never active: in a
// headless 24-racer Lap KO race nobody was knocked out over 7 minutes, and KO Royale knocked out
// only racer 0.
//
// Pulsar's System::UpdateContext (0x8182E52C) is the only reader of SETTING_KOENABLED (0x6C000:
// 1 KO, 2 Lap KO) and SETTING_KOROYALEENABLED (0x70000); every mode bit comes from there. Its
// calls to Settings::Mgr::GetSettingValue (0x8183D040) get 0 for those two while the race has more
// than 12 racers. The settings pages read the stored value, so they still show the player's choice.
// Addresses of the known Retro Rewind build only (hle/mod_build.h).

#include "hle/mod_build.h"
#include "abi_bridge.h"
#include "boot_race.h"
#include "guest_abi/player_slots.h"
#include "guest_function_imports.h"
#include "local_vs.h"
#include "memory.h"
#include "runtime_log.h"

#include <algorithm>
#include <cstdint>

namespace {
constexpr uint32_t kRrUpdateContext = 0x8182E52C;
constexpr uint32_t kRrUpdateContextEnd = 0x8182F7EC;  // the next function
constexpr uint32_t kRrSettingKo = 0x6C000;            // KOENABLED: 0 off, 1 KO, 2 Lap KO
constexpr uint32_t kRrSettingKoRoyale = 0x70000;      // KOROYALEENABLED
constexpr uint32_t kMenuScenario = 0xC10;
constexpr uint32_t kRaceScenario = 0x20;
constexpr uint32_t kPlayerType = 0x10;
constexpr uint32_t kPlayerNone = 5;

uint32_t Racers(uint32_t racedata, uint32_t scenario) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < PlayerSlots::kMaxPlayers; ++i) {
        n += Memory::Read32(PlayerSlots::PlayerAddress(racedata, racedata + scenario, i) + kPlayerType) != kPlayerNone;
    }
    return n;
}

// The racers of the race being set up. The menu fills slots 12+ only when the race starts
// (LocalVs::PrepareRace), so a single-player offline race also counts the series' configured size.
// A direct-boot race (--race) has all its slots filled at boot and no menu series.
uint32_t UpcomingRacers() {
    const uint32_t rd = Memory::Read32(MKW_GADDR(809BD728));
    if (rd == 0) return 0;
    uint32_t n = std::max(Racers(rd, kMenuScenario), Racers(rd, kRaceScenario));
    if (!BootRace::Scripted() && LocalVs::LocalPlayers() <= 1) n = std::max(n, uint32_t(LocalVs::DesiredCount()));
    return n;
}
}  // namespace

extern "C" void RrKoModes_GetSettingValue_8183d040(CpuContext* ctx) {
    const uint32_t id = ctx->gpr[4];
    const uint32_t lr = ctx->lr;
    if ((id == kRrSettingKo || id == kRrSettingKoRoyale) && lr >= kRrUpdateContext && lr < kRrUpdateContextEnd &&
        ModBuild::IsKnownRetroRewind()) {
        const uint32_t racers = UpcomingRacers();
        if (racers > 12) {
            MKW_GUEST_NEXT(8183D040)(ctx);
            if (ctx->gpr[3] != 0) {
                static uint32_t logged[2] = {};
                uint32_t& last = logged[id == kRrSettingKoRoyale];
                if (last != racers) {
                    last = racers;
                    RT_LOGF("rr-ko", "%s needs 12 racers or fewer: off for %u racers\n",
                            id == kRrSettingKoRoyale ? "KO Royale" : ctx->gpr[3] == 2 ? "Lap KO" : "KO", racers);
                }
                ctx->gpr[3] = 0;
            }
            return;
        }
    }
    MKW_GUEST_NEXT(8183D040)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8183D040, RrKoModes_GetSettingValue_8183d040, "RrKoModes_GetSettingValue_8183d040");
