#ifndef KAMA_APP_RUNNER_H
#define KAMA_APP_RUNNER_H

// std::app::runner's bridge (KR-378), for the wasm target. The bodies live in kama_app_runner.js, linked by emcc via
// `--js-library` (the driver adds it when a program externs this header). Kama IMPORTS these, as it does kama_os.h, so
// no wasm exports, `expose` or Asyncify are involved.
//
// A request is an int32 index into a JS-side table, built a step at a time and then sent; its state is polled.

#include <stdint.h>
#include <stddef.h>

extern int32_t kama_app_runner_begin(void);   // -> handle, or -1: no runner served this page
extern void    kama_app_runner_step(int32_t h, int32_t kind, int32_t pointer, int32_t id, int32_t button,
                                    double x, double y, double dx, double dy, const char* text);   // kinds: the .js
extern void    kama_app_runner_send(int32_t h);
extern int32_t kama_app_runner_state(int32_t h);   // 0 Pending, 1 Done, 2 Refused
extern int32_t kama_app_runner_reason(int32_t h, uint8_t* buf, size_t n);   // FULL length; min(len, n) copied
extern void    kama_app_runner_drop(int32_t h);

#endif
