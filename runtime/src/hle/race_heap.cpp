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
// While GameScene::enter runs, the scene being entered is not yet the scene manager's current
// scene, so a race entered from the menus would still look up the menu scene's heaps. The scene
// being entered is used then.
//
// Up to 12 players nothing here changes the game's behaviour.

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"
#include "runtime_log.h"

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint32_t kRaceHeapRacedataGlobal = 0x809BD728;  // RaceConfig::spInstance
constexpr uint32_t kRaceHeapPlayerCount = 0x24;           // u8, race scenario player count
constexpr uint32_t kRaceHeapInlinePlayers = 12;
constexpr uint32_t kRkSystem = 0x802A4080;                // EGG::System object
constexpr uint32_t kSceneManager = 0x54;
constexpr uint32_t kCurrentScene = 0xC;
constexpr uint32_t kSceneId = 0x28;
constexpr uint32_t kStructsMem1 = 0xC88;   // GameScene, EGG::ExpHeap*
constexpr uint32_t kStructsMem2 = 0xC8C;
constexpr uint32_t kStructsDebug = 0xC90;
constexpr uint32_t kRvlHeap = 0x10;        // EGG::Heap: MEMiHeapHead*
constexpr uint32_t kHeapStart = 0x18;      // MEMiHeapHead
constexpr uint32_t kHeapEnd = 0x1C;

struct Overflow {
    uint32_t asked;  // MEMiHeapHead* the game allocated from
    uint32_t held;   // MEMiHeapHead* that holds the block
};

std::mutex g_lock;
std::unordered_map<uint32_t, Overflow> g_blocks;  // block -> heaps
uint32_t g_enteringScene = 0;                     // game thread only

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

uint32_t SceneRvlHeap(uint32_t scene, uint32_t field) {
    const uint32_t egg = Memory::Read32(scene + field);
    return egg == 0 ? 0 : Memory::Read32(egg + kRvlHeap);
}

bool Inside(uint32_t heap, uint32_t address) {
    return address >= Memory::Read32(heap + kHeapStart) && address < Memory::Read32(heap + kHeapEnd);
}

// The heap that really holds `block`, for a call that named `heap`.
uint32_t OwnerHeap(uint32_t heap, uint32_t block) {
    std::lock_guard<std::mutex> guard(g_lock);
    const auto it = g_blocks.find(block);
    if (it == g_blocks.end() || it->second.asked != heap || Inside(heap, block)) {
        return heap;
    }
    return it->second.held;
}

void Allocate(CpuContext* ctx, uint32_t heap, uint32_t size, uint32_t alignment) {
    ctx->gpr[3] = heap;
    ctx->gpr[4] = size;
    ctx->gpr[5] = alignment;
    MKW_GUEST_NEXT(80198D88)(ctx);
}

// Before `heap` is emptied or destroyed: free its blocks that live in other heaps, and forget the
// blocks that live in it (they go with it).
void ReleaseOverflow(CpuContext* ctx, uint32_t heap) {
    std::vector<std::pair<uint32_t, uint32_t>> release;  // block, holding heap
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (g_blocks.empty()) {
            return;
        }
        for (auto it = g_blocks.begin(); it != g_blocks.end();) {
            if (it->second.asked == heap) {
                release.emplace_back(it->first, it->second.held);
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

} // namespace

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
    if (heap == 0 || (heap != mem1 && heap != mem2)) {
        // Another heap: the game gets null. Logged (first 20), since it usually crashes later.
        static uint32_t logged = 0;
        if (logged < 20) {
            ++logged;
            RT_LOGF("race-heap", "allocation failed outside the scene heaps: size=%u heap=0x%08X lr=0x%08X\n", size,
                    heap, ctx->lr);
        }
        return;
    }
    uint32_t held = 0;
    if (heap == mem1 && mem2 != 0 && mem2 != mem1) {
        Allocate(ctx, mem2, size, alignment);
        held = ctx->gpr[3] != 0 ? mem2 : 0;
    }
    if (held == 0 && debug != 0 && debug != mem1 && debug != mem2) {
        Allocate(ctx, debug, size, alignment);
        held = ctx->gpr[3] != 0 ? debug : 0;
    }
    if (held == 0) {
        ctx->gpr[3] = 0;
        RT_LOGF("race-heap", "out of memory: size=%u asked=0x%08X (mem1=0x%08X mem2=0x%08X debug=0x%08X)\n",
                size, heap, mem1, mem2, debug);
        return;
    }
    const uint32_t block = ctx->gpr[3];
    std::lock_guard<std::mutex> guard(g_lock);
    g_blocks[block] = Overflow{heap, held};
}
REGISTER_NATIVE_WRAPPER_AS(0x80198D88, MEM_AllocFromExpHeapEx_80198d88, "MEM_AllocFromExpHeapEx_80198d88");

// MEMFreeToExpHeap(heap, block)
extern "C" void MEM_FreeToExpHeap_80199038(CpuContext* ctx) {
    const uint32_t block = ctx->gpr[4];
    ctx->gpr[3] = OwnerHeap(ctx->gpr[3], block);
    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_blocks.erase(block);
    }
    MKW_GUEST_NEXT(80199038)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80199038, MEM_FreeToExpHeap_80199038, "MEM_FreeToExpHeap_80199038");

// MEMResizeForMBlockExpHeap(heap, block, size)
extern "C" void MEM_ResizeForMBlockExpHeap_80198e38(CpuContext* ctx) {
    ctx->gpr[3] = OwnerHeap(ctx->gpr[3], ctx->gpr[4]);
    MKW_GUEST_NEXT(80198E38)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80198E38, MEM_ResizeForMBlockExpHeap_80198e38, "MEM_ResizeForMBlockExpHeap_80198e38");

// MEMDestroyExpHeap(heap)
extern "C" void MEM_DestroyExpHeap_80198d58(CpuContext* ctx) {
    ReleaseOverflow(ctx, ctx->gpr[3]);
    MKW_GUEST_NEXT(80198D58)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80198D58, MEM_DestroyExpHeap_80198d58, "MEM_DestroyExpHeap_80198d58");

// EGG::ExpHeap::FreeAll(this)
extern "C" void EGG_ExpHeap_FreeAll_80226ebc(CpuContext* ctx) {
    ReleaseOverflow(ctx, Memory::Read32(ctx->gpr[3] + kRvlHeap));
    MKW_GUEST_NEXT(80226EBC)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80226EBC, EGG_ExpHeap_FreeAll_80226ebc, "EGG_ExpHeap_FreeAll_80226ebc");

// EGG::ExpHeap::destroy(this)
extern "C" void EGG_ExpHeap_Destroy_80226b94(CpuContext* ctx) {
    ReleaseOverflow(ctx, Memory::Read32(ctx->gpr[3] + kRvlHeap));
    MKW_GUEST_NEXT(80226B94)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80226B94, EGG_ExpHeap_Destroy_80226b94, "EGG_ExpHeap_Destroy_80226b94");
