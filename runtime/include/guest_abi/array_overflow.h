#pragma once

// mkw100 per-player arrays inside singletons (docs/mkw100/DESIGN.md, singleton arrays).
//
// Managers such as AI::Manager hold inline `x[12]` arrays next to other fields, so the arrays
// cannot grow in place. Element i >= 12 of every array of a class lives instead in shadow window
// i - 12, addressed as if it were a copy of the object: element i of the array at object offset A
// sits at Window(i) + A. A window only has to hold element 0 of each array, so windows are
// `arraysEnd - arraysBegin` bytes apart and Window(i) itself points arraysBegin bytes before its
// block. The translator rewrites the instructions that index the arrays
// (projects/mkwii/mkw100/array-sites.txt, scripts/mkw100/array_sites.py):
//
//   scale  rD = index * stride, then rD = MKW100_IndexScaled(rD, class, stride): the product for
//          indices 0..11 (the N=12 race is unchanged), else Window(index) - instance, so the
//          game's instance + rD + A lands on Window(index) + A.
//   step   a pointer walk p += stride becomes p = MKW100_ArrayStep(p, class, stride, origin):
//          p + stride, except that the element after 11 is window 0 and the element after window
//          k is window k + 1 (origin: where p points relative to the instance at element 0).
//
// When the game creates the instance the windows are zeroed, as its constructor clears the inline
// arrays, and for an array of objects (elementCtor) each window's element is constructed. The
// class table mirrors projects/mkwii/mkw100/array-classes.tsv; ids are shared with the
// translator's site list and never reused.

#include <cstdint>
#include <iterator>

struct CpuContext;

namespace ArrayOverflow {

inline constexpr uint32_t kInlineSlots = 12;
inline constexpr uint32_t kMaxPlayers = 128;  // PlayerSlots::kMaxPlayers
inline constexpr uint32_t kWindows = kMaxPlayers - kInlineSlots;
// Above the player slot overflow (0x81A00000..0x82000000), below MEM2.
inline constexpr uint32_t kShadowBase = 0x82000000;
inline constexpr uint32_t kClassSpan = 0x100000;
inline constexpr uint32_t kMaxClasses = 16;
inline constexpr uint32_t kShadowSize = kClassSpan * kMaxClasses;
// The last class span is not a class's: it holds fixed-address tables the runtime gives the game in
// place of tables sized for 12 players that live as long as the game (effect_groups.cpp).
inline constexpr uint32_t kNativeTables = kShadowBase + (kMaxClasses - 1) * kClassSpan;
// Its second half: runtime strings handed to the menu text lookup (vs_racers_option.cpp).
inline constexpr uint32_t kMenuText = kNativeTables + kClassSpan / 2;

inline constexpr uint32_t kNoArray = 0xFFFFFFFF;
// ArrayClass::member for a static object: instanceGlobal is the object itself, not a pointer to it.
inline constexpr int32_t kStaticObject = -2;

struct ArrayClass {
    uint32_t id;
    const char* name;
    uint32_t instanceGlobal;  // guest address of the singleton pointer
    int32_t member;           // >= 0: the object is the pointer at *instanceGlobal + member;
                              // kStaticObject: the object is at instanceGlobal
    uint32_t arraysBegin;     // object offset of the first array
    uint32_t arraysEnd;       // end of element 0 of the last array
    uint32_t elementCtor;     // nonzero: guest ctor (this only) of the elements of the arrays at ctorArrays
    uint32_t ctorArrays[2];   // object offsets; kNoArray when unused

    constexpr uint32_t window() const { return (arraysEnd - arraysBegin + 3) & ~3u; }
};

inline constexpr ArrayClass kClasses[] = {
    // AI::Manager (mkw-sp Enemy::EnemyManager): mpCpuPlayers[12] at 0x24, mpRealPlayers[12] at 0x54.
    {1, "ai_manager", 0x809C2BE8, -1, 0x24, 0x58, 0, {kNoArray, kNoArray}},
    // Objects AI::Manager's ctor (0x80738ED4) allocates; arrays from the access trace.
    {2, "ai_rank", 0x809C2BE8, 0x84, 0xE8, 0xF0, 0, {kNoArray, kNoArray}},  // AIRankManager: {AI*, u32}[12] at 0xE8
    {3, "ai_sub88", 0x809C2BE8, 0x88, 0x4, 0x8, 0, {kNoArray, kNoArray}},   // [12] at 0x4, count at 0x34
    {4, "ai_sub90", 0x809C2BE8, 0x90, 0x4, 0x8, 0, {kNoArray, kNoArray}},   // [12] at 0x4
    // Input::Manager: AIControllerHolder[12] (0x180 each) at 0x3B4, the CPU racers' input.
    {5, "input_manager", 0x809BD70C, -1, 0x3B4, 0x534, 0x805234A0, {0x3B4, kNoArray}},
    // Item::Manager: Item::Player* [12] at 0x18 (online partners; its ctor 0x807992D8 zeroes one
    // per race player after building the item ObjHolders at 0x48, so 24 players wiped holders 0
    // and 1 and ObjHolder::Spawn spun forever). The ctor's sites are rewritten too; the runtime
    // publishes the instance before it runs (array_overflow.cpp ConstructorHook).
    {6, "item_manager", 0x809C3618, -1, 0x18, 0x1C, 0, {kNoArray, kNoArray}},
    // Item::GessoMgr (Blooper, 0x54 bytes): u32 ink[12] per player at 0x10, the local screens at
    // 0x44. ApplyBlooper (0x807A972C) fills it for every item player (24: over the screens).
    {7, "gesso_manager", 0x809C3648, -1, 0x10, 0x14, 0, {kNoArray, kNoArray}},
    // Race::DriverMgr (0x1F0 bytes, layout as Pulsar's DriverManager.hpp): DriverController*
    // models[12] at 0x10, Vec3 positions[12] at 0x40, Vec3* targetingItemPosition[12] at 0xD0,
    // frame counters [12] at 0x100 and 0x130, FinishType [12] at 0x178. Item code indexes models
    // by the item's owner (24 players: a float of positions[] used as a model pointer).
    // u8 racePositions[12] at 0x161 is stride 1 and stays inline (driver_manager.cpp).
    {8, "driver_manager", 0x809C2F38, -1, 0x10, 0x17C, 0, {kNoArray, kNoArray}},
    // ArchiveMgr (0x61C bytes, Pulsar's Archive/ArchiveMgr.hpp): ArchivesHolder kartModelsHolders[12]
    // at 0x8 and kartModelsHolders2[12] at 0x158 (0x1C each), one per race player. Players 12..23
    // landed in the second array; player 24 in contentArchives at 0x2A8 (25 players: a kart
    // archive path formatted from "HomeButton.arc"'s bytes). The ctor builds both arrays with
    // 0x8053FF14 (ArchivesHolder(1)), which allocates on the current heap.
    {9, "archive_manager", 0x809BD738, -1, 0x8, 0x174, 0x8053FF14, {0x8, 0x158}},
    // Retro Rewind's Code.pul: a static per-racer state table at 0x8189FC60, 0x38 bytes per racer
    // (a heap pointer, u16 fields at 0x1C..0x2E), indexed by Kart::Link::GetPlayerIdx at
    // 0x81837BC8, 0x81837DE8, 0x81838350 and 0x818383E4. Sized for 12: racers 12+ overwrote the
    // module's other data, and racer 25's record a Pulsar hook list node (26 racers jumped to 0).
    // Not in array-classes.tsv: array_sites.py only handles singletons; its sites are hand-listed.
    {10, "rr_racer_state", 0x8189FC60, kStaticObject, 0x0, 0x38, 0, {kNoArray, kNoArray}},
    // Balloon battle manager (0x4F8 bytes, created by 0x808697BC): {u8 held, Balloon* held[5]}[12]
    // at 0x3C4 (stride 0x18), one per player. More than 12 players: balloon_battle.cpp runs its
    // methods natively; the game's other readers are rewritten sites.
    {11, "balloon_manager", 0x809C4748, -1, 0x3C4, 0x3DC, 0, {kNoArray, kNoArray}},
};

constexpr bool ValidTable() {
    for (uint32_t i = 0; i < std::size(kClasses); ++i) {
        const ArrayClass& c = kClasses[i];
        if (c.id != i + 1 || c.arraysEnd <= c.arraysBegin || (kWindows + 1) * c.window() > kClassSpan) {
            return false;
        }
    }
    return std::size(kClasses) < kMaxClasses;  // the last span is kNativeTables
}
static_assert(ValidTable(), "class ids must be 1..n in order, and the windows must fit the span");

// First guest address of a class's window blocks.
constexpr uint32_t ClassBase(uint32_t id) { return kShadowBase + (id - 1) * kClassSpan; }

// The object-relative origin of the window holding element `index` (>= 12) of every array.
constexpr uint32_t Window(const ArrayClass& c, uint32_t index) {
    return ClassBase(c.id) + (index - kInlineSlots) * c.window() - c.arraysBegin;
}

const ArrayClass& Class(uint32_t id);

// Guest address of the class's object now (0 when it does not exist).
uint32_t Instance(const ArrayClass& c);

// The index of the element `pointer` points at in the class's array at object offset `origin`
// (inline or in a window), or kNoArray. For mod code that turns an element address back into a
// player index with (pointer - instance - origin) / stride, which breaks past 12.
uint32_t ElementIndex(const ArrayClass& c, uint32_t pointer, uint32_t origin, uint32_t stride);

// Zeroes every window of the class and constructs its object elements. Guest calls clobber the
// context's registers; the caller restores them.
void InitWindows(const ArrayClass& c, CpuContext* ctx);

} // namespace ArrayOverflow

// Called by translated code at each rewritten site; see above.
extern "C" uint32_t MKW100_IndexScaled(uint32_t scaled, uint32_t classId, uint32_t stride);
extern "C" uint32_t MKW100_ArrayStep(uint32_t pointer, uint32_t classId, uint32_t stride, uint32_t origin);
