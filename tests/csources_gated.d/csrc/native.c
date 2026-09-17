/* Gated `!ARCH_WASM32`. Right only where the compiler is NOT emscripten. */
#include "gated.h"

int kama_test_gated_side(void) { return
#ifdef __EMSCRIPTEN__
    100;
#else
    1;
#endif
}
