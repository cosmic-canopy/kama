/* The package's own C, calling the package's own `expose fn` by its link name. */
#include "cb.h"

int32_t cb_tick(void);

int32_t cb_c_run(void) { return cb_tick() + 1; }
