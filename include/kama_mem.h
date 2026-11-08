#ifndef KAMA_MEM_H
#define KAMA_MEM_H

// memcpy, memmove, memset and memcmp, for the runtime headers and std that call them, from the one place that knows where
// they come from. A hosted build takes them from <string.h>. A bare-metal build (`--target embedded`, any `os=none`
// triple) reads no C library header at all (KR-341): a compiler with no sysroot has none to read, and these four are the
// ones GCC and clang require of every freestanding link anyway, so they are declared here, and the entry unit defines
// them weak, so a board with no C library links and one with a tuned version (newlib's, the board's own) wins.

#include <stddef.h>

#if defined(KAMA_TARGET_EMBEDDED)
void* memcpy(void* restrict dst, const void* restrict src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
void* memset(void* dst, int c, size_t n);
int   memcmp(const void* a, const void* b, size_t n);
#else
#include <string.h>
#endif

#endif  // KAMA_MEM_H
