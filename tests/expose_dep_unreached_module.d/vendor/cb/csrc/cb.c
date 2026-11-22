/* The package's own C, calling two of the package's `expose fn`s by their link names. */
#include "cb.h"

int32_t cb_tick(void);
int32_t cb_tock(void);

int32_t cb_c_run(void) { return cb_tick() + cb_tock(); }
