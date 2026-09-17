// A dependency's C++ shim and its `cxxflags`, compiled into the consumer's build — which then links with
// the C++ driver although the consumer itself names no C++ at all.
#include <numeric>
#include <vector>
#include "shim.h"

int kama_test_cxxshim_sum(int n) {
    std::vector<int> v(n);
    std::iota(v.begin(), v.end(), 1);
    return std::accumulate(v.begin(), v.end(), 0) * KAMA_TEST_SHIM_SCALE;
}
