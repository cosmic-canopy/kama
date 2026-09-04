/* A package's OWN C, compiled by whoever consumes the package. This is the shape a binding wrapper
   has: kama declarations in src/, the C that binds the library beside them. */
#include "shim.h"

int kama_test_shim_triple(int n) { return n * 3; }
