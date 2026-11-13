// mkw100: room for the race scene's objects in races of more than 12 players.
//
// GameScene keeps its objects (karts, controls, pages, G3D scene groups) in structsMem1 (+0xC88),
// an ExpHeap of 7 MB that ends at the top of MEM1, and the rest in structsMem2 (+0xC8C). Each
// racer takes about 85 KB of structsMem1 (free at race start, M5 Pro: 3.4 MB with 24 racers,
// 1.4 MB with 48), and with 78 it ran out (0x488 bytes left) while ScnMgr::CreateScnGroupEx
// built its groups, which then called through a null group. MEM1 cannot grow (the player slot
// overflow and the shadow windows sit above it).
//
// With more than 12 players an allocation that fails in the current scene's structsMem1 is made
// in its structsMem2 instead, and one that fails there in its structsDebug (+0xC90). structsDebug
// is carved from the debug root heap that RKSystem builds from MEM2 above 64 MB (the runtime
// reports 128 MB), about 59 MB that the race never uses. At 100 racers (course 0x01, diverse
// roster) structsMem2's 15 MB was not enough on its own. This is done at MEMAllocFromExpHeapEx,
// the call under both EGG::ExpHeap::alloc and the MEMAllocator that G3D allocates through.
//
// Every block made elsewhere is recorded with the heap it was asked from. A free or resize that
// names that heap (MEMFreeToAllocator with the scene's allocator) is sent to the heap that holds
// the block. When the game empties or destroys a heap (EGG::ExpHeap::FreeAll and destroy,
// MEMDestroyExpHeap), its blocks made elsewhere are freed first: otherwise each race reinit left
// structsMem1's overflow behind in structsMem2 until a later race ran out there.
//
// Past about 200 racers the archives no longer fit either (docs/mkw100/255-racers.md).
// GameScene::enter first creates two archive heaps (+0xC80 from the scene's MEM1 heap, +0xC84
// from its MEM2 heap, 46 MB, each all of it), loads the scene's archives into them, shrinks them
// to fit and only then creates the three struct heaps from what is left. Every racer's kart
// archive is about 200 KB of the MEM2 one: 212 fit, and with 200 racers the course archive (4.9 MB)
// did not load. What an archive heap, or structsDebug itself, cannot hold goes to an arena of
// kArenaSize instead, and so does what the chain above cannot place.
//
// The arena cannot live in a scene's debug heap: EGG::SceneManager::createScene gives every scene
// new heaps made of all the free space of its parent's (the root scene's, or for the root scene
// the system's), and destroys them with the scene, so nothing put there outlives the scene. It is
// taken once from the end of the system's debug heap, before the root scene's heaps take all of
// that, and never freed; every scene's debug heap is that much shorter and starts where it always
// did. Only the base game has it: Retro Rewind and Item Fever stop at 128 racers, where the
// archives fit, and keep the whole debug heap.
//
// A new scene object is built where the last one was, and its heap fields (+0xC80..+0xC90) still
// hold the old scene's heaps until GameScene::enter stores the new ones, after the archives are
// loaded. Destroyed heaps are therefore remembered until a heap is created at their address
// again: 255 racers allocated from the old structsDebug while loading the second Grand Prix race,
// and the new scene's own heap then handed the same memory out again (freed kart models).
//
// While GameScene::enter runs, the scene being entered is not yet the scene manager's current
// scene, so a race entered from the menus would still look up the menu scene's heaps. The scene
// being entered is used then.
//
// Up to 12 players nothing here changes the game's behaviour.

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"
#include "runtime_log.h"
#include "runtime_product.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr uint32_t kRaceHeapRacedataGlobal = 0x809BD728;  // RaceConfig::spInstance
constexpr uint32_t kRaceHeapPlayerCount = 0x24;           // u8, race scenario player count
constexpr uint32_t kRaceHeapInlinePlayers = 12;
constexpr uint32_t kRkSystem = 0x802A4080;                // EGG::System object
constexpr uint32_t kSceneManager = 0x54;
constexpr uint32_t kCurrentScene = 0xC;
constexpr uint32_t kSceneId = 0x28;
constexpr uint32_t kArchivesMem1 = 0xC80;  // GameScene, EGG::ExpHeap*: the scene's archives
constexpr uint32_t kArchivesMem2 = 0xC84;
constexpr uint32_t kStructsMem1 = 0xC88;   // GameScene, EGG::ExpHeap*
constexpr uint32_t kStructsMem2 = 0xC8C;
constexpr uint32_t kStructsDebug = 0xC90;
constexpr uint32_t kRvlHeap = 0x10;        // EGG::Heap: MEMiHeapHead*
constexpr uint32_t kHeapStart = 0x18;      // MEMiHeapHead
constexpr uint32_t kHeapEnd = 0x1C;

// EGG::SceneManager::createScene(this, id, parent): a scene's heaps come from its parent's
// (+0x14 MEM1, +0x18 MEM2, +0x1C debug), or without a parent from the heaps the system keeps for
// that (the object at r13 - 0x5CA0: +0x18, +0x1C, +0x20).
constexpr uint32_t kSceneParent = 0x20;
constexpr uint32_t kSceneHeapDebug = 0x1C;
constexpr uint32_t kRootHeapsSda = 0x5CA0;
constexpr uint32_t kRootHeapDebug = 0x20;
constexpr uint32_t kArenaSize = 40u << 20;
constexpr uint32_t kArenaFromEnd = 0xFFFFFFE0;     // alignment -32: MEMAllocFromExpHeapEx allocates from the end
constexpr uint32_t kCreateExpHeapEx = 0x80198CA8;  // MEMCreateExpHeapEx(start, size, flags)
// Zeroed blocks, as the struct heaps give (GameScene creates those with flag 1 and its objects
// count on it: unzeroed arena blocks left a G3D heap's objects with stale fields), and thread
// safe, as archives are loaded on another thread.
constexpr uint32_t kArenaFlags = 1 | 4;
constexpr uint32_t kPeakStep = 4u << 20;

struct Overflow {
    uint32_t asked;  // MEMiHeapHead* the game allocated from
    uint32_t held;   // MEMiHeapHead* that holds the block
    uint32_t size;
};

std::mutex g_lock;
std::unordered_map<uint32_t, Overflow> g_blocks;  // block -> heaps
std::unordered_set<uint32_t> g_destroyed;         // MEMiHeapHead* destroyed and not created again
uint32_t g_enteringScene = 0;                     // game thread only
uint32_t g_arena = 0;                             // MEMiHeapHead* of the arena, 0 if there is none
uint32_t g_arenaParent = 0;                       // MEMiHeapHead* of the debug heap it was taken from
std::map<uint32_t, uint32_t> g_arenaSpans;        // arena block -> its end
uint32_t g_arenaBytes = 0;                        // in use, and the next use worth a log line
uint32_t g_arenaLogAt = kPeakStep;

// GameScene::GetCurrent (0x8051BED0): the current scene unless its id is 0 (root) or 5.
uint32_t CurrentGameScene() {
    if (g_enteringScene != 0) {
        return g_enteringScene;
    }
    const uint32_t manager = Memory::Read32(kRkSystem + kSceneManager);
    const uint32_t scene = manager == 0 ? 0 : Memory::Read32(manager + kCurrentScene);
    if (scene == 0) {
        return 0;
    }
    const uint32_t id = Memory::Read32(scene + kSceneId);
    return id == 0 || id == 5 ? 0 : scene;
}

// The heap behind a scene's EGG::ExpHeap* field, or 0 when there is none or it was destroyed
// (the previous scene's, see the header).
uint32_t SceneRvlHeap(uint32_t scene, uint32_t field) {
    const uint32_t egg = Memory::Read32(scene + field);
    const uint32_t heap = egg == 0 ? 0 : Memory::Read32(egg + kRvlHeap);
    std::lock_guard<std::mutex> guard(g_lock);
    return g_destroyed.count(heap) != 0 ? 0 : heap;
}

bool Inside(uint32_t heap, uint32_t address) {
    return address >= Memory::Read32(heap + kHeapStart) && address < Memory::Read32(heap + kHeapEnd);
}

// g_lock held. Keeps the arena's blocks and use, and logs each new 4 MiB of it.
void Count(uint32_t address, const Overflow& block, bool added) {
    if (block.held != g_arena || g_arena == 0) {
        return;
    }
    if (added) {
        g_arenaSpans[address] = address + block.size;
        g_arenaBytes += block.size;
    } else {
        g_arenaSpans.erase(address);
        g_arenaBytes -= std::min(g_arenaBytes, block.size);
    }
    if (g_arenaBytes >= g_arenaLogAt) {
        g_arenaLogAt = (g_arenaBytes / kPeakStep + 1) * kPeakStep;
        RT_LOGF("race-heap", "arena: %u of %u MiB in use\n", g_arenaBytes >> 20, kArenaSize >> 20);
    }
}

// The heap that really holds `block`, for a call that named `heap`.
uint32_t OwnerHeap(uint32_t heap, uint32_t block) {
    // Whatever heap the call names: EGG::Heap::free without a heap finds one by the block's
    // address, which for the arena is the debug heap around it.
    if (g_arena != 0 && Inside(g_arena, block)) {
        return g_arena;
    }
    std::lock_guard<std::mutex> guard(g_lock);
    const auto it = g_blocks.find(block);
    if (it == g_blocks.end() || it->second.asked != heap || Inside(heap, block)) {
        return heap;
    }
    return it->second.held;
}

void Forget(uint32_t block) {
    std::lock_guard<std::mutex> guard(g_lock);
    const auto it = g_blocks.find(block);
    if (it != g_blocks.end()) {
        Count(it->first, it->second, false);
        g_blocks.erase(it);
    }
}

// The heap the game allocated the arena block around `address` from, or the heap around the arena.
uint32_t ArenaAskedHeap(uint32_t address) {
    std::lock_guard<std::mutex> guard(g_lock);
    auto span = g_arenaSpans.upper_bound(address);
    if (span != g_arenaSpans.begin() && address < (--span)->second) {
        const auto block = g_blocks.find(span->first);
        if (block != g_blocks.end()) {
            return block->second.asked;
        }
    }
    return g_arenaParent;
}

void Allocate(CpuContext* ctx, uint32_t heap, uint32_t size, uint32_t alignment) {
    ctx->gpr[3] = heap;
    ctx->gpr[4] = size;
    ctx->gpr[5] = alignment;
    MKW_GUEST_NEXT(80198D88)(ctx);
}

// Before `heap` is emptied or destroyed: free its blocks that live in other heaps, and forget the
// blocks that live in it (they go with it).
void ReleaseOverflow(CpuContext* ctx, uint32_t heap, bool destroyed) {
    std::vector<std::pair<uint32_t, uint32_t>> release;  // block, holding heap
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (destroyed) {
            g_destroyed.insert(heap);
        }
        if (g_blocks.empty()) {
            return;
        }
        for (auto it = g_blocks.begin(); it != g_blocks.end();) {
            if (it->second.asked == heap) {
                release.emplace_back(it->first, it->second.held);
                Count(it->first, it->second, false);
                it = g_blocks.erase(it);
            } else if (it->second.held == heap) {
                it = g_blocks.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (release.empty()) {
        return;
    }
    const CpuContext saved = *ctx;
    for (const auto& [block, held] : release) {
        ctx->gpr[3] = held;
        ctx->gpr[4] = block;
        MKW_GUEST_NEXT(80199038)(ctx);
    }
    *ctx = saved;
}

// The debug heap that outlives every scene, as seen from a createScene call for a scene under
// `parent`: the system's when the root scene itself is being created, else the root scene's.
uint32_t PermanentDebugHeap(const CpuContext* ctx, uint32_t parent) {
    uint32_t egg = 0;
    if (parent == 0) {
        const uint32_t heaps = Memory::Read32(ctx->gpr[13] - kRootHeapsSda);
        egg = heaps == 0 ? 0 : Memory::Read32(heaps + kRootHeapDebug);
    } else {
        uint32_t top = parent;
        for (uint32_t depth = 0; depth < 8 && Memory::Read32(top + kSceneParent) != 0; ++depth) {
            top = Memory::Read32(top + kSceneParent);
        }
        egg = Memory::Read32(top + kSceneHeapDebug);
    }
    return egg == 0 ? 0 : Memory::Read32(egg + kRvlHeap);
}

// Takes the arena (see the header). Tried again at the next scene if the heap has no room now.
void CreateArena(CpuContext* ctx, uint32_t parent) {
    const uint32_t debug = PermanentDebugHeap(ctx, parent);
    if (debug == 0) {
        return;
    }
    const CpuContext saved = *ctx;
    Allocate(ctx, debug, kArenaSize, kArenaFromEnd);
    const uint32_t start = ctx->gpr[3];
    if (start != 0) {
        ctx->gpr[3] = start;
        ctx->gpr[4] = kArenaSize;
        ctx->gpr[5] = kArenaFlags;
        InvokeIndirectCpu(kCreateExpHeapEx, ctx);
        g_arena = ctx->gpr[3];
        g_arenaParent = debug;
    }
    static bool logged = false;
    if (g_arena != 0 || !logged) {
        logged = true;
        RT_LOGF("race-heap", "arena: %u MiB of debug heap 0x%08X at 0x%08X%s\n", kArenaSize >> 20, debug, start,
                g_arena != 0 ? "" : " (no room; races past about 200 racers will not load)");
    }
    *ctx = saved;
}

} // namespace

// EGG::SceneManager::createScene(this, id, parent): before the first scene's heaps exist.
extern "C" void RaceHeap_CreateScene_8023b0e4(CpuContext* ctx) {
    if (g_arena == 0 && !RuntimeProduct::IsRetroRewind()) {
        CreateArena(ctx, ctx->gpr[5]);
    }
    MKW_GUEST_NEXT(8023B0E4)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8023B0E4, RaceHeap_CreateScene_8023b0e4, "RaceHeap_CreateScene_8023b0e4");

// GameScene::enter(scene): the scene being entered owns the heaps until it becomes current.
extern "C" void RaceHeap_GameSceneEnter_8051ab58(CpuContext* ctx) {
    const uint32_t previous = g_enteringScene;
    g_enteringScene = ctx->gpr[3];
    try {
        MKW_GUEST_NEXT(8051AB58)(ctx);
    } catch (...) {
        g_enteringScene = previous;
        throw;
    }
    g_enteringScene = previous;
}
REGISTER_NATIVE_WRAPPER_AS(0x8051AB58, RaceHeap_GameSceneEnter_8051ab58, "RaceHeap_GameSceneEnter_8051ab58");

// MEMAllocFromExpHeapEx(heap, size, alignment)
extern "C" void MEM_AllocFromExpHeapEx_80198d88(CpuContext* ctx) {
    const uint32_t heap = ctx->gpr[3];
    const uint32_t size = ctx->gpr[4];
    const uint32_t alignment = ctx->gpr[5];
    MKW_GUEST_NEXT(80198D88)(ctx);
    if (ctx->gpr[3] != 0) {
        return;
    }
    const uint32_t racedata = Memory::Read32(kRaceHeapRacedataGlobal);
    if (racedata == 0 || Memory::Read8(racedata + kRaceHeapPlayerCount) <= kRaceHeapInlinePlayers) {
        return;
    }
    const uint32_t scene = CurrentGameScene();
    if (scene == 0) {
        return;
    }
    const uint32_t mem1 = SceneRvlHeap(scene, kStructsMem1);
    const uint32_t mem2 = SceneRvlHeap(scene, kStructsMem2);
    const uint32_t debug = SceneRvlHeap(scene, kStructsDebug);
    uint32_t held = 0;
    const auto place = [&](uint32_t target) {
        if (held == 0 && target != 0 && target != heap) {
            Allocate(ctx, target, size, alignment);
            held = ctx->gpr[3] != 0 ? target : 0;
        }
    };
    if (heap != 0 && heap == mem1) {
        place(mem2);
        place(debug);
        place(g_arena);
    } else if (heap != 0 && heap == mem2) {
        place(debug);
        place(g_arena);
    } else if (heap != 0 && (heap == debug || heap == SceneRvlHeap(scene, kArchivesMem1) ||
                             heap == SceneRvlHeap(scene, kArchivesMem2))) {
        place(g_arena);
    } else {
        // Another heap: the game gets null. Logged (first 20), since it usually crashes later.
        static uint32_t logged = 0;
        if (logged < 20) {
            ++logged;
            RT_LOGF("race-heap", "allocation failed outside the scene heaps: size=%u heap=0x%08X lr=0x%08X\n", size,
                    heap, ctx->lr);
        }
        return;
    }
    if (held == 0) {
        ctx->gpr[3] = 0;
        RT_LOGF("race-heap", "out of memory: size=%u asked=0x%08X (mem1=0x%08X mem2=0x%08X debug=0x%08X arena=0x%08X)\n",
                size, heap, mem1, mem2, debug, g_arena);
        return;
    }
    const uint32_t block = ctx->gpr[3];
    std::lock_guard<std::mutex> guard(g_lock);
    const Overflow made{heap, held, size};
    g_blocks[block] = made;
    Count(block, made, true);
}
REGISTER_NATIVE_WRAPPER_AS(0x80198D88, MEM_AllocFromExpHeapEx_80198d88, "MEM_AllocFromExpHeapEx_80198d88");

// MEMFindContainHeap(address): the innermost heap around an address. EGG::Heap::free without a heap
// and EGG::Heap::findContainHeap look the result up among the game's heaps, where the arena is
// not: a block there would never be freed, and "the heap of this object" would be none. The game
// gets the heap it allocated the block from instead (whose free comes back here by address).
extern "C" void MEM_FindContainHeap_80198658(CpuContext* ctx) {
    const uint32_t address = ctx->gpr[3];
    MKW_GUEST_NEXT(80198658)(ctx);
    if (g_arena != 0 && ctx->gpr[3] == g_arena) {
        ctx->gpr[3] = ArenaAskedHeap(address);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x80198658, MEM_FindContainHeap_80198658, "MEM_FindContainHeap_80198658");

// MEMFreeToExpHeap(heap, block)
extern "C" void MEM_FreeToExpHeap_80199038(CpuContext* ctx) {
    const uint32_t block = ctx->gpr[4];
    ctx->gpr[3] = OwnerHeap(ctx->gpr[3], block);
    Forget(block);
    MKW_GUEST_NEXT(80199038)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80199038, MEM_FreeToExpHeap_80199038, "MEM_FreeToExpHeap_80199038");

// MEMResizeForMBlockExpHeap(heap, block, size)
extern "C" void MEM_ResizeForMBlockExpHeap_80198e38(CpuContext* ctx) {
    ctx->gpr[3] = OwnerHeap(ctx->gpr[3], ctx->gpr[4]);
    MKW_GUEST_NEXT(80198E38)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80198E38, MEM_ResizeForMBlockExpHeap_80198e38, "MEM_ResizeForMBlockExpHeap_80198e38");

// MEMCreateExpHeapEx(start, size, flags): a heap at a destroyed heap's address is a heap again.
extern "C" void MEM_CreateExpHeapEx_80198ca8(CpuContext* ctx) {
    MKW_GUEST_NEXT(80198CA8)(ctx);
    if (ctx->gpr[3] != 0) {
        std::lock_guard<std::mutex> guard(g_lock);
        g_destroyed.erase(ctx->gpr[3]);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x80198CA8, MEM_CreateExpHeapEx_80198ca8, "MEM_CreateExpHeapEx_80198ca8");

// MEMDestroyExpHeap(heap)
extern "C" void MEM_DestroyExpHeap_80198d58(CpuContext* ctx) {
    ReleaseOverflow(ctx, ctx->gpr[3], true);
    MKW_GUEST_NEXT(80198D58)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80198D58, MEM_DestroyExpHeap_80198d58, "MEM_DestroyExpHeap_80198d58");

// EGG::ExpHeap::FreeAll(this)
extern "C" void EGG_ExpHeap_FreeAll_80226ebc(CpuContext* ctx) {
    ReleaseOverflow(ctx, Memory::Read32(ctx->gpr[3] + kRvlHeap), false);
    MKW_GUEST_NEXT(80226EBC)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80226EBC, EGG_ExpHeap_FreeAll_80226ebc, "EGG_ExpHeap_FreeAll_80226ebc");

// EGG::ExpHeap::destroy(this)
extern "C" void EGG_ExpHeap_Destroy_80226b94(CpuContext* ctx) {
    ReleaseOverflow(ctx, Memory::Read32(ctx->gpr[3] + kRvlHeap), true);
    MKW_GUEST_NEXT(80226B94)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80226B94, EGG_ExpHeap_Destroy_80226b94, "EGG_ExpHeap_Destroy_80226b94");
