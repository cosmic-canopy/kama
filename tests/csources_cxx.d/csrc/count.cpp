// A C++ `csources` entry. What is under test: it compiles as C++17 (`__cplusplus`), `cxxflags` reach it,
// and the link brings the C++ runtime — `std::string` and `std::vector` need it, and a static initializer
// only runs if the runtime's startup does.
#include <string>
#include <vector>
#include "count.h"

static std::vector<std::string> kama_test_words{ "one", "three" };

int kama_test_count_cxx(void) {
    const int std17 = __cplusplus == 201703L ? 0 : 100;
    return (int)kama_test_words[1].size() + (int)kama_test_words.size() + KAMA_TEST_CXX_BONUS + std17;   // 5 + 2 + 4
}
