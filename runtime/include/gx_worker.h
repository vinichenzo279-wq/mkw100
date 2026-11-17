#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

// Dedicated GX thread (Dolphin "dual core"): the game thread records GX HLE calls in order and
// the worker executes them. While active, GX work runs on the worker except inside inline scopes,
// which run on the game thread after the worker has caught up.
namespace GxWorker {

namespace detail {
inline constexpr size_t kAlign = 16;

extern std::atomic<bool> g_active;
extern constinit thread_local bool t_onWorker;
extern constinit thread_local int t_inlineDepth;
extern bool g_fifoPending;

void* Reserve(size_t payloadSize, void (*run)(void*));
void Commit();
void FlushFifo();
} // namespace detail

// Starts the worker unless [video] gx_worker disables it. Call once Aurora is up.
void Start();
void Stop();
// Stops without running queued work; bounded, for fatal exits.
void Abandon();

// Set on the game thread whenever recorded or inline work may change the CP vertex descriptor
// behind the game thread's copy of it (FIFO writes, display lists); see gx_vertex.cpp.
inline constinit bool g_vtxDescUnknown = true;
// Why the copy went unknown (test profiler statistics): 0 other, 1 FIFO bytes, 2 display list.
inline constinit uint64_t g_vtxMarks[3] = {};
// Display lists with a layout load: followed, walk failed, copy already unknown.
inline constinit uint64_t g_vtxFollow[3] = {};
// Marks that made a known copy unknown, by reason as g_vtxMarks.
inline constinit uint64_t g_vtxMarksEffective[3] = {};
inline void MarkVtxDescUnknown(int why = 0) noexcept {
    if (!detail::t_onWorker) { g_vtxMarksEffective[why] += !g_vtxDescUnknown; g_vtxDescUnknown = true; ++g_vtxMarks[why]; }
}
// Only a CP register load of the descriptor (command 0x08, register 0x50-0x5F VCD_LO, 0x60-0x6F
// VCD_HI) or of the attribute formats (0x70-0x9F VAT A/B/C) changes them, so the written byte stream is scanned for that pair instead of marking on
// every write: vertex data no longer forces GetVtxDesc to sync. A false match only costs a sync.
inline constinit uint8_t g_cpPrevByte = 0;
inline bool IsVcdLoad(uint8_t prev, uint8_t b) noexcept { return prev == 0x08 && b >= 0x50 && b < 0xA0; }
inline bool ScanForVcdLoad(const uint8_t* data, size_t size) noexcept {
    uint8_t prev = 0;
    for (size_t i = 0; i < size; ++i) {
        if (IsVcdLoad(prev, data[i])) return true;
        prev = data[i];
    }
    return false;
}
// The conservative scan: a "08 5x..9x" byte pair anywhere marks the copy unknown.
inline void ScanFifoBytes(const uint8_t* data, size_t size) noexcept {
    if (IsVcdLoad(g_cpPrevByte, data[0]) || ScanForVcdLoad(data, size)) MarkVtxDescUnknown(1);
    g_cpPrevByte = data[size - 1];
}
} // namespace GxWorker
// With [video] gx_follow_layout the game thread parses the FIFO command stream instead
// (gx_vertex.cpp): vertex data and XF payloads are stepped over, CP layout loads applied to its copy.
bool GxFollowLayoutLoads();
void GameFifoBytes(const uint8_t* data, size_t size);
namespace GxWorker {
inline void NoteFifoBytes(const uint8_t* data, size_t size) noexcept {
    if (detail::t_onWorker || size == 0) return;
    if (GxFollowLayoutLoads()) {
        GameFifoBytes(data, size);
        return;
    }
    ScanFifoBytes(data, size);
}
inline void NoteFifoWrite(uint32_t value, uint32_t sizeBytes) noexcept {
    uint8_t bytes[4];
    const uint32_t n = sizeBytes < 4 ? sizeBytes : 4;
    for (uint32_t i = 0; i < n; ++i) bytes[i] = uint8_t(value >> (8 * (n - 1 - i)));
    NoteFifoBytes(bytes, n);
}
inline bool Active() noexcept { return detail::g_active.load(std::memory_order_relaxed); }
inline bool OnWorker() noexcept { return detail::t_onWorker; }
// True when a GX entry point on this thread must be recorded instead of executed.
inline bool ShouldDefer() noexcept {
    return Active() && !detail::t_onWorker && detail::t_inlineDepth == 0;
}

template <typename F>
void Submit(F&& fn) {
    using Fn = std::decay_t<F>;
    static_assert(alignof(Fn) <= detail::kAlign, "over-aligned GX command");
    if (detail::g_fifoPending) {
        detail::FlushFifo();
    }
    void* storage = detail::Reserve(sizeof(Fn), [](void* p) {
        struct Destroy {
            Fn* f;
            ~Destroy() { f->~Fn(); }
        } guard{static_cast<Fn*>(p)};
        (*guard.f)();
    });
    new (storage) Fn(std::forward<F>(fn));
    detail::Commit();
}

// Records fn when deferring, otherwise runs it now. Read call-time guest memory before calling.
template <typename F>
void Run(F&& fn) {
    if (ShouldDefer()) {
        Submit(std::forward<F>(fn));
    } else {
        fn();
    }
}

// Gather-pipe writes, coalesced into one record per run of consecutive writes and replayed
// with their original sizes.
void FifoWrite(uint32_t value, uint32_t sizeBytes);
void FifoBurst(const uint8_t* data, uint32_t sizeBytes);

// Called about every millisecond while the game thread waits in Sync.
void SetWaitCallback(void (*callback)());

// Blocks until every recorded command has executed.
void Sync();

// GX entry points run inline on the game thread while inside (after a Sync), for work whose
// results the guest needs immediately.
void BeginInline();
void EndInline();

class InlineScope {
public:
    InlineScope() { BeginInline(); }
    ~InlineScope() { EndInline(); }
    InlineScope(const InlineScope&) = delete;
    InlineScope& operator=(const InlineScope&) = delete;
};

} // namespace GxWorker

// Records the enclosing call for the GX worker and returns, or falls through when it should run
// here (worker thread, inline scope, worker disabled).
#define GX_DEFER_CALL(fn, ...)                                                                     \
    do {                                                                                           \
        if (GxWorker::ShouldDefer()) {                                                             \
            GxWorker::Submit([=] { fn(__VA_ARGS__); });                                            \
            return;                                                                                \
        }                                                                                          \
    } while (false)

#define GX_DEFER_CALL0(fn)                                                                         \
    do {                                                                                           \
        if (GxWorker::ShouldDefer()) {                                                             \
            GxWorker::Submit([] { fn(); });                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (false)

// Runs the rest of the enclosing function on this thread once the worker has caught up.
#define GX_SYNC_INLINE() GxWorker::InlineScope gxInlineScope_
