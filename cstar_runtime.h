#ifndef CSTAR_RUNTIME_H
#define CSTAR_RUNTIME_H

// Minimal cstar runtime. Dependency-light and WASM-safe: it leans only on the
// freestanding-friendly C headers so generated code compiles for native and
// for clang/emscripten wasm targets alike. Generated translation units include
// this first, then their own module header.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>   // malloc/calloc/realloc/free/abort
#include <string.h>   // memcpy/memcmp
#include <stdio.h>    // fprintf (bounds trap)

// ---- Collections (M9) -----------------------------------------------------
// Generic collections are monomorphized per element type from these templates.
// All raw-pointer / heap access is confined HERE (trusted compiler runtime) —
// the cstar surface stays pointer-free and safe. Indexing is bounds-checked.

// A no-op per-element destructor, used when the element type isn't destructible.
#define CSTAR_ELEM_NODTOR(p) ((void)(p))

// Bounds-check trap: a clean panic (not undefined behavior) on out-of-range.
static inline void cstar_bounds_fail(size_t i, size_t len) {
    fprintf(stderr, "cstar: index %zu out of bounds (length %zu)\n",
            (size_t)i, (size_t)len);
    abort();
}

// Array<T> — fixed-size, owns a zero-initialized contiguous buffer (RAII frees).
#define CSTAR_ARRAY_DEFINE(T, NAME, ELEM_DTOR)                                  \
typedef struct NAME { T* data; size_t len; } NAME;                             \
static inline void NAME##__ctor(NAME* self, size_t n) {                        \
    self->len  = n;                                                            \
    self->data = (n ? (T*)calloc(n, sizeof(T)) : NULL);                        \
}                                                                              \
static inline void NAME##__dtor(NAME* self) {                                  \
    for (size_t i = 0; i < self->len; ++i) { T* e = &self->data[i]; ELEM_DTOR(e); } \
    free(self->data); self->data = NULL; self->len = 0;                        \
}                                                                              \
static inline T      NAME##__get(NAME* self, size_t i) {                       \
    if (i >= self->len) cstar_bounds_fail(i, self->len);                       \
    return self->data[i];                                                      \
}                                                                              \
static inline void   NAME##__set(NAME* self, size_t i, T v) {                  \
    if (i >= self->len) cstar_bounds_fail(i, self->len);                       \
    self->data[i] = v;                                                         \
}                                                                              \
static inline size_t NAME##__length(NAME* self) { return self->len; }

// List<T> — growable (capacity doubling), owns its buffer (RAII frees).
#define CSTAR_LIST_DEFINE(T, NAME, ELEM_DTOR)                                   \
typedef struct NAME { T* data; size_t len; size_t cap; } NAME;                 \
static inline void NAME##__ctor(NAME* self) { self->data=NULL; self->len=0; self->cap=0; } \
static inline void NAME##__dtor(NAME* self) {                                  \
    for (size_t i = 0; i < self->len; ++i) { T* e = &self->data[i]; ELEM_DTOR(e); } \
    free(self->data); self->data = NULL; self->len = 0; self->cap = 0;         \
}                                                                              \
static inline void   NAME##__add(NAME* self, T v) {                            \
    if (self->len == self->cap) {                                             \
        size_t nc = self->cap ? self->cap * 2 : 4;                            \
        self->data = (T*)realloc(self->data, nc * sizeof(T));                 \
        self->cap  = nc;                                                      \
    }                                                                         \
    self->data[self->len++] = v;                                             \
}                                                                              \
static inline T      NAME##__get(NAME* self, size_t i) {                       \
    if (i >= self->len) cstar_bounds_fail(i, self->len);                       \
    return self->data[i];                                                      \
}                                                                              \
static inline void   NAME##__set(NAME* self, size_t i, T v) {                  \
    if (i >= self->len) cstar_bounds_fail(i, self->len);                       \
    self->data[i] = v;                                                         \
}                                                                              \
static inline size_t NAME##__length(NAME* self) { return self->len; }


// cstar `string` lowers to a fat, length-prefixed view. `cap == 0` means the
// bytes are borrowed (e.g. a C string literal) and must not be freed; `cap > 0`
// will mean heap-owned once mutable strings land (RAII frees it).
typedef struct cstar_string {
    const char* data;  // UTF-8 bytes, not necessarily NUL terminated
    size_t      len;   // byte length
    size_t      cap;   // 0 => borrowed/literal, >0 => heap-owned
} cstar_string;

static inline cstar_string cstar_string_lit(const char* s, size_t n) {
    cstar_string r;
    r.data = s;
    r.len  = n;
    r.cap  = 0;
    return r;
}

// Tiny tracing hook for tests/debugging: a folding accumulator that records a
// sequence of integer events (e.g. constructor/destructor order). Declare in
// cstar with `extern void cstar_trace(int code);` / `extern int cstar_trace_get();`.
// Single-TU builds only (definition lives in this header).
static int cstar_trace_acc = 0;
static inline void cstar_trace(int code) { cstar_trace_acc = cstar_trace_acc * 31 + code; }
static inline int  cstar_trace_get(void) { return cstar_trace_acc; }

#endif // CSTAR_RUNTIME_H
