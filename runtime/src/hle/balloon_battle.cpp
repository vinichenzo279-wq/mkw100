// mkw100: the balloon battle manager in battles of more than 12 players.
//
// Balloon battle creates one manager (0x808697BC, 0x4F8 bytes, instance at 0x809C4748):
//   +0x0   u8 player count (Kart::Manager +0x24)
//   +0x1   u8 5 * count, +0x2 u8 10 * count: the balloon pool size, two halves (group 0 / 1)
//   +0x4   {Balloon* obj, u8 group, u8 owner, u8 slot, pad}[120]: the pool (owner == count: free)
//   +0x3C4 {u8 held, Balloon* held[5], pad}[12] (stride 0x18), one per player
//   +0x4E4 u32 frame, +0x4E8 u32 first-grab frame (0 or 120), +0x4EC u8[12] per player
// Its methods count the pool with u8 counters and size it 10 per player, so 13+ players write the
// pool over the per-player array and past the object (24 players: a heap free-list pointer, the
// crash at scene load), and 26+ wrap the u8 sizes.
//
// With more than 12 players every method below runs natively instead, from the same logic:
//  - the pool lives here (u32 counters, 10 per player); the first 120 balloon pointers are also
//    written to +0x4, which Kart code at 0x8059840C walks with the u8 count at +0x2 (set to 120:
//    it only touches those, a deliberate simplification);
//  - the per-player array is overflow class 11 (array_overflow.h): players 12+ live in shadow
//    windows, and the game's readers (GMDataBalloonBattle, items, balloon objects) are rewritten
//    sites in array-sites.txt;
//  - the object is allocated 0x4EC + count bytes so +0x4EC[count] fits (Kart::Movement::DoRespawn
//    and 0x8058C0DC write it, the balloon objects read it).
// Up to 12 players the game's own methods run unchanged.

#include "abi_bridge.h"
#include "guest_abi/array_overflow.h"
#include "guest_abi/player_slots.h"
#include "guest_function_imports.h"
#include "memory.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace {

constexpr uint32_t kBalloonGlobal = 0x809C4748;
constexpr uint32_t kBalloonClass = 11;
constexpr uint32_t kBalloonKartManager = 0x809C18F8;  // u8 player count at +0x24
constexpr uint32_t kBalloonRacedata = 0x809BD728;
constexpr uint32_t kBalloonRaceinfo = 0x809BD730;     // players at +0xC
constexpr uint32_t kBalloonOperatorNew = 0x80229DCC;
constexpr uint32_t kBalloonObjCtor = 0x8086E224;      // Balloon(this, index, group), 0x1E4 bytes
constexpr uint32_t kBalloonObjSize = 0x1E4;
constexpr uint32_t kBalloonAttach = 0x8086EC5C;       // obj, frame, player, slot, arg
constexpr uint32_t kBalloonDrop = 0x8086F01C;         // obj, frame, arg, arg
constexpr uint32_t kBalloonSteal = 0x8086EE50;        // obj, frame, player, slot
constexpr uint32_t kBalloonInlineSize = 0x4F8;
constexpr uint32_t kBalloonInlinePool = 120;
constexpr uint32_t kBalloonsHeld = 5;
constexpr uint32_t kBalloonInline = 12;

struct Balloon {
    uint32_t obj;
    uint32_t group;
    uint32_t owner;
    uint32_t slot;
};

bool g_balloonNative = false;
uint32_t g_balloonCount = 0;
std::vector<Balloon> g_balloons;

uint32_t BalloonPlayerCount() {
    const uint32_t mgr = Memory::Read32(kBalloonKartManager);
    return mgr == 0 ? 0 : Memory::Read8(mgr + 0x24);
}

uint32_t Entry(uint32_t mgr, uint32_t player) {
    if (player < kBalloonInline) return mgr + 0x3C4 + player * 0x18;
    const ArrayOverflow::ArrayClass& c = ArrayOverflow::Class(kBalloonClass);
    return ArrayOverflow::Window(c, player) + c.arraysBegin;
}

uint32_t Guest(CpuContext* ctx, uint32_t fn, std::initializer_list<uint32_t> args) {
    uint32_t r = 3;
    for (uint32_t a : args) ctx->gpr[r++] = a;
    InvokeIndirectCpu(fn, ctx);
    return ctx->gpr[3];
}

void VCall(CpuContext* ctx, uint32_t obj, uint32_t slot) {
    Guest(ctx, Memory::Read32(Memory::Read32(obj) + slot), {obj});
}

void Construct(CpuContext* ctx, uint32_t mgr) {
    const uint32_t n = g_balloonCount;
    Memory::Write8(mgr + 0, n);
    Memory::Write8(mgr + 1, std::min<uint32_t>(5 * n, 0xFF));
    Memory::Write8(mgr + 2, kBalloonInlinePool);
    for (uint32_t p = 0; p < n; ++p) {
        const uint32_t e = Entry(mgr, p);
        Memory::Write8(e, 0);
        for (uint32_t k = 0; k < kBalloonsHeld; ++k) Memory::Write32(e + 4 + 4 * k, 0);
    }
    g_balloons.assign(10 * n, Balloon{});
    for (uint32_t i = 0; i < g_balloons.size(); ++i) {
        const uint32_t group = i / (5 * n);
        const uint32_t obj = Guest(ctx, kBalloonOperatorNew, {kBalloonObjSize});
        if (obj != 0) Guest(ctx, kBalloonObjCtor, {obj, i, group});
        g_balloons[i] = Balloon{obj, group, n, kBalloonsHeld};
        if (i < kBalloonInlinePool) {
            Memory::Write32(mgr + 4 + 8 * i, obj);
            Memory::Write8(mgr + 4 + 8 * i + 4, group);
        }
        VCall(ctx, obj, 0x20);
    }
}

void Init(CpuContext* ctx, uint32_t mgr) {
    const uint32_t n = g_balloonCount;
    for (uint32_t p = 0; p < n; ++p) {
        const uint32_t e = Entry(mgr, p);
        Memory::Write8(e, 0);
        for (uint32_t k = 0; k < kBalloonsHeld; ++k) Memory::Write32(e + 4 + 4 * k, 0);
        Memory::Write8(mgr + 0x4EC + p, 0);
    }
    for (Balloon& b : g_balloons) {
        b.owner = n;
        b.slot = kBalloonsHeld;
        VCall(ctx, b.obj, 0xC);
    }
    const uint32_t racedata = Memory::Read32(kBalloonRacedata);
    const uint32_t gametype = Memory::Read32(racedata + 0xB74);
    const uint32_t gamemode = Memory::Read32(racedata + 0xB70);
    uint32_t limit = 0x78;
    if (gametype - 2 <= 2 || (gamemode >= 7 && gamemode <= 10)) limit = 0;
    Memory::Write32(mgr + 0x4E8, limit);
    Memory::Write32(mgr + 0x4E4, 0);
}

// 0x80869DF4: give `player` up to `max` free balloons of `group`.
void Grab(CpuContext* ctx, uint32_t mgr, uint32_t player, uint32_t group, uint32_t arg6, uint32_t delay,
          uint32_t max, uint32_t step) {
    const uint32_t n = g_balloonCount;
    if (group >= 2) group = 0;
    const uint32_t e = Entry(mgr, player);
    const uint32_t start = Memory::Read32(mgr + 0x4E4) + delay;
    const int32_t room = int32_t(kBalloonsHeld) - int32_t(Memory::Read8(e));
    if (room <= int32_t(max)) max = uint32_t(room) & 0xFF;
    std::vector<uint32_t> picked;
    for (uint32_t i = 0; i < g_balloons.size() && picked.size() < max; ++i) {
        const Balloon& b = g_balloons[i];
        if (b.owner < n || b.group != group) continue;
        const uint32_t a = Memory::Read32(b.obj + 0x180);
        if (Memory::Read32(Memory::Read32(b.obj + 0x178) + a * 16) != 0) continue;
        if (Memory::Read32(b.obj + 0x17C) == a && Memory::Read8(b.obj + 0x185) != 0) picked.push_back(i);
    }
    for (uint32_t i = 0; i < g_balloons.size() && picked.size() < max; ++i) {
        const Balloon& b = g_balloons[i];
        if (b.owner >= n && b.group == group) picked.push_back(i);
    }
    for (uint32_t k = 0; k < picked.size(); ++k) {
        Balloon& b = g_balloons[picked[k]];
        const uint32_t held = Memory::Read8(e);
        Guest(ctx, kBalloonAttach, {b.obj, start + k * step, player, held, arg6});
        b.owner = player;
        b.slot = held;
        Memory::Write32(e + 4 + 4 * held, b.obj);
        Memory::Write8(e, held + 1);
    }
}

// 0x80869FD0: `player` drops up to `count` balloons.
void Release(CpuContext* ctx, uint32_t mgr, uint32_t player, uint32_t arg5, uint32_t arg6, uint32_t delay,
             uint32_t count, uint32_t step) {
    const uint32_t n = g_balloonCount;
    const uint32_t e = Entry(mgr, player);
    const uint32_t start = Memory::Read32(mgr + 0x4E4) + delay;
    count = std::min<uint32_t>(count, Memory::Read8(e));
    for (uint32_t k = 0; k < count; ++k) {
        const uint32_t slot = Memory::Read8(e) - 1;
        for (Balloon& b : g_balloons) {
            if (b.owner != player || b.slot != slot) continue;
            Guest(ctx, kBalloonDrop, {b.obj, start + k * step, arg5, arg6});
            b.owner = n;
            b.slot = kBalloonsHeld;
            Memory::Write32(e + 4 + 4 * slot, 0);
            Memory::Write8(e, slot);
            break;
        }
    }
}

// 0x8086A0DC: `thief` takes up to `count` balloons from `victim`.
void Transfer(CpuContext* ctx, uint32_t mgr, uint32_t thief, uint32_t victim, uint32_t delay, uint32_t count,
              uint32_t step) {
    const uint32_t n = g_balloonCount;
    if (thief == victim) return;
    const uint32_t et = Entry(mgr, thief);
    if (Memory::Read8(et) == 0) return;
    const uint32_t ev = Entry(mgr, victim);
    const uint32_t start = Memory::Read32(mgr + 0x4E4) + delay;
    count = std::min<uint32_t>(count, Memory::Read8(ev));
    for (uint32_t k = 0; k < count; ++k) {
        const uint32_t slot = Memory::Read8(ev) - 1;
        for (Balloon& b : g_balloons) {
            if (b.owner != victim || b.slot != slot) continue;
            const uint32_t held = Memory::Read8(et);
            if (held < kBalloonsHeld) {
                Guest(ctx, kBalloonSteal, {b.obj, start + k * step, thief, held});
                b.owner = thief;
                b.slot = held;
                Memory::Write32(et + 4 + 4 * held, b.obj);
                Memory::Write8(et, held + 1);
            } else {
                Guest(ctx, kBalloonDrop, {b.obj, start + k * step, 1, 1});
                b.owner = n;
                b.slot = kBalloonsHeld;
            }
            Memory::Write32(ev + 4 + 4 * slot, 0);
            Memory::Write8(ev, slot);
            break;
        }
    }
}

// The registers a native replacement must hand back as the guest would: r1, r2, r13 and
// r14..r31 are the caller's, and the guest calls above keep them.
struct Saved {
    explicit Saved(CpuContext* c) : ctx(c), lr(c->lr), r3(c->gpr[3]) {}
    void Done(uint32_t ret) { ctx->gpr[3] = ret; ctx->lr = lr; }
    CpuContext* ctx;
    uint32_t lr;
    uint32_t r3;
};

} // namespace

// 0x808697BC: creates the manager in balloon battles. More than 12 players: a larger object, the
// overflow windows set up, the native constructor.
extern "C" void BalloonMgr_CreateInstance_808697bc(CpuContext* ctx) {
    const uint32_t n = BalloonPlayerCount();
    g_balloonNative = n > kBalloonInline;
    if (!g_balloonNative || Memory::Read32(kBalloonGlobal) != 0) {
        MKW_GUEST_NEXT(808697BC)(ctx);
        return;
    }
    const uint32_t racedata = Memory::Read32(kBalloonRacedata);
    const uint32_t mode = Memory::Read32(racedata + 0xB70);
    const uint32_t battleType = Memory::Read32(racedata + 0xB78);
    if (!((mode == 3 || mode == 9 || mode == 10) && battleType == 0)) return;
    Saved s(ctx);
    g_balloonCount = n;
    const uint32_t size = std::max<uint32_t>(kBalloonInlineSize, (0x4EC + n + 3) & ~3u);
    const uint32_t mgr = Guest(ctx, kBalloonOperatorNew, {size});
    if (mgr != 0) {
        Memory::Write32(kBalloonGlobal, mgr);
        ArrayOverflow::InitWindows(ArrayOverflow::Class(kBalloonClass), ctx);
        Construct(ctx, mgr);
    }
    s.Done(mgr);
}
REGISTER_NATIVE_WRAPPER_AS(0x808697BC, BalloonMgr_CreateInstance_808697bc, "BalloonMgr_CreateInstance_808697bc");

extern "C" void BalloonMgr_Init_808699d8(CpuContext* ctx) {
    if (!g_balloonNative) return MKW_GUEST_NEXT(808699D8)(ctx);
    Saved s(ctx);
    Init(ctx, s.r3);
    s.Done(s.r3);
}
REGISTER_NATIVE_WRAPPER_AS(0x808699D8, BalloonMgr_Init_808699d8, "BalloonMgr_Init_808699d8");

// 0x80869B2C: at the first-grab frame every player takes 3 balloons of their team; players whose
// Raceinfo state has bit 0x10 drop theirs.
extern "C" void BalloonMgr_Update_80869b2c(CpuContext* ctx) {
    if (!g_balloonNative) return MKW_GUEST_NEXT(80869B2C)(ctx);
    Saved s(ctx);
    const uint32_t mgr = s.r3;
    const uint32_t racedata = Memory::Read32(kBalloonRacedata);
    const uint32_t gametype = Memory::Read32(racedata + 0xB74);
    if (Memory::Read32(mgr + 0x4E4) == Memory::Read32(mgr + 0x4E8) && gametype != 5 && gametype != 6) {
        for (uint32_t p = 0; p < g_balloonCount; ++p) {
            const uint32_t player = PlayerSlots::PlayerAddress(racedata, racedata + 0x20, p);
            Grab(ctx, mgr, p, Memory::Read32(player + 0xCC) & 0xFF, 1, 0, 3, 0x14);
        }
    }
    const uint32_t players = Memory::Read32(Memory::Read32(kBalloonRaceinfo) + 0xC);
    for (uint32_t p = 0; p < g_balloonCount; ++p) {
        const uint32_t info = Memory::Read32(players + 4 * p);
        if ((Memory::Read32(info + 0x38) & 0x10) == 0) continue;
        const uint32_t held = Memory::Read8(Entry(mgr, p));
        if (held != 0) Release(ctx, mgr, p, 1, 1, 0, held, 0);
    }
    s.Done(s.r3);
}
REGISTER_NATIVE_WRAPPER_AS(0x80869B2C, BalloonMgr_Update_80869b2c, "BalloonMgr_Update_80869b2c");

extern "C" void BalloonMgr_Calc_80869c48(CpuContext* ctx) {
    if (!g_balloonNative) return MKW_GUEST_NEXT(80869C48)(ctx);
    Saved s(ctx);
    const uint32_t mgr = s.r3;
    for (const Balloon& b : g_balloons) VCall(ctx, b.obj, 0x18);
    for (const Balloon& b : g_balloons) VCall(ctx, b.obj, 0x1C);
    for (uint32_t p = 0; p < g_balloonCount; ++p) Memory::Write8(mgr + 0x4EC + p, 0);
    Memory::Write32(mgr + 0x4E4, Memory::Read32(mgr + 0x4E4) + 1);
    s.Done(s.r3);
}
REGISTER_NATIVE_WRAPPER_AS(0x80869C48, BalloonMgr_Calc_80869c48, "BalloonMgr_Calc_80869c48");

extern "C" void BalloonMgr_Grab_80869df4(CpuContext* ctx) {
    if (!g_balloonNative) return MKW_GUEST_NEXT(80869DF4)(ctx);
    Saved s(ctx);
    const uint32_t* g = ctx->gpr;
    Grab(ctx, g[3], g[4], g[5], g[6], g[7], g[8] & 0xFF, g[9]);
    s.Done(s.r3);
}
REGISTER_NATIVE_WRAPPER_AS(0x80869DF4, BalloonMgr_Grab_80869df4, "BalloonMgr_Grab_80869df4");

extern "C" void BalloonMgr_Release_80869fd0(CpuContext* ctx) {
    if (!g_balloonNative) return MKW_GUEST_NEXT(80869FD0)(ctx);
    Saved s(ctx);
    const uint32_t* g = ctx->gpr;
    Release(ctx, g[3], g[4], g[5], g[6], g[7], g[8] & 0xFF, g[9]);
    s.Done(s.r3);
}
REGISTER_NATIVE_WRAPPER_AS(0x80869FD0, BalloonMgr_Release_80869fd0, "BalloonMgr_Release_80869fd0");

extern "C" void BalloonMgr_Transfer_8086a0dc(CpuContext* ctx) {
    if (!g_balloonNative) return MKW_GUEST_NEXT(8086A0DC)(ctx);
    Saved s(ctx);
    const uint32_t* g = ctx->gpr;
    Transfer(ctx, g[3], g[4], g[5], g[6], g[7] & 0xFF, g[8]);
    s.Done(s.r3);
}
REGISTER_NATIVE_WRAPPER_AS(0x8086A0DC, BalloonMgr_Transfer_8086a0dc, "BalloonMgr_Transfer_8086a0dc");

// Battle AI (the object at AI::Manager +0x94): 0x80727F60(this, team-0 count, team-1 count, the
// player count at AI::Manager +0x14) allocates two 0x1C-byte lists at +0xC0 (team 1)
// and +0xC4 (team 0): KartObject*[6], then the count at +0x18. It stores every racer's kart into
// its team's list without a bound, so a 7th teammate overwrites the count (13 players: the AI
// picked a target with the heap header after the list as its kart, crash at the first frame).
// With more than 6 on a team the lists are built here with at most 6 karts each: battle CPUs
// only consider the first 6 of each team (a deliberate simplification; a native list for the
// whole team needs their 6 readers, 0x80727DA0 .. 0x80728CC8, rewritten).
namespace {
constexpr uint32_t kTeamListSlots = 6;
}

extern "C" void BattleAI_InitTeamLists_80727f60(CpuContext* ctx) {
    if (ctx->gpr[4] <= kTeamListSlots && ctx->gpr[5] <= kTeamListSlots) {
        MKW_GUEST_NEXT(80727F60)(ctx);
        return;
    }
    Saved s(ctx);
    const uint32_t self = s.r3;
    const uint32_t counts[2] = {std::min<uint32_t>(ctx->gpr[5], kTeamListSlots),   // +0xC0: team 1
                                std::min<uint32_t>(ctx->gpr[4], kTeamListSlots)};  // +0xC4: team 0
    const uint32_t players = Memory::Read32(Memory::Read32(0x809C2BE8) + 0x14);
    uint32_t lists[2];
    for (uint32_t l = 0; l < 2; ++l) {
        lists[l] = Guest(ctx, kBalloonOperatorNew, {0x1C});
        if (lists[l] != 0) {
            for (uint32_t k = 0; k < kTeamListSlots; ++k) Memory::Write32(lists[l] + 4 * k, 0);
            Memory::Write32(lists[l] + 0x18, counts[l]);
        }
        Memory::Write32(self + 0xC0 + 4 * l, lists[l]);
    }
    const uint32_t racedata = Memory::Read32(kBalloonRacedata);
    uint32_t filled[2] = {0, 0};
    for (uint32_t i = 0; i < players; ++i) {
        const uint32_t team = Memory::Read32(PlayerSlots::PlayerAddress(racedata, racedata + 0x20, i) + 0xCC);
        if (team > 1) continue;
        const uint32_t l = team == 0 ? 1 : 0;
        if (filled[l] >= counts[l] || lists[l] == 0) continue;
        const uint32_t kartPlayer = Guest(ctx, 0x80590100, {Memory::Read32(kBalloonKartManager), i});
        const uint32_t kart = Guest(ctx, 0x805910C0, {kartPlayer});
        Memory::Write32(lists[l] + 4 * filled[l]++, kart);
    }
    s.Done(self);
}
REGISTER_NATIVE_WRAPPER_AS(0x80727F60, BattleAI_InitTeamLists_80727f60, "BattleAI_InitTeamLists_80727f60");

// ObjectsMgr::__ct (0x8082A38C) gives its object lists fixed arrays of 200 pointers (new[] 0x320
// at +0x1C, +0x24, +0x2C, +0x34, +0x3C, +0x40, +0x48) and adds without a bound. Balloon battle has
// 10 balloon objects per player on top of the course's, so 50 players overflowed the list at
// +0x1C into the next heap block (--heap-check, heap group 4). More than 12 players: the arrays
// are replaced by ones of 200 + 12 per player (the small ones stay allocated, 5.6 KB, freed with
// the scene heap).
namespace {
constexpr uint32_t kObjListOffsets[] = {0x1C, 0x24, 0x2C, 0x34, 0x3C, 0x40, 0x48};
constexpr uint32_t kObjListBase = 200;
constexpr uint32_t kObjListPerPlayer = 12;
constexpr uint32_t kOperatorNewArray = 0x80229DF0;
}

extern "C" void ObjectsMgr_ct_8082a38c(CpuContext* ctx) {
    MKW_GUEST_NEXT(8082A38C)(ctx);
    const uint32_t racedata = Memory::Read32(kBalloonRacedata);
    const uint32_t players = racedata == 0 ? 0 : Memory::Read8(racedata + 0x24);
    if (players <= kBalloonInline) return;
    Saved s(ctx);
    const uint32_t mgr = s.r3;
    const uint32_t bytes = 4 * (kObjListBase + kObjListPerPlayer * players);
    for (uint32_t off : kObjListOffsets) {
        const uint32_t list = Guest(ctx, kOperatorNewArray, {bytes});
        if (list != 0) Memory::Write32(mgr + off, list);
    }
    s.Done(mgr);
}
REGISTER_NATIVE_WRAPPER_AS(0x8082A38C, ObjectsMgr_ct_8082a38c, "ObjectsMgr_ct_8082a38c");

// GMDataBattle::HandlePositionTracking (0x80538418): ranks the racers by battle score into u8/float
// arrays on its 0x90-byte stack frame, indexed by position - 1. 30+ players ran past them into the
// saved registers (crash in GMDataBalloonBattle::Update). Same algorithm natively: place by current
// position, stable insertion sort by score descending, write positions back to Raceinfo player +0x20
// and the rank table at Raceinfo +0x18.
namespace {
constexpr uint32_t kRaceinfoPlayers = 0xC;
constexpr uint32_t kRaceinfoRankTable = 0x18;
constexpr uint32_t kPlayerPosition = 0x20;
constexpr uint32_t kPlayerBattleScore = 0x22;
}

extern "C" void GMDataBattle_HandlePositionTracking_80538418(CpuContext* ctx) {
    const uint32_t racedata = Memory::Read32(kBalloonRacedata);
    const uint32_t count = racedata == 0 ? 0 : Memory::Read8(racedata + 0x24);
    if (count <= kBalloonInline) return MKW_GUEST_NEXT(80538418)(ctx);
    Saved s(ctx);
    const uint32_t raceinfo = Memory::Read32(s.r3 + 4);
    const uint32_t players = Memory::Read32(raceinfo + kRaceinfoPlayers);
    std::vector<uint8_t> idx(count, 0);
    std::vector<float> score(count, 0.0f);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t pl = Memory::Read32(players + 4 * i);
        const int32_t pos = int8_t(Memory::Read8(pl + kPlayerPosition)) - 1;
        if (pos < 0 || uint32_t(pos) >= count) continue;
        idx[pos] = uint8_t(i);
        score[pos] = float(Memory::Read16(pl + kPlayerBattleScore));
    }
    for (uint32_t k = 1; k < count; ++k) {
        const float f = score[k];
        const uint8_t b = idx[k];
        uint32_t j = k;
        for (; j > 0 && score[j - 1] < f; --j) {
            score[j] = score[j - 1];
            idx[j] = idx[j - 1];
        }
        score[j] = f;
        idx[j] = b;
    }
    for (uint32_t k = 0; k < count; ++k) {
        if (idx[k] >= count) continue;
        Memory::Write8(Memory::Read32(players + 4 * idx[k]) + kPlayerPosition, uint8_t(k + 1));
        Memory::Write8(Memory::Read32(raceinfo + kRaceinfoRankTable) + k, idx[k]);
    }
    s.Done(s.r3);
}
REGISTER_NATIVE_WRAPPER_AS(0x80538418, GMDataBattle_HandlePositionTracking_80538418,
                           "GMDataBattle_HandlePositionTracking_80538418");
