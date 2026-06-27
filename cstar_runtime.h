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

// Owned<T> — unique heap ownership (Box / unique_ptr). Move-only; RAII frees.
// The struct + dtor live here; the heap alloc + T's constructor are emitted
// INLINE by the compiler (it knows T's ctor + named-arg order). ELEM_DTOR runs
// T's destructor on the pointee before free. A moved-from Owned has ptr==NULL,
// so its dtor is a safe no-op — the single surviving owner frees exactly once.
#define CSTAR_OWNED_DEFINE(T, NAME, ELEM_DTOR)                                  \
typedef struct NAME { T* ptr; } NAME;                                          \
static inline void NAME##__dtor(NAME* self) {                                  \
    if (self->ptr) { ELEM_DTOR(self->ptr); free(self->ptr); self->ptr = NULL; } \
}

// Shared<T> — ref-counted shared ownership (shared_ptr / Rc). Copy retains;
// drop releases; the pointee is destroyed + freed when the last strong handle
// goes away. The control block (counts) is a SEPARATE allocation so a future
// Weak<T> can outlive the T. `ptr` mirrors Owned's, so auto-deref is identical.
typedef struct cstar_ctrl { size_t strong; size_t weak; } cstar_ctrl;   // weak: reserved for Weak<T>
static inline cstar_ctrl* cstar_ctrl_new(void) {
    cstar_ctrl* c = (cstar_ctrl*)malloc(sizeof(cstar_ctrl));
    c->strong = 1; c->weak = 0;
    return c;
}
#define CSTAR_SHARED_DEFINE(T, NAME, ELEM_DTOR)                                 \
typedef struct NAME { T* ptr; cstar_ctrl* ctrl; } NAME;                        \
static inline void NAME##__dtor(NAME* self) {                                  \
    if (self->ctrl) {                                                          \
        if (--self->ctrl->strong == 0) {                                       \
            ELEM_DTOR(self->ptr); free(self->ptr);                            \
            if (self->ctrl->weak == 0) free(self->ctrl);                       \
        }                                                                      \
        self->ptr = NULL; self->ctrl = NULL;                                  \
    }                                                                          \
}                                                                              \
static inline bool NAME##__valid(NAME* self) { return self->ptr != NULL; }

// Weak<T> — a non-owning reference to a Shared<T>'s pointee. Counts `weak`, not
// `strong`, so it does NOT keep the pointee alive (it breaks Shared cycles). You
// cannot deref a Weak directly; `lock()` upgrades to a Shared if still alive.
// Same layout as Shared. Drop releases the weak count and frees the control
// block only when BOTH counts reach 0 (never touches the pointee).
#define CSTAR_WEAK_DEFINE(T, NAME, SHARED_NAME)                                 \
typedef struct NAME { T* ptr; cstar_ctrl* ctrl; } NAME;                        \
static inline void NAME##__dtor(NAME* self) {                                  \
    if (self->ctrl) {                                                          \
        if (--self->ctrl->weak == 0 && self->ctrl->strong == 0) free(self->ctrl); \
        self->ptr = NULL; self->ctrl = NULL;                                  \
    }                                                                          \
}                                                                              \
static inline bool NAME##__expired(NAME* self) {                              \
    return self->ctrl == NULL || self->ctrl->strong == 0;                     \
}                                                                              \
static inline SHARED_NAME NAME##__lock(NAME* self) {                          \
    SHARED_NAME s;                                                            \
    if (self->ctrl && self->ctrl->strong > 0) {                              \
        self->ctrl->strong++; s.ptr = self->ptr; s.ctrl = self->ctrl;        \
    } else { s.ptr = NULL; s.ctrl = NULL; }                                   \
    return s;                                                                 \
}

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


// cstar `string` lowers to a fat, length-prefixed value (M9). `cap == 0` means
// the bytes are BORROWED (e.g. a C string literal in static storage) and must
// never be written or freed; `cap > 0` means HEAP-OWNED (NUL-terminated) and is
// freed by RAII. All string ops read uniformly; only concat allocates. Raw
// memory stays confined here — the cstar surface sees only a safe `string`.
typedef struct cstar_string {
    char*  data;   // UTF-8 bytes; borrowed (cap==0) bytes are never mutated/freed
    size_t len;    // byte length
    size_t cap;    // 0 => borrowed/literal, >0 => heap-owned
} cstar_string;

// Borrowed view of a string literal (static storage; valid for the whole run).
static inline cstar_string cstar_string_lit(const char* s, size_t n) {
    cstar_string r;
    r.data = (char*)s;   // never written/freed while cap==0
    r.len  = n;
    r.cap  = 0;
    return r;
}

// RAII: free only heap-owned strings; borrowed views are a no-op.
static inline void cstar_string__dtor(cstar_string* self) {
    if (self->cap) free(self->data);
    self->data = NULL; self->len = 0; self->cap = 0;
}
static inline size_t cstar_string__length(cstar_string* self) { return self->len; }
// FFI (M16): the underlying NUL-terminated bytes, for passing to a C `const char*`.
static inline char* cstar_string__cstr(cstar_string* self) { return self->data; }
static inline bool cstar_string__equals(cstar_string* self, cstar_string other) {
    return self->len == other.len &&
           (self->len == 0 || memcmp(self->data, other.data, self->len) == 0);
}
// Returns a fresh heap-owned string (the caller binds it -> RAII frees it).
static inline cstar_string cstar_string__concat(cstar_string* self, cstar_string other) {
    size_t n = self->len + other.len;
    char*  buf = (char*)malloc(n + 1);
    if (self->len) memcpy(buf, self->data, self->len);
    if (other.len) memcpy(buf + self->len, other.data, other.len);
    buf[n] = '\0';
    cstar_string r; r.data = buf; r.len = n; r.cap = n + 1; return r;
}

// Tiny tracing hook for tests/debugging: a folding accumulator that records a
// sequence of integer events (e.g. constructor/destructor order). Declare in
// cstar with `extern void cstar_trace(int code);` / `extern int cstar_trace_get();`.
// Single-TU builds only (definition lives in this header).
static int cstar_trace_acc = 0;
static inline void cstar_trace(int code) { cstar_trace_acc = cstar_trace_acc * 31 + code; }
static inline int  cstar_trace_get(void) { return cstar_trace_acc; }

#endif // CSTAR_RUNTIME_H
