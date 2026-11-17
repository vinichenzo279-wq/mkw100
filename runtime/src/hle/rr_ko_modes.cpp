// mkw100: Retro Rewind's KO, Lap KO and KO Royale past 12 racers, offline.
//
// The three modes (rr-pulsar PulsarEngine/Gamemodes/KO, LapKO, BattleRoyale; GPL-3) keep their
// racers in [12] tables:
// - LapKO::Mgr (Lap KO, and KO Royale on top of it) has active[12], crossed[12], crossOrder[12] and
//   lastLapValue[12] and tracks lap crossings of the first 12 racers only. A round ends once
//   activeCount - eliminations racers have crossed, which never happened with more racers (a
//   24-racer Lap KO race knocked nobody out in 7 minutes).
// - KO::Mgr ranks every racer into a PlayerPosition[12] on the stack each race frame
//   (CalcWouldBeKnockedOut) and keeps status[12][2] by player id offline, so past 12 it wrote over
//   its stack frame and its own fields. Its post-race ProcessKOs is called from
//   GPVSLeaderboardTotal::FillRows, which LocalVs_TotalRows replaces past 12, so no racer was ever
//   knocked out there.
// - KO Royale's balloon code (BattleRoyale.cpp) reads 0 balloons for racers 12+, removes none when
//   they are hit and walks racers 0-11 for knockouts and the last racer in (24 racers: only racer 0
//   was ever knocked out).
//
// Past 12 racers in an offline race this file runs those parts natively for every racer and keeps
// the managers' own fields (activeCount, roundIndex, totalRounds, raceFinished, the 12 table
// entries, winnerPlayerId, offlineRaceNumber) current, so the rest of Retro Rewind (HUD, item
// rules, the 12-racer code once a KO series is down to 12) reads what it expects. Up to 12 racers,
// and online, Retro Rewind's code runs unchanged.
//
// Scaling for big fields:
// - Lap KO knocks out exactly N racers at each lap line, N being F10's "Lap KO knockouts per lap"
//   (1-25) or else Retro Rewind's KO-per-race setting (1-4), and the race gets the laps that takes
//   (LapKoLaps sets them before the race): 100 racers at 25 a lap, 4 laps; 160 at 5, 32 laps. At
//   most 50 laps (the two-digit lap counter's most); a field too big for that at N gets just enough
//   more per lap. Retro Rewind's own
//   code reads 12 rounds of the plan: past lap 12 its "in danger" highlight is off.
//   (The first version kept Retro Rewind's 12-racer race length and knocked out 48 per lap.)
// - KO Royale needs no scaling: every racer starts with the balloon setting's count.
// - KO knocks out the same share of the field per KO race as at 12: KOs per race x racers / 12,
//   rounded up (100 racers, 4 KOs per race: 34, then 22, 15, 10, 7, ...), until 12 or fewer
//   remain and Retro Rewind's own count takes over.
//
// Addresses of the known Retro Rewind build only (hle/mod_build.h).

#include "rr_ko_modes.h"

#include "hle/mod_build.h"
#include "abi_bridge.h"
#include "boot_race.h"
#include "guest_abi/array_overflow.h"
#include "guest_abi/player_slots.h"
#include "guest_function_imports.h"
#include "local_vs.h"
#include "memory.h"
#include "runtime_exit.h"
#include "runtime_log.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace rr_ko_detail {
constexpr uint32_t kRacedata = 0x809BD728;    // System::RaceConfig::spInstance
constexpr uint32_t kRaceinfo = 0x809BD730;    // Raceinfo::sInstance
constexpr uint32_t kController = 0x809C20D8;  // RKNet::Controller::sInstance (+0xE8 room type, 0 offline)
constexpr uint32_t kSectionMgr = 0x809C1E38;  // SectionMgr::sInstance (+0x98 SectionParams)
constexpr uint32_t kRaceScenario = 0x20;
constexpr uint32_t kMenuScenario = 0xC10;
constexpr uint32_t kPlayerType = 0x10;
constexpr uint32_t kTypeLocal = 0;
constexpr uint32_t kTypeNone = 5;
// Raceinfo / RaceinfoPlayer
constexpr uint32_t kRiPlayers = 0xC, kRiGamemode = 0x10, kRiFrames = 0x20, kRiStage = 0x28;
constexpr uint32_t kRiStageFinishing = 3;
constexpr uint32_t kRpPosition = 0x20, kRpLap = 0x24, kRpFlags = 0x38, kRpFinishTime = 0x40;

// Pulsar::System
constexpr uint32_t kRrSystem = 0x8189E6F8;  // System::sInstance
constexpr uint32_t kSysContext = 0x60, kSysKoMgr = 0xF0, kSysRacers = 0x108;
constexpr uint32_t kCtxKoFinal = 1u << 6, kCtxModeKo = 1u << 9, kCtxModeLapKo = 1u << 10;
constexpr uint32_t kSysLapKoMgr = 0xF4;
constexpr uint32_t kLapCount = 0xB48 + 0x25;          // RacedataSettings lapCount
constexpr uint32_t kLapCounterName = 0x808A9CC7;     // the lap counter's resource name, "la"p_number

bool KnownOffline() {
    if (!ModBuild::IsKnownRetroRewind()) return false;
    const uint32_t controller = Memory::Read32(kController);
    return controller != 0 && Memory::Read32(controller + 0xE8) == 0;
}

uint32_t System() { return Memory::Read32(kRrSystem); }

uint32_t SlotOf(uint32_t scenario, uint32_t id) {
    const uint32_t rd = Memory::Read32(kRacedata);
    return PlayerSlots::PlayerAddress(rd, rd + scenario, id);
}

uint32_t RaceinfoPlayer(uint32_t id) {
    const uint32_t ri = Memory::Read32(kRaceinfo);
    const uint32_t players = ri ? Memory::Read32(ri + kRiPlayers) : 0;
    return players ? Memory::Read32(players + 4 * id) : 0;
}

bool Finished(uint32_t id) {
    const uint32_t p = RaceinfoPlayer(id);
    return p != 0 && (Memory::Read32(p + kRpFlags) & 0x2) != 0;
}

// Calls a guest function with up to seven arguments; the caller's registers are kept.
uint32_t CallGuest(CpuContext* ctx, uint32_t fn, uint32_t r3 = 0, uint32_t r4 = 0, uint32_t r5 = 0, uint32_t r6 = 0,
                   uint32_t r7 = 0, uint32_t r8 = 0, uint32_t r9 = 0) {
    const CpuContext saved = *ctx;
    ctx->gpr[3] = r3;
    ctx->gpr[4] = r4;
    ctx->gpr[5] = r5;
    ctx->gpr[6] = r6;
    ctx->gpr[7] = r7;
    ctx->gpr[8] = r8;
    ctx->gpr[9] = r9;
    InvokeIndirectCpu(fn, ctx);
    const uint32_t result = ctx->gpr[3];
    *ctx = saved;
    return result;
}

// Tests: MKW100_TEST_KO_SPARE_LOCAL never knocks out the local racer, so a headless series or
// Lap KO race (where racer 0 is the local one) runs on to a winner instead of ending when racer 0
// is out.
bool SpareLocal() {
    static const bool spare = std::getenv("MKW100_TEST_KO_SPARE_LOCAL") != nullptr;
    return spare;
}

// ---------------------------------------------------------------------------------------------
// Lap KO (and the LapKO::Mgr under KO Royale)
// ---------------------------------------------------------------------------------------------
namespace lapko {
constexpr uint32_t kInitForRace = 0x8180FABC;
constexpr uint32_t kUpdateFrame = 0x81810610;
constexpr uint32_t kProcessElimination = 0x8180FF90;
constexpr uint32_t kDisplayTick = 0x8180B308;        // EliminationDisplay::Tick()
constexpr uint32_t kDisplayRecord = 0x8180B330;      // EliminationDisplay::RecordRoundElimination(id, round)
constexpr uint32_t kUsualLapCount = 0x818108B0;      // Mgr::GetUsualTrackLapCount
constexpr uint32_t kVanish = 0x80534C78;             // RaceinfoPlayer::Vanish
// LapKO::Mgr fields
constexpr uint32_t kKoPerRace = 0x0, kPlan = 0x1, kTotalRounds = 0xD, kActive = 0xE, kCrossed = 0x1A;
constexpr uint32_t kOrderCursor = 0x4A, kActiveCount = 0x4B, kPlayerCount = 0x4C, kRoundIndex = 0x4D;
constexpr uint32_t kDebits = 0x4E, kRaceFinished = 0x6C, kRaceInitDone = 0x6D, kLastRaceFrames = 0x6E;
constexpr uint32_t kReweightFrames = 0x70;
constexpr uint32_t kMaxRounds = 12;
// Lap KO past 12 racers: at most this many laps, the most Retro Rewind's two-digit lap counter
// (RRp_number, game_image_fap_texture_pattern_0_9.brlan: frames 0..50) can show.
constexpr uint32_t kMaxLaps = 50;

struct State {
    bool on = false;
    uint32_t mgr = 0;
    uint32_t n = 0;
    uint32_t usual = 3;
    std::array<uint8_t, PlayerSlots::kMaxPlayers> active{};
    std::array<uint8_t, PlayerSlots::kMaxPlayers> crossed{};
    std::array<uint16_t, PlayerSlots::kMaxPlayers> lastLap{};
    std::vector<uint8_t> order;   // lap crossings of this round
    std::vector<uint32_t> plan;   // knockouts per round (lap)
    bool resolvePending = false;  // a new round began with racers already past its line
};
State s;

// Retro Rewind's LapKO::Mgr::BuildPlan for 12 racers: the plan its race length comes from.
std::vector<uint32_t> Plan12(uint32_t koPerRace, uint32_t usual) {
    std::vector<uint32_t> plan;
    if (usual <= 1) return {11};
    if (usual == 2) koPerRace *= 2;
    uint32_t left = 12;
    while (left > 1 && plan.size() < kMaxRounds) {
        const uint32_t out = std::min(koPerRace, left - 1);
        plan.push_back(out);
        left -= out;
    }
    return plan;
}

// Knockouts per lap past 12 racers: F10's "Lap KO knockouts per lap" (1-25), else Retro Rewind's
// own KO-per-race setting (1-4), which its lap count hook stored in the manager.
uint32_t KnockoutsPerLap(uint32_t mgr) {
    if (const int f10 = LocalVs::LapKoPerLap(); f10 > 0) return uint32_t(f10);
    return std::max<uint32_t>(mgr ? Memory::Read8(mgr + kKoPerRace) : 1, 1);
}

// Exactly `perLap` racers go out at each lap line, as Retro Rewind does with 12 (4 per lap: 4, 4,
// then the last 4 race the final lap for the places); the last entry is the final lap. At most
// kMaxLaps laps (50): a field too big for that at
// `perLap` gets just enough more per lap (100 racers at 4: 9 a lap, 11 laps).
std::vector<uint32_t> PlanPerLap(uint32_t n, uint32_t perLap, uint32_t usual) {
    if (n < 2) return {0};
    if (usual <= 1) return {n - 1};  // a 1-lap course: the race decides everything, as with 12
    perLap = std::max({perLap, 1u, (n - 1 + kMaxLaps - 1) / kMaxLaps});
    std::vector<uint32_t> plan;
    for (uint32_t left = n - 1; left > 0; left -= std::min(left, perLap)) plan.push_back(std::min(left, perLap));
    return plan;
}

void Init(CpuContext* ctx, uint32_t mgr) {
    const uint32_t rd = Memory::Read32(kRacedata);
    s = State{};
    s.mgr = mgr;
    s.n = std::min<uint32_t>({Memory::Read8(mgr + kPlayerCount), Memory::Read8(rd + kRaceScenario + 4), PlayerSlots::kMaxPlayers});
    for (uint32_t i = 0; i < s.n; ++i) s.active[i] = 1;
    s.usual = CallGuest(ctx, kUsualLapCount, mgr) & 0xFF;
    const uint32_t sys = System();
    const bool lapKo = sys != 0 && (Memory::Read32(sys + kSysContext) & kCtxModeLapKo) != 0;
    // Lap KO: N per lap, over the laps LapKoLaps gave the race. KO Royale (no Lap KO bit) keeps
    // its own race length; the knockouts its balloons leave are spread over those laps.
    const uint32_t koPerRace = lapKo ? KnockoutsPerLap(mgr) : std::max<uint32_t>(Memory::Read8(mgr + kKoPerRace), 1);
    if (lapKo) {
        s.plan = PlanPerLap(s.n, koPerRace, s.usual);
    } else {
        // The race's laps (Retro Rewind's lap count hook set them from its 12-racer plan).
        uint32_t laps = Memory::Read8(rd + 0xB8D);
        if (laps == 0) laps = s.usual;
        laps = std::min(laps, kMaxRounds);
        if (s.usual <= 1 || laps <= 1) {
            s.plan = {s.n - 1};
        } else {
            // Racers left for the final lap, as with 12.
            const auto plan12 = Plan12(koPerRace, s.usual);
            uint32_t out12 = 0;
            for (size_t r = 0; r + 1 < plan12.size(); ++r) out12 += plan12[r];
            const uint32_t survivors = std::clamp<uint32_t>(12 - out12, 2, s.n);
            const uint32_t out = s.n - survivors;
            // Spread over the laps before the last; never a round of 0 before the knockouts are done
            // (a round only ends with a knockout).
            const uint32_t rounds = std::min(laps - 1, out);
            s.plan.assign(laps, 0);
            for (uint32_t r = 0; r < rounds; ++r) s.plan[r] = out / rounds + (r < out % rounds ? 1 : 0);
            s.plan[laps - 1] = survivors - 1;
        }
    }
    for (uint32_t i = 0; i < kMaxRounds; ++i) Memory::Write8(mgr + kPlan + i, uint8_t(std::min<uint32_t>(i < s.plan.size() ? s.plan[i] : 0, 255)));
    Memory::Write8(mgr + kTotalRounds, uint8_t(s.plan.size()));
    // Retro Rewind re-weights the item odds for the racers left (ProcessPendingItemReweight, 120
    // frames after InitForRace and after each knockout) by running Item::SlotData::
    // SetupVSProbabilities again, which allocates new tables from the race heap: past 12 racers
    // that heap is full and the game panicked (eggExpHeap.cpp:174). The odds stay those of the
    // starting field instead.
    Memory::Write8(mgr + kReweightFrames, 0);
    s.on = true;
    std::string text;
    for (uint32_t v : s.plan) text += std::to_string(v) + " ";
    RT_LOGF("rr-ko", "lap ko: %u racers, %u laps (usual %u, %u KO per lap set): knockouts per lap %s\n", s.n,
            uint32_t(s.plan.size()), s.usual, koPerRace, text.c_str());
}

bool LocalRacer(uint32_t id) { return Memory::Read32(SlotOf(kRaceScenario, id) + kPlayerType) == kTypeLocal; }

void SetActive(uint32_t id, bool on) {
    s.active[id] = on;
    if (id < 12) Memory::Write8(s.mgr + kActive + id, on);
}

// LapKO::Mgr::FinishOfflineAtCurrentStandings for every racer.
void FinishAtStandings(CpuContext* ctx) {
    const uint32_t ri = Memory::Read32(kRaceinfo);
    if (ri == 0 || Memory::Read32(ri + kRiPlayers) == 0 || Memory::Read32(ri + kRiGamemode) == 0) return;
    const uint32_t total = Memory::Read8(Memory::Read32(kRacedata) + kRaceScenario + 4);
    uint32_t finished = 0;
    for (uint32_t id = 0; id < total; ++id) {
        const uint32_t p = RaceinfoPlayer(id);
        finished += p != 0 && (Memory::Read32(p + kRpFlags) & 0x12) != 0;
    }
    const uint32_t mode = Memory::Read32(ri + kRiGamemode);
    if (CallGuest(ctx, Memory::Read32(Memory::Read32(mode) + 0x24), mode, finished, total) != 0) {
        Memory::Write32(ri + kRiStage, kRiStageFinishing);
    }
    RT_LOGF("rr-ko", "lap ko: race ends at the current standings (%u of %u finished)\n", finished, total);
}

// FinishOfflineWhenLocalPlayersResolved: the race ends once every local racer is out or finished.
bool FinishWhenLocalsResolved(CpuContext* ctx) {
    if (Memory::Read8(s.mgr + kRaceFinished)) return false;
    bool local = false;
    for (uint32_t id = 0; id < s.n; ++id) {
        if (!LocalRacer(id)) continue;
        local = true;
        if (s.active[id] && !Finished(id)) return false;
    }
    if (!local) return false;
    Memory::Write8(s.mgr + kRaceFinished, 1);
    FinishAtStandings(ctx);
    return true;
}

void ResetRound() {
    s.order.clear();
    std::fill(s.crossed.begin(), s.crossed.end(), uint8_t(0));
    Memory::Write8(s.mgr + kOrderCursor, 0);
    Memory::Write8(s.mgr + kDebits, 0);
    for (uint32_t i = 0; i < 12; ++i) Memory::Write8(s.mgr + kCrossed + i, 0);
}

// EndRaceWithEliminationFinishTime: a knocked-out racer finishes with 99:99.xxx, later knockouts
// with smaller times (placement = racers still in when it went out).
void FinishKnockedOut(uint32_t id, uint32_t placement) {
    const uint32_t p = RaceinfoPlayer(id);
    if (p == 0 || placement < 2 || Finished(id)) return;
    const uint32_t timer = Memory::Read32(p + kRpFinishTime);
    if (timer == 0) return;
    Memory::Write16(timer + 4, 99);
    Memory::Write8(timer + 6, 99);
    Memory::Write16(timer + 8, uint16_t(999 - std::min<uint32_t>(PlayerSlots::kMaxPlayers - placement, 999)));
    Memory::Write8(timer + 0xA, 1);
    Memory::Write32(p + kRpFlags, Memory::Read32(p + kRpFlags) | 0x2);
}

// A round counts a racer's crossing when its lap goes past the round. With a big field the leaders
// are often past the next line before a round is settled; uncounted, every round then settled a
// little later, and at 100 racers the leader finished lap 11 with 19 racers and two rounds left. The
// new round counts them at once, in race order (TryResolveRound on the next frame).
void CreditCrossed(uint32_t round) {
    std::vector<uint32_t> ahead;
    for (uint32_t id = 0; id < s.n; ++id) {
        const uint32_t p = RaceinfoPlayer(id);
        if (p != 0 && s.active[id] && Memory::Read16(p + kRpLap) > round) ahead.push_back(id);
    }
    std::stable_sort(ahead.begin(), ahead.end(), [](uint32_t a, uint32_t b) {
        return Memory::Read8(RaceinfoPlayer(a) + kRpPosition) < Memory::Read8(RaceinfoPlayer(b) + kRpPosition);
    });
    for (uint32_t id : ahead) {
        s.crossed[id] = 1;
        if (id < 12) Memory::Write8(s.mgr + kCrossed + id, 1);
        s.order.push_back(uint8_t(id));
    }
    Memory::Write8(s.mgr + kOrderCursor, uint8_t(std::min<size_t>(s.order.size(), 255)));
    s.resolvePending = !ahead.empty();
}

// LapKO::Mgr::ProcessElimination (offline, round cause).
void Eliminate(CpuContext* ctx, uint32_t id, bool suppressRoundAdvance) {
    if (id >= s.n || !s.active[id]) return;
    const uint32_t mgr = s.mgr;
    const uint32_t round = Memory::Read8(mgr + kRoundIndex);
    uint32_t active = Memory::Read8(mgr + kActiveCount);
    FinishKnockedOut(id, active);
    SetActive(id, false);
    if (active > 0) Memory::Write8(mgr + kActiveCount, uint8_t(--active));
    CallGuest(ctx, kDisplayRecord, id, round);  // names racers 0-11 only
    if (const uint32_t p = RaceinfoPlayer(id)) CallGuest(ctx, kVanish, p);
    // EnterSpectateIfLocal
    if (Memory::Read8(mgr + kRaceFinished)) return;
    if (LocalRacer(id) && FinishWhenLocalsResolved(ctx)) return;
    if (!suppressRoundAdvance) ResetRound();
    if (active <= 1) {
        Memory::Write8(mgr + kRaceFinished, 1);
        uint32_t winner = 0xFF;
        for (uint32_t i = 0; i < s.n && winner == 0xFF; ++i) if (s.active[i]) winner = i;
        RT_LOGF("rr-ko", "lap ko: racer %u wins\n", winner);
        FinishAtStandings(ctx);
        return;
    }
    if (!suppressRoundAdvance) {
        Memory::Write8(mgr + kRoundIndex, uint8_t(round + 1));
        CreditCrossed(round + 1);
    }
}

// TryResolveRound: once all but this round's knockouts have crossed, the slowest go out, those who
// have not crossed first (from the back of the field), then the last to cross.
void TryResolveRound(CpuContext* ctx) {
    const uint32_t mgr = s.mgr;
    const uint32_t round = Memory::Read8(mgr + kRoundIndex);
    if (round >= Memory::Read8(mgr + kTotalRounds)) return;  // no knockouts on the final lap
    const uint32_t active = Memory::Read8(mgr + kActiveCount);
    if (active <= 1) return;
    uint32_t out = s.usual <= 1 ? active - 1 : (round >= 1 && round - 1 < s.plan.size() ? s.plan[round - 1] : 0);
    out = std::min(out, active - 1);
    if (out == 0) return;
    const uint32_t required = s.usual <= 1 ? 1 : active - out;
    if (s.order.size() < required) return;
    std::vector<uint32_t> byPlace;
    for (uint32_t id = 0; id < s.n; ++id) if (s.active[id]) byPlace.push_back(id);
    std::stable_sort(byPlace.begin(), byPlace.end(), [](uint32_t a, uint32_t b) {
        const uint32_t pa = RaceinfoPlayer(a) ? Memory::Read8(RaceinfoPlayer(a) + kRpPosition) : 0;
        const uint32_t pb = RaceinfoPlayer(b) ? Memory::Read8(RaceinfoPlayer(b) + kRpPosition) : 0;
        return pa > pb;
    });
    std::vector<uint32_t> list;
    const auto take = [&](uint32_t id) {
        if (list.size() >= out || (SpareLocal() && LocalRacer(id))) return;
        if (std::find(list.begin(), list.end(), id) == list.end()) list.push_back(id);
    };
    for (uint32_t id : byPlace) if (!s.crossed[id]) take(id);
    for (auto it = s.order.rbegin(); it != s.order.rend(); ++it) if (s.active[*it]) take(*it);
    if (list.empty()) return;
    RT_LOGF("rr-ko", "lap ko: round %u ends, %u of %u out\n", round, uint32_t(list.size()), active);
    for (size_t i = 0; i < list.size(); ++i) Eliminate(ctx, list[i], i + 1 != list.size());
}

void UpdateLapProgress(CpuContext* ctx) {
    if (s.resolvePending) {
        s.resolvePending = false;
        TryResolveRound(ctx);
        if (Memory::Read8(s.mgr + kRaceFinished)) return;
    }
    for (uint32_t id = 0; id < s.n; ++id) {
        const uint32_t p = RaceinfoPlayer(id);
        if (p == 0) continue;
        const uint16_t lap = Memory::Read16(p + kRpLap);
        if (lap == s.lastLap[id]) continue;
        const bool forward = lap > s.lastLap[id];
        s.lastLap[id] = lap;
        // OnLapComplete
        if (!forward || !s.active[id] || s.crossed[id] || lap <= Memory::Read8(s.mgr + kRoundIndex)) continue;
        s.crossed[id] = 1;
        if (id < 12) Memory::Write8(s.mgr + kCrossed + id, 1);
        s.order.push_back(uint8_t(id));
        Memory::Write8(s.mgr + kOrderCursor, uint8_t(std::min<size_t>(s.order.size(), 255)));
        TryResolveRound(ctx);
        if (Memory::Read8(s.mgr + kRaceFinished)) return;
    }
}

bool Eligible() { return KnownOffline() && System() != 0 && Memory::Read8(System() + kSysRacers) > 12; }
}  // namespace lapko

// ---------------------------------------------------------------------------------------------
// KO (a VS series)
// ---------------------------------------------------------------------------------------------
namespace ko {
constexpr uint32_t kUpdate = 0x8180D710;            // Mgr::Update (race frame hook)
constexpr uint32_t kPrepareNextRace = 0x8180C794;   // Mgr::PrepareOfflineVSNextRace
constexpr uint32_t kAddRaceStats = 0x8180CA20;      // Mgr::AddRaceStats
constexpr uint32_t kSetPrevFinishPos = 0x8052E658;  // RacedataPlayer::SetPrevFinishPos
constexpr uint32_t kComputePlayerCounts = 0x8052F788;
// KO::Mgr fields
constexpr uint32_t kStatus = 0x0, kWouldBeOut = 0x18, kOffline = 0x26, kRaceNumber = 0x27, kRacesPerKo = 0x29;
constexpr uint32_t kKoPerRace = 0x2A, kAlwaysFinal = 0x2B, kThreshold = 0x2D, kChange = 0x2E, kWinner = 0x2F;
enum : uint8_t { kNormal = 0, kTie = 1, kKod = 2 };

// Status by player id, all racers (offline, aid = player id and the hud slot is 0); ids 0-11 are
// also written to the manager's own status[id][0].
std::array<uint8_t, PlayerSlots::kMaxPlayers> status{};
// Set by every race frame of a KO race, cleared when its standings are scored.
bool raceToScore = false;

void SetStatus(uint32_t mgr, uint32_t id, uint8_t value) {
    status[id] = value;
    if (id < 12) Memory::Write8(mgr + kStatus + 2 * id, value);
}

uint32_t KoManager() {
    const uint32_t sys = System();
    if (sys == 0 || (Memory::Read32(sys + kSysContext) & kCtxModeKo) == 0) return 0;
    const uint32_t mgr = Memory::Read32(sys + kSysKoMgr);
    return mgr != 0 && Memory::Read8(mgr + kOffline) ? mgr : 0;
}

// Mgr::GetRoundKoCount, past 12 scaled to the field.
uint32_t RoundKoCount(uint32_t mgr, uint32_t count) {
    uint32_t out = Memory::Read8(mgr + kKoPerRace);
    const uint32_t threshold = Memory::Read8(mgr + kThreshold);
    if (threshold != 0 && count <= threshold) out = Memory::Read8(mgr + kChange);
    if (count > 12) out = (out * count + 11) / 12;
    if (count - std::min(out, count) < 2 && Memory::Read8(mgr + kAlwaysFinal)) out = count - 2;
    if (out >= count) out = count - 1;
    return out;
}

bool LocalRacer(uint32_t id) { return Memory::Read32(SlotOf(kRaceScenario, id) + kPlayerType) == kTypeLocal; }

// Mgr::CalcWouldBeKnockedOut for every racer: the danger mark of racers 0-11 (the local ones).
void CalcWouldBeKnockedOut(uint32_t mgr, uint32_t count) {
    for (uint32_t id = 0; id < 12; ++id) Memory::Write8(mgr + kWouldBeOut + id, 0);
    const uint32_t raceNumber = Memory::Read8(mgr + kRaceNumber) + 1;
    const uint32_t racesPerKo = std::max<uint32_t>(Memory::Read8(mgr + kRacesPerKo), 1);
    if (raceNumber % racesPerKo != 0) return;
    struct Entry { uint32_t id, key, place; };
    std::vector<Entry> field;
    for (uint32_t id = 0; id < count; ++id) {
        const uint32_t p = RaceinfoPlayer(id);
        const uint32_t place = p ? Memory::Read8(p + kRpPosition) : count;
        const uint32_t key = racesPerKo > 1
            ? Memory::Read16(SlotOf(kMenuScenario, id) + 0xDA) + LocalVs::PointsFor(place, count)
            : count + 1 - place;  // larger is better either way
        field.push_back({id, key, place});
    }
    std::stable_sort(field.begin(), field.end(), [](const Entry& a, const Entry& b) { return a.key < b.key; });
    const uint32_t out = RoundKoCount(mgr, count);
    uint32_t marked = 0;
    for (const Entry& e : field) {
        if (marked >= out) break;
        if (racesPerKo == 1 && e.place == 1) continue;
        if (racesPerKo > 1 && &e == &field.back()) continue;
        if (e.id < 12) Memory::Write8(mgr + kWouldBeOut + e.id, 1);
        ++marked;
    }
}

uint32_t SectionParams() {
    const uint32_t sm = Memory::Read32(kSectionMgr);
    return sm ? Memory::Read32(sm + 0x98) : 0;
}

void EndSeries() {
    const uint32_t params = SectionParams();
    if (params) Memory::Write32(params + 0x60, Memory::Read32(params + 0x64));  // vsRaceNumber = vsRaceCount
}
}  // namespace ko

// ---------------------------------------------------------------------------------------------
// KO Royale (a VS race with balloons on LapKO::Mgr: the last balloon lost knocks a racer out)
// ---------------------------------------------------------------------------------------------
namespace royale {
constexpr uint32_t kAddBalloons = 0x8180846C;        // AddBalloons(balloonMgr, id, count)
constexpr uint32_t kBalloonEliminations = 0x81809B8C;  // ProcessBalloonEliminations(lapKoMgr, balloonMgr)
constexpr uint32_t kFinishSole = 0x81809D44;         // FinishSoleActiveUnfinishedPlayer(lapKoMgr, raceinfo)
constexpr uint32_t kRemoveHit = 0x8180936C;          // OnRemoveHit(raceMode, hitter, hit), r31 = item object
constexpr uint32_t kMoveHit = 0x81809490;            // OnMoveHit(raceMode, losing, gaining)
constexpr uint32_t kPoweredHit = 0x81809688;         // FinishPoweredHitAction(action, source)
constexpr uint32_t kShouldApply = 0x8180902C;        // ShouldApplyBattleRoyale()
constexpr uint32_t kStartingBalloons = 0x81808544;   // GetStartingBalloonCountSetting()
constexpr uint32_t kLossBlink = 0x81808590;          // StartBalloonLossBlink(id)
constexpr uint32_t kVictimProtected = 0x81808C3C;    // IsMushroomStealVictimProtected(id)
constexpr uint32_t kClearGolden = 0x81808F18;        // ClearActiveGoldenMushroom(id)
constexpr uint32_t kBalloonGlobal = 0x809C4748;      // RaceBalloonManager (balloon_battle.cpp)
constexpr uint32_t kRelease = 0x80869FD0;            // RaceBalloonManager::Remove
constexpr uint32_t kTransfer = 0x8086A0DC;           // RaceBalloonManager::Move
constexpr uint32_t kGetPlayerIdx = 0x80590A5C;       // Kart::Link::GetPlayerIdx
constexpr uint32_t kTimerCtor = 0x805442C0, kTimerSetActive = 0x80544310, kCloneTimer = 0x80535CA0;
constexpr uint32_t kEndRace = 0x805347F4;            // RaceinfoPlayer::EndRace(timer, bool, u32)
constexpr uint32_t kEndPlayerRace = 0x80533C6C;      // Raceinfo::EndPlayerRace(id)
constexpr uint32_t kObjBlueShell = 5;
constexpr uint32_t kStealLockFrames = 120;

struct State {
    uint32_t mgr = 0;  // the LapKO::Mgr of the race these belong to
    uint32_t n = 0;
    uint32_t eliminations = 0;
    std::vector<uint8_t> previous;      // balloons last frame
    std::vector<uint16_t> poweredLoss;  // race frame of the last star / bullet / mega balloon loss
    std::vector<uint8_t> stealLock;     // [gaining * n + losing]: frames a mushroom steal is locked
};
State r;

bool Eligible(CpuContext* ctx) {
    return lapko::s.on && lapko::Eligible() && CallGuest(ctx, kShouldApply) != 0;
}

uint16_t RaceFrames() { return uint16_t(Memory::Read32(Memory::Read32(kRaceinfo) + kRiFrames)); }

// Balloons held by racer `id` (balloon_battle.cpp keeps them for every racer past 12).
uint32_t Balloons(uint32_t id) {
    const uint32_t mgr = Memory::Read32(kBalloonGlobal);
    if (mgr == 0) return 0;
    if (id < 12) return Memory::Read8(mgr + 0x3C4 + id * 0x18);
    const ArrayOverflow::ArrayClass& c = ArrayOverflow::Class(11);
    return Memory::Read8(ArrayOverflow::Window(c, id) + c.arraysBegin);
}

void Sync() {
    if (r.mgr == lapko::s.mgr && r.n == lapko::s.n) return;
    r = State{};
    r.mgr = lapko::s.mgr;
    r.n = lapko::s.n;
    r.previous.resize(r.n);
    for (uint32_t id = 0; id < r.n; ++id) r.previous[id] = uint8_t(Balloons(id));
    r.poweredLoss.assign(r.n, 0xFFFF);
    r.stealLock.assign(size_t(r.n) * r.n, 0);
}

bool RemoveBalloon(CpuContext* ctx, uint32_t id) {
    const uint32_t before = Balloons(id);
    if (before == 0) return false;
    CallGuest(ctx, kRelease, Memory::Read32(kBalloonGlobal), id, 1, 1, 0, 1, 0);
    if (Balloons(id) >= before) return false;
    CallGuest(ctx, kLossBlink, id);
    return true;
}

bool MoveBalloon(CpuContext* ctx, uint32_t to, uint32_t from) {
    const uint32_t before = Balloons(from);
    if (before == 0) return false;
    CallGuest(ctx, kTransfer, Memory::Read32(kBalloonGlobal), to, from, 0, 1, 0);
    if (Balloons(from) >= before) return false;
    CallGuest(ctx, kLossBlink, from);
    return true;
}
}  // namespace royale
}  // namespace rr_ko_detail

// --- Lap KO hooks ---

// LapKO::Mgr::InitForRace: Retro Rewind sets up its 12 entries; past 12 every racer gets one here.
extern "C" void RrKoModes_LapKoInitForRace_8180fabc(CpuContext* ctx) {
    using namespace rr_ko_detail;
    const uint32_t mgr = ctx->gpr[3];
    MKW_GUEST_NEXT(8180FABC)(ctx);
    if (KnownOffline() && Memory::Read8(mgr + lapko::kPlayerCount) > 12) {
        const CpuContext saved = *ctx;
        lapko::Init(ctx, mgr);
        *ctx = saved;
    } else if (lapko::s.mgr == mgr) {
        lapko::s.on = false;
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x8180FABC, RrKoModes_LapKoInitForRace_8180fabc, "RrKoModes_LapKoInitForRace_8180fabc");

// LapKO::Mgr::UpdateFrame (Lap KO's race frame hook), offline past 12: the offline parts of it for
// every racer (the online parts, host events and disconnects, have nothing to do offline).
extern "C" void RrKoModes_LapKoUpdateFrame_81810610(CpuContext* ctx) {
    using namespace rr_ko_detail;
    if (!lapko::Eligible()) {
        MKW_GUEST_NEXT(81810610)(ctx);
        return;
    }
    const uint32_t mgr = ctx->gpr[3];
    const uint32_t ri = Memory::Read32(kRaceinfo);
    CallGuest(ctx, lapko::kDisplayTick);
    // EnsureRaceInitialized
    const uint16_t frames = uint16_t(Memory::Read32(ri + kRiFrames));
    const uint16_t last = Memory::Read16(mgr + lapko::kLastRaceFrames);
    if (last != 0xFFFF && frames < last) {
        Memory::Write8(mgr + lapko::kRaceInitDone, 0);
        Memory::Write8(mgr + lapko::kRaceFinished, 0);
        Memory::Write8(mgr + lapko::kPlayerCount, 0);
        lapko::s.on = false;
    }
    if (!Memory::Read8(mgr + lapko::kRaceInitDone) && frames == 0) CallGuest(ctx, lapko::kInitForRace, mgr);
    Memory::Write16(mgr + lapko::kLastRaceFrames, frames);
    if (!lapko::s.on || lapko::s.mgr != mgr) return;
    if (lapko::FinishWhenLocalsResolved(ctx)) return;
    if (Memory::Read8(mgr + lapko::kRaceFinished)) return;
    lapko::UpdateLapProgress(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x81810610, RrKoModes_LapKoUpdateFrame_81810610, "RrKoModes_LapKoUpdateFrame_81810610");

// LapKO::Mgr::ProcessElimination(id, cause, fromNetwork, suppressRoundAdvance): past 12 for every
// racer (KO Royale knocks out racers whose last balloon is gone through it).
extern "C" void RrKoModes_LapKoProcessElimination_8180ff90(CpuContext* ctx) {
    using namespace rr_ko_detail;
    const uint32_t mgr = ctx->gpr[3];
    if (lapko::s.on && lapko::s.mgr == mgr && KnownOffline()) {
        const CpuContext saved = *ctx;
        lapko::Eliminate(ctx, ctx->gpr[4] & 0xFF, (ctx->gpr[7] & 0xFF) != 0);
        *ctx = saved;
        return;
    }
    MKW_GUEST_NEXT(8180FF90)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8180FF90, RrKoModes_LapKoProcessElimination_8180ff90, "RrKoModes_LapKoProcessElimination_8180ff90");

// --- KO hooks ---

// KO::Mgr::Update (race frame hook): past 12 offline, the danger marks without the 12-entry stack
// table. (Its stats are for online races only.)
extern "C" void RrKoModes_KoUpdate_8180d710(CpuContext* ctx) {
    using namespace rr_ko_detail;
    const uint32_t mgr = KnownOffline() ? ko::KoManager() : 0;
    const uint32_t count = System() ? Memory::Read8(System() + kSysRacers) : 0;
    if (mgr != 0) ko::raceToScore = true;
    if (mgr == 0 || count <= 12) {
        MKW_GUEST_NEXT(8180D710)(ctx);
        return;
    }
    ko::CalcWouldBeKnockedOut(mgr, std::min<uint32_t>(count, PlayerSlots::kMaxPlayers));
}
REGISTER_NATIVE_WRAPPER_AS(0x8180D710, RrKoModes_KoUpdate_8180d710, "RrKoModes_KoUpdate_8180d710");

// KO::Mgr::PrepareOfflineVSNextRace (scene enter of each race): drops the knocked-out racers from
// the menu scenario. Past 12 for every slot, and the series roster of local_vs.cpp with it.
extern "C" void RrKoModes_KoPrepareNextRace_8180c794(CpuContext* ctx) {
    using namespace rr_ko_detail;
    const uint32_t mgr = ctx->gpr[3];
    const uint32_t rd = Memory::Read32(kRacedata);
    const uint32_t menu = rd + kMenuScenario;
    const uint32_t count = rd ? std::min<uint32_t>(Memory::Read8(menu + 4), PlayerSlots::kMaxPlayers) : 0;
    if (!KnownOffline() || count <= 12 || !Memory::Read8(mgr + ko::kOffline) || Memory::Read8(mgr + ko::kWinner) != 0xFF) {
        MKW_GUEST_NEXT(8180C794)(ctx);
        return;
    }
    const CpuContext saved = *ctx;
    std::vector<uint32_t> kept;
    for (uint32_t id = 0; id < count; ++id) {
        if (id < 12) ko::status[id] = Memory::Read8(mgr + ko::kStatus + 2 * id);
        if (ko::status[id] != ko::kKod) kept.push_back(id);
    }
    if (kept.size() != count) {
        for (uint32_t k = 0; k < kept.size(); ++k) {
            if (kept[k] == k) continue;
            const uint32_t from = SlotOf(kMenuScenario, kept[k]), to = SlotOf(kMenuScenario, k);
            for (uint32_t off = 0; off < PlayerSlots::kStride; off += 4) Memory::Write32(to + off, Memory::Read32(from + off));
        }
        for (uint32_t id = uint32_t(kept.size()); id < std::max<uint32_t>(count, 12); ++id)
            Memory::Write32(SlotOf(kMenuScenario, id) + kPlayerType, kTypeNone);
        // Start ranks 1..kept (+0xE1). Past 12 player_slots.cpp's InitRace makes each racer's last
        // place (+0xE2) its start rank, and those places ran up to the old field size: a 44-racer
        // race with a start rank of 60 crashed in GMData::HandlePositionTracking. Both get k + 1.
        for (uint32_t k = 0; k < kept.size(); ++k) {
            CallGuest(ctx, ko::kSetPrevFinishPos, SlotOf(kMenuScenario, k), k + 1);
            Memory::Write8(SlotOf(kMenuScenario, k) + 0xE2, uint8_t(k + 1));
        }
        CallGuest(ctx, ko::kComputePlayerCounts, menu, menu + 4, menu + 5, menu + 6);
        for (uint32_t id = 0; id < PlayerSlots::kMaxPlayers; ++id) ko::SetStatus(mgr, id, ko::kNormal);
        LocalVs::KoKeep(kept);
        RT_LOGF("rr-ko", "ko: next race with %u of %u racers (menu scenario now %u)\n", uint32_t(kept.size()), count,
                Memory::Read8(menu + 4));
    }
    *ctx = saved;
}
REGISTER_NATIVE_WRAPPER_AS(0x8180C794, RrKoModes_KoPrepareNextRace_8180c794, "RrKoModes_KoPrepareNextRace_8180c794");

// KO::Mgr::ProcessKOs for an offline VS race past 12 racers, from the series standings page.
void RrKoModes::OnSeriesStandings(CpuContext* ctx, const std::vector<Standing>& standings) {
    using namespace rr_ko_detail;
    using namespace rr_ko_detail::ko;
    if (!KnownOffline()) return;
    const uint32_t mgr = KoManager();
    if (mgr == 0 || standings.size() <= 12) return;
    // Once per race (the page can fill its rows again).
    if (!raceToScore) return;
    raceToScore = false;
    const CpuContext saved = *ctx;
    const uint32_t count = uint32_t(std::min<size_t>(standings.size(), PlayerSlots::kMaxPlayers));
    const uint32_t params = SectionParams();
    Memory::Write8(mgr + kAlwaysFinal, (Memory::Read32(System() + kSysContext) & kCtxKoFinal) != 0);
    const uint32_t raceNumber = Memory::Read8(mgr + kRaceNumber) + 1;
    const uint32_t racesPerKo = std::max<uint32_t>(Memory::Read8(mgr + kRacesPerKo), 1);
    if (raceNumber == 1) std::fill(status.begin(), status.end(), uint8_t(kNormal));
    for (uint32_t id = 0; id < 12; ++id) status[id] = Memory::Read8(mgr + kStatus + 2 * id);
    const auto out = [&](uint32_t id) { return status[id] == kKod; };
    uint32_t koCount = RoundKoCount(mgr, count);
    const bool koRace = raceNumber % racesPerKo == 0;
    std::vector<Standing> byPlace(standings.begin(), standings.begin() + count), byTotal = byPlace;
    std::stable_sort(byPlace.begin(), byPlace.end(), [](const Standing& a, const Standing& b) { return a.place < b.place; });
    std::stable_sort(byTotal.begin(), byTotal.end(), [](const Standing& a, const Standing& b) {
        return a.total != b.total ? a.total > b.total : a.place < b.place;
    });
    bool ties = false;
    uint32_t knocked = 0;
    if (koRace && racesPerKo > 1 && koCount > 0) {
        // Ties at the knockout line: no knockout this time and the race is run again (or, when
        // exactly the tied racers are to go, they all go).
        uint32_t activeCount = 0;
        for (const Standing& s : byTotal) activeCount += !out(s.id);
        const uint32_t line = activeCount - koCount;
        uint32_t position = 0, tieScore = 0;
        bool found = false;
        for (const Standing& s : byTotal) {
            if (out(s.id)) continue;
            if (position == line) { tieScore = s.total; found = true; break; }
            ++position;
        }
        if (found) {
            uint32_t tied = 0, inKo = 0, notInKo = 0;
            position = 0;
            for (const Standing& s : byTotal) {
                if (out(s.id)) continue;
                if (s.total == tieScore) { ++tied; (position >= line ? inKo : notInKo)++; }
                ++position;
            }
            if (inKo > 0 && notInKo > 0) {
                ties = true;
                koCount = 0;
                if (params) Memory::Write32(params + 0x60, Memory::Read32(params + 0x60) - 1);
                RT_LOGF("rr-ko", "ko: %u racers tied at the knockout line, race again\n", tied);
            } else if (tied == koCount) {
                for (const Standing& s : byTotal) if (!out(s.id) && s.total == tieScore) { SetStatus(mgr, s.id, kKod); ++knocked; }
                koCount = 0;
            }
        }
        if (!ties) {
            for (uint32_t id = 0; id < count; ++id) {
                Memory::Write16(SlotOf(kMenuScenario, id) + 0xDA, 0);  // score
                Memory::Write16(SlotOf(kMenuScenario, id) + 0xD8, 0);  // previous score
            }
        }
    }
    if (koRace && koCount > 0) {
        const auto& order = racesPerKo == 1 ? byPlace : byTotal;
        for (auto it = order.rbegin(); it != order.rend() && knocked < koCount; ++it) {
            if (out(it->id) || (SpareLocal() && LocalRacer(it->id))) continue;
            SetStatus(mgr, it->id, kKod);
            ++knocked;
        }
    }
    uint32_t left = 0, winner = 0xFF;
    for (uint32_t id = 0; id < count; ++id) if (!out(id)) { ++left; winner = id; }
    if (left == 1) Memory::Write8(mgr + kWinner, uint8_t(winner));
    CallGuest(ctx, kAddRaceStats, mgr);
    // FinishOfflineVSIfAllLocalPlayersAreOut
    bool local = false, localIn = false;
    for (uint32_t id = 0; id < count; ++id) {
        if (!LocalRacer(id)) continue;
        local = true;
        localIn = localIn || !out(id);
    }
    if (local && !localIn) EndSeries();
    if (left == 1) EndSeries();
    if (!ties) Memory::Write8(mgr + kRaceNumber, uint8_t(Memory::Read8(mgr + kRaceNumber) + 1));
    *ctx = saved;
    RT_LOGF("rr-ko", "ko: race %u of the series (%s race): %u racers, %u knocked out, %u left%s%s\n", raceNumber,
            koRace ? "KO" : "points", count, knocked, left, left == 1 ? ", winner racer " : "",
            left == 1 ? std::to_string(winner).c_str() : "");
    std::printf("[rr-ko] series race %u: %u racers, %u out, %u left\n", raceNumber, count, knocked, left);
    if (local && !localIn) std::printf("[rr-ko] series over: every local racer is out\n");
    if (left == 1) std::printf("[rr-ko] series over: winner racer %u\n", winner);
    std::fflush(stdout);
    // Tests (MKW100_TEST_SERIES): the run ends with the series.
    if (std::getenv("MKW100_TEST_SERIES") && (left == 1 || (local && !localIn))) RuntimeFlushAndExit(EXIT_SUCCESS);
}

// --- KO Royale hooks ---
//
// KO Royale's own code (BattleRoyale.cpp) skips racers 12+ everywhere: their balloon count reads 0,
// hits on them remove nothing, eliminations and the last-racer finish walk racers 0-11. Past 12
// these run natively for every racer, on the balloon manager balloon_battle.cpp keeps past 12. Up
// to 12 racers, and online, Retro Rewind's code runs unchanged. Hits between two of racers 0-11
// stay Retro Rewind's (its own star/bullet/mega and mushroom-steal tables). Its item ejection on
// damage writes a per-racer frame table for every racer in every mode: the entries of racers 12+
// are an array-sites.txt shadow.

uint32_t RrKoModes::RoyaleStartingBalloons(CpuContext* ctx) {
    using namespace rr_ko_detail;
    if (!KnownOffline() || System() == 0) return 0;
    const CpuContext saved = *ctx;
    const uint32_t count = CallGuest(ctx, royale::kShouldApply) != 0 ? CallGuest(ctx, royale::kStartingBalloons) & 0xFF : 0;
    *ctx = saved;
    return count;
}

// AddBalloons(balloonMgr, id, count): the starting balloons of racers 0-11, from Retro Rewind's own
// pool walk. Past 12 balloon_battle.cpp's manager owns the pool and hands every racer the
// starting count itself (RoyaleStartingBalloons above), so nothing here.
extern "C" void RrKoModes_RoyaleAddBalloons_8180846c(CpuContext* ctx) {
    using namespace rr_ko_detail;
    if (royale::Eligible(ctx)) return;
    MKW_GUEST_NEXT(8180846C)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8180846C, RrKoModes_RoyaleAddBalloons_8180846c, "RrKoModes_RoyaleAddBalloons_8180846c");

// ProcessBalloonEliminations: a racer whose last balloon is gone is knocked out (placement from
// the back of the field).
extern "C" void RrKoModes_RoyaleBalloonEliminations_81809b8c(CpuContext* ctx) {
    using namespace rr_ko_detail;
    if (!royale::Eligible(ctx)) {
        MKW_GUEST_NEXT(81809B8C)(ctx);
        return;
    }
    const CpuContext saved = *ctx;
    royale::Sync();
    auto& r = royale::r;
    for (auto& lock : r.stealLock) if (lock) --lock;
    for (uint32_t id = 0; id < r.n && !Memory::Read8(r.mgr + lapko::kRaceFinished); ++id) {
        const uint32_t now = royale::Balloons(id);
        if (lapko::s.active[id] && r.previous[id] != 0 && now == 0 && !(SpareLocal() && lapko::LocalRacer(id))) {
            ++r.eliminations;
            lapko::FinishKnockedOut(id, r.n + 1 - std::min(r.eliminations, r.n - 1));
            RT_LOGF("rr-ko", "royale: racer %u lost the last balloon (%u out)\n", id, r.eliminations);
            lapko::Eliminate(ctx, id, true);
        }
        r.previous[id] = uint8_t(now);
    }
    *ctx = saved;
}
REGISTER_NATIVE_WRAPPER_AS(0x81809B8C, RrKoModes_RoyaleBalloonEliminations_81809b8c,
                           "RrKoModes_RoyaleBalloonEliminations_81809b8c");

// FinishSoleActiveUnfinishedPlayer: the last racer still in finishes the race now.
extern "C" void RrKoModes_RoyaleFinishSole_81809d44(CpuContext* ctx) {
    using namespace rr_ko_detail;
    if (!royale::Eligible(ctx)) {
        MKW_GUEST_NEXT(81809D44)(ctx);
        return;
    }
    const CpuContext saved = *ctx;
    const uint32_t mgr = ctx->gpr[3], ri = ctx->gpr[4];
    uint32_t last = 0xFF, left = 0;
    for (uint32_t id = 0; id < lapko::s.n; ++id) {
        if (!lapko::s.active[id] || Finished(id)) continue;
        last = id;
        ++left;
    }
    const uint32_t player = left == 1 ? RaceinfoPlayer(last) : 0;
    if (player != 0) {
        // A Timer on the guest stack, below the caller's frame.
        const uint32_t timer = (saved.gpr[1] - 0x40) & ~0xFu;
        ctx->gpr[1] = timer - 0x40;
        Memory::Write32(ctx->gpr[1], saved.gpr[1]);  // back chain
        CallGuest(ctx, royale::kTimerCtor, timer, 0);
        CallGuest(ctx, royale::kCloneTimer, ri, timer);
        CallGuest(ctx, royale::kTimerSetActive, timer, 1);
        CallGuest(ctx, royale::kEndRace, player, timer, 0, 0);
        CallGuest(ctx, royale::kEndPlayerRace, ri, last);
        Memory::Write8(mgr + lapko::kRaceFinished, 1);
        RT_LOGF("rr-ko", "royale: racer %u is the last one in and wins\n", last);
    }
    *ctx = saved;
}
REGISTER_NATIVE_WRAPPER_AS(0x81809D44, RrKoModes_RoyaleFinishSole_81809d44, "RrKoModes_RoyaleFinishSole_81809d44");

// OnRemoveHit(raceMode, hitter, hit): a hit takes one balloon (a blue shell never the last one).
extern "C" void RrKoModes_RoyaleRemoveHit_8180936c(CpuContext* ctx) {
    using namespace rr_ko_detail;
    const uint32_t hitter = ctx->gpr[4], hit = ctx->gpr[5];
    if ((hitter < 12 && hit < 12) || !royale::Eligible(ctx)) {
        MKW_GUEST_NEXT(8180936C)(ctx);
        return;
    }
    const CpuContext saved = *ctx;
    royale::Sync();
    auto& r = royale::r;
    if (hitter != hit && hit < r.n && r.poweredLoss[hit] != royale::RaceFrames() && !Finished(hit)) {
        const bool blueShell = Memory::Read32(saved.gpr[31] + 4) == royale::kObjBlueShell;
        if (!(blueShell && royale::Balloons(hit) == 1)) royale::RemoveBalloon(ctx, hit);
    }
    *ctx = saved;
}
REGISTER_NATIVE_WRAPPER_AS(0x8180936C, RrKoModes_RoyaleRemoveHit_8180936c, "RrKoModes_RoyaleRemoveHit_8180936c");

// OnMoveHit(raceMode, losing, gaining): a mushroom bump steals one balloon (not the last one, not
// twice from the same racer within 2 seconds).
extern "C" void RrKoModes_RoyaleMoveHit_81809490(CpuContext* ctx) {
    using namespace rr_ko_detail;
    const uint32_t losing = ctx->gpr[4], gaining = ctx->gpr[5];
    if ((losing < 12 && gaining < 12) || !royale::Eligible(ctx)) {
        MKW_GUEST_NEXT(81809490)(ctx);
        return;
    }
    const CpuContext saved = *ctx;
    royale::Sync();
    auto& r = royale::r;
    if (losing != gaining && losing < r.n && gaining < r.n && r.poweredLoss[losing] != royale::RaceFrames() &&
        r.stealLock[size_t(gaining) * r.n + losing] == 0 && royale::Balloons(losing) > 1 && !Finished(losing) &&
        CallGuest(ctx, royale::kVictimProtected, losing) == 0 && royale::MoveBalloon(ctx, gaining, losing)) {
        r.stealLock[size_t(gaining) * r.n + losing] = royale::kStealLockFrames;
        CallGuest(ctx, royale::kClearGolden, gaining);
    }
    *ctx = saved;
}
REGISTER_NATIVE_WRAPPER_AS(0x81809490, RrKoModes_RoyaleMoveHit_81809490, "RrKoModes_RoyaleMoveHit_81809490");

// FinishPoweredHitAction(action, source): a star, bullet or mega hit takes one balloon, once a frame.
extern "C" void RrKoModes_RoyalePoweredHit_81809688(CpuContext* ctx) {
    using namespace rr_ko_detail;
    const uint32_t source = ctx->gpr[4];
    if (!royale::Eligible(ctx)) {
        MKW_GUEST_NEXT(81809688)(ctx);
        return;
    }
    const CpuContext saved = *ctx;
    const uint32_t id = CallGuest(ctx, royale::kGetPlayerIdx, saved.gpr[3]) & 0xFF;
    if (source < 12 && id < 12) {
        *ctx = saved;
        MKW_GUEST_NEXT(81809688)(ctx);
        return;
    }
    royale::Sync();
    auto& r = royale::r;
    const uint16_t frames = royale::RaceFrames();
    if (id < r.n && r.poweredLoss[id] != frames && royale::RemoveBalloon(ctx, id)) r.poweredLoss[id] = frames;
    *ctx = saved;
}
REGISTER_NATIVE_WRAPPER_AS(0x81809688, RrKoModes_RoyalePoweredHit_81809688, "RrKoModes_RoyalePoweredHit_81809688");

// RaceinfoPlayer's constructor (race_laps.cpp): the laps of an offline Lap KO race past 12 racers,
// N per lap (PlanPerLap), or 0. Retro Rewind's lap count hook (LoadCustomLapCount, at 0x805328D4)
// has just set them from its own 12-racer plan and stored the KO-per-race setting in the manager.
uint32_t RrKoModes::LapKoLaps(CpuContext* ctx) {
    using namespace rr_ko_detail;
    if (!KnownOffline()) return 0;
    const uint32_t sys = System();
    if (sys == 0 || (Memory::Read32(sys + kSysContext) & kCtxModeLapKo) == 0) return 0;
    const uint32_t rd = Memory::Read32(kRacedata);
    const uint32_t n = rd ? Memory::Read8(rd + kRaceScenario + 4) : 0;
    if (n <= 12) return 0;
    const uint32_t mgr = Memory::Read32(sys + kSysLapKoMgr);
    const uint32_t usual = mgr ? (CallGuest(ctx, lapko::kUsualLapCount, mgr) & 0xFF) : 3;
    const uint32_t laps = uint32_t(lapko::PlanPerLap(n, lapko::KnockoutsPerLap(mgr), usual).size());
    Memory::Write8(rd + kRaceScenario + kLapCount, uint8_t(laps));
    Memory::Write8(rd + kMenuScenario + kLapCount, uint8_t(laps));
    // The lap counter to match, as its hook does: past 9 laps Retro Rewind's own two-digit one
    // (RRp_number.brctr), else the game's (lap_number). Its hook chose by its own laps: 11 for 1 KO
    // per lap chose RRp_number, which then panicked drawing a 4-lap race (TPL.c, invalid palette).
    Memory::Write8(kLapCounterName, laps > 9 ? 'R' : 'l');
    Memory::Write8(kLapCounterName + 1, laps > 9 ? 'R' : 'a');
    return laps;
}
