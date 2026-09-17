/* Gated `ARCH_WASM32`. Right only where the compiler is emscripten. */
#include "gated.h"

int kama_test_gated_side(void) { return
#ifdef __EMSCRIPTEN__
    1;
#else
    100;
#endif
}
