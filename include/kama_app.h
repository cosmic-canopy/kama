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

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "kama_runtime.h"   /* kama_alloc, kama_panic */

// std::app's `Lifecycle` discriminants (lib/std/app/app.kama).
enum {
    KAMA_APP_RESUMED = 0, KAMA_APP_SUSPENDED = 1, KAMA_APP_SURFACE_READY = 2, KAMA_APP_SURFACE_LOST = 3,
    KAMA_APP_FOCUS_GAINED = 4, KAMA_APP_FOCUS_LOST = 5, KAMA_APP_LOW_MEMORY = 6
};

typedef struct kama__loop {
    int32_t (*tick)(void*);
    void (*lifecycle)(void*, int32_t);
    void* state;
    int suspended;
} kama__loop;

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include <emscripten/html5.h>

// emscripten's callback is void(void*); the loop is one heap arg (leaked — the program exits when the loop ends).
// fps 0 = requestAnimationFrame in a browser, which stops while the tab is hidden; an event-loop tick under Node.
static void kama__loop_step(void* arg) {
    kama__loop* l = (kama__loop*)arg;
    if (l->suspended) return;   // a browser keeps no rAF for a hidden tab; a timer-driven host still might call
    if (!l->tick(l->state)) emscripten_cancel_main_loop();
}
// The DOM's visibilitychange: hidden is SUSPENDED and shown is RESUMED. A page that starts hidden hears RESUMED,
// and SURFACE_READY, when it is first shown. Node has no document, so the registration fails there and the
// program stays resumed.
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
    }
    return 0;
}
static inline void kama_run_loop(int32_t (*tick)(void*), void (*lifecycle)(void*, int32_t), void* state) {
    kama__loop* l = (kama__loop*)kama_alloc(sizeof *l, _Alignof(kama__loop));   // lives until the process ends
    if (!l) return;
    l->tick = tick; l->lifecycle = lifecycle; l->state = state; l->suspended = 0;
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
static inline void kama_app_exit(uint32_t code) { emscripten_force_exit((int)code); }

#elif defined(KAMA_ANDROID_APP)
#include <android_native_app_glue.h>
#include <pthread.h>

// The entry's (include/kama_android_app.h): the glue's app, the thread `android_main` runs on (the loop's), the one
// input handler, and whether the program asked to keep ticking while suspended.
extern struct android_app* kama__android_app;
extern pthread_t kama__android_thread;
extern bool (*kama__android_input)(void*);
extern bool kama__app_awake;

static void kama__android_cmd(struct android_app* app, int32_t cmd) {
    kama__loop* l = (kama__loop*)app->userData;
    switch (cmd) {
        case APP_CMD_RESUME:       l->suspended = 0; l->lifecycle(l->state, KAMA_APP_RESUMED); break;
        case APP_CMD_PAUSE:        l->suspended = 1; l->lifecycle(l->state, KAMA_APP_SUSPENDED); break;
        case APP_CMD_INIT_WINDOW:  if (app->window) l->lifecycle(l->state, KAMA_APP_SURFACE_READY); break;
        case APP_CMD_TERM_WINDOW:  l->lifecycle(l->state, KAMA_APP_SURFACE_LOST); break;   // before the glue lets it go
        case APP_CMD_GAINED_FOCUS: l->lifecycle(l->state, KAMA_APP_FOCUS_GAINED); break;
        case APP_CMD_LOST_FOCUS:   l->lifecycle(l->state, KAMA_APP_FOCUS_LOST); break;
        case APP_CMD_LOW_MEMORY:   l->lifecycle(l->state, KAMA_APP_LOW_MEMORY); break;
        default: break;
    }
}
// Each pass answers every pending command and input event, then ticks. It waits in the looper only while suspended
// and not kept awake, so a paused app uses no CPU and an OpenXR session can keep polling. `DESTROY` (Back, or the
// activity finished) ends it, after its SUSPENDED and SURFACE_LOST were heard, with no further tick.
static inline void kama_run_loop(int32_t (*tick)(void*), void (*lifecycle)(void*, int32_t), void* state) {
    struct android_app* app = kama__android_app;
    kama__loop l = { tick, lifecycle, state, app->activityState != APP_CMD_RESUME };
    app->userData = &l;
    app->onAppCmd = kama__android_cmd;
    if (!l.suspended) lifecycle(state, KAMA_APP_RESUMED);           // a second `run` in the same activity
    if (app->window) lifecycle(state, KAMA_APP_SURFACE_READY);
    while (!app->destroyRequested) {
        for (;;) {
            struct android_poll_source* src = NULL;
            int events = 0;
            const int wait = l.suspended && !kama__app_awake && !app->destroyRequested;
            const int r = ALooper_pollOnce(wait ? -1 : 0, NULL, &events, (void**)&src);
            if (r == ALOOPER_POLL_TIMEOUT || r == ALOOPER_POLL_ERROR || r == ALOOPER_POLL_WAKE) break;
            if (src) src->process(app, src);
            if (app->destroyRequested) break;
        }
        if (app->destroyRequested || (l.suspended && !kama__app_awake)) continue;
        if (!tick(state)) break;
    }
    app->onAppCmd = NULL;
    app->userData = NULL;
}
static inline void kama_app_tick_while_suspended(bool on) { kama__app_awake = on; }
static inline void kama_app_exit(uint32_t code) { exit((int)code); }

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
// SURFACE_READY are its only events.
static inline void kama_run_loop(int32_t (*tick)(void*), void (*lifecycle)(void*, int32_t), void* state) {
    lifecycle(state, KAMA_APP_RESUMED);
    lifecycle(state, KAMA_APP_SURFACE_READY);
    while (tick(state)) { }
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
#endif  // KAMA_APP_H
