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

// The four, defined once per bare-metal program: the entry unit defines KAMA__MEM_DEFINE and includes this header again
// (emitRuntimeSlotDefinitions), and the block sits outside the include guard so that second include reaches it. WEAK, so a
// board that links a C library, or its own tuned versions, wins; a word at a time where both ends are aligned, so a copy
// is not a byte loop. A compiler recognizes a copy loop and lowers it to a call to memcpy, which inside memcpy is itself:
// `no_builtin` (clang) and the loop-distribution switch (GCC) turn that off for these bodies alone.
#if defined(KAMA_TARGET_EMBEDDED) && defined(KAMA__MEM_DEFINE) && !defined(KAMA__MEM_DEFINED)
#define KAMA__MEM_DEFINED 1
#include <stdint.h>
#if defined(__clang__)
  #define KAMA__MEM_DEF(name) __attribute__((weak, no_builtin(#name)))
#else
  #define KAMA__MEM_DEF(name) __attribute__((weak, optimize("no-tree-loop-distribute-patterns")))
#endif
typedef uintptr_t __attribute__((may_alias)) kama__mem_word;   /* any object's bytes, read a word at a time */
#define KAMA__MEM_W sizeof(kama__mem_word)
#define KAMA__MEM_ALIGNED(a, b) ((((uintptr_t)(a) | (uintptr_t)(b)) & (KAMA__MEM_W - 1)) == 0)

KAMA__MEM_DEF(memcpy) void* memcpy(void* restrict dst, const void* restrict src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (KAMA__MEM_ALIGNED(d, s))
        for (; n >= KAMA__MEM_W; n -= KAMA__MEM_W, d += KAMA__MEM_W, s += KAMA__MEM_W)
            *(kama__mem_word*)d = *(const kama__mem_word*)s;
    while (n--) *d++ = *s++;
    return dst;
}

KAMA__MEM_DEF(memmove) void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d == s || n == 0) return dst;
    if (d < s || d >= s + n) {   /* forward cannot overwrite a source byte before it is read */
        if (KAMA__MEM_ALIGNED(d, s))
            for (; n >= KAMA__MEM_W; n -= KAMA__MEM_W, d += KAMA__MEM_W, s += KAMA__MEM_W)
                *(kama__mem_word*)d = *(const kama__mem_word*)s;
        while (n--) *d++ = *s++;
    } else {                     /* the destination overlaps the source's tail: copy from the end */
        d += n; s += n;
        if (KAMA__MEM_ALIGNED(d, s))
            for (; n >= KAMA__MEM_W; n -= KAMA__MEM_W) {
                d -= KAMA__MEM_W; s -= KAMA__MEM_W;
                *(kama__mem_word*)d = *(const kama__mem_word*)s;
            }
        while (n--) *--d = *--s;
    }
    return dst;
}

KAMA__MEM_DEF(memset) void* memset(void* dst, int c, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char b = (unsigned char)c;
    if (((uintptr_t)d & (KAMA__MEM_W - 1)) == 0 && n >= KAMA__MEM_W) {
        kama__mem_word w = 0;
        for (size_t i = 0; i < KAMA__MEM_W; ++i) w = (w << 8) | b;
        for (; n >= KAMA__MEM_W; n -= KAMA__MEM_W, d += KAMA__MEM_W) *(kama__mem_word*)d = w;
    }
    while (n--) *d++ = b;
    return dst;
}

KAMA__MEM_DEF(memcmp) int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* x = (const unsigned char*)a;
    const unsigned char* y = (const unsigned char*)b;
    for (; n; --n, ++x, ++y)
        if (*x != *y) return *x < *y ? -1 : 1;
    return 0;
}
#endif
