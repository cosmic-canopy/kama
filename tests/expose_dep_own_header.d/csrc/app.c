/* The project's own C: its header declares the package's `expose fn`s as well as its own. */
#include "app.h"
#include "hdrapp.kama.h"

int32_t app_c(void) { return cb_tick() - hdrapp_two(); }   /* 4 - 2 */
