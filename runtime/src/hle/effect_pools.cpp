// mkw100: room for particle effects in big races.
//
// Effect::EffectManager::createInstance (0x8067B4C8) has EGG::EffectManager::CreateMemoryManager
// (0x80224768) carve the nw4r effect pools out of a heap once, at boot, for 150 effects, 250
// emitters, 250 particle managers and 1500 particles: enough for 12 karts. Every kart keeps
// exhaust and drift effects going, so past a few dozen racers the pools ran dry and
// nw4r::ef::EffectSystem::CreateEffect returned null: a lightning strike flashed on only some
// of the field, and other one-off effects went missing the same way. The pools are made up to
// kScale times larger (about 0.5 MB per step, once for the session), as far as the heap keeps
// kHeadroom free; with the 14.5 MB the boot heap has free in all three products that is 2x.
// Measured headless: 128 racers with items had over 1,000 failed effect creations at 1x and none
// at 2x, 3x or 4x; 255 racers and Item Fever / Retro Rewind at 100 none at 2x. Failed effect
// creations are counted in the log so a shortage shows up.

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"
#include "runtime_log.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace {

constexpr uint32_t kScale = 4;  // 3x already left no failed effects at 128 racers with items
constexpr uint32_t kCountFields[] = {0x4, 0x8, 0xC, 0x10};  // effects, emitters, particle managers, particles
constexpr uint32_t kUnitBytes[] = {160, 312, 220, 232};      // their sizes in CreateMemoryManager
constexpr uint32_t kHeadroom = 13u << 20;  // left free: Item Fever later needs about 12.3 MB in one block (5x crashed it)
constexpr uint32_t kExpHeapMagic = 0x45585048;  // 'EXPH', MEMiHeapHead
constexpr uint32_t kAllocatableForExpHeap = 0x80199180;  // MEM::GetAllocatableSizeForExpHeapEx(heap, align)
uint32_t g_failed = 0;

} // namespace

// The largest free block of the EGG heap at `heap` (0 if it is not an expanded heap).
static uint32_t HeapAllocatable(CpuContext* ctx, uint32_t heap) {
    if (heap < 0x80000000u) return 0;
    const uint32_t rvl = Memory::Read32(heap + 0x10);  // EGG::Heap: MEMiHeapHead*
    if (rvl < 0x80000000u || Memory::Read32(rvl) != kExpHeapMagic) return 0;
    const CpuContext saved = *ctx;
    ctx->gpr[3] = rvl;
    ctx->gpr[4] = 32;
    InvokeIndirectCpu(kAllocatableForExpHeap, ctx);
    const uint32_t size = ctx->gpr[3];
    *ctx = saved;
    return size;
}

// EGG::EffectManager::CreateMemoryManager(this, r4 config {heap, effects, emitters, managers, particles})
// The factor is the largest up to kScale that still leaves kHeadroom free in the heap: Item Fever
// fills its boot heap more than the base game, and the full 6x starved it (an archive load then
// called through a stomped pointer). MKW100_TEST_EFFECT_SCALE (tests only) forces a factor.
extern "C" void EffectPools_CreateMemoryManager_80224768(CpuContext* ctx) {
    const uint32_t config = ctx->gpr[4];
    static const char* const testScale = std::getenv("MKW100_TEST_EFFECT_SCALE");
    if (config >= 0x80000000u) {
        uint32_t heap = Memory::Read32(config);
        if (heap == 0) heap = Memory::Read32(0x8038CC00 - 23904);  // the game's default (r13 - 0x5D60)
        uint64_t base = 544;
        for (int i = 0; i < 4; ++i) base += uint64_t(Memory::Read32(config + kCountFields[i]) & 0xFFFF) * kUnitBytes[i] + 32;
        const uint32_t free = HeapAllocatable(ctx, heap);
        uint32_t scale = kScale;
        if (testScale) {
            scale = uint32_t(std::max(1, std::atoi(testScale)));
        } else {
            while (scale > 1 && base * scale + kHeadroom > free) --scale;
        }
        uint32_t counts[4];
        for (int i = 0; i < 4; ++i) {
            const uint32_t field = config + kCountFields[i];
            counts[i] = std::min<uint32_t>(Memory::Read32(field) * scale, 0xFFFF);
            Memory::Write32(field, counts[i]);
        }
        RT_LOGF("effects", "effect pools x%u (heap %u KB free): %u effects, %u emitters, %u particle managers, %u particles\n",
                scale, free >> 10, counts[0], counts[1], counts[2], counts[3]);
    }
    MKW_GUEST_NEXT(80224768)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80224768, EffectPools_CreateMemoryManager_80224768,
                           "EffectPools_CreateMemoryManager_80224768");

// nw4r::ef::EffectSystem::CreateEffect(this, resource, group, ...) -> Effect* or null
extern "C" void EffectPools_CreateEffect_80027380(CpuContext* ctx) {
    MKW_GUEST_NEXT(80027380)(ctx);
    if (ctx->gpr[3] == 0 && (g_failed++ % 1000) == 0) {
        RT_LOGF("effects", "an effect could not be created (pools full); %u so far\n", g_failed);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x80027380, EffectPools_CreateEffect_80027380, "EffectPools_CreateEffect_80027380");
