/* Ungated: compiled everywhere. `which.h` comes from whichever gated `cincludes` directory is on the path. */
#include "gated.h"
#include "which.h"

int kama_test_gated_include(void) { return KAMA_TEST_WHICH_IS_RIGHT; }
