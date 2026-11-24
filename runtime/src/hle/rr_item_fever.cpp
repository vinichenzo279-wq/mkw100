// mkw100: Retro Rewind with Item Fever's items (experimental, docs/mkw100/rr-item-fever.md).
//
// The combined Kamek module (scripts/mkw100/combine_pul.py) carries both packs' code; this file
// holds the run-time side of that build: a test hook that hands CPUs a chosen item, and the audio
// heaps for Item Fever's larger boot sound heap.

#include "hle/rr_item_fever.h"

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "hle/mod_build.h"
#include "memory.h"
#include "runtime_log.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace {

constexpr uint32_t kItemManager = 0x809C3618;   // Item::Manager::sInstance (PAL)
constexpr uint32_t kItemPlayerSize = 0x248;      // Item::Player
constexpr uint32_t kInventory = 0x88;            // Item::Player::inventory
constexpr uint32_t kSetItem = 0x807BC940;        // Item::PlayerInventory::SetItem(id, forced)
constexpr uint32_t kItemNone = 0x14;

long GiveItemFromEnv() {
    const char* text = std::getenv("MKW100_TEST_GIVE_ITEM");
    if (!text || !*text) return -1;
    return std::strtol(text, nullptr, 0);
}

} // namespace

namespace RrItemFever {

// MKW100_TEST_GIVE_ITEM=<item id>: from race frame 600, every 300 frames, every CPU holding nothing
// gets that item through the game's own (patched) Item::PlayerInventory::SetItem, so the item's
// whole path runs: inventory, CPU decision to use it, its object, hits. Logs how many of the
// previous round's items were used.
void TestGiveItem(CpuContext* ctx, uint32_t raceinfo) {
    static const long item = GiveItemFromEnv();
    // Plain Item Fever has its own (item_fever.cpp); here only the combined build.
    if (item < 0 || ModBuild::Current() != ModBuild::Kind::RetroRewindItemFever) return;
    if (Memory::Read32(raceinfo + 0x28) != 2) return;  // racing
    const uint32_t frame = Memory::Read32(raceinfo + 0x20);
    if (frame < 600 || frame % 300 != 0) return;
    const uint32_t manager = Memory::Read32(kItemManager);
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
        const uint32_t player = players + i * kItemPlayerSize;
        if (Memory::Read8(player + 0x19)) continue;  // human
        const uint32_t inventory = player + kInventory;
        const uint32_t held = Memory::Read32(inventory + 4);
        if (held == uint32_t(item)) { ++stillHeld; continue; }
        if (held != kItemNone) { ++other; continue; }
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
        InvokeIndirectCpu(kSetItem, ctx);
        *ctx = saved;
        given += Memory::Read32(inventory + 4) == uint32_t(item);
    }
    RT_LOGF("rr-item-fever", "frame %u: gave item 0x%lX to %u CPUs; of last round's %u, %u still held, "
            "%u CPUs hold something else, %u skipped (no room)\n", frame, item, given, lastGiven, stillHeld, other, full);
    lastGiven = given + stillHeld;
}

} // namespace RrItemFever

// Item Fever's sound archive needs a 10 MiB boot sound heap (the plan sets Audio::Manager::InitSelf
// to lis r3, 0xA0 as Item Fever does; the game has 6 MiB), or its sound group 14 does not load and
// the race's sounds do not start (silent countdown). With Retro Rewind loaded too, the heap the game
// passes has no 10 MiB left: EGG::SoundHeapMgr::CreateSoundHeap gets nothing, creates no sound heap,
// and the boot stops in SoundArchivePlayer::SetupMram. The system debug heap is no help either (the
// root scene's heaps took all of it by then). In the combined build the audio heaps come from the
// root scene's debug heap when the passed heap cannot hold the sound heap: it lives as long as the
// game and holds tens of MB the game barely uses (the DSP reads all of MEM2).
namespace {

constexpr uint32_t kRkSystem = 0x802A4080;       // EGG::System object (race_heap.cpp)
constexpr uint32_t kSceneManager = 0x54;
constexpr uint32_t kCurrentScene = 0xC;
constexpr uint32_t kSceneParent = 0x20;
constexpr uint32_t kSceneHeapDebug = 0x1C;       // EGG::Scene: debug heap (EGG::Heap*)
constexpr uint32_t kSoundHeapSize = 0xA00000;    // what the plan sets (rr-item-fever.txt write32)
constexpr uint32_t kSoundHeapMargin = 0x100000;  // room for the rest of the audio setup

uint32_t AllocatableSize(CpuContext* ctx, uint32_t heap) {
    const CpuContext saved = *ctx;
    ctx->gpr[3] = heap;
    ctx->gpr[4] = 0x20;
    InvokeIndirectCpu(Memory::Read32(Memory::Read32(heap) + 0x24), ctx);  // EGG::Heap::getAllocatableSize
    const uint32_t size = ctx->gpr[3];
    *ctx = saved;
    return size;
}

} // namespace

extern "C" void RrItemFever_AudioManagerInitSelf_8071724c(CpuContext* ctx) {
    if (ModBuild::Current() == ModBuild::Kind::RetroRewindItemFever) {
        const uint32_t passed = ctx->gpr[4];
        const uint32_t need = kSoundHeapSize + kSoundHeapMargin;
        const uint32_t passedFree = passed ? AllocatableSize(ctx, passed) : 0;
        if (passedFree < need) {
            const uint32_t manager = Memory::Read32(kRkSystem + kSceneManager);
            const uint32_t scene = manager ? Memory::Read32(manager + kCurrentScene) : 0;
            const bool root = scene && Memory::Read32(scene + kSceneParent) == 0;
            const uint32_t debug = root ? Memory::Read32(scene + kSceneHeapDebug) : 0;
            const uint32_t debugFree = debug ? AllocatableSize(ctx, debug) : 0;
            RT_LOGF("rr-item-fever", "audio heaps: passed heap 0x%08X has 0x%X free; scene 0x%08X (root %d) debug heap 0x%08X has 0x%X\n",
                    passed, passedFree, scene, root ? 1 : 0, debug, debugFree);
            if (debugFree >= need) {
                ctx->gpr[4] = debug;
                RT_LOGF("rr-item-fever", "audio heaps from the root scene's debug heap\n");
            }
        }
    }
    MKW_GUEST_NEXT(8071724C)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8071724C, RrItemFever_AudioManagerInitSelf_8071724c, "RrItemFever_AudioManagerInitSelf_8071724c");
