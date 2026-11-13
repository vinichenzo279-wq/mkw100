// mkw100: MKW100_TEST_PROFILE (with MKW100_TEST_PERF): a sampling profiler for headless races. A
// thread suspends the game thread about every millisecond and counts its instruction pointer
// (relative to the exe's image base); the histogram goes to the file the variable names when the
// perf run ends. Resolve it offline against `llvm-nm -n WiiCompiled.exe` (image base 0x140000000).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
#ifdef _WIN32
std::atomic<bool> g_running{false};
std::thread g_sampler;
HANDLE g_target = nullptr;
std::unordered_map<uint64_t, uint32_t> g_hits;
uint64_t g_samples = 0;
#endif
} // namespace

void TestProfiler_Start() {
#ifdef _WIN32
    if (g_running || std::getenv("MKW100_TEST_PROFILE") == nullptr) return;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_target,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        return;
    const uint64_t base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    g_running = true;
    g_sampler = std::thread([base] {
        while (g_running) {
            if (SuspendThread(g_target) != DWORD(-1)) {
                CONTEXT c{};
                c.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(g_target, &c)) {
                    ++g_hits[c.Rip - base + 0x140000000ull];
                    ++g_samples;
                }
                ResumeThread(g_target);
            }
            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(500);
            while (std::chrono::steady_clock::now() < until) std::this_thread::yield();
        }
    });
#endif
}

void TestProfiler_Dump() {
#ifdef _WIN32
    if (!g_running) return;
    g_running = false;
    g_sampler.join();
    const char* path = std::getenv("MKW100_TEST_PROFILE");
    FILE* f = std::fopen(path, "w");
    if (f == nullptr) return;
    std::vector<std::pair<uint64_t, uint32_t>> rows(g_hits.begin(), g_hits.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::fprintf(f, "samples %llu\n", static_cast<unsigned long long>(g_samples));
    for (const auto& [rip, n] : rows) std::fprintf(f, "%llx %u\n", static_cast<unsigned long long>(rip), n);
    std::fclose(f);
    std::printf("[race-perf] profile: %llu samples -> %s\n", static_cast<unsigned long long>(g_samples), path);
#endif
}
