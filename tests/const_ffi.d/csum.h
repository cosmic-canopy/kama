/* A const-correct C API: kama declares it as `const UnsafePtr<int32>` and calls it with
   no cast (the kama signature spells `const int32_t*` exactly). */
#include <stdint.h>
static int32_t csum(const int32_t* a, int32_t n) {
    int32_t s = 0;
    for (int32_t i = 0; i < n; i++) s += a[i];
    return s;
}
