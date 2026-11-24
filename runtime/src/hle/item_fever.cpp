// mkw100: Item Fever (Pulsar pack) past 12 racers.
//
// Item Fever keeps some per-racer state in static [12] tables in its own module. The unchecked
// accesses are listed as `index` sites in projects/mkwii/mkw100/array-sites.txt (profile
// item-fever): racers 12+ use a shadow table instead. This file resets those shadows when Item
// Fever resets its own tables, and applies the item reserve Item Rain patches into game code.
// Retro Rewind has no function at 0x81816EE8; the Item::Manager wrappers change nothing unpatched.
//
// Retro Rewind with Item Fever's items (docs/mkw100/rr-item-fever.md) carries the same Item Fever
// code ModBuild::kItemFeverCombinedOffset higher; the same hooks are registered there too, and the
// tables move with them (ModBuild::ItemFeverOffset).

#include "hle/mod_build.h"
#include "hle/item_fever.h"
#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"
#include "hle/race_heap.h"
#include "local_vs.h"
#include "recomp_mod_loader.h"
#include "runtime_log.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kFeverFlagsShadow = 0x82D00000;  // u8 flags[12] at 0x818A5B08, racers 12..254
constexpr uint32_t kFeverTimerShadow = 0x82D20000;  // float[12] at 0x818A5B14
constexpr uint32_t kFeverStateShadow = 0x82D20600;  // u8[12] at 0x818A5B44
constexpr uint32_t kFeverShadowRacers = 255 - 12;

// A [12] table and its shadow (racers 12..254; array-sites.txt, profile item-fever).
struct FeverTable { uint32_t table, stride, shadow; };

// Item Fever's resets of these tables run a fixed 12 times and write one value per table; the
// shadow elements get the value element 0 was just given.
void CopyResetToShadow(const FeverTable& t) {
    const uint32_t table = t.table + ModBuild::ItemFeverOffset();
    for (uint32_t i = 0; i < kFeverShadowRacers; ++i) {
        for (uint32_t b = 0; b < t.stride; ++b) {
            Memory::Write8(t.shadow + i * t.stride + b, Memory::Read8(table + b));
        }
    }
}

// The struct at 0x818BB9BC (one item's per-racer state, its functions 0x81849494..0x8184B644).
// Reset by 0x81849494; the rest of it (0x818BBAAC [12] x 64 bytes, ...) by 0x818496F0's loop,
// whose bound is a cmpimm site, so it fills the shadows itself.
constexpr FeverTable kResetTables89F0[] = {
    {0x818BB9BC, 2, 0x82D23000},
    {0x818BB9D4, 4, 0x82D23300},
    {0x818BBA04, 4, 0x82D23900},
    {0x818BBA34, 1, 0x82D23F00},
    {0x818BBA40, 1, 0x82D24200},
    {0x818BBA4C, 1, 0x82D24500},
    {0x818BBA5C, 4, 0x82D24800},
    {0x818BBA8C, 1, 0x82D24E00},
};
// The struct at 0x818BC318 (the per-kart routine Item Fever hooks into Kart::Sub::Update,
// 0x8184D5AC), reset by 0x8184E210.
constexpr FeverTable kResetTables90E0[] = {
    {0x818BC318, 1, 0x82D2DE00},
    {0x818BC324, 1, 0x82D2E100},
    {0x818BC330, 1, 0x82D2E400},
    {0x818BC33C, 1, 0x82D2E700},
    {0x818BC348, 1, 0x82D2EA00},
    {0x818BC354, 1, 0x82D2ED00},
    {0x818BC360, 2, 0x82D2F000},
    {0x818BC378, 4, 0x82D2F300},
    {0x818BC3A8, 4, 0x82D2F900},
    {0x818BC3D8, 1, 0x82D2FF00},
    {0x818BC3E4, 1, 0x82D30200},
};

void LogFeverUnlocks(CpuContext* ctx);  // below, with Item Fever's unlocks

} // namespace

// Item Fever's race-load hook (node 0x818A5B54): clears its float[12] at 0x818A5B14 and u8[12] at
// 0x818A5B44 (a fixed count of 12, no compare to raise). Their shadows and the flags' are cleared
// with them, so a racer 12+ starts each race like racers 0..11.
static void ResetRacerTables(CpuContext* ctx, bool combinedSite) {
    // Each address holds Item Fever's function in one build only; elsewhere it is another pack's code.
    if (combinedSite ? ModBuild::Current() != ModBuild::Kind::RetroRewindItemFever : !ModBuild::IsKnownItemFever()) return;
    for (uint32_t i = 0; i < kFeverShadowRacers; ++i) {
        Memory::Write8(kFeverFlagsShadow + i, 0);
    }
    CopyResetToShadow({0x818A5B14, 4, kFeverTimerShadow});
    CopyResetToShadow({0x818A5B44, 1, kFeverStateShadow});
    LogFeverUnlocks(ctx);
}
static void Reset89F0(bool combinedSite) {
    if (combinedSite ? ModBuild::Current() != ModBuild::Kind::RetroRewindItemFever : !ModBuild::IsKnownItemFever()) return;
    for (const FeverTable& t : kResetTables89F0) CopyResetToShadow(t);
}
static void Reset90E0(bool combinedSite) {
    if (combinedSite ? ModBuild::Current() != ModBuild::Kind::RetroRewindItemFever : !ModBuild::IsKnownItemFever()) return;
    for (const FeverTable& t : kResetTables90E0) CopyResetToShadow(t);
}

extern "C" void ItemFever_ResetRacerTables_81816ee8(CpuContext* ctx) {
    MKW_GUEST_NEXT(81816EE8)(ctx);
    ResetRacerTables(ctx, false);
}
REGISTER_NATIVE_WRAPPER_AS(0x81816EE8, ItemFever_ResetRacerTables_81816ee8, "ItemFever_ResetRacerTables_81816ee8");

extern "C" void ItemFever_Reset89F0_81849494(CpuContext* ctx) {
    MKW_GUEST_NEXT(81849494)(ctx);
    Reset89F0(false);
}
REGISTER_NATIVE_WRAPPER_AS(0x81849494, ItemFever_Reset89F0_81849494, "ItemFever_Reset89F0_81849494");

extern "C" void ItemFever_Reset90E0_8184e210(CpuContext* ctx) {
    MKW_GUEST_NEXT(8184E210)(ctx);
    Reset90E0(false);
}
REGISTER_NATIVE_WRAPPER_AS(0x8184E210, ItemFever_Reset90E0_8184e210, "ItemFever_Reset90E0_8184e210");

// Boo Mushroom past 12 racers. Its per-frame pass (module +0x58414) runs a fixed min(racers, 12)
// times: that bound also limits the column of its [12][12] pass-through table (+0xC0278), so it
// stays at 12 (pack_racer_tables.py KEEP_AT_12). The pass also runs each racer's two countdowns:
// the ghost (flag +0xC0234, frames left +0xC0250) and the boost of a Boo Mushroom used during a
// spin (+0xC0268). A racer 12+ starts a ghost like the others (+0x5821C, its tables shadowed),
// but nothing ended it: invisible for the rest of the race. The countdowns run here for racers
// 12+, on the shadows, with Item Fever's own calls, and so does the pass-through (stealing) of
// every pair with a racer 12+ (BooPassPast12). Its steal (+0x58074) takes a held item from racers
// up to 63 (pack_racer_tables.py: its tables hold 64).
static uint32_t ShadowOfRacerTable(uint32_t table);  // below, from item_fever_racer_tables.inc
namespace {
// The Boo Mushroom's [12] tables (module +0xC0234 ghost flag u8, +0xC0240 no-steal flag u8, +0xC0250
// ghost frames s16, +0xC0268 post-spin countdown u8, +0xC0278 pass-through counters u16[12] per racer).
struct BooShadows { uint32_t ghost, block, frames, spin, pass; };
BooShadows Shadows() {
    return {ShadowOfRacerTable(0x818C0234), ShadowOfRacerTable(0x818C0240), ShadowOfRacerTable(0x818C0250),
            ShadowOfRacerTable(0x818C0268), ShadowOfRacerTable(0x818C0278)};
}

uint32_t BooCall(CpuContext* ctx, uint32_t function, uint32_t r3, uint32_t r4 = 0) {
    const CpuContext saved = *ctx;
    ctx->gpr[3] = r3;
    ctx->gpr[4] = r4;
    InvokeIndirectCpu(function, ctx);
    const uint32_t result = ctx->gpr[3];
    *ctx = saved;
    return result;
}

bool BooSite(bool combinedSite) {
    return combinedSite ? ModBuild::Current() == ModBuild::Kind::RetroRewindItemFever : ModBuild::IsKnownItemFever() &&
                                                                                           ModBuild::ItemFeverOffset() == 0;
}

// Pass-through counters for the pairs Item Fever's [12][12] table has no room for (a racer 12+ on
// either side): frames before that Boo user can go through that racer again.
std::vector<uint16_t> g_booPairs;

float GuestFloat(uint32_t address) {
    const uint32_t bits = Memory::Read32(address);
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

// Item Fever's pass-through (0x585d4..0x586f0) for every pair with a racer 12+: a Boo user whose
// ghost is on goes through a racer within 140 units (squared distance from module +0x9B65C), not on
// its team (+0x35CDC), at most once every 60 frames; that racer gets the Boo's effects and the
// user what +0x58074 takes from it.
void BooPassPast12(CpuContext* ctx, uint32_t code, uint32_t manager, uint32_t count, const BooShadows& sh) {
    const uint32_t tables = 0x818C0000 + ModBuild::ItemFeverOffset();
    if (g_booPairs.size() != 255u * 255u) g_booPairs.assign(255u * 255u, 0);
    for (uint32_t u = 0; u < count; ++u) {
        for (uint32_t j = 0; j < count; ++j) {
            uint16_t& left = g_booPairs[u * 255u + j];
            if (left) --left;
        }
    }
    if (count <= 12) return;
    const float reach = GuestFloat(code + 0x9B65C);
    for (uint32_t u = 0; u < count; ++u) {
        const bool high = u >= 12;
        const uint32_t ghost = high ? sh.ghost + (u - 12) : tables + 0x234 + u;
        const uint32_t block = high ? sh.block + (u - 12) : tables + 0x240 + u;
        if (!Memory::Read8(ghost) || Memory::Read8(block)) continue;
        if ((BooCall(ctx, code + 0x34AD0, u) & 0xFF) || (BooCall(ctx, code + 0x22808, u) & 0xFF)) continue;
        const uint32_t user = BooCall(ctx, 0x80590100, manager, u);  // Kart::Manager::GetKartPlayer
        if (!user) continue;
        const uint32_t at = BooCall(ctx, 0x8059020C, user);          // Kart::Link::GetKartPosition
        const float ux = GuestFloat(at), uy = GuestFloat(at + 4), uz = GuestFloat(at + 8);
        for (uint32_t j = 0; j < count; ++j) {
            if (j == u || (!high && j < 12)) continue;  // racers 0..11 with each other: Item Fever's own pass
            uint16_t& left = g_booPairs[u * 255u + j];
            if (left) continue;
            if (BooCall(ctx, code + 0x35CDC, u, j) & 0xFF) continue;
            const uint32_t other = BooCall(ctx, 0x80590100, manager, j);
            if (!other) continue;
            const uint32_t to = BooCall(ctx, 0x8059020C, other);
            const float dx = GuestFloat(to) - ux, dy = GuestFloat(to + 4) - uy, dz = GuestFloat(to + 8) - uz;
            if (dx * dx + dy * dy + dz * dz > reach) continue;
            left = 60;
            BooCall(ctx, code + 0x5E3D0, j);
            BooCall(ctx, code + 0x5E550, j);
            BooCall(ctx, code + 0x5E514, j);
            const uint32_t taken = BooCall(ctx, code + 0x58074, u, j);
            BooCall(ctx, code + 0x5E67C, u, taken);
            RT_LOGF("item-fever", "Boo: p%u goes through p%u -> %s\n", u, j, taken ? "STEAL" : "nothing taken");
        }
    }
}

void BooPast12(CpuContext* ctx, bool combinedSite) {
    if (!BooSite(combinedSite)) return;
    const uint32_t code = 0x81800000 + ModBuild::ItemFeverOffset();
    const uint32_t manager = Memory::Read32(0x809C18F8);  // Kart::Manager
    if (!manager) return;
    const uint32_t count = Memory::Read8(manager + 0x24);
    const BooShadows sh = Shadows();
    if (!sh.ghost || !sh.frames || !sh.spin) return;
    for (uint32_t p = 12; p < count; ++p) {
        const uint32_t i = p - 12;
        // A Boo Mushroom used during a spin: its boost until the spin ends, then the ghost (0x584C4).
        if (const uint8_t spin = Memory::Read8(sh.spin + i)) {
            const uint32_t player = BooCall(ctx, 0x80590100, manager, p);  // Kart::Manager::GetKartPlayer
            const uint32_t movement = player ? Memory::Read32(player + 0x44) : 0;
            if (!movement) {
                Memory::Write8(sh.spin + i, 0);
            } else {
                BooCall(ctx, 0x8057F3D8, movement);  // Kart::Movement::ActivateMushroom
                if (BooCall(ctx, code + 0x58188, movement) & 0xFF) {
                    Memory::Write8(sh.spin + i, 0);
                    BooCall(ctx, code + 0x5821C, p);
                } else {
                    Memory::Write8(sh.spin + i, uint8_t(spin - 1));
                    if (spin == 1) BooCall(ctx, code + 0x5821C, p);
                }
            }
        }
        // The ghost (0x58548): its frames count down, then Item Fever ends it.
        if (!Memory::Read8(sh.ghost + i)) continue;
        const uint32_t player = BooCall(ctx, 0x80590100, manager, p);
        if (!player || !Memory::Read32(player + 0x20)) {
            Memory::Write8(sh.ghost + i, 0);
            continue;
        }
        BooCall(ctx, code + 0x5E58C, p);
        int16_t frames = int16_t(Memory::Read16(sh.frames + 2 * i));
        if (frames > 0) Memory::Write16(sh.frames + 2 * i, uint16_t(--frames));
        if (frames > 0) continue;
        BooCall(ctx, code + 0x5E3EC, p, 0);
        BooCall(ctx, code + 0x5E3D0, p);
        BooCall(ctx, code + 0x5E6D0, p);
        Memory::Write8(sh.ghost + i, 0);
        RT_LOGF("item-fever", "Boo: ghost of racer %u over\n", p);
    }
    if (sh.block && sh.pass) BooPassPast12(ctx, code, manager, count, sh);
}

// Its race reset (+0x581AC, on Pulsar's race-load list) clears racers 0..11; the shadows with it.
void BooReset(bool combinedSite) {
    if (!BooSite(combinedSite)) return;
    const BooShadows sh = Shadows();
    if (!sh.ghost || !sh.block || !sh.frames || !sh.spin || !sh.pass) return;
    for (uint32_t i = 0; i < kFeverShadowRacers; ++i) {
        Memory::Write8(sh.ghost + i, 0);
        Memory::Write8(sh.block + i, 0);
        Memory::Write16(sh.frames + 2 * i, 0);
        Memory::Write8(sh.spin + i, 0);
        for (uint32_t b = 0; b < 0x18; ++b) Memory::Write8(sh.pass + i * 0x18 + b, 0);
    }
    g_booPairs.assign(255u * 255u, 0);
}
} // namespace

extern "C" void ItemFever_BooPass_81858414(CpuContext* ctx) {
    MKW_GUEST_NEXT(81858414)(ctx);
    BooPast12(ctx, false);
}
REGISTER_NATIVE_WRAPPER_AS(0x81858414, ItemFever_BooPass_81858414, "ItemFever_BooPass_81858414");

extern "C" void ItemFever_BooReset_818581ac(CpuContext* ctx) {
    MKW_GUEST_NEXT(818581AC)(ctx);
    BooReset(false);
}
REGISTER_NATIVE_WRAPPER_AS(0x818581AC, ItemFever_BooReset_818581ac, "ItemFever_BooReset_818581ac");

extern "C" void RrItemFever_BooPass_81918414(CpuContext* ctx) {
    MKW_GUEST_NEXT(81918414)(ctx);
    BooPast12(ctx, true);
}
REGISTER_NATIVE_WRAPPER_AS(0x81918414, RrItemFever_BooPass_81918414, "RrItemFever_BooPass_81918414");

extern "C" void RrItemFever_BooReset_819181ac(CpuContext* ctx) {
    MKW_GUEST_NEXT(819181AC)(ctx);
    BooReset(true);
}
REGISTER_NATIVE_WRAPPER_AS(0x819181AC, RrItemFever_BooReset_819181ac, "RrItemFever_BooReset_819181ac");

// The freeze (Ice Flower, Penguin Suit) past 12 racers. Its per-frame pass (module +0x3F8E4: thaw
// countdown, ice block) runs its racer loop up to 24 racers (array-sites.txt; tables 0xB5664 and
// 0xB7244 hold 24, and the freeze itself stops there too). Its last two loops, over the 6 ice
// blocks, look at racers 0..11 only: the first hides a block no racer 0..11 holds, the second
// clears the slot of racers 0..11 no longer frozen. For racers 12..23 they are completed here:
// a frozen racer's block is shown again, a thawed racer's slot cleared.
namespace {
void FreezePast12(CpuContext* ctx, bool combinedSite) {
    if (!BooSite(combinedSite)) return;
    const uint32_t frozen = ShadowOfRacerTable(0x818B90E8);  // u8 [12] at +0xB90E8 (0xB3F04 + 0x51E4)
    const uint32_t slots = ShadowOfRacerTable(0x818A2E64);   // u8 [12] at +0xA2E64
    const uint32_t manager = Memory::Read32(0x809C18F8);
    if (!frozen || !slots || !manager) return;
    const uint32_t code = 0x81800000 + ModBuild::ItemFeverOffset();
    const uint32_t blocks = 0x818B9EBC + ModBuild::ItemFeverOffset();  // u32 [6] at +0xB9EBC (0xB3F04 + 0x5FB8)
    const uint32_t count = std::min<uint32_t>(Memory::Read8(manager + 0x24), 24);
    static uint8_t was[12] = {};
    for (uint32_t p = 12; p < count; ++p) {
        const uint32_t i = p - 12;
        const uint8_t slot = Memory::Read8(slots + i);
        const uint8_t now = Memory::Read8(frozen + i) ? 1 : 0;
        if (now != was[i]) RT_LOGF("item-fever", "freeze: racer %u %s (ice block %u)\n", p, now ? "frozen" : "thawed", slot);
        was[i] = now;
        if (!now) {
            if (slot != 0xFF) Memory::Write8(slots + i, 0xFF);
        } else if (slot < 6 && Memory::Read32(blocks + 4 * slot)) {
            BooCall(ctx, code + 0x3D3CC, slot, 1);
        }
    }
}
} // namespace

// Item Fever gives a newly frozen racer an ice block (+0x3DB60, one of 6) none of racers 0..11
// holds; a racer 12+ holding one is not seen, so two frozen racers could share it. After it, a
// racer 12+ given a block someone else holds moves to a free one (its pass places it next frame).
namespace {
void FreezeOwnBlock(bool combinedSite, uint32_t victim) {
    if (!BooSite(combinedSite) || victim < 12 || victim >= 24) return;
    const uint32_t frozen = ShadowOfRacerTable(0x818B90E8);
    const uint32_t slots = ShadowOfRacerTable(0x818A2E64);
    const uint32_t manager = Memory::Read32(0x809C18F8);
    if (!frozen || !slots || !manager) return;
    const uint32_t offset = ModBuild::ItemFeverOffset();
    const uint32_t blocks = 0x818B9EBC + offset;
    const uint32_t count = std::min<uint32_t>(Memory::Read8(manager + 0x24), 24);
    auto slotOf = [&](uint32_t p) {
        return p < 12 ? Memory::Read8(0x818A2E64 + offset + p) : Memory::Read8(slots + p - 12);
    };
    auto frozenNow = [&](uint32_t p) {
        return p < 12 ? Memory::Read8(0x818B90E8 + offset + p) != 0 : Memory::Read8(frozen + p - 12) != 0;
    };
    const uint8_t mine = slotOf(victim);
    if (mine >= 6) return;
    bool shared = false;
    bool used[6] = {};
    for (uint32_t p = 0; p < count; ++p) {
        if (p == victim || !frozenNow(p)) continue;
        const uint8_t s = slotOf(p);
        if (s < 6) used[s] = true;
        shared |= s == mine;
    }
    if (!shared) return;
    for (uint8_t s = 0; s < 6; ++s) {
        if (!used[s] && s != mine && Memory::Read32(blocks + 4 * s)) {
            Memory::Write8(slots + victim - 12, s);
            return;
        }
    }
}
} // namespace

extern "C" void ItemFever_FreezeBlock_8183db60(CpuContext* ctx) {
    const uint32_t victim = ctx->gpr[4];
    MKW_GUEST_NEXT(8183DB60)(ctx);
    FreezeOwnBlock(false, victim);
}
REGISTER_NATIVE_WRAPPER_AS(0x8183DB60, ItemFever_FreezeBlock_8183db60, "ItemFever_FreezeBlock_8183db60");

extern "C" void RrItemFever_FreezeBlock_818fdb60(CpuContext* ctx) {
    const uint32_t victim = ctx->gpr[4];
    MKW_GUEST_NEXT(818FDB60)(ctx);
    FreezeOwnBlock(true, victim);
}
REGISTER_NATIVE_WRAPPER_AS(0x818FDB60, RrItemFever_FreezeBlock_818fdb60, "RrItemFever_FreezeBlock_818fdb60");

extern "C" void ItemFever_FreezePass_8183f8e4(CpuContext* ctx) {
    MKW_GUEST_NEXT(8183F8E4)(ctx);
    FreezePast12(ctx, false);
}
REGISTER_NATIVE_WRAPPER_AS(0x8183F8E4, ItemFever_FreezePass_8183f8e4, "ItemFever_FreezePass_8183f8e4");

extern "C" void RrItemFever_FreezePass_818ff8e4(CpuContext* ctx) {
    MKW_GUEST_NEXT(818FF8E4)(ctx);
    FreezePast12(ctx, true);
}
REGISTER_NATIVE_WRAPPER_AS(0x818FF8E4, RrItemFever_FreezePass_818ff8e4, "RrItemFever_FreezePass_818ff8e4");

// Item Fever's unlocks, available from the start while "All characters, vehicles and cups
// available" (local_vs unlock_all, on by default) is on:
//   200cc: Item Fever opens its 200cc class (picked like mirror) for a license once all 8 cups are
//       gold on Very Hard. It keeps that as bit <license> of a flags word in its save (ifever.dat:
//       *(0x818A5AFC) +0x40 -> buffer, buffer + *(buffer + 0x14) + 0x30; bit 4+n: "200cc unlocked"
//       shown, 12+n: Meteor Storm shown, 16+n: dark theme shown) and several menu functions test
//       that bit inline, so the bit is set: for every license, whenever Item Fever asks for the
//       current license (0x8180A4D4), which every one of those tests does first. Item Fever saves
//       the buffer, so the bit then stays in ifever.dat.
//   Meteor Storm: the secret reward for a bronze or better in all 8 cups of a license's 200cc row,
//       tested by 0x8180B878(license) and nowhere else; it answers yes. Item Fever then shows its
//       one-time "unlocked" notice once, as if the trophies had just been won.
//   Dark theme: the reward for all 8 of those cups gold, tested by 0x8180B99C(license); it answers
//       yes too, with the same one-time notice.
// MKW100_TEST_IF_UNLOCKS (tests only) applies them in scripted runs, which otherwise keep the save
// as it is, and logs at race load what Item Fever's own tests answer.
namespace {

constexpr uint32_t kFeverSave = 0x818A5AFC;
constexpr uint32_t kFeverUnlockFlags = 0x30;
constexpr uint32_t kFever200ccBits = 0xF;  // bit n: license n

bool FeverUnlocksOn() {
    if (!ModBuild::IsKnownItemFever()) return false;
    static const bool test = std::getenv("MKW100_TEST_IF_UNLOCKS") != nullptr;
    return test || LocalVs::UnlocksEnabled();
}

// The flags word of Item Fever's loaded save, or 0 before the save is loaded.
uint32_t FeverUnlockFlagsAddress() {
    const uint32_t save = Memory::Read32(kFeverSave);
    if (save < 0x80000000u || !Memory::Contains(save + 0x40, 4)) return 0;
    const uint32_t buffer = Memory::Read32(save + 0x40);
    if (buffer < 0x80000000u || !Memory::Contains(buffer + 0x14, 4)) return 0;
    const uint32_t flags = buffer + Memory::Read32(buffer + 0x14) + kFeverUnlockFlags;
    return Memory::Contains(flags, 4) ? flags : 0;
}

} // namespace

// Item Fever: the current license, 0..3 (SaveManager +0x36).
extern "C" void ItemFever_CurrentLicense_8180a4d4(CpuContext* ctx) {
    MKW_GUEST_NEXT(8180A4D4)(ctx);
    if (!FeverUnlocksOn()) return;
    const uint32_t flags = FeverUnlockFlagsAddress();
    if (flags == 0) return;
    const uint32_t value = Memory::Read32(flags);
    if ((value & kFever200ccBits) == kFever200ccBits) return;
    Memory::Write32(flags, value | kFever200ccBits);
    RT_LOGF("item-fever", "200cc unlocked for every license (flags %08X -> %08X)\n", value, value | kFever200ccBits);
}
REGISTER_NATIVE_WRAPPER_AS(0x8180A4D4, ItemFever_CurrentLicense_8180a4d4, "ItemFever_CurrentLicense_8180a4d4");

// Item Fever: Meteor Storm earned by `license` (r3), 1 or 0.
extern "C" void ItemFever_MeteorStormEarned_8180b878(CpuContext* ctx) {
    if (!FeverUnlocksOn()) { MKW_GUEST_NEXT(8180B878)(ctx); return; }
    static bool logged = false;
    if (!logged) { logged = true; RT_LOGF("item-fever", "Meteor Storm unlocked\n"); }
    ctx->gpr[3] = 1;
}
REGISTER_NATIVE_WRAPPER_AS(0x8180B878, ItemFever_MeteorStormEarned_8180b878, "ItemFever_MeteorStormEarned_8180b878");

// Item Fever: dark theme earned by `license` (r3), 1 or 0.
extern "C" void ItemFever_DarkThemeEarned_8180b99c(CpuContext* ctx) {
    if (!FeverUnlocksOn()) { MKW_GUEST_NEXT(8180B99C)(ctx); return; }
    static bool logged = false;
    if (!logged) { logged = true; RT_LOGF("item-fever", "dark theme unlocked\n"); }
    ctx->gpr[3] = 1;
}
REGISTER_NATIVE_WRAPPER_AS(0x8180B99C, ItemFever_DarkThemeEarned_8180b99c, "ItemFever_DarkThemeEarned_8180b99c");

namespace {
// MKW100_TEST_IF_UNLOCKS: Item Fever's own answers, logged at race load (no menus run headless).
//   0x8180A500(): the GP classes offered, 3 or 4 with 200cc;
//   0x8180B8F4() / 0x8180BA14(): Meteor Storm / dark theme earned by the current license.
void LogFeverUnlocks(CpuContext* ctx) {
    static const bool test = std::getenv("MKW100_TEST_IF_UNLOCKS") != nullptr;
    if (!test || !ModBuild::IsKnownItemFever()) return;
    const CpuContext saved = *ctx;
    uint32_t answer[3];
    const uint32_t calls[3] = {0x8180A500, 0x8180B8F4, 0x8180BA14};
    for (int i = 0; i < 3; ++i) {
        InvokeIndirectCpu(calls[i], ctx);
        answer[i] = ctx->gpr[3];
        *ctx = saved;
    }
    const uint32_t flags = FeverUnlockFlagsAddress();
    RT_LOGF("item-fever", "test: classes %u, Meteor Storm %u, dark theme %u, flags %08X\n", answer[0], answer[1],
            answer[2], flags ? Memory::Read32(flags) : 0u);
}
} // namespace

// Item Fever checks EGG::ExpHeap::getTotalFreeSize on the race scene's heaps before it loads a
// model, about a dozen times, and skips the model when the heap reports too little: Item Rain's
// and the dropped items' models (MKWCI-DROP: about 900 KB of structsMem1, and 700 KB of
// structsMem2 per extra copy), its projectile model sets (MKWCI-BANK "sets loaded 0", then
// "SUBSTITUTION: family N spawned green"), the penguin suit. Past 12 racers the karts fill those
// heaps, and race_heap.cpp places what does not fit in structsMem2 and structsDebug instead, but
// the checks only saw the heaps' last bytes. 80 racers: "scene space exhausted (768 bytes): drop
// models stopped at type 0", the rain's items lay on the track invisible; 100 racers (user's play
// test, 2026-11-16): Pull Star, Shell Bomb, Purple Shell and the giant shell came out as green
// shells, the Chain Chomp, Snowball and wand did nothing, the penguin suit was invisible, and a ?
// block crashed on an item object without a model. For calls from Item Fever's module the answer
// now counts what the fallback still has behind the heap (RaceHeap_FallbackFree, which asks this
// function itself: hence the guard). In Retro Rewind with Item Fever's items the fallback is on at
// 12 racers too (race_heap.cpp OverflowActive), and Item Fever's code sits 0xC0000 higher, still
// inside the module range.
extern "C" void ItemFever_ExpHeapTotalFree_80226c88(CpuContext* ctx) {
    static thread_local bool inside = false;
    const uint32_t egg = ctx->gpr[3];
    const uint32_t lr = ctx->lr;
    MKW_GUEST_NEXT(80226C88)(ctx);
    if (inside || !ModBuild::HasKnownItemFeverCode() || lr < 0x81800000u || lr >= 0x81A00000u) return;
    inside = true;
    const uint32_t own = ctx->gpr[3];
    uint32_t extra = 0;
    try {
        extra = RaceHeap_FallbackFree(ctx, egg);
    } catch (...) {
        inside = false;
        throw;
    }
    inside = false;
    ctx->gpr[3] = uint32_t(std::min<uint64_t>(uint64_t(own) + extra, 0x7FFFFFFFu));
}
REGISTER_NATIVE_WRAPPER_AS(0x80226C88, ItemFever_ExpHeapTotalFree_80226c88, "ItemFever_ExpHeapTotalFree_80226c88");

// The same three functions in Retro Rewind + Item Fever items (+0xC0000).
extern "C" void RrItemFever_ResetRacerTables_818d6ee8(CpuContext* ctx) {
    MKW_GUEST_NEXT(818D6EE8)(ctx);
    ResetRacerTables(ctx, true);
}
REGISTER_NATIVE_WRAPPER_AS(0x818D6EE8, RrItemFever_ResetRacerTables_818d6ee8, "RrItemFever_ResetRacerTables_818d6ee8");

extern "C" void RrItemFever_Reset89F0_81909494(CpuContext* ctx) {
    MKW_GUEST_NEXT(81909494)(ctx);
    Reset89F0(true);
}
REGISTER_NATIVE_WRAPPER_AS(0x81909494, RrItemFever_Reset89F0_81909494, "RrItemFever_Reset89F0_81909494");

extern "C" void RrItemFever_Reset90E0_8190e210(CpuContext* ctx) {
    MKW_GUEST_NEXT(8190E210)(ctx);
    Reset90E0(true);
}
REGISTER_NATIVE_WRAPPER_AS(0x8190E210, RrItemFever_Reset90E0_8190e210, "RrItemFever_Reset90E0_8190e210");

// Item::Manager +0x42C is the sum of the item pools' capacities minus 30, set by `addi r0, rN, -30`
// at 0x8079992C (Update) and 0x80799AB0 (UpdateTotalItemCountMinus30). Item Fever rewrites both
// immediates when a race loads: -100 with Item Rain on, -30 off (0x818413C4). Translated code keeps
// -30, so the writes are let through to memory and the wrappers apply the immediate found there.
// Unpatched (the base game, Retro Rewind) the word holds -30 and nothing changes.
namespace {

constexpr uint32_t kItemManagerReserve = 0x42C;
constexpr int32_t kStockReserveImmediate = -30;
constexpr uint32_t kReserveUnset = 0xFEEDF00Du;  // marks "the call did not write the field"

struct AllowReserveWords {
    AllowReserveWords() {
        RecompMod::AllowExecutableWord(0x8079992C, "Item::Manager::Update reserve immediate");
        RecompMod::AllowExecutableWord(0x80799AB0, "Item::Manager::UpdateTotalItemCountMinus30 immediate");
    }
} gAllowReserveWords;

void ApplyReserveImmediate(CpuContext* ctx, uint32_t word, ::GuestFunctions::Entry next) {
    const uint32_t manager = ctx->gpr[3];
    const uint32_t field = manager + kItemManagerReserve;
    const uint32_t before = Memory::Read32(field);
    Memory::Write32(field, kReserveUnset);
    next(ctx);
    const uint32_t written = Memory::Read32(field);
    if (written == kReserveUnset) {
        Memory::Write32(field, before);
        return;
    }
    const int32_t immediate = static_cast<int16_t>(Memory::Read32(word) & 0xFFFF);
    Memory::Write32(field, written + static_cast<uint32_t>(immediate - kStockReserveImmediate));
}

} // namespace

extern "C" void ItemManager_Update_80799850(CpuContext* ctx) {
    ApplyReserveImmediate(ctx, 0x8079992C, MKW_GUEST_NEXT(80799850));
}
REGISTER_NATIVE_WRAPPER_AS(0x80799850, ItemManager_Update_80799850, "ItemManager_Update_80799850");

extern "C" void ItemManager_UpdateTotalItemCountMinus30_80799a50(CpuContext* ctx) {
    ApplyReserveImmediate(ctx, 0x80799AB0, MKW_GUEST_NEXT(80799A50));
}
REGISTER_NATIVE_WRAPPER_AS(0x80799A50, ItemManager_UpdateTotalItemCountMinus30_80799a50,
                           "ItemManager_UpdateTotalItemCountMinus30_80799a50");

// Item Fever's per-racer tables found by scripts/mkw100/pack_racer_tables.py: every indexed access
// to one is an index/indexb/indexadd site (array-sites.txt, generated block), so racers 12..254
// use its shadow, and the `racer < 12` compares in front of them compare with 255. Item Fever sets
// these tables up for 12 racers (a constructor or reset loop with a fixed count of 12, or a
// pointer stepping through them, which must stay at 12). When a race starts, a shadow gets the
// value all 12 elements were given if they agree (a reset value), else zeroes.
namespace {

struct RacerTable { uint32_t table, size, shadow; };
constexpr RacerTable kRacerTables[] = {
#include "item_fever_racer_tables.inc"
};
constexpr uint32_t kShadowRacers = 255 - 12;
} // namespace

// The shadow of one of Item Fever's [12] tables (its plain Item Fever address), 0 if it has none.
static uint32_t ShadowOfRacerTable(uint32_t table) {
    for (const RacerTable& t : kRacerTables) {
        if (t.table == table) return t.shadow;
    }
    return 0;
}
namespace {

// In Retro Rewind with Item Fever's items the tables sit ModBuild::ItemFeverOffset() higher (the
// shadows stay where they are).
void ReadyRacerTables() {
    const uint32_t offset = ModBuild::ItemFeverOffset();
    for (const RacerTable& rt : kRacerTables) {
        const RacerTable t{rt.table + offset, rt.size, rt.shadow};
        bool uniform = true;
        for (uint32_t i = 1; i < 12 && uniform; ++i) {
            for (uint32_t b = 0; b < t.size; ++b) {
                if (Memory::Read8(t.table + i * t.size + b) != Memory::Read8(t.table + b)) { uniform = false; break; }
            }
        }
        for (uint32_t i = 0; i < kShadowRacers; ++i) {
            for (uint32_t b = 0; b < t.size; ++b) {
                Memory::Write8(t.shadow + i * t.size + b, uniform ? Memory::Read8(t.table + b) : 0);
            }
        }
    }
    // The freeze's ice-block slot per racer (0xFF none): 0xFF at boot only for element 0.
    if (const uint32_t slots = ShadowOfRacerTable(0x818A2E64)) {
        for (uint32_t i = 0; i < kShadowRacers; ++i) Memory::Write8(slots + i, 0xFF);
    }
}

// MKW100_TEST_RACER_TABLES: every 600 race frames, which tables racers 12+ have written to (a
// shadow element that differs from element 0's value at race start).
std::vector<uint8_t> g_startValues;
void LogRacerTables(uint32_t frame) {
    static const bool test = std::getenv("MKW100_TEST_RACER_TABLES") != nullptr;
    if (!test) return;
    if (frame == 0 || g_startValues.empty()) {
        g_startValues.clear();
        for (const RacerTable& t : kRacerTables)
            for (uint32_t b = 0; b < t.size; ++b) g_startValues.push_back(Memory::Read8(t.shadow + b));
        return;
    }
    if (frame % 600 != 0) return;
    uint32_t used = 0, offset = 0;
    std::string list;
    for (const RacerTable& t : kRacerTables) {
        uint32_t racers = 0;
        for (uint32_t i = 0; i < kShadowRacers; ++i) {
            for (uint32_t b = 0; b < t.size; ++b) {
                if (Memory::Read8(t.shadow + i * t.size + b) != g_startValues[offset + b]) { ++racers; break; }
            }
        }
        offset += t.size;
        if (racers) {
            ++used;
            if (list.size() < 900) {
                char item[32];
                std::snprintf(item, sizeof item, " %08X:%u", t.table, racers);
                list += item;
            }
        }
    }
    RT_LOGF("item-fever", "frame %u: racers 12+ wrote %u of %zu tables:%s\n", frame, used,
            std::size(kRacerTables), list.c_str());
}

// MKW100_TEST_GIVE_ITEM=<item id>: from race frame 600, every 300 frames, every CPU holding nothing
// gets that item through the game's own (patched) Item::PlayerInventory::SetItem, so the item's
// whole path runs. Logs how many of the previous round's items were used.
void TestGiveItem(CpuContext* ctx, uint32_t raceinfo) {
    static const long item = [] {
        const char* text = std::getenv("MKW100_TEST_GIVE_ITEM");
        return text && *text ? std::strtol(text, nullptr, 0) : -1L;
    }();
    if (item < 0) return;
    if (Memory::Read32(raceinfo + 0x28) != 2) return;  // racing
    const uint32_t frame = Memory::Read32(raceinfo + 0x20);
    if (frame < 600 || frame % 300 != 0) return;
    const uint32_t manager = Memory::Read32(0x809C3618);  // Item::Manager::sInstance
    if (!manager) return;
    const uint32_t count = Memory::Read8(manager + 0x10);
    const uint32_t players = Memory::Read32(manager + 0x14);
    static uint32_t lastGiven = 0;
    uint32_t stillHeld = 0, given = 0, other = 0, full = 0;
    // MKW100_TEST_GIVE_ITEM_EVERY=<n>: only racers 0, n, 2n, ... get it (the others keep their box items).
    static const uint32_t every = [] {
        const char* text = std::getenv("MKW100_TEST_GIVE_ITEM_EVERY");
        return text && *text ? uint32_t(std::max(1L, std::strtol(text, nullptr, 0))) : 1u;
    }();
    for (uint32_t i = 0; i < count; i += every) {
        const uint32_t player = players + i * 0x248;     // Item::Player
        if (Memory::Read8(player + 0x19)) continue;      // human
        const uint32_t inventory = player + 0x88;
        const uint32_t held = Memory::Read32(inventory + 4);
        if (held == uint32_t(item)) { ++stillHeld; continue; }
        if (held != 0x14) { ++other; continue; }         // 0x14: no item
        // Only while the item's objects have room (Item::Manager::IsThereCapacityForItem, as the
        // roulette asks): past it the item is held without its object (Black Bob-omb at 24 racers:
        // a dragged null object, crash in Item::PlayerInventory::Update).
        {
            const CpuContext before = *ctx;
            ctx->gpr[3] = uint32_t(item);
            InvokeIndirectCpu(0x80799BE8, ctx);
            const bool room = (ctx->gpr[3] & 0xFF) != 0;
            *ctx = before;
            if (!room) { ++full; continue; }
        }
        const CpuContext saved = *ctx;
        ctx->gpr[3] = inventory;
        ctx->gpr[4] = uint32_t(item);
        ctx->gpr[5] = 0;
        InvokeIndirectCpu(0x807BC940, ctx);              // Item::PlayerInventory::SetItem
        *ctx = saved;
        given += Memory::Read32(inventory + 4) == uint32_t(item);
    }
    RT_LOGF("item-fever", "frame %u: gave item 0x%lX to %u CPUs; of last round's %u, %u still held, "
            "%u CPUs hold something else, %u skipped (no room)\n", frame, item, given, lastGiven, stillHeld, other, full);
    lastGiven = given + stillHeld;
}

} // namespace

void ItemFever::RaceFrame(CpuContext* ctx, uint32_t raceinfo) {
    if (!ModBuild::HasKnownItemFeverCode()) return;
    static uint32_t lastRaceinfo = 0, lastFrame = 0;
    const uint32_t frame = Memory::Read32(raceinfo + 0x20);
    if (raceinfo != lastRaceinfo || frame < lastFrame) {
        ReadyRacerTables();
        LogRacerTables(0);
    }
    lastRaceinfo = raceinfo;
    lastFrame = frame;
    LogRacerTables(frame);
    if (ModBuild::IsKnownItemFever()) TestGiveItem(ctx, raceinfo);  // the combined build: rr_item_fever.cpp
    // MKW100_TEST_FREEZE=<racer>[,<racer>...]: at race frame 900, racer 0 freezes those racers through
    // Item Fever's own freeze (module +0x3E0C8: owner r4, victim r5), as an Ice Flower hit does.
    static const std::vector<uint32_t> freezeVictims = [] {
        std::vector<uint32_t> out;
        if (const char* text = std::getenv("MKW100_TEST_FREEZE")) {
            for (char* end = nullptr; *text; text = *end ? end + 1 : end) {
                out.push_back(uint32_t(std::strtoul(text, &end, 0)));
                if (end == text) break;
            }
        }
        return out;
    }();
    if (!freezeVictims.empty() && frame == 900 && Memory::Read32(raceinfo + 0x28) == 2) {
        const uint32_t code = 0x81800000 + ModBuild::ItemFeverOffset();
        for (const uint32_t victim : freezeVictims) {
            const CpuContext saved = *ctx;
            ctx->gpr[3] = 0;
            ctx->gpr[4] = 0;
            ctx->gpr[5] = victim;
            InvokeIndirectCpu(code + 0x3E0C8, ctx);
            *ctx = saved;
            RT_LOGF("item-fever", "test: racer 0 froze racer %u\n", victim);
        }
    }
}
