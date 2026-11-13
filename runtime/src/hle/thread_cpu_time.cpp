// mkw100: CPU time the calling (game) thread has used, for the adaptive frame skip in local_vs.cpp.
// Unlike wall time it leaves out the waits for the next retrace, so per second of wall time it is
// how busy the game is.

#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#else
#include <ctime>
#endif

uint64_t GameThreadCpuNanos() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
    const auto ticks = [](const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return (ticks(kernel) + ticks(user)) * 100;  // 100 ns units
#else
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec);
#endif
}
