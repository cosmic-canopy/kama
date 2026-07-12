#ifndef KAMA_APP_H
#define KAMA_APP_H

// The application run-loop shim for `std::app` — the ONE place the native-vs-web frame-loop split lives.
// A synchronous busy-loop on the web never sees async events (sockets/timers), because the JS event loop
// only runs when the wasm program yields to it; emscripten's main loop is that yield. Pulled in only by a
// program that `extern "kama_app.h";`'s (i.e. imports std::app), so non-loop programs pay nothing.
//
// `tick(state)` returns 1 to keep running, 0 to stop. On the web, run does NOT return (the loop is driven by
// the host); the tick ends the program itself (e.g. via exit()) when its work is done.

#include <stdlib.h>
#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// emscripten's callback is void(void*); pack the kama tick + its state into one heap arg (leaked — the
// program exits when the loop ends). fps 0 = requestAnimationFrame in a browser / event-loop tick under Node.
typedef struct kama__loop { int (*tick)(void*); void* state; } kama__loop;
static void kama__loop_step(void* arg) {
    kama__loop* l = (kama__loop*)arg;
    if (!l->tick(l->state)) emscripten_cancel_main_loop();
}
static inline void kama_run_loop(int (*tick)(void*), void* state) {
    kama__loop* l = (kama__loop*)malloc(sizeof *l);
    if (!l) return;
    l->tick = tick; l->state = state;
    emscripten_set_main_loop_arg(kama__loop_step, l, 0, 1);   // 1 = simulate infinite loop (does not return)
}
// A plain exit() is ignored while the main loop keeps the runtime alive (keepRuntimeAlive); force a true
// shutdown so the process exits with `code`.
static inline void kama_app_exit(int32_t code) { emscripten_force_exit((int)code); }

#else

// Native: a plain loop. Returns once tick returns 0.
static inline void kama_run_loop(int (*tick)(void*), void* state) {
    while (tick(state)) { }
}
static inline void kama_app_exit(int32_t code) { exit((int)code); }

#endif
#endif  // KAMA_APP_H
