#ifndef KAMA_APP_RUNNER_H
#define KAMA_APP_RUNNER_H

// std::app::runner's bridge (KR-378, KR-397). Kama IMPORTS these, as it does kama_os.h, so no wasm exports, `expose` or
// Asyncify are involved. A request is an int32 index into the bridge's table, built a step at a time and then sent;
// its state is polled.
//
// Two runners can serve a program, and each brings the bodies:
//   * wasm: kama_app_runner.js, linked by emcc via `--js-library` (the driver adds it when a program externs this
//     header), talking to `kama run`'s browser runner (src/webrun/runner.mjs);
//   * an Android app: kama_android_app.h, in the entry unit, talking to `kama run`'s device runner (the driver) over an
//     abstract UNIX socket the app listens on.
// Everywhere else nothing can run a program that asks, so `begin` answers -1 here and the program hears NoRunner.

#include <stdint.h>
#include <stddef.h>

#if defined(__EMSCRIPTEN__) || defined(KAMA_ANDROID_APP)
extern int32_t kama_app_runner_begin(void);   // -> handle, or -1: no runner started this program
extern void    kama_app_runner_step(int32_t h, int32_t kind, int32_t pointer, int32_t id, int32_t button,
                                    double x, double y, double dx, double dy, const char* text);   // kinds: the .js
extern void    kama_app_runner_send(int32_t h);
extern int32_t kama_app_runner_state(int32_t h);   // 0 Pending, 1 Done, 2 Refused
extern int32_t kama_app_runner_reason(int32_t h, uint8_t* buf, size_t n);   // FULL length; min(len, n) copied
extern void    kama_app_runner_drop(int32_t h);
#else
static inline int32_t kama_app_runner_begin(void) { return -1; }
static inline void kama_app_runner_step(int32_t h, int32_t kind, int32_t pointer, int32_t id, int32_t button,
                                        double x, double y, double dx, double dy, const char* text) {
    (void)h; (void)kind; (void)pointer; (void)id; (void)button; (void)x; (void)y; (void)dx; (void)dy; (void)text;
}
static inline void kama_app_runner_send(int32_t h) { (void)h; }
static inline int32_t kama_app_runner_state(int32_t h) { (void)h; return 2; }
static inline int32_t kama_app_runner_reason(int32_t h, uint8_t* buf, size_t n) { (void)h; (void)buf; (void)n; return 0; }
static inline void kama_app_runner_drop(int32_t h) { (void)h; }
#endif

#endif
