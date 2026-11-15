#ifndef KAMA_APP_H
#define KAMA_APP_H

// The application run-loop shim for `std::app` — the ONE place the loop's per-host split lives: a plain loop
// natively, emscripten's main loop on the web, and the NDK glue's looper in an Android app (KR-283).
// A synchronous busy-loop on the web never sees async events (sockets/timers), because the JS event loop
// only runs when the wasm program yields to it; emscripten's main loop is that yield. Pulled in only by a
// program that imports std::app (its module says `extern "kama_app.h";`), so non-loop programs pay nothing.
//
// `tick(state)` returns 1 to keep running, 0 to stop. `lifecycle(state, event)` is called on the loop's thread
// the moment the host reports a change, between ticks, never queued for a later tick: Android's UI thread waits
// until a destroyed window's handler has RETURNED (the glue clears `window` after it), and a hidden browser tab
// runs no next tick. Every host says RESUMED and then SURFACE_READY before the first tick. On the web `run` does
// NOT return (the host drives the loop); the tick ends the program itself (`quit`) when its work is done.
//
// THE PACE (KR-281 decision 4g, ruled 2026-11-13). `run` states one: every refresh (a game: no wait at all), at most a
// rate (a wait to the next deadline after each tick), or on demand (a wait until something wakes the loop). An app
// on demand ticks once per wake, and four things wake it: a lifecycle event that resumes it, `requestTick(at:)`, the
// app's `Waker` (std::concurrent's, so a channel's `wakeOnSend` reaches it from any isolate), and input. The first
// tick after `run`, and after `setPace(OnDemand)`, runs at once, so an app draws its first frame unasked.
//
// THE WAKE. One process-wide `kama__app`, defined once by the emitter (emitRuntimeSlotDefinitions), begins with a
// waker's operations, so it IS the state of the program's one app `Waker`, and retain/release do nothing. A wake adds
// one to `generation` and wakes whatever the host's idle wait is: `Atomic.wait`'s table natively (which parks the root
// fiber of an isolate that runs fibers, so its scheduler keeps running), `ALooper_wake` in an Android app, the main
// loop resumed on the web (posted to the loop's thread from any other). The loop reads `generation` BEFORE each tick
// and waits only while it is unchanged, so a wake that lands during a tick is a tick after it: none is lost.

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "kama_runtime.h"   /* kama_alloc, kama_panic, kama_waker_ops */
#include "kama_time.h"      /* kama_now_mono_ns: the clock an `Instant` holds */
#if !defined(__EMSCRIPTEN__) && !defined(KAMA_ANDROID_APP)
#include "kama_wait.h"      /* kama_atomic_wait_until / kama_atomic_notify: the native idle wait */
#endif
#if defined(__EMSCRIPTEN_PTHREADS__)
#include <pthread.h>
#endif

// std::app's `Lifecycle` discriminants (lib/std/app/app.kama).
enum {
    KAMA_APP_RESUMED = 0, KAMA_APP_SUSPENDED = 1, KAMA_APP_SURFACE_READY = 2, KAMA_APP_SURFACE_LOST = 3,
    KAMA_APP_FOCUS_GAINED = 4, KAMA_APP_FOCUS_LOST = 5, KAMA_APP_LOW_MEMORY = 6
};
// std::app's `Pace`, as `run` and `setPace` hand it over.
enum { KAMA_APP_EVERY_REFRESH = 0, KAMA_APP_AT_MOST = 1, KAMA_APP_ON_DEMAND = 2 };
#define KAMA__APP_NEVER UINT64_MAX   /* no tick requested */

typedef struct kama__app_shared {
    const kama_waker_ops* ops;   // first: a std::concurrent::Waker's state begins with its operations (kama_runtime.h)
    uint32_t generation;         // a wake adds one; the idle wait waits while it is unchanged
    int32_t pending;             // a window package's wake, coalesced as a Poller's is
    void (*eventWake)(void);     // the window package's wake (setEventWait), callable from any thread; set once
    const char* owner;           // ...and the package that installed it, for a second install's panic
    // The loop's own, read and written on its thread only:
    int32_t running;             // a loop runs (on the thread whose kama__app_here is set)
    int32_t pace;                // KAMA_APP_*
    float hz;                    // AtMost's rate
    uint64_t request;            // the earliest requested tick, or KAMA__APP_NEVER
    int32_t now;                 // tick at once: the first tick, and the first after setPace(OnDemand)
    uint32_t seen;               // `generation` as the last tick began
    // The web loop's, on the loop's thread (one per process, so a wake reaching it from any unit finds the same):
    // whether emscripten's main loop is paused (an app on demand with nothing due), the timer a later request armed
    // and its instant, AtMost's next deadline in performance.now() milliseconds, and the loop's thread.
    void* loop;
    int32_t paused;
    int32_t timer;
    uint64_t timerAt;
    double next;
#if defined(__EMSCRIPTEN_PTHREADS__)
    pthread_t thread;
#endif
} kama__app_shared;
extern kama__app_shared kama__app;          // defined in the entry unit (emitRuntimeSlotDefinitions)
extern KAMA_ISOLATE_LOCAL int kama__app_here;        // this thread runs the loop
extern KAMA_ISOLATE_LOCAL int kama__app_installer;   // this thread installed the window package's wait

static inline void kama__app_panic(const char* msg, size_t n) { kama_panic(kama_string_lit(msg, n)); }
#define KAMA__APP_PANIC(lit) kama__app_panic(lit, sizeof lit - 1)

static void kama__app_host_wake(void);   // each host's own, below

static int32_t kama__app_wake(void* w) {
    kama__app_shared* a = (kama__app_shared*)w;
    __atomic_add_fetch(&a->generation, 1u, __ATOMIC_SEQ_CST);
    kama__app_host_wake();
    return 0;
}
static void kama__app_retain(void* w) { (void)w; }    // the state lives as long as the program
static void kama__app_release(void* w) { (void)w; }
static inline const kama_waker_ops* kama__app_ops(void) {
    static const kama_waker_ops ops = { kama__app_wake, kama__app_retain, kama__app_release };
    return &ops;
}
// std::app::waker(): the state a `Waker` adopts. Retained by nothing, since it is never freed.
static inline void* kama_app_waker(void) {
    if (!__atomic_load_n(&kama__app.ops, __ATOMIC_ACQUIRE))
        __atomic_store_n(&kama__app.ops, kama__app_ops(), __ATOMIC_RELEASE);   // every caller stores the same table
    return &kama__app;
}

// A `Pace` checked where it is stated: a rate is above zero and finite.
static inline void kama__app_check_pace(int32_t pace, float hz) {
    if (pace == KAMA_APP_AT_MOST && !(hz > 0.0f && !__builtin_isinf(hz)))
        KAMA__APP_PANIC("Pace::AtMost(hz:) takes a rate above zero and finite");
}
static inline void kama__app_begin(int32_t pace, float hz) {
    kama__app_check_pace(pace, hz);
    if (!__atomic_load_n(&kama__app.ops, __ATOMIC_ACQUIRE)) (void)kama_app_waker();
    kama__app.running = 1; kama__app.pace = pace; kama__app.hz = hz;
    kama__app.request = KAMA__APP_NEVER; kama__app.now = 1;
    kama__app_here = 1;
}
static inline void kama__app_end(void) { kama__app.running = 0; kama__app_here = 0; }
static inline void kama__app_on_loop(const char* what, size_t n) {
    if (!kama__app_here || !kama__app.running) kama__app_panic(what, n);
}
static void kama__app_paced(void);   // each host's reaction to a new pace, below

// std::app::setPace and std::app::requestTick: on the loop's thread, while it runs.
static inline void kama_app_set_pace(int32_t pace, float hz) {
    static const char msg[] = "std::app::setPace is called on the thread that runs the app's loop, while `run` runs";
    kama__app_on_loop(msg, sizeof msg - 1);
    kama__app_check_pace(pace, hz);
    const int32_t was = kama__app.pace;
    kama__app.pace = pace; kama__app.hz = hz;
    if (pace == KAMA_APP_ON_DEMAND && was != KAMA_APP_ON_DEMAND) kama__app.now = 1;
    kama__app_paced();
}
static inline void kama_app_request_tick(uint64_t at) {
    static const char msg[] = "std::app::requestTick is called on the thread that runs the app's loop, while `run` runs";
    kama__app_on_loop(msg, sizeof msg - 1);
    if (at < kama__app.request) kama__app.request = at;
}
// The next tick an app on demand owes: at once (0), at the earliest request, or never.
static inline uint64_t kama__app_due(void) {
    if (kama__app.now || __atomic_load_n(&kama__app.generation, __ATOMIC_SEQ_CST) != kama__app.seen) return 0;
    return kama__app.request;
}
// Before a tick: what woke it is seen, and every request whose instant has passed is answered by it.
static inline void kama__app_ticking(void) {
    kama__app.seen = __atomic_load_n(&kama__app.generation, __ATOMIC_SEQ_CST);
    kama__app.now = 0;
    if (kama__app.request != KAMA__APP_NEVER && kama__app.request <= kama_now_mono_ns()) kama__app.request = KAMA__APP_NEVER;
    __atomic_store_n(&kama__app.pending, 0, __ATOMIC_SEQ_CST);
}
// The interval AtMost(hz) keeps between ticks, in nanoseconds.
static inline uint64_t kama__app_period_ns(void) { return (uint64_t)(1e9 / (double)kama__app.hz); }

typedef struct kama__loop {
    int32_t (*tick)(void*);
    void (*lifecycle)(void*, int32_t);
    void (*idle)(uint64_t);
    void* state;
    int suspended;
} kama__loop;

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/eventloop.h>
#if defined(__EMSCRIPTEN_PTHREADS__)
#include <emscripten/proxying.h>
#include <pthread.h>
#endif

// Paused, the main loop holds nothing that keeps the runtime alive: in a threaded build `main`'s worker would end and
// the process with it, before a wake from another thread could arrive (measured: the app exited 0 after its first
// tick). So a pause holds a keepalive, and a resume lets it go. Under node with no threads, a paused app whose every
// wake source is gone (no timer, no socket) still ends as node does when its event loop is empty.
static inline void kama__web_pause(void) {
    if (!kama__app.paused) { kama__app.paused = 1; emscripten_pause_main_loop(); emscripten_runtime_keepalive_push(); }
}
static inline void kama__web_resume(void) {
    if (kama__app.paused) { kama__app.paused = 0; emscripten_runtime_keepalive_pop(); emscripten_resume_main_loop(); }
}
// What a page's script calls when it delivers an event (std::net::web's transports, a window package's DOM input):
// `Module._kama_app_wake()`. Defined once, exported, beside `kama__app` (emitRuntimeSlotDefinitions); named here so
// the pass that drops what nothing reaches keeps it, as it keeps every name a runtime header mentions.
void kama_app_wake(void);
static void kama__web_timer_fired(void* arg) { (void)arg; kama__app.timer = 0; kama__app.timerAt = KAMA__APP_NEVER; kama__web_resume(); }
static void kama__web_wake_here(void* arg) { (void)arg; if (kama__app.loop) kama__web_resume(); }
// A wake on the loop's thread resumes the loop; from any other it is posted there first.
static void kama__app_host_wake(void) {
#if defined(__EMSCRIPTEN_PTHREADS__)
    if (!kama__app.running || pthread_equal(pthread_self(), kama__app.thread)) { kama__web_wake_here(NULL); return; }
    (void)emscripten_proxy_async(emscripten_proxy_get_system_queue(), kama__app.thread, kama__web_wake_here, NULL);
#else
    kama__web_wake_here(NULL);
#endif
}
static void kama__app_paced(void) {
    if (kama__app.pace == KAMA_APP_AT_MOST) kama__app.next = emscripten_get_now();
    kama__web_resume();
}
// The request's timer, cleared when the loop ends or the program quits: a timer left armed fires after the runtime
// has ended, and its own keepalive count, which `emscripten_force_exit` zeroed, aborts (measured, a debug build).
static inline void kama__web_disarm(void) {
    if (kama__app.timer) { emscripten_clear_timeout(kama__app.timer); kama__app.timer = 0; kama__app.timerAt = KAMA__APP_NEVER; }
}
// After a tick on demand: run on if something is due now, else pause until a wake or the request's timer.
static void kama__web_settle(void) {
    if (kama__app.pace != KAMA_APP_ON_DEMAND) return;
    const uint64_t due = kama__app_due();
    const uint64_t now = kama_now_mono_ns();
    if (due != KAMA__APP_NEVER && due <= now) return;   // the next frame
    kama__web_pause();
    if (due != KAMA__APP_NEVER && due < kama__app.timerAt) {
        if (kama__app.timer) emscripten_clear_timeout(kama__app.timer);
        kama__app.timerAt = due;
        kama__app.timer = (int)emscripten_set_timeout(kama__web_timer_fired, (double)(due - now) / 1e6, NULL);
    }
}

// emscripten's callback is void(void*); the loop is one heap arg (leaked — the program exits when the loop ends).
// fps 0 = requestAnimationFrame in a browser, which stops while the tab is hidden; an event-loop tick under Node.
static void kama__loop_step(void* arg) {
    kama__loop* l = (kama__loop*)arg;
    if (l->suspended) return;   // a browser keeps no rAF for a hidden tab; a timer-driven host still might call
    if (kama__app.pace == KAMA_APP_AT_MOST) {
        const double now = emscripten_get_now(), period = 1000.0 / (double)kama__app.hz;
        if (now + 1.0 < kama__app.next) return;   // a frame early by under a millisecond of jitter still ticks
        kama__app.next = (kama__app.next + period > now) ? kama__app.next + period : now + period;
    } else if (kama__app.pace == KAMA_APP_ON_DEMAND && kama__app_due() > kama_now_mono_ns()) {
        kama__web_settle();   // woken by nothing that is due (a timer for a request since moved later)
        return;
    }
    kama__app_ticking();
    if (!l->tick(l->state)) { kama__web_disarm(); kama__app_end(); emscripten_cancel_main_loop(); return; }
    kama__web_settle();
}
// The DOM's visibilitychange: hidden is SUSPENDED and shown is RESUMED. A page that starts hidden hears RESUMED,
// and SURFACE_READY, when it is first shown. Node has no document, so the registration fails there and the
// program stays resumed. A resume is a wake, so an app on demand ticks when it is shown again.
static EM_BOOL kama__loop_visibility(int type, const EmscriptenVisibilityChangeEvent* e, void* arg) {
    (void)type;
    kama__loop* l = (kama__loop*)arg;
    if (e->hidden && !l->suspended) {
        l->suspended = 1;
        l->lifecycle(l->state, KAMA_APP_SUSPENDED);
    } else if (!e->hidden && l->suspended) {
        const int first = l->suspended == 2;
        l->suspended = 0;
        l->lifecycle(l->state, KAMA_APP_RESUMED);
        if (first) l->lifecycle(l->state, KAMA_APP_SURFACE_READY);
        kama__app.now = 1;
        kama__web_resume();
    }
    return 0;
}
static inline void kama_run_loop(int32_t (*tick)(void*), void (*lifecycle)(void*, int32_t), void (*idle)(uint64_t),
                                 void* state, int32_t pace, float hz) {
    kama__loop* l = (kama__loop*)kama_alloc(sizeof *l, _Alignof(kama__loop));   // lives until the process ends
    if (!l) return;
    l->tick = tick; l->lifecycle = lifecycle; l->idle = idle; l->state = state; l->suspended = 0;
    kama__app_begin(pace, hz);
    kama__app.loop = l; kama__app.timerAt = KAMA__APP_NEVER;
    kama__app.next = emscripten_get_now();
#if defined(__EMSCRIPTEN_PTHREADS__)
    kama__app.thread = pthread_self();
#endif
    EmscriptenVisibilityChangeEvent v;
    const int watched = emscripten_set_visibilitychange_callback(l, false, kama__loop_visibility) == EMSCRIPTEN_RESULT_SUCCESS;
    if (watched && emscripten_get_visibility_status(&v) == EMSCRIPTEN_RESULT_SUCCESS && v.hidden) {
        l->suspended = 2;   // hidden from the start: RESUMED and SURFACE_READY wait for the first show
    } else {
        lifecycle(state, KAMA_APP_RESUMED);
        lifecycle(state, KAMA_APP_SURFACE_READY);
    }
    emscripten_set_main_loop_arg(kama__loop_step, l, 0, 1);   // 1 = simulate infinite loop (does not return)
}
// The web cannot keep a hidden tab ticking: the browser runs no frame for it.
static inline void kama_app_tick_while_suspended(bool on) { (void)on; }
// A plain exit() is ignored while the main loop keeps the runtime alive (keepRuntimeAlive); force a true
// shutdown so the process exits with `code`.
static inline void kama_app_exit(uint32_t code) { kama__web_disarm(); emscripten_force_exit((int)code); }
// The native idle wait has no web twin: the main loop is paused instead.
static inline void kama_app_idle_wait(uint64_t by) { (void)by; }

#elif defined(KAMA_ANDROID_APP)
#include <android_native_app_glue.h>
#include <pthread.h>

// The entry's (include/kama_android_app.h): the glue's app, the thread `android_main` runs on (the loop's), the one
// input handler, whether the program asked to keep ticking while suspended, and whether a source delivered anything
// since the last tick (an input event or a command: an app on demand ticks for it).
extern struct android_app* kama__android_app;
extern pthread_t kama__android_thread;
extern bool (*kama__android_input)(void*);
extern bool kama__app_awake;

static void kama__app_host_wake(void) {
    if (kama__android_app && kama__android_app->looper) ALooper_wake(kama__android_app->looper);   // any thread
}
static void kama__app_paced(void) { }   // the loop reads the pace before its next wait

static void kama__android_cmd(struct android_app* app, int32_t cmd) {
    kama__loop* l = (kama__loop*)app->userData;
    switch (cmd) {
        case APP_CMD_RESUME:       l->suspended = 0; kama__app.now = 1; l->lifecycle(l->state, KAMA_APP_RESUMED); break;
        case APP_CMD_PAUSE:        l->suspended = 1; l->lifecycle(l->state, KAMA_APP_SUSPENDED); break;
        case APP_CMD_INIT_WINDOW:  if (app->window) { kama__app.now = 1; l->lifecycle(l->state, KAMA_APP_SURFACE_READY); } break;
        case APP_CMD_TERM_WINDOW:  l->lifecycle(l->state, KAMA_APP_SURFACE_LOST); break;   // before the glue lets it go
        case APP_CMD_GAINED_FOCUS: l->lifecycle(l->state, KAMA_APP_FOCUS_GAINED); break;
        case APP_CMD_LOST_FOCUS:   l->lifecycle(l->state, KAMA_APP_FOCUS_LOST); break;
        case APP_CMD_LOW_MEMORY:   l->lifecycle(l->state, KAMA_APP_LOW_MEMORY); break;
        default: break;
    }
}
// The looper's timeout for the first poll of a pass, in milliseconds: 0 when a tick is due, -1 to wait for a source
// or a wake, else the time to the next deadline, rounded up so the tick after it is never early.
static inline int kama__android_timeout(int ticking, uint64_t* next) {
    if (!ticking) return -1;
    uint64_t due = 0;
    if (kama__app.pace == KAMA_APP_AT_MOST) due = *next;
    else if (kama__app.pace == KAMA_APP_ON_DEMAND) due = kama__app_due();
    if (due == KAMA__APP_NEVER) return -1;
    const uint64_t now = kama_now_mono_ns();
    if (due <= now) return 0;
    const uint64_t ms = (due - now + 999999u) / 1000000u;
    return ms > 0x7fffffff ? 0x7fffffff : (int)ms;
}
// Each pass answers every pending command and input event, then ticks if the pace says one is due. It waits in the
// looper while suspended and not kept awake (no CPU, and an OpenXR session can keep polling), and while an app on
// demand has nothing due. `DESTROY` (Back, or the activity finished) ends it, after its SUSPENDED and SURFACE_LOST
// were heard, with no further tick.
static inline void kama_run_loop(int32_t (*tick)(void*), void (*lifecycle)(void*, int32_t), void (*idle)(uint64_t),
                                 void* state, int32_t pace, float hz) {
    struct android_app* app = kama__android_app;
    kama__loop l = { tick, lifecycle, idle, state, app->activityState != APP_CMD_RESUME };
    app->userData = &l;
    app->onAppCmd = kama__android_cmd;
    kama__app_begin(pace, hz);
    uint64_t next = kama_now_mono_ns();
    if (!l.suspended) lifecycle(state, KAMA_APP_RESUMED);           // a second `run` in the same activity
    if (app->window) lifecycle(state, KAMA_APP_SURFACE_READY);
    while (!app->destroyRequested) {
        const int ticking = (!l.suspended || kama__app_awake) && !app->destroyRequested;
        int timeout = kama__android_timeout(ticking, &next);
        int delivered = 0;
        for (;;) {
            struct android_poll_source* src = NULL;
            int events = 0;
            const int r = ALooper_pollOnce(timeout, NULL, &events, (void**)&src);
            timeout = 0;   // the rest of the pass drains what is already there
            if (r == ALOOPER_POLL_TIMEOUT || r == ALOOPER_POLL_ERROR || r == ALOOPER_POLL_WAKE) break;
            if (src) { src->process(app, src); delivered = 1; }
            if (app->destroyRequested) break;
        }
        if (app->destroyRequested || (l.suspended && !kama__app_awake)) continue;
        if (kama__app.pace == KAMA_APP_AT_MOST) {
            const uint64_t now = kama_now_mono_ns();
            if (now < next) continue;
            next = (next + kama__app_period_ns() > now) ? next + kama__app_period_ns() : now + kama__app_period_ns();
        } else if (kama__app.pace == KAMA_APP_ON_DEMAND && !delivered && kama__app_due() > kama_now_mono_ns()) {
            continue;
        }
        kama__app_ticking();
        if (!tick(state)) break;
    }
    kama__app_end();
    app->onAppCmd = NULL;
    app->userData = NULL;
}
static inline void kama_app_tick_while_suspended(bool on) { kama__app_awake = on; }
static inline void kama_app_exit(uint32_t code) { exit((int)code); }
static inline void kama_app_idle_wait(uint64_t by) { (void)by; }   // the looper is the wait

// std::app::android's accessors. The window is the glue's: set on the loop's thread, valid from SURFACE_READY to
// SURFACE_LOST.
static inline void* kama_app_android_window(void) { return kama__android_app->window; }
static inline void* kama_app_android_activity(void) { return kama__android_app->activity; }
// The handler runs on the loop's thread, which is what its `@callerThread` type promises, so it is set from there.
static inline void kama_app_android_set_input(bool (*handler)(void*)) {
    if (!pthread_equal(pthread_self(), kama__android_thread)) {
        static const char msg[] = "std::app::android::setInputHandler is called on the thread that runs the app's loop";
        kama_panic(kama_string_lit(msg, sizeof msg - 1));
    }
    kama__android_input = handler;
}

#else

// Native: a plain loop. Returns once tick returns 0. Nothing suspends a desktop program, so RESUMED and
// SURFACE_READY are its only events. A window package that owns the OS's event queue installs its wait
// (std::app::setEventWait): then the idle wait is the package's, called through kama's `idle`, and its wake is
// called beside the generation's notify, so input and another isolate's wake both end it.
static void kama__app_host_wake(void) {
    kama_atomic_notify(&kama__app.generation, 1);
    void (*w)(void) = __atomic_load_n(&kama__app.eventWake, __ATOMIC_ACQUIRE);
    if (w && !__atomic_exchange_n(&kama__app.pending, 1, __ATOMIC_ACQ_REL)) w();
}
static void kama__app_paced(void) { }   // the loop reads the pace before its next wait

// The idle wait with no window package: until `generation` moves on from what the last tick saw, or `by`. A wait on
// an isolate that runs fibers parks its root, so the scheduler runs the isolate's other fibers meanwhile.
static inline void kama_app_idle_wait(uint64_t by) {
    if (__atomic_load_n(&kama__app.eventWake, __ATOMIC_ACQUIRE) && !kama__app_installer)
        KAMA__APP_PANIC("std::app::setEventWait is called on the thread that runs the app's loop, and it was called on "
                        "another: the loop would never wait on the window's events");
    uint32_t seen = kama__app.seen, got;
    (void)kama_atomic_wait_until(&kama__app.generation, &seen, &got, sizeof seen, by == KAMA__APP_NEVER ? KAMA__WAIT_NONE : by);
}
// Wait until `by` whatever wakes the loop (AtMost: a wake brings no earlier tick), through the package's wait when
// one is installed, so the window is answered meanwhile.
static inline void kama__app_wait_until(void (*idle)(uint64_t), uint64_t by) {
    for (uint64_t now = kama_now_mono_ns(); now < by; now = kama_now_mono_ns()) {
        kama__app.seen = __atomic_load_n(&kama__app.generation, __ATOMIC_SEQ_CST);
        idle(by);
    }
}
static inline void kama_run_loop(int32_t (*tick)(void*), void (*lifecycle)(void*, int32_t), void (*idle)(uint64_t),
                                 void* state, int32_t pace, float hz) {
    kama__app_begin(pace, hz);
    lifecycle(state, KAMA_APP_RESUMED);
    lifecycle(state, KAMA_APP_SURFACE_READY);
    uint64_t next = kama_now_mono_ns();
    for (;;) {
        if (kama__app.pace == KAMA_APP_ON_DEMAND) {
            // Without a window package only a wake or a due request ends the wait. A package's wait returns for any
            // of its window's events too, and that is input, so it is one tick.
            const int package = __atomic_load_n(&kama__app.eventWake, __ATOMIC_ACQUIRE) != NULL;
            for (uint64_t due = kama__app_due(); due != 0 && (due == KAMA__APP_NEVER || due > kama_now_mono_ns());
                 due = kama__app_due()) {
                idle(due);
                if (package) break;
            }
            next = kama_now_mono_ns();
        } else if (kama__app.pace == KAMA_APP_AT_MOST) {
            kama__app_wait_until(idle, next);
            const uint64_t now = kama_now_mono_ns(), period = kama__app_period_ns();
            next = (next + period > now) ? next + period : now + period;
        }
        kama__app_ticking();
        if (!tick(state)) break;
    }
    kama__app_end();
}
static inline void kama_app_tick_while_suspended(bool on) { (void)on; }
static inline void kama_app_exit(uint32_t code) { exit((int)code); }
#if defined(__ANDROID__)
// An Android executable (`adb shell`) has no activity: std::app::android hands out nothing.
static inline void* kama_app_android_window(void) { return NULL; }
static inline void* kama_app_android_activity(void) { return NULL; }
static inline void kama_app_android_set_input(bool (*handler)(void*)) { (void)handler; }
#endif

#endif

// std::app::setEventWait's C half: the package's wake, process-wide, and which package it is. A second install panics
// naming both, since one package owns a desktop's OS event queue.
static inline void kama_app_set_event_wake(const char* owner, size_t n, void (*wake)(void)) {
    if (__atomic_load_n(&kama__app.eventWake, __ATOMIC_ACQUIRE)) {
        const char* first = kama__app.owner ? kama__app.owner : "?";
        static const char a[] = "std::app::setEventWait: `", b[] = "` installed the loop's event wait, and `",
                          c[] = "` installs another — one package owns the OS's event queue";
        const size_t fn = __builtin_strlen(first), len = (sizeof a - 1) + fn + (sizeof b - 1) + n + (sizeof c - 1);
        char* m = (char*)kama_alloc(len, 1);
        if (m) {
            char* p = m;
            __builtin_memcpy(p, a, sizeof a - 1); p += sizeof a - 1;
            __builtin_memcpy(p, first, fn); p += fn;
            __builtin_memcpy(p, b, sizeof b - 1); p += sizeof b - 1;
            __builtin_memcpy(p, owner, n); p += n;
            __builtin_memcpy(p, c, sizeof c - 1);
            kama_panic(kama_string_lit(m, len));
        }
        KAMA__APP_PANIC("std::app::setEventWait: a second package installs the loop's event wait");
    }
    char* copy = (char*)kama_alloc(n + 1, 1);   // kept for the program's life, as the install is
    if (copy) { __builtin_memcpy(copy, owner, n); copy[n] = 0; }
    kama__app.owner = copy;
    kama__app_installer = 1;
    __atomic_store_n(&kama__app.eventWake, wake, __ATOMIC_RELEASE);
}
static inline bool kama_app_on_loop_thread(void) { return kama__app_here != 0; }

#endif  // KAMA_APP_H
