#ifndef CSTAR_RUNTIME_H
#define CSTAR_RUNTIME_H

// Minimal cstar runtime. Dependency-light and WASM-safe: it leans only on the
// freestanding-friendly C headers so generated code compiles for native and
// for clang/emscripten wasm targets alike. Generated translation units include
// this first, then their own module header.

#include <stdint.h>    // int32_t … (types only — no callable C functions)
#include <stdbool.h>   // bool       (type only)
#include <stddef.h>    // size_t, NULL (types only)

// The runtime needs a few libc functions (malloc/free/memcpy/…) for collections,
// strings, and the bounds trap. It declares them at BLOCK scope inside these
// wrappers, NOT via <stdlib.h>/<string.h>/<stdio.h> — so those declarations stay
// invisible to user code. Consequence (and the point): EVERY C function a cstar
// program calls must be brought in explicitly with `extern "<header.h>";`. The
// runtime's own dependencies never leak. (All raw memory access is confined here.)
static inline void* cstar_alloc(size_t n)              { extern void* malloc(size_t);                  return malloc(n); }
static inline void* cstar_calloc(size_t count, size_t size) { extern void* calloc(size_t, size_t);     return calloc(count, size); }
static inline void* cstar_realloc(void* p, size_t n)   { extern void* realloc(void*, size_t);          return realloc(p, n); }
static inline void  cstar_free(void* p)                { extern void  free(void*);                     free(p); }
static inline void  cstar_copy(void* d, const void* s, size_t n) { extern void* memcpy(void*, const void*, size_t); memcpy(d, s, n); }
static inline int   cstar_cmp(const void* a, const void* b, size_t n) { extern int memcmp(const void*, const void*, size_t); return memcmp(a, b, n); }

// ---- Collections ----------------------------------------------------------
// Generic collections are monomorphized per element type from these templates.
// The cstar surface stays pointer-free and safe. Indexing is bounds-checked.

// A no-op per-element destructor, used when the element type isn't destructible.
#define CSTAR_ELEM_NODTOR(p) ((void)(p))

// Per-element copy. A bitwise-copyable element (owns nothing) copies memberwise; a
// `Copyable` resource element deep-copies via its own `Elem__copy(&e)`. Given an element POINTER,
// both yield the copied element BY VALUE, so `NAME##__copy` assigns `r.data[i] = ELEM_COPY(&src[i])`.
#define CSTAR_ELEM_MEMBERWISE(e) (*(e))

// Owned<T> — unique heap ownership (Box / unique_ptr). Move-only; RAII frees.
// The struct + dtor live here; the heap alloc + T's constructor are emitted
// INLINE by the compiler (it knows T's ctor + named-arg order). ELEM_DTOR runs
// T's destructor on the pointee before free. A moved-from Owned has ptr==NULL,
// so its dtor is a safe no-op — the single surviving owner frees exactly once.
// Split into _TYPE (the struct — needs only T forward-declared, since it stores T*)
// and _FUNCS (the dtor — needs T's dtor). The emitter emits all _TYPEs before class
// struct bodies (so a class may hold a collection/smart-ptr BY VALUE as a field) and
// all _FUNCS after class prototypes (where element dtors are declared).
#define CSTAR_OWNED_TYPE(T, NAME) typedef struct NAME { T* ptr; } NAME;
#define CSTAR_OWNED_FUNCS(T, NAME, ELEM_DTOR)                                   \
static inline void NAME##__dtor(NAME* self) {                                  \
    if (self->ptr) { ELEM_DTOR(self->ptr); cstar_free(self->ptr); self->ptr = NULL; } \
}
#define CSTAR_OWNED_DEFINE(T, NAME, ELEM_DTOR) CSTAR_OWNED_TYPE(T, NAME) CSTAR_OWNED_FUNCS(T, NAME, ELEM_DTOR)

// Owned<I> over a CONTRACT — a unique-owning fat pointer: the handle IS the contract
// fat pointer {obj, vtbl}, with `obj` the heap-owned CONCRETE object. Drop dispatches the concrete
// destructor through the vtable's `__dtor` slot (NULL for a non-destructible impl), then frees obj.
#define CSTAR_OWNED_IFACE_TYPE(NAME, VTBL) typedef struct NAME { void* obj; const VTBL* vtbl; } NAME;
#define CSTAR_OWNED_IFACE_FUNCS(NAME)                                          \
static inline void NAME##__dtor(NAME* self) {                                  \
    if (self->obj) {                                                           \
        if (self->vtbl && self->vtbl->__dtor) self->vtbl->__dtor(self->obj);   \
        cstar_free(self->obj); self->obj = NULL;                              \
    }                                                                          \
}

// Shared<T> — ref-counted shared ownership (shared_ptr / Rc). Copy retains;
// drop releases; the pointee is destroyed + freed when the last strong handle
// goes away. The control block (counts) is a SEPARATE allocation so a future
// Weak<T> can outlive the T. `ptr` mirrors Owned's, so auto-deref is identical.
typedef struct cstar_ctrl { size_t strong; size_t weak; } cstar_ctrl;   // weak: reserved for Weak<T>
static inline cstar_ctrl* cstar_ctrl_new(void) {
    cstar_ctrl* c = (cstar_ctrl*)cstar_alloc(sizeof(cstar_ctrl));
    c->strong = 1; c->weak = 0;
    return c;
}
#define CSTAR_SHARED_TYPE(T, NAME) typedef struct NAME { T* ptr; cstar_ctrl* ctrl; } NAME;
#define CSTAR_SHARED_FUNCS(T, NAME, ELEM_DTOR)                                  \
static inline void NAME##__dtor(NAME* self) {                                  \
    if (self->ctrl) {                                                          \
        if (--self->ctrl->strong == 0) {                                       \
            ELEM_DTOR(self->ptr); cstar_free(self->ptr);                            \
            if (self->ctrl->weak == 0) cstar_free(self->ctrl);                       \
        }                                                                      \
        self->ptr = NULL; self->ctrl = NULL;                                  \
    }                                                                          \
}                                                                              \
static inline bool NAME##__valid(NAME* self) { return self->ptr != NULL; }
#define CSTAR_SHARED_DEFINE(T, NAME, ELEM_DTOR) CSTAR_SHARED_TYPE(T, NAME) CSTAR_SHARED_FUNCS(T, NAME, ELEM_DTOR)

// Shared<I> over a CONTRACT — ref-counted fat pointer {obj, vtbl} + ctrl. Retain/release
// on the shared count; the last strong handle drops the concrete object via the vtable's `__dtor`.
#define CSTAR_SHARED_IFACE_TYPE(NAME, VTBL) typedef struct NAME { void* obj; const VTBL* vtbl; cstar_ctrl* ctrl; } NAME;
#define CSTAR_SHARED_IFACE_FUNCS(NAME)                                         \
static inline void NAME##__dtor(NAME* self) {                                  \
    if (self->ctrl) {                                                          \
        if (--self->ctrl->strong == 0) {                                       \
            if (self->vtbl && self->vtbl->__dtor) self->vtbl->__dtor(self->obj); \
            cstar_free(self->obj);                                            \
            if (self->ctrl->weak == 0) cstar_free(self->ctrl);                     \
        }                                                                      \
        self->obj = NULL; self->ctrl = NULL;                                  \
    }                                                                          \
}                                                                              \
static inline bool NAME##__valid(NAME* self) { return self->obj != NULL; }

// Weak<T> — a non-owning reference to a Shared<T>'s pointee. Counts `weak`, not
// `strong`, so it does NOT keep the pointee alive (it breaks Shared cycles). You
// cannot deref a Weak directly; `upgrade()` upgrades to a Shared if still alive.
// Same layout as Shared. Drop releases the weak count and frees the control
// block only when BOTH counts reach 0 (never touches the pointee).
#define CSTAR_WEAK_TYPE(T, NAME) typedef struct NAME { T* ptr; cstar_ctrl* ctrl; } NAME;
#define CSTAR_WEAK_FUNCS(T, NAME, SHARED_NAME)                                  \
static inline void NAME##__dtor(NAME* self) {                                  \
    if (self->ctrl) {                                                          \
        if (--self->ctrl->weak == 0 && self->ctrl->strong == 0) cstar_free(self->ctrl); \
        self->ptr = NULL; self->ctrl = NULL;                                  \
    }                                                                          \
}                                                                              \
static inline bool NAME##__expired(NAME* self) {                              \
    return self->ctrl == NULL || self->ctrl->strong == 0;                     \
}                                                                              \
static inline SHARED_NAME NAME##__upgrade(NAME* self) {                          \
    SHARED_NAME s;                                                            \
    if (self->ctrl && self->ctrl->strong > 0) {                              \
        self->ctrl->strong++; s.ptr = self->ptr; s.ctrl = self->ctrl;        \
    } else { s.ptr = NULL; s.ctrl = NULL; }                                   \
    return s;                                                                 \
}
#define CSTAR_WEAK_DEFINE(T, NAME, SHARED_NAME) CSTAR_WEAK_TYPE(T, NAME) CSTAR_WEAK_FUNCS(T, NAME, SHARED_NAME)

// Weak<I> over a CONTRACT — same fat layout as Shared<I>; counts `weak`, never touches
// the concrete object. `upgrade()` yields a live Shared<I> (obj/vtbl/ctrl) or an empty one.
#define CSTAR_WEAK_IFACE_TYPE(NAME, VTBL) typedef struct NAME { void* obj; const VTBL* vtbl; cstar_ctrl* ctrl; } NAME;
#define CSTAR_WEAK_IFACE_FUNCS(NAME, SHARED_NAME)                              \
static inline void NAME##__dtor(NAME* self) {                                  \
    if (self->ctrl) {                                                          \
        if (--self->ctrl->weak == 0 && self->ctrl->strong == 0) cstar_free(self->ctrl); \
        self->obj = NULL; self->ctrl = NULL;                                  \
    }                                                                          \
}                                                                              \
static inline bool NAME##__expired(NAME* self) {                              \
    return self->ctrl == NULL || self->ctrl->strong == 0;                     \
}                                                                              \
static inline SHARED_NAME NAME##__upgrade(NAME* self) {                          \
    SHARED_NAME s;                                                            \
    if (self->ctrl && self->ctrl->strong > 0) {                              \
        self->ctrl->strong++; s.obj = self->obj; s.vtbl = self->vtbl; s.ctrl = self->ctrl; \
    } else { s.obj = NULL; s.vtbl = NULL; s.ctrl = NULL; }                     \
    return s;                                                                 \
}

// BindableFunctionPtr<Sig> — a callable that optionally OWNS its bound
// receiver (RAII). Fully type-erased, so one definition serves every signature:
//   obj      — the bound receiver (NULL => a free function, no object)
//   ctrl     — refcount block, set only when the object was bound from a Shared<T>
//   fn       — the callable, stored type-erased; invoked as ret(void*,P…) when
//              obj!=NULL (a bound method, object passed first) else ret(P…) (free)
//   elemdtor — the bound object's destructor (NULL if trivially destructible/free)
// Move-only (it may uniquely own the object). Drop releases per ownership kind.
#define CSTAR_BINDABLE_TYPE(NAME)                                              \
typedef struct NAME { void* obj; cstar_ctrl* ctrl;                            \
                      void (*fn)(void); void (*elemdtor)(void*); } NAME;
#define CSTAR_BINDABLE_FUNCS(NAME)                                            \
static inline void NAME##__dtor(NAME* self) {                                 \
    if (self->ctrl) {                          /* Shared: refcount */         \
        if (--self->ctrl->strong == 0) {                                      \
            if (self->elemdtor) self->elemdtor(self->obj); cstar_free(self->obj); \
            if (self->ctrl->weak == 0) cstar_free(self->ctrl);                \
        }                                                                     \
    } else if (self->obj) {                    /* Owned: sole owner */        \
        if (self->elemdtor) self->elemdtor(self->obj); cstar_free(self->obj); \
    }                                          /* free fn: nothing to drop */ \
    self->obj = NULL; self->ctrl = NULL; self->fn = NULL; self->elemdtor = NULL; \
}
#define CSTAR_BINDABLE_DEFINE(NAME) CSTAR_BINDABLE_TYPE(NAME) CSTAR_BINDABLE_FUNCS(NAME)

// Bounds-check trap: a clean panic (not undefined behavior) on out-of-range.
// Formats its own message and writes to stderr (fd 2) so it needs no <stdio.h>.
static inline void cstar_u64_to_buf(char* buf, size_t* p, size_t v) {
    char tmp[20]; int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
    while (t) buf[(*p)++] = tmp[--t];
}
static inline void cstar_bounds_fail(size_t i, size_t len) {
    extern void abort(void);
    char buf[96]; size_t p = 0;
    const char* a = "cstar: index ";              while (*a) buf[p++] = *a++;
    cstar_u64_to_buf(buf, &p, i);
    const char* b = " out of bounds (length ";    while (*b) buf[p++] = *b++;
    cstar_u64_to_buf(buf, &p, len);
    const char* c = ")\n";                        while (*c) buf[p++] = *c++;
    // Raw stderr write (no <stdio.h>): POSIX/emscripten spell it `write`, Windows
    // (MSVC/UCRT/MinGW) spell it `_write`.
#if defined(_WIN32)
    { extern int _write(int, const void*, unsigned int); (void)_write(2, buf, (unsigned int)p); }
#else
    { extern long write(int, const void*, size_t);       (void)write(2, buf, p); }
#endif
    abort();
}

// Array<T> — fixed-size, owns a zero-initialized contiguous buffer (RAII frees).
#define CSTAR_ARRAY_TYPE(T, NAME) typedef struct NAME { T* data; size_t len; } NAME;
#define CSTAR_ARRAY_FUNCS(T, NAME, ELEM_DTOR, ELEM_COPY)                        \
static inline void NAME##__ctor(NAME* self, size_t n) {                        \
    self->len  = n;                                                            \
    self->data = (n ? (T*)cstar_calloc(n, sizeof(T)) : NULL);                        \
}                                                                              \
static inline void NAME##__dtor(NAME* self) {                                  \
    for (size_t i = 0; i < self->len; ++i) { T* e = &self->data[i]; ELEM_DTOR(e); } \
    cstar_free(self->data); self->data = NULL; self->len = 0;                        \
}                                                                              \
static inline T      NAME##__get(NAME* self, size_t i) {                       \
    if (i >= self->len) cstar_bounds_fail(i, self->len);                       \
    return self->data[i];                                                      \
}                                                                              \
static inline void   NAME##__set(NAME* self, size_t i, T v) {                  \
    if (i >= self->len) cstar_bounds_fail(i, self->len);                       \
    self->data[i] = v;                                                         \
}                                                                              \
static inline size_t NAME##__length(NAME* self) { return self->len; }\
 static inline T* NAME##__dataPtr(NAME* self) { return self->data; }                 \
 static inline size_t NAME##__byteLen(NAME* self) { return self->len * sizeof(T); }  \
 static inline T*     NAME##__at(NAME* self, size_t i) { if (i >= self->len) cstar_bounds_fail(i, self->len); return &self->data[i]; } \
 static inline NAME   NAME##__copy(NAME* self) {  /* deep copy (fresh buffer) */        \
    NAME r; r.len = self->len;                                                       \
    r.data = (self->len ? (T*)cstar_alloc(self->len * sizeof(T)) : NULL);            \
    for (size_t i = 0; i < self->len; ++i) r.data[i] = ELEM_COPY(&self->data[i]);    \
    return r;                                                                        \
 }
#define CSTAR_ARRAY_DEFINE(T, NAME, ELEM_DTOR, ELEM_COPY) CSTAR_ARRAY_TYPE(T, NAME) CSTAR_ARRAY_FUNCS(T, NAME, ELEM_DTOR, ELEM_COPY)

// List<T> — growable (capacity doubling), owns its buffer (RAII frees).
#define CSTAR_LIST_TYPE(T, NAME) typedef struct NAME { T* data; size_t len; size_t cap; } NAME;
#define CSTAR_LIST_FUNCS(T, NAME, ELEM_DTOR, ELEM_COPY)                         \
static inline void NAME##__ctor(NAME* self) { self->data=NULL; self->len=0; self->cap=0; } \
static inline void NAME##__dtor(NAME* self) {                                  \
    for (size_t i = 0; i < self->len; ++i) { T* e = &self->data[i]; ELEM_DTOR(e); } \
    cstar_free(self->data); self->data = NULL; self->len = 0; self->cap = 0;         \
}                                                                              \
static inline void   NAME##__add(NAME* self, T v) {                            \
    if (self->len == self->cap) {                                             \
        size_t nc = self->cap ? self->cap * 2 : 4;                            \
        self->data = (T*)cstar_realloc(self->data, nc * sizeof(T));                 \
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
static inline size_t NAME##__length(NAME* self) { return self->len; }\
 static inline T* NAME##__dataPtr(NAME* self) { return self->data; }                 \
 static inline size_t NAME##__byteLen(NAME* self) { return self->len * sizeof(T); }  \
 static inline T*     NAME##__at(NAME* self, size_t i) { if (i >= self->len) cstar_bounds_fail(i, self->len); return &self->data[i]; } \
 static inline NAME   NAME##__copy(NAME* self) {  /* deep copy (fresh buffer, cap=len) */  \
    NAME r; r.len = self->len; r.cap = self->len;                                    \
    r.data = (self->len ? (T*)cstar_alloc(self->len * sizeof(T)) : NULL);            \
    for (size_t i = 0; i < self->len; ++i) r.data[i] = ELEM_COPY(&self->data[i]);    \
    return r;                                                                        \
 }
#define CSTAR_LIST_DEFINE(T, NAME, ELEM_DTOR, ELEM_COPY) CSTAR_LIST_TYPE(T, NAME) CSTAR_LIST_FUNCS(T, NAME, ELEM_DTOR, ELEM_COPY)

// Fixed<T,N> — a fixed-size, bounds-checked VALUE array (`struct { T v[N]; }`). It owns no heap:
// it copies by value (a plain struct blit), has no destructor, and never decays to a raw pointer.
// The element must be a `value` (owns nothing), so there is no per-element dtor/copy. This is how
// cstar reintroduces raw arrays SAFELY — indexing is bounds-checked (a runtime trap), the size is
// part of the type (monomorphized per (T,N)), and the whole thing is a first-class value.
#define CSTAR_FIXED_TYPE(T, N, NAME) typedef struct NAME { T v[N]; } NAME;
#define CSTAR_FIXED_FUNCS(T, N, NAME)                                           \
static inline T      NAME##__get(NAME* self, size_t i) {                        \
    if (i >= (size_t)(N)) cstar_bounds_fail(i, (size_t)(N));                    \
    return self->v[i];                                                          \
}                                                                               \
static inline void   NAME##__set(NAME* self, size_t i, T x) {                   \
    if (i >= (size_t)(N)) cstar_bounds_fail(i, (size_t)(N));                    \
    self->v[i] = x;                                                             \
}                                                                               \
static inline T*     NAME##__at(NAME* self, size_t i) {                         \
    if (i >= (size_t)(N)) cstar_bounds_fail(i, (size_t)(N));                    \
    return &self->v[i];                                                         \
}                                                                               \
static inline size_t NAME##__length(NAME* self) { (void)self; return (size_t)(N); } \
static inline NAME   NAME##__fill(T x) {                                        \
    NAME r; for (size_t i = 0; i < (size_t)(N); ++i) r.v[i] = x; return r;      \
}
#define CSTAR_FIXED_DEFINE(T, N, NAME) CSTAR_FIXED_TYPE(T, N, NAME) CSTAR_FIXED_FUNCS(T, N, NAME)


// cstar `string` lowers to a fat, length-prefixed value. `cap == 0` means
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
    if (self->cap) cstar_free(self->data);
    self->data = NULL; self->len = 0; self->cap = 0;
}
static inline size_t cstar_string__length(cstar_string* self) { return self->len; }
// FFI: the underlying NUL-terminated bytes, for passing to a C `const char*`.
static inline char* cstar_string__cstr(cstar_string* self) { return self->data; }
static inline bool cstar_string__equals(cstar_string* self, cstar_string other) {
    return self->len == other.len &&
           (self->len == 0 || cstar_cmp(self->data, other.data, self->len) == 0);
}
// Deep copy -> a fresh heap-owned string (even copying a borrowed literal).
static inline cstar_string cstar_string__copy(const cstar_string* self) {
    cstar_string r; r.len = self->len;
    if (self->len == 0) { r.data = NULL; r.cap = 0; return r; }
    char* buf = (char*)cstar_alloc(self->len + 1);
    cstar_copy(buf, self->data, self->len);
    buf[self->len] = '\0';
    r.data = buf; r.cap = self->len + 1;   // heap-owned
    return r;
}
// Returns a fresh heap-owned string (the caller binds it -> RAII frees it).
static inline cstar_string cstar_string__concat(cstar_string* self, cstar_string other) {
    size_t n = self->len + other.len;
    char*  buf = (char*)cstar_alloc(n + 1);
    if (self->len) cstar_copy(buf, self->data, self->len);
    if (other.len) cstar_copy(buf + self->len, other.data, other.len);
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
