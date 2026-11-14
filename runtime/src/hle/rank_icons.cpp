// mkw100: position icons for 13th..255th on the race HUD's position counter (CtrlRaceRankNum).
//
// The game's counter only has pictures for 1st..12th. rank_icons/rank013.tpl .. rank255.tpl next
// to the executable (runtime/assets/rank_icons: 13-24 by hammerbro101, 25-255 generated; see the
// README there) are read once, and each race copies the ones it needs into the race scene's
// structsDebug heap (MEM2) and binds them as TPLBind does, then hands them to the counter's own
// texture pattern animation (see Show). Textures must sit in MEM1/MEM2: the GX layer rejects
// texture data outside them (a fixed runtime span gave "GXLoadTexObj rejected data-out-of-range").
// When the files are missing the HUD overlay's vector digits are used instead.

#include "rank_icons.h"

#include "abi_bridge.h"
#include "guest_abi/player_slots.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <vector>

namespace {

constexpr uint32_t kFirstPlace = 13;
constexpr uint32_t kLastPlace = PlayerSlots::kMaxPlayers;
constexpr uint32_t kMaxTplBytes = 0x4100;  // a 64x64 RGBA8 TPL is 0x4040 bytes
constexpr uint32_t kIconRkSystem = 0x802A4080;
constexpr uint32_t kIconSceneManager = 0x54;    // EGG::SceneManager*: current scene at +0xC
constexpr uint32_t kIconStructsDebug = 0xC90;   // GameScene: EGG::ExpHeap* (MEM2; see race_heap.cpp)
constexpr uint32_t kIconRvlHeap = 0x10;
constexpr uint32_t kIconAllocFromExpHeap = 0x80198D88;  // MEMAllocFromExpHeapEx(heap, size, align)

std::array<int8_t, kLastPlace + 1> g_file{};          // 0 unread, 1 read, -1 missing
std::array<std::vector<uint8_t>, kLastPlace + 1> g_bytes;
std::array<uint32_t, kLastPlace + 1> g_guest{};       // this race's bound copy, 0 if none
bool g_anyMissingLogged = false;

bool ReadFile(uint32_t place) {
    if (g_file[place] != 0) return g_file[place] > 0;
    g_file[place] = -1;
    const auto dir = RuntimeConfigFile::ExecutableDirectory();
    if (!dir) return false;
    char name[32];
    std::snprintf(name, sizeof name, "rank%03u.tpl", place);
    std::ifstream file(*dir / "rank_icons" / name, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const bool tpl = bytes.size() >= 0x40 && bytes.size() <= kMaxTplBytes && bytes[0] == 0x00 && bytes[1] == 0x20 &&
                     bytes[2] == 0xAF && bytes[3] == 0x30 && bytes[7] == 1;
    if (!tpl) {
        if (!g_anyMissingLogged) {
            g_anyMissingLogged = true;
            RT_LOGF("rank-icons", "rank_icons/%s missing or invalid (%zu bytes); using the overlay counter\n", name,
                    bytes.size());
        }
        return false;
    }
    g_bytes[place] = std::move(bytes);
    g_file[place] = 1;
    return true;
}

// This race's bound copy of `place`'s TPL in structsDebug, allocated on first use.
uint32_t GuestCopy(CpuContext* ctx, uint32_t place) {
    if (g_guest[place] != 0) return g_guest[place];
    const uint32_t manager = Memory::Read32(kIconRkSystem + kIconSceneManager);
    const uint32_t scene = manager ? Memory::Read32(manager + 0xC) : 0;
    const uint32_t debug = scene ? Memory::Read32(scene + kIconStructsDebug) : 0;
    const uint32_t heap = debug ? Memory::Read32(debug + kIconRvlHeap) : 0;
    if (heap < 0x80000000u) return 0;
    const auto& bytes = g_bytes[place];
    const CpuContext entry = *ctx;
    ctx->gpr[3] = heap;
    ctx->gpr[4] = uint32_t(bytes.size());
    ctx->gpr[5] = 32;
    InvokeIndirectCpu(kIconAllocFromExpHeap, ctx);
    const uint32_t tpl = ctx->gpr[3];
    *ctx = entry;
    if (tpl < 0x80000000u) return 0;
    for (uint32_t i = 0; i < bytes.size(); ++i) Memory::Write8(tpl + i, bytes[i]);
    // Bind as TPLBind does: the game's texture tables hold bound TPLs, whose offsets are pointers.
    // Header: version, image count, descriptor table offset; descriptor: image header, palette
    // header; image header +0x8 data, +0x23 "unpacked".
    const uint32_t descriptors = tpl + Memory::Read32(tpl + 8);
    Memory::Write32(tpl + 8, descriptors);
    const uint32_t header = tpl + Memory::Read32(descriptors);
    Memory::Write32(descriptors, header);
    if (Memory::Read32(descriptors + 4) != 0) Memory::Write32(descriptors + 4, tpl + Memory::Read32(descriptors + 4));
    Memory::Write32(header + 8, tpl + Memory::Read32(header + 8));
    Memory::Write8(header + 0x23, 1);
    g_guest[place] = tpl;
    return tpl;
}

} // namespace

namespace RankIcons {

bool Available(uint32_t place) {
    return place >= kFirstPlace && place <= kLastPlace && ReadFile(place);
}

namespace {
// CtrlRaceRankNum's three pictures (position_sha, position, position_l_00 in Race_E.szs
// game_image_position.brlyt) are switched by a texture pattern, animation group 0 (eRank), whose
// AnimTransformBasic keeps the 12 textures it applies in a table (+0x14). Past 12th the game
// already holds that animation on its last frame (12th); its table entry 11 is pointed at the
// place's icon instead, so the game's own animation draws it, and put back for 1st..12th.
// (Replacing the panes' textures directly lost to the animation, which re-applies the table.)
constexpr uint32_t kAnimator = 0x98;      // UIAnimator: AnimationGroup* groups (0x44 each)
constexpr uint32_t kAnimations = 0x0;     // AnimationGroup: Animation* (0x10 each)
constexpr uint32_t kTransform = 0x0;      // Animation: AnimTransformBasic*
constexpr uint32_t kTextureTable = 0x14;  // AnimTransformBasic: TPL* per pattern texture
constexpr uint32_t kLastStock = 11;       // the 12th place's entry

// One per local player's counter (split screen: up to 4): its table entry and original TPL.
struct IconCounter { uint32_t control = 0, entry = 0, stock = 0; };
std::array<IconCounter, 4> g_counters{};
uint32_t g_race = 0;      // the Raceinfo instance the copies belong to

IconCounter& CounterFor(uint32_t control) {
    for (auto& c : g_counters) if (c.control == control) return c;
    for (auto& c : g_counters) if (c.control == 0) { c.control = control; return c; }
    g_counters[0] = IconCounter{control};
    return g_counters[0];
}

bool RankInRam(uint32_t a) { return a >= 0x80000000u && a < 0x98000000u; }

uint32_t TableEntry(uint32_t control) {
    const uint32_t groups = Memory::Read32(control + kAnimator);
    if (!RankInRam(groups)) return 0;
    const uint32_t animations = Memory::Read32(groups + kAnimations);
    if (!RankInRam(animations)) return 0;
    const uint32_t transform = Memory::Read32(animations + kTransform);
    if (!RankInRam(transform)) return 0;
    const uint32_t table = Memory::Read32(transform + kTextureTable);
    return RankInRam(table) ? table + kLastStock * 4 : 0;
}
} // namespace

bool Show(CpuContext* ctx, uint32_t control, uint32_t place) {
    // A new race (a new Raceinfo): its scene heaps hold none of our copies, and its counters are
    // new. Split screen has a counter per player, which share the copies (one set per race).
    const uint32_t race = Memory::Read32(0x809BD730);
    if (race != g_race) {
        g_race = race;
        g_counters.fill(IconCounter{});
        g_guest.fill(0);
    }
    IconCounter& counter = CounterFor(control);
    if (place < kFirstPlace) {
        if (counter.entry) Memory::Write32(counter.entry, counter.stock);
        counter.entry = 0;
        return true;
    }
    if (!Available(place)) return false;
    const uint32_t entry = TableEntry(control);
    if (!entry) return false;
    if (!counter.entry) {
        counter.entry = entry;
        counter.stock = Memory::Read32(entry);
    }
    const uint32_t tpl = GuestCopy(ctx, place);
    if (!tpl) return false;
    Memory::Write32(entry, tpl);
    return true;
}

} // namespace RankIcons
