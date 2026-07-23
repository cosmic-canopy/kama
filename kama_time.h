#ifndef KAMA_TIME_H
#define KAMA_TIME_H

// kama monotonic-clock binding — the FFI boundary for `std::time`.
//
// The ONE place the platform clock difference is absorbed, so `std::time` (Duration/Instant) stays 100%
// platform-agnostic — the same bindings-layer role kama_os.h plays for I/O, kept SEPARATE so a program that
// only wants a clock (a game frame timer, an MCU delay, a scheduler benchmark) never pulls in <winsock2.h>.
//
// One primitive: a monotonic nanosecond counter. It is NOT wall-clock — only *differences* are meaningful, and
// it never runs backward. Everything else (Duration arithmetic, Instant deltas, timeouts) is pure kama on top.
//
// Freestanding-friendly: pulls only <stdint.h> plus the one platform clock header (guarded), matching
// kama_os.h's `static inline` (single-TU, pruned-if-unused) convention.

#include <stdint.h>

#if defined(__EMSCRIPTEN__)

#include <emscripten.h>
// emscripten_get_now() is a monotonic high-resolution millisecond timer (performance.now under the hood).
static inline uint64_t kama_now_mono_ns(void) {
    return (uint64_t)(emscripten_get_now() * 1e6);   // ms -> ns
}

#elif defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// QueryPerformanceCounter is monotonic; convert ticks -> ns without overflow (split whole seconds + remainder,
// since counter*1e9 overflows uint64 within seconds).
static inline uint64_t kama_now_mono_ns(void) {
    LARGE_INTEGER freq, ctr;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&ctr);
    uint64_t f = (uint64_t)freq.QuadPart;
    uint64_t c = (uint64_t)ctr.QuadPart;
    uint64_t whole = c / f;
    uint64_t rem   = c % f;
    return whole * 1000000000ull + (rem * 1000000000ull) / f;
}

#else

// POSIX (Linux/macOS/BSD/iOS/Android). CLOCK_MONOTONIC is unaffected by wall-clock adjustments.
// `struct timespec` is ISO C11 (always visible), but `clock_gettime`/`CLOCK_MONOTONIC` are POSIX and glibc
// feature-gates them behind _POSIX_C_SOURCE — which is already locked by the time this header is reached
// (kama_runtime.h's <stdint.h> pulled <features.h> under the strict-ISO `-std=c11` build). So: if the header
// exposed the POSIX clock, use it; otherwise declare the one symbol ourselves (the kama_os.h convention —
// same linked libc symbol) and name the monotonic clock id per-OS (a stable kernel ABI constant).
#include <time.h>
#ifdef CLOCK_MONOTONIC
static inline uint64_t kama_now_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#else
extern int clock_gettime(int clk_id, struct timespec* ts);   // clockid_t is int-compatible on all targets
#if defined(__APPLE__)
#  define KAMA_CLOCK_MONOTONIC 6      // <sys/_clock_id.h>
#elif defined(__FreeBSD__)
#  define KAMA_CLOCK_MONOTONIC 4
#else
#  define KAMA_CLOCK_MONOTONIC 1      // Linux (glibc/musl)
#endif
static inline uint64_t kama_now_mono_ns(void) {
    struct timespec ts;
    clock_gettime(KAMA_CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

#endif

#endif  // KAMA_TIME_H
