#ifndef CSTAR_RUNTIME_H
#define CSTAR_RUNTIME_H

// Minimal cstar runtime. Dependency-light and WASM-safe: it leans only on the
// freestanding-friendly C headers so generated code compiles for native and
// for clang/emscripten wasm targets alike. Generated translation units include
// this first, then their own module header.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

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
