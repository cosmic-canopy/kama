#ifndef KAMA_TIME_H
#define KAMA_TIME_H

// kama clock + sleep binding — the FFI boundary for `std::time`.
//
// The ONE place the platform clock difference is absorbed, so `std::time` (Duration/Instant/SystemTime)
// stays 100% platform-agnostic — the same bindings-layer role kama_os.h plays for I/O, kept SEPARATE so a
// program that only wants a clock (a game frame timer, an MCU delay, a scheduler benchmark) never pulls in
// <winsock2.h>. That separation is why `sleep` lives here and not beside kama_os.h's `kama_sleep_ms`, which
// predates it and stays where it is for the `proc_*` test helper.
//
// THREE primitives, and the split between the first two is the whole point:
//   kama_now_mono_ns  — a MONOTONIC counter. Only *differences* are meaningful; it never runs backward,
//                       so it is what a timeout, a frame time and a benchmark measure with.
//   kama_now_wall_ns  — the WALL clock, signed nanoseconds from the UNIX epoch. It is what a timestamp
//                       means, and it can jump in either direction (NTP, a user setting the clock), so it
//                       must never be used to measure a span.
//   kama_sleep_ns     — block the calling thread for at least this long.
// Everything else (Duration arithmetic, Instant deltas, timeouts) is pure kama on top.
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
// The wall clock, through emscripten's POSIX shim rather than through `emscripten_date_now()` — which is
// Date.now() and would read better, but was added to <emscripten.h> after the emsdk this repo's container
// pins, and an undeclared function is a hard error in C99 whether or not anything calls it. Measured, not
// assumed: the whole wasm leg failed to build on THREE fixtures, two of which never mention the clock.
// clock_gettime(CLOCK_REALTIME) is Date.now() underneath anyway.
//
// ⚠️ And the shim is behind the SAME strict-ISO gate the POSIX branch below documents: under `-std=c11`
// emscripten's <time.h> declares neither `clock_gettime` nor `CLOCK_REALTIME`. Declaring the one symbol
// ourselves is that branch's convention, reused here — musl (which emscripten's libc is) numbers the
// realtime clock 0, as every other target does.
#include <time.h>
extern int clock_gettime(int clk_id, struct timespec* ts);
static inline int64_t kama_now_wall_ns(void) {
    struct timespec ts;
    clock_gettime(0 /* CLOCK_REALTIME */, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + (int64_t)ts.tv_nsec;
}
// ⚠️ A BUSY WAIT, deliberately, and the only honest option here. A wasm program cannot yield to the host
// and resume mid-function without ASYNCIFY, which is a whole-program link-time transform paying code size
// and speed on every call in the binary — against the performance invariant, for one library function. So
// this spins on the monotonic clock: the semantics are right (control returns after the span has passed),
// the cost is a burned core, and on the browser's main thread it freezes the page for the duration. Which
// is the real lesson to hand a web author: on the event loop you do not sleep, you schedule. Node — the
// wasm test leg — runs this correctly.
static inline void kama_sleep_ns(uint64_t ns) {
    double until = emscripten_get_now() + (double)ns / 1e6;
    while (emscripten_get_now() < until) { }
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
// The wall clock. FILETIME counts 100-nanosecond ticks from 1601-01-01; 116444736000000000 of them separate
// that epoch from 1970-01-01. GetSystemTimeAsFileTime is the cheap one (~15 ms granularity, no syscall);
// GetSystemTimePreciseAsFileTime is finer but Windows-8-and-later only, and a timestamp does not need it.
static inline int64_t kama_now_wall_ns(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t ticks = ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
    return ((int64_t)ticks - 116444736000000000ll) * 100ll;
}
// Sleep() takes whole milliseconds, so ROUND UP: a 1 ns sleep must not be Sleep(0), which yields the rest
// of the timeslice and returns immediately — "at least this long" is the contract every caller assumes.
static inline void kama_sleep_ns(uint64_t ns) {
    if (ns == 0) return;
    Sleep((DWORD)((ns + 999999ull) / 1000000ull));
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

// The wall clock and the sleep, written ONCE for both cases above — whichever branch ran, `clock_gettime`
// is declared by the time control reaches here (the header exposed it, or the `extern` above did).
// CLOCK_REALTIME is 0 on Linux, macOS and the BSDs alike, so the fallback needs no per-OS table the way
// the monotonic id did.
#ifdef CLOCK_REALTIME
#  define KAMA_CLOCK_REALTIME_ID CLOCK_REALTIME
#else
#  define KAMA_CLOCK_REALTIME_ID 0
#endif
static inline int64_t kama_now_wall_ns(void) {
    struct timespec ts;
    clock_gettime(KAMA_CLOCK_REALTIME_ID, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + (int64_t)ts.tv_nsec;
}

#include <errno.h>
#ifndef CLOCK_MONOTONIC
extern int nanosleep(const struct timespec* req, struct timespec* rem);   // the same strict-ISO case
#endif
// `nanosleep` is interruptible: a signal cuts the nap short and writes what is LEFT into `rem`. Passing the
// same struct as both arguments is the standard idiom for finishing it — without the loop, a program that
// sleeps a second and happens to receive SIGCHLD (any child of a `std::process` `run()`, say) wakes early
// and silently, which is the classic version of this bug. EINTR is the only failure reachable here: the
// span is built from an unsigned count, so tv_nsec is in range and tv_sec is never negative.
static inline void kama_sleep_ns(uint64_t ns) {
    struct timespec req;
    req.tv_sec  = (time_t)(ns / 1000000000ull);
    req.tv_nsec = (long)(ns % 1000000000ull);
    while (nanosleep(&req, &req) != 0 && errno == EINTR) { }
}

#endif

#endif  // KAMA_TIME_H
