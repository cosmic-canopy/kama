#include "greet.h"
// Every target that is not Apple's has no Swift, so it has its own C (the `csources` entry is gated `!DARWIN`).
int32_t greet_add(int32_t a, int32_t b) { return a + b; }
int32_t greet_count(const char* s) {   // UTF-8: a byte that does not continue a sequence starts a character
    int32_t n = 0;
    for (; *s; ++s) if (((unsigned char)*s & 0xC0) != 0x80) ++n;
    return n;
}
