// wait_cxx.cpp — the C++ standard library's own `std::atomic<uint32_t>::wait` and `notify_one`, in wait.c's shapes, for KR-3
// step 2's W6. wait.c's `cell-count` arm models libc++'s mechanism without its spin; this is the library as shipped, which
// polls and yields before it sleeps.
//
// Usage: WAIT_SHAPE=pingpong|slow|idle WAIT_N=<ops> ./wait_cxx
// Prints the same TSV row as wait.c, with the arm named after the library it was built against.
// Build: c++ -std=c++20 -O2 wait_cxx.cpp -o wait_cxx    (macOS: libc++; Linux: libstdc++ unless -stdlib=libc++)
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/resource.h>
#endif

static uint64_t now_ns() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec; }
static uint64_t cpu_ns() {
#if defined(_WIN32)
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    return ((((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) * 100u;
#else
    rusage r;
    getrusage(RUSAGE_SELF, &r);
    return (uint64_t)(r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1000000000u + (uint64_t)(r.ru_utime.tv_usec + r.ru_stime.tv_usec) * 1000u;
#endif
}

#if defined(_LIBCPP_VERSION)
static const char* arm = "libc++";
#elif defined(__GLIBCXX__)
static const char* arm = "libstdc++";
#else
static const char* arm = "c++";
#endif

static std::atomic<uint32_t> ping{0}, pong{0};

int main() {
    const char* shape = getenv("WAIT_SHAPE") ? getenv("WAIT_SHAPE") : "pingpong";
    const long n = getenv("WAIT_N") ? atol(getenv("WAIT_N")) : 20000;
    const bool slow = !strcmp(shape, "slow");
    uint64_t t0, t1;
    const uint64_t c0 = cpu_ns();
    if (!strcmp(shape, "pingpong") || slow) {
        std::thread ponger([&] {
            for (uint32_t i = 0; i < (uint32_t)n; ++i) {
                ping.wait(i);
                if (slow) { const uint64_t t = now_ns(); while (now_ns() - t < 50000) { } }
                pong.store(i + 1);
                pong.notify_one();
            }
        });
        t0 = now_ns();
        for (uint32_t i = 0; i < (uint32_t)n; ++i) { ping.store(i + 1); ping.notify_one(); pong.wait(i); }
        t1 = now_ns();
        ponger.join();
    } else if (!strcmp(shape, "idle")) {
        t0 = now_ns();
        for (long i = 0; i < n; ++i) { ping.store((uint32_t)i); ping.notify_one(); }
        t1 = now_ns();
    } else { fprintf(stderr, "wait_cxx: unknown WAIT_SHAPE %s\n", shape); return 2; }
    const uint64_t c1 = cpu_ns();
    printf("%s\t%s\t%ld\t%.1f\t%.1f\n", arm, shape, n, (double)(t1 - t0) / (double)n, (double)(c1 - c0) / (double)n);
    return 0;
}
