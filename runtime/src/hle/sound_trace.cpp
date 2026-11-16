// mkw100: diagnostic for sounds that do not play (MKW100_TRACE_SOUND=1).
//
// Logs every sound the game fails to start (nw4r::snd::SoundArchivePlayer::SetupSoundImpl: the
// sound id and the StartResult) and every sound group it fails to load (LoadGroup by id). Written
// for Retro Rewind with Item Fever's items (docs/mkw100/rr-item-fever.md), whose countdown and
// race start were silent; it works in every product. Off by default: nothing changes then.

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "runtime_log.h"

#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <utility>

namespace {

bool TraceSound() {
    static const bool on = [] {
        const char* v = std::getenv("MKW100_TRACE_SOUND");
        return v && *v && *v != 48;  // set and not "0"
    }();
    return on;
}

// At most a few lines per (sound, result), so a looping failure does not flood the log.
bool FirstFew(uint32_t kind, uint32_t id, uint32_t result) {
    static std::mutex mutex;
    static std::map<std::pair<uint64_t, uint32_t>, uint32_t> seen;
    std::lock_guard lock(mutex);
    uint32_t& n = seen[{(uint64_t(kind) << 32) | id, result}];
    return ++n <= 3;
}

} // namespace

// StartResult SoundArchivePlayer::SetupSoundImpl(this, SoundHandle*, u32 soundId, ...): 0 is success.
extern "C" void SoundTrace_SetupSoundImpl_800a1820(CpuContext* ctx) {
    const uint32_t soundId = ctx->gpr[5];
    MKW_GUEST_NEXT(800A1820)(ctx);
    if (!TraceSound()) return;
    const uint32_t result = ctx->gpr[3];
    if (result != 0 && FirstFew(0, soundId, result)) {
        RT_LOGF("sound-trace", "sound 0x%X did not start: StartResult %u\n", soundId, result);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x800A1820, SoundTrace_SetupSoundImpl_800a1820, "SoundTrace_SetupSoundImpl_800a1820");

// bool SoundArchivePlayer::LoadGroup(this, u32 groupId, SoundMemoryAllocatable*, u32 blockSize).
extern "C" void SoundTrace_LoadGroup_800a28b0(CpuContext* ctx) {
    const uint32_t groupId = ctx->gpr[4];
    const uint32_t allocator = ctx->gpr[5];
    MKW_GUEST_NEXT(800A28B0)(ctx);
    if (!TraceSound()) return;
    const bool ok = (ctx->gpr[3] & 0xFF) != 0;
    if (FirstFew(1, groupId, ok)) {
        RT_LOGF("sound-trace", "group %u (allocator 0x%08X) %s\n", groupId, allocator, ok ? "loaded" : "FAILED to load");
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x800A28B0, SoundTrace_LoadGroup_800a28b0, "SoundTrace_LoadGroup_800a28b0");
