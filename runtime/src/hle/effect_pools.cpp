// mkw100: room for particle effects in big races.
//
// Effect::EffectManager::createInstance (0x8067B4C8) has EGG::EffectManager::CreateMemoryManager
// (0x80224768) carve the nw4r effect pools out of a heap once, at boot, for 150 effects, 250
// emitters, 250 particle managers and 1500 particles: enough for 12 karts. Every kart keeps
// exhaust and drift effects going, so past a few dozen racers the pools ran dry and
// nw4r::ef::EffectSystem::CreateEffect returned null: a lightning strike flashed on only some
// of the field, and other one-off effects went missing the same way. The pools are made
// kScale times larger (about 3 MB more, once for the session; the config's counts are u16).
// Failed effect creations are counted in the log so a shortage shows up.

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"
#include "runtime_log.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace {

constexpr uint32_t kScale = 6;
constexpr uint32_t kCountFields[] = {0x4, 0x8, 0xC, 0x10};  // effects, emitters, particle managers, particles
uint32_t g_failed = 0;

} // namespace

// EGG::EffectManager::CreateMemoryManager(this, r4 config {heap, effects, emitters, managers, particles})
extern "C" void EffectPools_CreateMemoryManager_80224768(CpuContext* ctx) {
    const uint32_t config = ctx->gpr[4];
    // MKW100_TEST_EFFECT_SCALE (tests only): another factor, 1 for the game's own pools.
    static const char* const testScale = std::getenv("MKW100_TEST_EFFECT_SCALE");
    const uint32_t kScale = testScale ? uint32_t(std::max(1, std::atoi(testScale))) : ::kScale;
    if (config >= 0x80000000u) {
        uint32_t counts[4];
        for (int i = 0; i < 4; ++i) {
            const uint32_t field = config + kCountFields[i];
            counts[i] = Memory::Read32(field) * kScale;
            if (counts[i] > 0xFFFF) counts[i] = 0xFFFF;
            Memory::Write32(field, counts[i]);
        }
        RT_LOGF("effects", "effect pools: %u effects, %u emitters, %u particle managers, %u particles\n", counts[0],
                counts[1], counts[2], counts[3]);
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
