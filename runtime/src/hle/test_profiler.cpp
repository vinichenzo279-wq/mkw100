// mkw100: MKW100_TEST_PROFILE (with MKW100_TEST_PERF): a sampling profiler for test races. A
// thread suspends the game thread and the GX worker about every half millisecond and records each
// one's call stack (walked with the x64 unwind tables, so samples inside Dawn, the D3D12 runtime or
// the GPU driver keep the game function that called them). When the perf run ends the file the
// variable names gets:
//   module <base> <size> <path>        every loaded module
//   thread <name> <cpu ms> <wall ms>   CPU time of every thread in the process over the sampled span
//   samples <thread> <count>
//   s <thread> <pc0> <pc1> ...         one line per sample, leaf first, absolute addresses
// Exe addresses resolve against `llvm-nm -n WiiCompiled.exe` once rebased (the exe's preferred base
// is 0x140000000; the module line gives the real one). scripts/mkw100/profile_report.py does that.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#endif

namespace GxWorker { void* NativeThreadHandle(); }
void GxVertexStats(uint64_t out[12]);
void GxFifoParserStats();  // gx_vertex.cpp  // gx_vertex.cpp

namespace {
#ifdef _WIN32
struct Target {
    const char* name;
    HANDLE handle = nullptr;
    std::vector<uint64_t> stacks;  // per sample: depth, then depth pcs
    uint64_t samples = 0;
};
struct ThreadTimes {
    DWORD id;
    std::string name;
    uint64_t cpuStart;
};

constexpr int kMaxDepth = 64;
std::atomic<bool> g_running{false};
std::thread g_sampler;
Target g_targets[2] = {{"game"}, {"gx"}};
std::vector<ThreadTimes> g_threads;
std::chrono::steady_clock::time_point g_wallStart;

uint64_t CpuNanos(HANDLE h) {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0;
    const uint64_t kn = (uint64_t(k.dwHighDateTime) << 32) | k.dwLowDateTime;
    const uint64_t un = (uint64_t(u.dwHighDateTime) << 32) | u.dwLowDateTime;
    return (kn + un) * 100;
}

std::string Describe(HANDLE h) {
    PWSTR desc = nullptr;
    std::string out;
    if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc != nullptr) {
        for (const wchar_t* p = desc; *p; ++p) out.push_back(*p < 128 && *p > 32 ? char(*p) : '_');
        LocalFree(desc);
    }
    return out;
}

template <typename F> void ForEachThread(F&& f) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (h == nullptr) continue;
        f(te.th32ThreadID, h);
        CloseHandle(h);
    }
    CloseHandle(snap);
}

// Walks the suspended thread's stack. Frames without unwind data are treated as leaf functions
// (return address at rsp), and the walk stops once rsp leaves the thread's stack.
void Walk(const CONTEXT& start, Target& t) {
    CONTEXT c = start;
    const size_t at = t.stacks.size();
    t.stacks.push_back(0);
    uint64_t depth = 0;
    const uint64_t rspLow = c.Rsp, rspHigh = c.Rsp + (uint64_t(8) << 20);
    while (depth < kMaxDepth && c.Rip > 0x10000) {
        t.stacks.push_back(c.Rip);
        ++depth;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
        const uint64_t prevRsp = c.Rsp;
        if (fn == nullptr) {
            if (c.Rsp < rspLow || c.Rsp + 8 > rspHigh) break;
            c.Rip = *reinterpret_cast<const uint64_t*>(c.Rsp);
            c.Rsp += 8;
        } else {
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fn, &c, &handlerData, &establisher, nullptr);
        }
        if (c.Rsp <= prevRsp || c.Rsp >= rspHigh) break;
    }
    t.stacks[at] = depth;
}
#endif
} // namespace

void TestProfiler_Start() {
#ifdef _WIN32
    if (g_running || std::getenv("MKW100_TEST_PROFILE") == nullptr) return;
    constexpr DWORD kAccess = THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_targets[0].handle, kAccess,
                         FALSE, 0))
        return;
    if (HANDLE gx = static_cast<HANDLE>(GxWorker::NativeThreadHandle())) {
        DuplicateHandle(GetCurrentProcess(), gx, GetCurrentProcess(), &g_targets[1].handle, kAccess, FALSE, 0);
    }
    g_threads.clear();
    ForEachThread([](DWORD id, HANDLE h) { g_threads.push_back({id, Describe(h), CpuNanos(h)}); });
    g_wallStart = std::chrono::steady_clock::now();
    g_running = true;
    g_sampler = std::thread([] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        while (g_running) {
            for (Target& t : g_targets) {
                if (t.handle == nullptr || SuspendThread(t.handle) == DWORD(-1)) continue;
                CONTEXT c{};
                c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                if (GetThreadContext(t.handle, &c)) {
                    Walk(c, t);
                    ++t.samples;
                }
                ResumeThread(t.handle);
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
    const double wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_wallStart).count();
    const char* path = std::getenv("MKW100_TEST_PROFILE");
    FILE* f = std::fopen(path, "w");
    if (f == nullptr) return;
    HMODULE mods[1024];
    DWORD needed = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024; ++i) {
            MODULEINFO mi{};
            char name[MAX_PATH] = {};
            K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi));
            GetModuleFileNameA(mods[i], name, MAX_PATH);
            for (char* p = name; *p; ++p) if (*p == ' ') *p = '_';
            std::fprintf(f, "module %llx %lx %s\n", (unsigned long long)(uintptr_t)mi.lpBaseOfDll, (unsigned long)mi.SizeOfImage, name);
        }
    }
    ForEachThread([&](DWORD id, HANDLE h) {
        uint64_t start = 0;
        std::string name = Describe(h);
        for (const auto& t : g_threads) if (t.id == id) start = t.cpuStart;
        if (name.empty()) name = "tid" + std::to_string(id);
        std::fprintf(f, "thread %s %.1f %.1f\n", name.c_str(), (CpuNanos(h) - start) * 1e-6, wallMs);
    });
    for (const Target& t : g_targets) {
        std::fprintf(f, "samples %s %llu\n", t.name, (unsigned long long)t.samples);
        for (size_t i = 0; i < t.stacks.size();) {
            const uint64_t n = t.stacks[i++];
            std::fprintf(f, "s %s", t.name);
            for (uint64_t k = 0; k < n; ++k) std::fprintf(f, " %llx", (unsigned long long)t.stacks[i + k]);
            std::fputc('\n', f);
            i += n;
        }
    }
    std::fclose(f);
    uint64_t v[12];
    GxVertexStats(v);
    std::printf("[race-perf] vertex state: %llu queries, %llu synced; unknown after %llu other, %llu FIFO, %llu display list; %llu wrong\n",
                (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2], (unsigned long long)v[3],
                (unsigned long long)v[4], (unsigned long long)v[5]);
    std::printf("[race-perf] vertex state: lists followed %llu, not followable %llu, already unknown %llu; "
                "marks that mattered %llu other, %llu FIFO, %llu display list\n",
                (unsigned long long)v[6], (unsigned long long)v[7], (unsigned long long)v[8], (unsigned long long)v[9],
                (unsigned long long)v[10], (unsigned long long)v[11]);
    GxFifoParserStats();
    std::printf("[race-perf] profile: %llu game / %llu gx samples -> %s\n", (unsigned long long)g_targets[0].samples,
                (unsigned long long)g_targets[1].samples, path);
#endif
}
