#ifndef KAMA_TIME_H
#define KAMA_TIME_H

// kama clock + sleep binding — the FFI boundary for `std::time`.
//
// The ONE place the platform clock difference is absorbed, so `std::time` (Duration/Instant/Timestamp)
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
//   kama_sleep_ns     — wait at least this long: the calling thread, or on an isolate that runs fibers, only the
//                       calling fiber (KR-3 step 5), so the core runs the others meanwhile.
// Everything else (Duration arithmetic, Instant deltas, timeouts) is pure kama on top.
//
// Freestanding-friendly: pulls only <stdint.h> plus the one platform clock header (guarded), matching
// kama_os.h's `static inline` (single-TU, pruned-if-unused) convention.

#include <stdint.h>

#if defined(KAMA_TARGET_EMBEDDED)

// Bare metal (KR-341): no OS clock and no C library header to read. A board declares its clocks in kama
// (`@monotonicClock`, `@wallClock`), and the compiler emits these two from the declaration; a program that reaches a
// clock with none declared is refused at compile time, with the chain that reaches it.
uint64_t kama_now_mono_ns(void);
int64_t  kama_now_wall_ns(void);
// A sleep spins on the board's clock, as `Atomic.wait` polls there: nothing but interrupt handlers runs beside it.
static inline void kama__sleep_os(uint64_t ns) {
    const uint64_t t0 = kama_now_mono_ns();
    while (kama_now_mono_ns() - t0 < ns) { }
}

#elif defined(__EMSCRIPTEN__)

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
#if defined(KAMA_OWN_HEADERS_ONLY)   // the trims kama_os.h explains (KR-68)
#  ifndef NOGDI
#    define NOGDI
#  endif
#  ifndef NOUSER
#    define NOUSER
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#endif
#include <windows.h>
// The monotonic clock counts the time the system is awake, as on every target (SPEC.md, *Time*; KR-372):
// QueryUnbiasedInterruptTimePrecise, the interrupt time less what the system spent asleep, in 100 ns units. Every wait
// function's relative timeout has counted the same time since Windows 8. QueryPerformanceCounter, read here before
// 0.9.918, counts a sleep, so a deadline set before a lid shut had passed when it opened.
// Measured on an AMD Ryzen AI 9 465 (Windows 11 26200, 15 interleaved rounds): 19.6 ns a read, against 22.8 for the
// counter as it was read here (asking QueryPerformanceFrequency every time) and 16.7 for the bare counter; both step by
// 100 ns. kernelbase alone exports it, and mingw-w64 declares it only in the mincore umbrella library, which would
// rebind a program's other imports to API sets. So it is looked up once, by the API set Microsoft documents for it,
// which every process already has loaded: a pointer costs the same as an import (19.6 against 19.7 ns). It exists on
// every Windows 10, and kama's floor is 1803.
typedef VOID (WINAPI *kama__unbiased_t)(PULONGLONG);
static inline uint64_t kama_now_mono_ns(void) {
    static kama__unbiased_t unbiased;
    kama__unbiased_t f = __atomic_load_n(&unbiased, __ATOMIC_ACQUIRE);
    if (!f) {
        f = (kama__unbiased_t)(void*)GetProcAddress(GetModuleHandleW(L"api-ms-win-core-realtime-l1-1-1.dll"),
                                                    "QueryUnbiasedInterruptTimePrecise");
        __atomic_store_n(&unbiased, f, __ATOMIC_RELEASE);
    }
    ULONGLONG t;
    f(&t);
    return (uint64_t)t * 100ull;
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
// ⚠️ `Sleep()` does not sleep for what you asked — it sleeps to the next SCHEDULER TICK, ~15.625 ms by
// default. So `sleep(1 ms)` and `sleep(8 ms)` both cost one tick and `sleep(16 ms)` costs two, and any
// program pacing itself with `sleep` — a frame loop's 8 ms nap, a poll loop's 10 ms wait — ran at half
// rate on Windows and at the asked rate everywhere else. Measured on this repo's Windows box before this
// arm existed: 1 ms -> 13.4 ms, 8 ms -> 15.6 ms, 16 ms -> 23.6 ms (KR-76).
//
// The contract was never broken — SPEC promises *at least* `d` — which is why this is reach rather than a
// bug, and why the fix is here rather than in every program that noticed.
//
// A high-resolution waitable timer honours the span as asked, WITHOUT the process-global cost of the
// obvious alternative. ⚠️ NOT `timeBeginPeriod(1)`: that raises the tick for the whole process (and
// historically the whole system), burning power in every thread including ones that never sleep, and a
// library has no business spending a caller's battery to fix its own resolution. The timer is per call
// and affects nobody else.
//
// `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` needs Windows 10 1803. On anything older the create FAILS
// rather than silently degrading, which is what makes the fallback safe to write as one branch: no
// version test, no feature probe — ask, and take `Sleep` when the answer is no.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#  define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
// The span is judged by the clock an `Instant` reads, and the wait re-armed for what is left until it has passed, as a
// fiber's sleep is (kama_sched.h). The wait functions' relative timeouts leave a sleep out, but a waitable timer's
// relative due time is not documented to, and one that counted a sleep would end a nap early on wake (KR-372).
static inline void kama__sleep_os(uint64_t ns) {
    if (ns == 0) return;
    const uint64_t t0 = kama_now_mono_ns();
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL,
                                          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    for (uint64_t left = ns, gone; ; left = ns - gone) {
        LARGE_INTEGER due;
        // Negative = RELATIVE, in 100-ns units. Rounded UP, for the same reason the `Sleep` line below
        // rounds up to the millisecond: "at least this long" must survive the change of unit. The clamp
        // is against a nonsense span overflowing the sign — 2^63 hundred-ns is ~29,000 years, so nothing
        // a caller means is lost, and `-(LONGLONG)x` on an x past LLONG_MAX is undefined.
        uint64_t units = (left + 99ull) / 100ull;
        if (units > 0x7FFFFFFFFFFFFFFFull) units = 0x7FFFFFFFFFFFFFFFull;
        due.QuadPart = -(LONGLONG)units;
        if (timer != NULL && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
            WaitForSingleObject(timer, INFINITE);
        else   // Sleep() takes whole milliseconds, so ROUND UP: a 1 ns sleep must not be Sleep(0), which yields the
               // rest of the timeslice and returns immediately.
            Sleep((DWORD)((left + 999999ull) / 1000000ull));
        if ((gone = kama_now_mono_ns() - t0) >= ns) break;
    }
    if (timer != NULL) CloseHandle(timer);
}

#else

// POSIX (Linux/macOS/BSD/iOS/Android). The monotonic clock counts the time the system is AWAKE, as every OS's own
// timed waits do (epoll's, kqueue's, a condvar's), so a deadline taken from an `Instant` and the wait that arms it
// agree, and a suspend does not fire every deadline at once on wake (SPEC.md, *Time*). Linux and Android:
// CLOCK_MONOTONIC. Apple: CLOCK_UPTIME_RAW, `mach_absolute_time` in nanoseconds, read with `clock_gettime_nsec_np` as
// Apple's documentation asks: 8.2 ns and 42 ns steps on an M4, where Apple's CLOCK_MONOTONIC, which counts sleep and is
// the time of day less the boot time, took 16.2 ns and stepped by a whole microsecond (`clock_gettime` through a
// timespec, 12.8 ns; 15 interleaved rounds).
// `struct timespec` is ISO C11 (always visible), but `clock_gettime` and the clock ids are POSIX and glibc
// feature-gates them behind _POSIX_C_SOURCE — which is already locked by the time this header is reached
// (kama_runtime.h's <stdint.h> pulled <features.h> under the strict-ISO `-std=c11` build). So: if the header
// exposed the POSIX clock, use it; otherwise declare the symbols ourselves (the kama_os.h convention —
// same linked libc symbols) and name the clock id per-OS (a stable kernel ABI constant).
#include <time.h>
#ifndef CLOCK_MONOTONIC
extern int clock_gettime(int clk_id, struct timespec* ts);   // clockid_t is int-compatible on all targets
#  if defined(__APPLE__)
extern uint64_t clock_gettime_nsec_np(int clk_id);
#    define CLOCK_UPTIME_RAW 8        // <_time.h>
#  elif defined(__FreeBSD__)
#    define KAMA_CLOCK_MONOTONIC 4
#  else
#    define KAMA_CLOCK_MONOTONIC 1    // Linux (glibc/musl)
#  endif
#else
#  define KAMA_CLOCK_MONOTONIC CLOCK_MONOTONIC
#endif
#if defined(__APPLE__)
static inline uint64_t kama_now_mono_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
#else
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
static inline void kama__sleep_os(uint64_t ns) {
    struct timespec req;
    req.tv_sec  = (time_t)(ns / 1000000000ull);
    req.tv_nsec = (long)(ns % 1000000000ull);
    while (nanosleep(&req, &req) != 0 && errno == EINTR) { }
}

#endif

#if !defined(__EMSCRIPTEN__)
// On an isolate that runs fibers a sleep parks only its fiber (KR-3 step 5, fibers.md decision 7: one `sleep`, one
// meaning). A program that runs fibers defines KAMA_FIBERS, and the scheduler's half is kama_sched.h's, included at
// the end of this file — unless kama_os.h is part way through reading it, since kama_os.h includes kama_sched.h itself
// at its own end, once its socket half is declared.
#if defined(KAMA_FIBERS)
static inline int kama__time_park(uint64_t ns);       // 1 when it parked the running fiber that long; 0 on a thread
#endif
static inline void kama_sleep_ns(uint64_t ns) {
#if defined(KAMA_FIBERS)
    if (kama__time_park(ns)) return;
#endif
    kama__sleep_os(ns);
}
#if defined(KAMA_FIBERS) && !defined(KAMA_OS_H)
#include "kama_sched.h"
#endif
#endif

#endif  // KAMA_TIME_H
