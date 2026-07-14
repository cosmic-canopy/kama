#ifndef KAMA_RUNTIME_H
#define KAMA_RUNTIME_H

// Minimal kama runtime. Dependency-light and WASM-safe: it leans only on the
// freestanding-friendly C headers so generated code compiles for native and
// for clang/emscripten wasm targets alike. Generated translation units include
// this first, then their own module header.

#include <stdint.h>    // int32_t … (types only — no callable C functions)
#include <stdbool.h>   // bool       (type only)
#include <stddef.h>    // size_t, NULL (types only)

// KAMA_EXPORT — the kama→host boundary decoration for an `expose fn`. It gives the
// (unmangled, bare-named) function stable C-ABI linkage a host can resolve: `dlsym`
// on a `--shared` native `.so`/`.dylib`/`.dll`, or `Module._name` on a wasm build.
// emscripten: KEEPALIVE marks it `used` + exports it (survives -Oz DCE). Native ELF/
// Mach-O: `visibility("default"), used` keeps it past --gc-sections/-dead_strip and
// -fvisibility=hidden (so ONLY exposed symbols leak from a shared lib). Windows: dllexport.
#if defined(__EMSCRIPTEN__)
  #include <emscripten.h>
  #define KAMA_EXPORT EMSCRIPTEN_KEEPALIVE
#elif defined(_WIN32)
  #define KAMA_EXPORT __declspec(dllexport)
#else
  #define KAMA_EXPORT __attribute__((visibility("default"), used))
#endif

// The runtime needs a few libc functions (malloc/free/memcpy/…) for collections,
// strings, and the bounds trap. It declares them at BLOCK scope inside these
// wrappers, NOT via <stdlib.h>/<string.h>/<stdio.h> — so those declarations stay
// invisible to user code. Consequence (and the point): EVERY C function a kama
// program calls must be brought in explicitly with `extern "<header.h>";`. The
// runtime's own dependencies never leak. (All raw memory access is confined here.)
static inline void* kama_alloc(size_t n)              { extern void* malloc(size_t);                  return malloc(n); }
static inline void* kama_calloc(size_t count, size_t size) { extern void* calloc(size_t, size_t);     return calloc(count, size); }
static inline void* kama_realloc(void* p, size_t n)   { extern void* realloc(void*, size_t);          return realloc(p, n); }
static inline void  kama_free(void* p)                { extern void  free(void*);                     free(p); }
static inline void  kama_copy(void* d, const void* s, size_t n) { extern void* memcpy(void*, const void*, size_t); memcpy(d, s, n); }
static inline int   kama_cmp(const void* a, const void* b, size_t n) { extern int memcmp(const void*, const void*, size_t); return memcmp(a, b, n); }

// ---- Collections ----------------------------------------------------------
// Generic collections are monomorphized per element type from these templates.
// The kama surface stays pointer-free and safe. Indexing is bounds-checked.

// A no-op per-element destructor, used when the element type isn't destructible.
#define KAMA_ELEM_NODTOR(p) ((void)(p))

// Per-element copy. A bitwise-copyable element (owns nothing) copies memberwise; a
// `Copyable` resource element deep-copies via its own `Elem__copy(&e)`. Given an element POINTER,
// both yield the copied element BY VALUE, so `NAME##__copy` assigns `r.data[i] = ELEM_COPY(&src[i])`.
#define KAMA_ELEM_MEMBERWISE(e) (*(e))

// Owned<T> — unique heap ownership (Box / unique_ptr). Move-only; RAII frees.
// The struct + dtor live here; the heap alloc + T's constructor are emitted
// INLINE by the compiler (it knows T's ctor + named-arg order). ELEM_DTOR runs
// T's destructor on the pointee before free. A moved-from Owned has ptr==NULL,
// so its dtor is a safe no-op — the single surviving owner frees exactly once.
// Split into _TYPE (the struct — needs only T forward-declared, since it stores T*)
// and _FUNCS (the dtor — needs T's dtor). The emitter emits all _TYPEs before class
// struct bodies (so a class may hold a collection/smart-ptr BY VALUE as a field) and
// all _FUNCS after class prototypes (where element dtors are declared).
#define KAMA_OWNED_TYPE(T, NAME) typedef struct NAME { T* ptr; } NAME;
#define KAMA_OWNED_FUNCS(T, NAME, ELEM_DTOR)                                   \
static inline void NAME##__dtor(NAME* self) {                                  \
    if (self->ptr) { ELEM_DTOR(self->ptr); kama_free(self->ptr); self->ptr = NULL; } \
}
#define KAMA_OWNED_DEFINE(T, NAME, ELEM_DTOR) KAMA_OWNED_TYPE(T, NAME) KAMA_OWNED_FUNCS(T, NAME, ELEM_DTOR)

// Owned<I> over a CONTRACT — a unique-owning fat pointer: the handle IS the contract
// fat pointer {obj, vtbl}, with `obj` the heap-owned CONCRETE object. Drop dispatches the concrete
// destructor through the vtable's `__dtor` slot (NULL for a non-destructible impl), then frees obj.
#define KAMA_OWNED_IFACE_TYPE(NAME, VTBL) typedef struct NAME { void* obj; const VTBL* vtbl; } NAME;
#define KAMA_OWNED_IFACE_FUNCS(NAME)                                          \
static inline void NAME##__dtor(NAME* self) {                                  \
    if (self->obj) {                                                           \
        if (self->vtbl && self->vtbl->__dtor) self->vtbl->__dtor(self->obj);   \
        kama_free(self->obj); self->obj = NULL;                              \
    }                                                                          \
}

// Shared<T> — ref-counted shared ownership (shared_ptr / Rc). Copy retains;
// drop releases; the pointee is destroyed + freed when the last strong handle
// goes away. The control block (counts) is a SEPARATE allocation so a future
// Weak<T> can outlive the T. `ptr` mirrors Owned's, so auto-deref is identical.
typedef struct kama_ctrl { size_t strong; size_t weak; } kama_ctrl;   // weak: reserved for Weak<T>
static inline kama_ctrl* kama_ctrl_new(void) {
    kama_ctrl* c = (kama_ctrl*)kama_alloc(sizeof(kama_ctrl));
    c->strong = 1; c->weak = 0;
    return c;
}
#define KAMA_SHARED_TYPE(T, NAME) typedef struct NAME { T* ptr; kama_ctrl* ctrl; } NAME;
// The last strong drop keeps `strong` at 1 while the pointee dtor runs, then releases it: dropping the
// pointee can free a `Weak` back-edge into THIS same ctrl (a cycle), which would free the ctrl early
// (strong already 0) and leave the `weak == 0` check reading freed memory. See prelude shared.kama.
#define KAMA_SHARED_FUNCS(T, NAME, ELEM_DTOR)                                  \
static inline void NAME##__dtor(NAME* self) {                                  \
    if (self->ctrl) {                                                          \
        if (self->ctrl->strong == 1) {                                         \
            ELEM_DTOR(self->ptr); kama_free(self->ptr);                            \
            self->ctrl->strong = 0;                                            \
            if (self->ctrl->weak == 0) kama_free(self->ctrl);                       \
        } else { self->ctrl->strong--; }                                      \
        self->ptr = NULL; self->ctrl = NULL;                                  \
    }                                                                          \
}                                                                              \
static inline bool NAME##__valid(NAME* self) { return self->ptr != NULL; }
#define KAMA_SHARED_DEFINE(T, NAME, ELEM_DTOR) KAMA_SHARED_TYPE(T, NAME) KAMA_SHARED_FUNCS(T, NAME, ELEM_DTOR)

// Shared<I> over a CONTRACT — ref-counted fat pointer {obj, vtbl} + ctrl. Retain/release
// on the shared count; the last strong handle drops the concrete object via the vtable's `__dtor`.
#define KAMA_SHARED_IFACE_TYPE(NAME, VTBL) typedef struct NAME { void* obj; const VTBL* vtbl; kama_ctrl* ctrl; } NAME;
#define KAMA_SHARED_IFACE_FUNCS(NAME)                                         \
static inline void NAME##__dtor(NAME* self) {                                  \
    if (self->ctrl) {                                                          \
        if (self->ctrl->strong == 1) {   /* last strong: release AFTER the drop (cycle-safe, see above) */ \
            if (self->vtbl && self->vtbl->__dtor) self->vtbl->__dtor(self->obj); \
            kama_free(self->obj);                                            \
            self->ctrl->strong = 0;                                           \
            if (self->ctrl->weak == 0) kama_free(self->ctrl);                     \
        } else { self->ctrl->strong--; }                                     \
        self->obj = NULL; self->ctrl = NULL;                                  \
    }                                                                          \
}                                                                              \
static inline bool NAME##__valid(NAME* self) { return self->obj != NULL; }

// Weak<T> — a non-owning reference to a Shared<T>'s pointee. Counts `weak`, not
// `strong`, so it does NOT keep the pointee alive (it breaks Shared cycles). You
// cannot deref a Weak directly; `upgrade()` upgrades to a Shared if still alive.
// Same layout as Shared. Drop releases the weak count and frees the control
// block only when BOTH counts reach 0 (never touches the pointee).
#define KAMA_WEAK_TYPE(T, NAME) typedef struct NAME { T* ptr; kama_ctrl* ctrl; } NAME;
#define KAMA_WEAK_FUNCS(T, NAME, SHARED_NAME)                                  \
static inline void NAME##__dtor(NAME* self) {                                  \
    if (self->ctrl) {                                                          \
        if (--self->ctrl->weak == 0 && self->ctrl->strong == 0) kama_free(self->ctrl); \
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
#define KAMA_WEAK_DEFINE(T, NAME, SHARED_NAME) KAMA_WEAK_TYPE(T, NAME) KAMA_WEAK_FUNCS(T, NAME, SHARED_NAME)

// Weak<I> over a CONTRACT — same fat layout as Shared<I>; counts `weak`, never touches
// the concrete object. `upgrade()` yields a live Shared<I> (obj/vtbl/ctrl) or an empty one.
#define KAMA_WEAK_IFACE_TYPE(NAME, VTBL) typedef struct NAME { void* obj; const VTBL* vtbl; kama_ctrl* ctrl; } NAME;
#define KAMA_WEAK_IFACE_FUNCS(NAME, SHARED_NAME)                              \
static inline void NAME##__dtor(NAME* self) {                                  \
    if (self->ctrl) {                                                          \
        if (--self->ctrl->weak == 0 && self->ctrl->strong == 0) kama_free(self->ctrl); \
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
#define KAMA_BINDABLE_TYPE(NAME)                                              \
typedef struct NAME { void* obj; kama_ctrl* ctrl;                            \
                      void (*fn)(void); void (*elemdtor)(void*); } NAME;
#define KAMA_BINDABLE_FUNCS(NAME)                                            \
static inline void NAME##__dtor(NAME* self) {                                 \
    if (self->ctrl) {                          /* Shared: refcount */         \
        if (--self->ctrl->strong == 0) {                                      \
            if (self->elemdtor) self->elemdtor(self->obj); kama_free(self->obj); \
            if (self->ctrl->weak == 0) kama_free(self->ctrl);                \
        }                                                                     \
    } else if (self->obj) {                    /* Owned: sole owner */        \
        if (self->elemdtor) self->elemdtor(self->obj); kama_free(self->obj); \
    }                                          /* free fn: nothing to drop */ \
    self->obj = NULL; self->ctrl = NULL; self->fn = NULL; self->elemdtor = NULL; \
}
#define KAMA_BINDABLE_DEFINE(NAME) KAMA_BINDABLE_TYPE(NAME) KAMA_BINDABLE_FUNCS(NAME)

// Bounds-check trap: a clean panic (not undefined behavior) on out-of-range.
// Formats its own message and writes to stderr (fd 2) so it needs no <stdio.h>.
static inline void kama_u64_to_buf(char* buf, size_t* p, size_t v) {
    char tmp[20]; int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
    while (t) buf[(*p)++] = tmp[--t];
}
static inline void kama_bounds_fail(size_t i, size_t len) {
    extern void abort(void);
    char buf[96]; size_t p = 0;
    const char* a = "kama: index ";              while (*a) buf[p++] = *a++;
    kama_u64_to_buf(buf, &p, i);
    const char* b = " out of bounds (length ";    while (*b) buf[p++] = *b++;
    kama_u64_to_buf(buf, &p, len);
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

// Left shift with NO undefined behavior. A signed left shift into/past the sign bit is UB in C; do the
// shift in the matching UNSIGNED type (a defined two's-complement bitwise shift) and convert back. Unsigned
// operands shift as-is. `_Generic` dispatches on the operand's static type (evaluating `a`/`b` once each),
// so no per-call type info is needed at the emitter. The shift AMOUNT is untouched, so an out-of-range
// exponent still trips `-fsanitize=shift-exponent` (a clean trap) exactly as a plain `<<` would.
#define kama_lshift(a, b) _Generic((a),                 \
    int8_t:   (int8_t) ((uint8_t) (a) << (b)),          \
    int16_t:  (int16_t)((uint16_t)(a) << (b)),          \
    int32_t:  (int32_t)((uint32_t)(a) << (b)),          \
    int64_t:  (int64_t)((uint64_t)(a) << (b)),          \
    default:  ((a) << (b)))

// Array<T> — fixed-size, owns a zero-initialized contiguous buffer (RAII frees).
#define KAMA_ARRAY_TYPE(T, NAME) typedef struct NAME { T* data; size_t len; } NAME;
#define KAMA_ARRAY_FUNCS(T, NAME, ELEM_DTOR, ELEM_COPY)                        \
static inline void NAME##__ctor(NAME* self, size_t n) {                        \
    self->len  = n;                                                            \
    self->data = (n ? (T*)kama_calloc(n, sizeof(T)) : NULL);                        \
}                                                                              \
static inline void NAME##__dtor(NAME* self) {                                  \
    for (size_t i = 0; i < self->len; ++i) { T* e = &self->data[i]; ELEM_DTOR(e); } \
    kama_free(self->data); self->data = NULL; self->len = 0;                        \
}                                                                              \
static inline T      NAME##__get(NAME* self, size_t i) {                       \
    if (i >= self->len) kama_bounds_fail(i, self->len);                       \
    return self->data[i];                                                      \
}                                                                              \
static inline void   NAME##__set(NAME* self, size_t i, T v) {                  \
    if (i >= self->len) kama_bounds_fail(i, self->len);                       \
    self->data[i] = v;                                                         \
}                                                                              \
static inline size_t NAME##__length(NAME* self) { return self->len; }\
 static inline T* NAME##__dataPtr(NAME* self) { return self->data; }                 \
 static inline size_t NAME##__byteLen(NAME* self) { return self->len * sizeof(T); }  \
 static inline T*     NAME##__at(NAME* self, size_t i) { if (i >= self->len) kama_bounds_fail(i, self->len); return &self->data[i]; } \
 static inline NAME   NAME##__copy(NAME* self) {  /* deep copy (fresh buffer) */        \
    NAME r; r.len = self->len;                                                       \
    r.data = (self->len ? (T*)kama_alloc(self->len * sizeof(T)) : NULL);            \
    for (size_t i = 0; i < self->len; ++i) r.data[i] = ELEM_COPY(&self->data[i]);    \
    return r;                                                                        \
 }
#define KAMA_ARRAY_DEFINE(T, NAME, ELEM_DTOR, ELEM_COPY) KAMA_ARRAY_TYPE(T, NAME) KAMA_ARRAY_FUNCS(T, NAME, ELEM_DTOR, ELEM_COPY)

// List<T> — growable (capacity doubling), owns its buffer (RAII frees).
#define KAMA_LIST_TYPE(T, NAME) typedef struct NAME { T* data; size_t len; size_t cap; } NAME;
#define KAMA_LIST_FUNCS(T, NAME, ELEM_DTOR, ELEM_COPY)                         \
static inline void NAME##__ctor(NAME* self) { self->data=NULL; self->len=0; self->cap=0; } \
static inline void NAME##__dtor(NAME* self) {                                  \
    for (size_t i = 0; i < self->len; ++i) { T* e = &self->data[i]; ELEM_DTOR(e); } \
    kama_free(self->data); self->data = NULL; self->len = 0; self->cap = 0;         \
}                                                                              \
static inline void   NAME##__add(NAME* self, T v) {                            \
    if (self->len == self->cap) {                                             \
        size_t nc = self->cap ? self->cap * 2 : 4;                            \
        self->data = (T*)kama_realloc(self->data, nc * sizeof(T));                 \
        self->cap  = nc;                                                      \
    }                                                                         \
    self->data[self->len++] = v;                                             \
}                                                                              \
static inline T      NAME##__get(NAME* self, size_t i) {                       \
    if (i >= self->len) kama_bounds_fail(i, self->len);                       \
    return self->data[i];                                                      \
}                                                                              \
static inline void   NAME##__set(NAME* self, size_t i, T v) {                  \
    if (i >= self->len) kama_bounds_fail(i, self->len);                       \
    self->data[i] = v;                                                         \
}                                                                              \
static inline size_t NAME##__length(NAME* self) { return self->len; }\
 static inline T* NAME##__dataPtr(NAME* self) { return self->data; }                 \
 static inline size_t NAME##__byteLen(NAME* self) { return self->len * sizeof(T); }  \
 static inline T*     NAME##__at(NAME* self, size_t i) { if (i >= self->len) kama_bounds_fail(i, self->len); return &self->data[i]; } \
 static inline NAME   NAME##__copy(NAME* self) {  /* deep copy (fresh buffer, cap=len) */  \
    NAME r; r.len = self->len; r.cap = self->len;                                    \
    r.data = (self->len ? (T*)kama_alloc(self->len * sizeof(T)) : NULL);            \
    for (size_t i = 0; i < self->len; ++i) r.data[i] = ELEM_COPY(&self->data[i]);    \
    return r;                                                                        \
 }
#define KAMA_LIST_DEFINE(T, NAME, ELEM_DTOR, ELEM_COPY) KAMA_LIST_TYPE(T, NAME) KAMA_LIST_FUNCS(T, NAME, ELEM_DTOR, ELEM_COPY)

// InlineArray<T,N> — a fixed-size, bounds-checked VALUE array (`struct { T v[N]; }`). It owns no heap:
// it copies by value (a plain struct blit), has no destructor, and never decays to a raw pointer.
// The element must be a `value` (owns nothing), so there is no per-element dtor/copy. This is how
// kama reintroduces raw arrays SAFELY — indexing is bounds-checked (a runtime trap), the size is
// part of the type (monomorphized per (T,N)), and the whole thing is a first-class value.
#define KAMA_FIXED_TYPE(T, N, NAME) typedef struct NAME { T v[N]; } NAME;
#define KAMA_FIXED_FUNCS(T, N, NAME)                                           \
static inline T      NAME##__get(NAME* self, size_t i) {                        \
    if (i >= (size_t)(N)) kama_bounds_fail(i, (size_t)(N));                    \
    return self->v[i];                                                          \
}                                                                               \
static inline void   NAME##__set(NAME* self, size_t i, T x) {                   \
    if (i >= (size_t)(N)) kama_bounds_fail(i, (size_t)(N));                    \
    self->v[i] = x;                                                             \
}                                                                               \
static inline T*     NAME##__at(NAME* self, size_t i) {                         \
    if (i >= (size_t)(N)) kama_bounds_fail(i, (size_t)(N));                    \
    return &self->v[i];                                                         \
}                                                                               \
static inline size_t NAME##__length(NAME* self) { (void)self; return (size_t)(N); } \
static inline NAME   NAME##__fill(T x) {                                        \
    NAME r; for (size_t i = 0; i < (size_t)(N); ++i) r.v[i] = x; return r;      \
}
#define KAMA_FIXED_DEFINE(T, N, NAME) KAMA_FIXED_TYPE(T, N, NAME) KAMA_FIXED_FUNCS(T, N, NAME)


// kama `string` lowers to a fat, length-prefixed value. `cap == 0` means
// the bytes are BORROWED (e.g. a C string literal in static storage) and must
// never be written or freed; `cap > 0` means HEAP-OWNED (NUL-terminated) and is
// freed by RAII. All string ops read uniformly; only concat allocates. Raw
// memory stays confined here — the kama surface sees only a safe `string`.
typedef struct kama_string {
    char*  data;   // UTF-8 bytes; borrowed (cap==0) bytes are never mutated/freed
    size_t len;    // byte length
    size_t cap;    // 0 => borrowed/literal, >0 => heap-owned
} kama_string;

// Borrowed view of a string literal (static storage; valid for the whole run).
static inline kama_string kama_string_lit(const char* s, size_t n) {
    kama_string r;
    r.data = (char*)s;   // never written/freed while cap==0
    r.len  = n;
    r.cap  = 0;
    return r;
}

// RAII: free only heap-owned strings; borrowed views are a no-op.
static inline void kama_string__dtor(kama_string* self) {
    if (self->cap) kama_free(self->data);
    self->data = NULL; self->len = 0; self->cap = 0;
}
static inline size_t kama_string__length(kama_string* self) { return self->len; }
// FFI: the underlying NUL-terminated bytes, for passing to a C `const char*`.
static inline char* kama_string__cstr(kama_string* self) { return self->data; }
static inline bool kama_string__equals(kama_string* self, kama_string other) {
    return self->len == other.len &&
           (self->len == 0 || kama_cmp(self->data, other.data, self->len) == 0);
}
// Deep copy -> a fresh heap-owned string (even copying a borrowed literal).
static inline kama_string kama_string__copy(const kama_string* self) {
    kama_string r; r.len = self->len;
    if (self->len == 0) { r.data = NULL; r.cap = 0; return r; }
    char* buf = (char*)kama_alloc(self->len + 1);
    kama_copy(buf, self->data, self->len);
    buf[self->len] = '\0';
    r.data = buf; r.cap = self->len + 1;   // heap-owned
    return r;
}
// Returns a fresh heap-owned string (the caller binds it -> RAII frees it).
static inline kama_string kama_string__concat(kama_string* self, kama_string other) {
    size_t n = self->len + other.len;
    char*  buf = (char*)kama_alloc(n + 1);
    if (self->len) kama_copy(buf, self->data, self->len);
    if (other.len) kama_copy(buf + self->len, other.data, other.len);
    buf[n] = '\0';
    kama_string r; r.data = buf; r.len = n; r.cap = n + 1; return r;
}
// Bounds-checked byte access: `s[i]` returns the i-th UTF-8 byte (a uint8). Traps on out-of-range.
// (Codepoints come from `.chars()`; this is the raw byte, honest to the UTF-8-bytes model.)
static inline uint8_t kama_string__get(kama_string* self, size_t i) {
    if (i >= self->len) kama_bounds_fail(i, self->len);
    return (uint8_t)self->data[i];
}

// --- Strings Phase 3 (ergonomics) -------------------------------------------
// Owned byte-range copy of `[start, end)` — a fresh heap-owned string (cap>0). Traps
// (kama_bounds_fail, the same clean abort as __get) on `start > end || end > len`. This is a
// BYTE range, NOT codepoint-validated — honest to the UTF-8-bytes model; use .chars() for codepoints.
static inline kama_string kama_string__substring(kama_string* self, size_t start, size_t end) {
    if (start > end || end > self->len) kama_bounds_fail(end, self->len);
    size_t n = end - start;
    kama_string r; r.len = n;
    if (n == 0) { r.data = NULL; r.cap = 0; return r; }
    char* buf = (char*)kama_alloc(n + 1);
    kama_copy(buf, self->data + start, n);
    buf[n] = '\0';
    r.data = buf; r.cap = n + 1;   // heap-owned
    return r;
}
// Byte offset of the first occurrence of `needle`; an empty needle matches at 0. Internal helper
// (raw found-flag + offset); the emitter wraps it as `Optional<usize>` for `.find()`. A plain
// borrowed-safe byte scan (no <string.h>).
static inline bool kama_string__find_raw(kama_string* self, kama_string needle, size_t* out) {
    if (needle.len == 0) { *out = 0; return true; }
    if (needle.len > self->len) return false;
    for (size_t i = 0; i + needle.len <= self->len; ++i)
        if (kama_cmp(self->data + i, needle.data, needle.len) == 0) { *out = i; return true; }
    return false;
}
static inline bool kama_string__contains(kama_string* self, kama_string needle) {
    size_t o; return kama_string__find_raw(self, needle, &o);
}
static inline bool kama_string__startsWith(kama_string* self, kama_string prefix) {
    return prefix.len <= self->len &&
           (prefix.len == 0 || kama_cmp(self->data, prefix.data, prefix.len) == 0);
}
static inline bool kama_string__endsWith(kama_string* self, kama_string suffix) {
    return suffix.len <= self->len &&
           (suffix.len == 0 || kama_cmp(self->data + (self->len - suffix.len), suffix.data, suffix.len) == 0);
}
static inline bool kama_string__isEmpty(kama_string* self) { return self->len == 0; }

// ASCII whitespace only (space, tab, LF, VT, FF, CR). Unicode whitespace is deferred to a Unicode module.
static inline int kama_string__is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}
// Owned copy of `[lo, hi)` (a heap string, or the empty string). Shared by the trim family.
static inline kama_string kama_string__slice_owned(kama_string* self, size_t lo, size_t hi) {
    size_t n = hi - lo;
    kama_string r; r.len = n;
    if (n == 0) { r.data = NULL; r.cap = 0; return r; }
    char* buf = (char*)kama_alloc(n + 1);
    kama_copy(buf, self->data + lo, n);
    buf[n] = '\0';
    r.data = buf; r.cap = n + 1;
    return r;
}
static inline kama_string kama_string__trimStart(kama_string* self) {
    size_t lo = 0;
    while (lo < self->len && kama_string__is_ws(self->data[lo])) ++lo;
    return kama_string__slice_owned(self, lo, self->len);
}
static inline kama_string kama_string__trimEnd(kama_string* self) {
    size_t hi = self->len;
    while (hi > 0 && kama_string__is_ws(self->data[hi - 1])) --hi;
    return kama_string__slice_owned(self, 0, hi);
}
static inline kama_string kama_string__trim(kama_string* self) {
    size_t lo = 0, hi = self->len;
    while (lo < hi && kama_string__is_ws(self->data[lo])) ++lo;
    while (hi > lo && kama_string__is_ws(self->data[hi - 1])) --hi;
    return kama_string__slice_owned(self, lo, hi);
}
// Owned copy with every non-overlapping occurrence of `old` replaced by `with` (byte-literal, greedy
// left-to-right). Two-pass: count matches, allocate exactly, fill. An empty (or too-long) `old`
// returns a copy of self — no infinite loop. `n = len - count*old.len + count*with.len` never
// underflows: non-overlapping matches guarantee `count*old.len <= len`.
static inline kama_string kama_string__replace(kama_string* self, kama_string old, kama_string with) {
    if (old.len == 0 || old.len > self->len) return kama_string__copy(self);
    size_t count = 0, j = 0;
    while (j + old.len <= self->len) {
        if (kama_cmp(self->data + j, old.data, old.len) == 0) { ++count; j += old.len; }
        else ++j;
    }
    if (count == 0) return kama_string__copy(self);
    size_t n = self->len - count * old.len + count * with.len;
    kama_string r; r.len = n;
    if (n == 0) { r.data = NULL; r.cap = 0; return r; }
    char* buf = (char*)kama_alloc(n + 1);
    size_t w = 0, i = 0;
    while (i + old.len <= self->len) {
        if (kama_cmp(self->data + i, old.data, old.len) == 0) {
            if (with.len) kama_copy(buf + w, with.data, with.len);
            w += with.len; i += old.len;
        } else buf[w++] = self->data[i++];
    }
    while (i < self->len) buf[w++] = self->data[i++];
    buf[n] = '\0';
    r.data = buf; r.cap = n + 1;
    return r;
}
// ASCII-only case mapping — bytes >= 0x80 (signed char < 0) are left untouched, which is UTF-8-safe
// (an ASCII byte never occurs inside a multibyte sequence). Full Unicode casing is deferred.
static inline kama_string kama_string__toLower(kama_string* self) {
    if (self->len == 0) { kama_string r; r.data = NULL; r.len = 0; r.cap = 0; return r; }
    char* buf = (char*)kama_alloc(self->len + 1);
    for (size_t i = 0; i < self->len; ++i) {
        char c = self->data[i];
        buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    buf[self->len] = '\0';
    kama_string r; r.data = buf; r.len = self->len; r.cap = self->len + 1; return r;
}
static inline kama_string kama_string__toUpper(kama_string* self) {
    if (self->len == 0) { kama_string r; r.data = NULL; r.len = 0; r.cap = 0; return r; }
    char* buf = (char*)kama_alloc(self->len + 1);
    for (size_t i = 0; i < self->len; ++i) {
        char c = self->data[i];
        buf[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    buf[self->len] = '\0';
    kama_string r; r.data = buf; r.len = self->len; r.cap = self->len + 1; return r;
}
// Owned (heap) string from a raw byte range `base[start .. start+len)`. This lets the `.split()`
// iterator hold a borrowed `Ptr<uint8>` (so it stays a POD `value` type, like Chars) yet yield OWNED
// pieces, without exposing raw allocation to kama source. Declared in the prelude as
// `extern fn string kama_string_from_raw(Ptr<uint8> base, int32 start, int32 len);`. len<=0 -> "".
static inline kama_string kama_string_from_raw(const uint8_t* base, int32_t start, int32_t len) {
    kama_string r;
    if (start < 0 || len <= 0) { r.data = NULL; r.len = 0; r.cap = 0; return r; }   // defensive: caller (Split) always passes >=0
    char* buf = (char*)kama_alloc((size_t)len + 1);
    kama_copy(buf, base + start, (size_t)len);
    buf[len] = '\0';
    r.data = buf; r.len = (size_t)len; r.cap = (size_t)len + 1;
    return r;
}

// User-triggerable trap for `panic(msg: …)` and a failed `assert(cond: …)`. Writes
// "kama: panic: <msg>" to stderr and `abort()`s — the same clean-abort mechanism as the bounds
// trap (no <stdio.h>, no undefined behavior). Never returns.
static inline void kama_panic(kama_string msg) {
    extern void abort(void);
#if defined(_WIN32)
    extern int _write(int, const void*, unsigned int);
    (void)_write(2, "kama: panic: ", 13);
    if (msg.len) (void)_write(2, msg.data, (unsigned int)msg.len);
    (void)_write(2, "\n", 1);
#else
    extern long write(int, const void*, size_t);
    (void)write(2, "kama: panic: ", 13);
    if (msg.len) (void)write(2, msg.data, msg.len);
    (void)write(2, "\n", 1);
#endif
    abort();
}

// Tiny tracing hook for tests/debugging: a folding accumulator that records a
// sequence of integer events (e.g. constructor/destructor order). Declare in
// kama with `extern void kama_trace(int code);` / `extern int kama_trace_get();`.
// Single-TU builds only (definition lives in this header).
static int kama_trace_acc = 0;
static inline void kama_trace(int code) { kama_trace_acc = kama_trace_acc * 31 + code; }
static inline int  kama_trace_get(void) { return kama_trace_acc; }

// ---- Serialization graph context ------------------------------------------
// Pure-C substrate for the compiler's object-graph serialization (a `@generate` type that transitively
// reaches a Shared/Weak/Owned — see `reachesPointer`). No std::collections dependency: the compiler's own
// graph machinery can't lean on kama library types (that circularity is why serialization became a compiler
// intrinsic). This is the id-table / worklist / registry substrate; the reserve/intern/drain sequencing and
// the two-pass read are emitted C driven onto it by the lowering. Inert until then. See docs/SPEC.md
// "Serialization" and ROADMAP §4. (Replaces the generated-kama `std::serialization::graph` SerContext.)

// An open-addressing uint64->uint64 map (linear probing, power-of-two capacity). Used two ways by the graph
// lowering: write-side pointee-address -> id (dedup), and read-side id -> object pointer. Key 0 is the empty
// sentinel — safe here because an interned address is never null and ids start at 1, so 0 is never a live key.
typedef struct kama_gmap {
    uint64_t* keys;   // 0 == empty slot
    uint64_t* vals;
    size_t    cap;    // power of two, or 0 when unallocated
    size_t    len;    // live entries
} kama_gmap;

static inline void kama_gmap_init(kama_gmap* m) { m->keys = NULL; m->vals = NULL; m->cap = 0; m->len = 0; }
static inline void kama_gmap_free(kama_gmap* m) { kama_free(m->keys); kama_free(m->vals); kama_gmap_init(m); }

// splitmix64 finalizer — the same mix the prelude's integer hash() uses.
static inline uint64_t kama_gmap_hash(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

static inline void kama_gmap_put(kama_gmap* m, uint64_t k, uint64_t v);   // fwd — rehash reinserts via put

// Grow to `newcap` (power of two) and reinsert every live entry.
static inline void kama_gmap_grow(kama_gmap* m, size_t newcap) {
    kama_gmap old = *m;
    m->keys = (uint64_t*)kama_calloc(newcap, sizeof(uint64_t));
    m->vals = (uint64_t*)kama_calloc(newcap, sizeof(uint64_t));
    m->cap = newcap; m->len = 0;
    for (size_t i = 0; i < old.cap; ++i)
        if (old.keys[i] != 0) kama_gmap_put(m, old.keys[i], old.vals[i]);
    kama_free(old.keys); kama_free(old.vals);
}

// Insert or overwrite. Grows at ~0.7 load. (A 0 key is the empty sentinel and must never be inserted; the
// graph lowering only ever keys on nonzero addresses / ids, so no guard is needed.)
static inline void kama_gmap_put(kama_gmap* m, uint64_t k, uint64_t v) {
    if (m->cap == 0)                             kama_gmap_grow(m, 8);
    else if ((m->len + 1) * 10 >= m->cap * 7)    kama_gmap_grow(m, m->cap * 2);
    size_t mask = m->cap - 1;
    size_t i = (size_t)kama_gmap_hash(k) & mask;
    while (m->keys[i] != 0) {
        if (m->keys[i] == k) { m->vals[i] = v; return; }   // overwrite existing
        i = (i + 1) & mask;
    }
    m->keys[i] = k; m->vals[i] = v; m->len++;
}

// Look up `k`; on hit store its value in *out and return 1, else return 0.
static inline int kama_gmap_get(const kama_gmap* m, uint64_t k, uint64_t* out) {
    if (m->cap == 0) return 0;
    size_t mask = m->cap - 1;
    size_t i = (size_t)kama_gmap_hash(k) & mask;
    while (m->keys[i] != 0) {
        if (m->keys[i] == k) { *out = m->vals[i]; return 1; }
        i = (i + 1) & mask;
    }
    return 0;
}

// Write-side graph context (ports SerContext). Dedups objects by pointee address -> stable id, and holds a
// worklist of pending objects (BORROWED — the live graph is kept alive by the caller's root handle during the
// read-only serialize traversal, so the context owns nothing). The lowering emits the drain loop: walk
// count() (re-checked, it grows as each entry interns more), invoking each node's writer.
//
// The worklist is heterogeneous (a graph mixes concrete types) and must be written in discovery (== id) order
// so the wire stays byte-identical. So each enqueued node carries a `kama_node_writer` — the compiler-emitted
// `T__serializeNode` for its concrete type. This is an internal jump table between compiler-emitted C, not a
// public ABI; `w` is typed `void*` because this pure-C header can't name the kama-lowered `Serializer` struct.
struct kama_ser_graph;
typedef void (*kama_node_writer)(void* obj, void* w, struct kama_ser_graph* g, uint64_t id);

typedef struct kama_ser_graph {
    kama_gmap         ids;        // pointee address -> id
    void**            nodes;      // worklist objects (borrowed)
    uint64_t*         node_ids;   // parallel ids
    kama_node_writer* writers;    // parallel per-node writer (T__serializeNode)
    size_t            node_len;
    size_t            node_cap;
    uint64_t          next_id;
} kama_ser_graph;

static inline void kama_ser_graph_init(kama_ser_graph* g) {
    kama_gmap_init(&g->ids);
    g->nodes = NULL; g->node_ids = NULL; g->writers = NULL; g->node_len = 0; g->node_cap = 0; g->next_id = 1;
}
static inline void kama_ser_graph_free(kama_ser_graph* g) {
    kama_gmap_free(&g->ids); kama_free(g->nodes); kama_free(g->node_ids); kama_free(g->writers);
    g->nodes = NULL; g->node_ids = NULL; g->writers = NULL; g->node_len = 0; g->node_cap = 0;
}

// Reserve an id for the root (written inline by the driver): record for dedup, do NOT enqueue.
static inline uint64_t kama_ser_graph_reserve(kama_ser_graph* g, uint64_t addr) {
    uint64_t id = g->next_id++;
    kama_gmap_put(&g->ids, addr, id);
    return id;
}

// Intern a child reached via a handle: dedup by address; on first sight assign an id and enqueue (with its
// concrete-type writer). Returns id.
static inline uint64_t kama_ser_graph_intern(kama_ser_graph* g, uint64_t addr, void* obj, kama_node_writer wr) {
    uint64_t id;
    if (kama_gmap_get(&g->ids, addr, &id)) return id;   // already seen -> same id, no new entry (cycles ok)
    id = g->next_id++;
    kama_gmap_put(&g->ids, addr, id);
    if (g->node_len == g->node_cap) {
        size_t nc = g->node_cap ? g->node_cap * 2 : 8;
        g->nodes    = (void**)kama_realloc(g->nodes, nc * sizeof(void*));
        g->node_ids = (uint64_t*)kama_realloc(g->node_ids, nc * sizeof(uint64_t));
        g->writers  = (kama_node_writer*)kama_realloc(g->writers, nc * sizeof(kama_node_writer));
        g->node_cap = nc;
    }
    g->nodes[g->node_len] = obj;
    g->node_ids[g->node_len] = id;
    g->writers[g->node_len] = wr;
    g->node_len++;
    return id;
}

static inline size_t           kama_ser_graph_count(const kama_ser_graph* g)             { return g->node_len; }
static inline void*            kama_ser_graph_node(const kama_ser_graph* g, size_t i)    { return g->nodes[i]; }
static inline uint64_t         kama_ser_graph_node_id(const kama_ser_graph* g, size_t i) { return g->node_ids[i]; }
static inline kama_node_writer kama_ser_graph_writer(const kama_ser_graph* g, size_t i)  { return g->writers[i]; }

// Read-side graph context: id -> the boxed `Shared<T>` shell handle. Pass 1 registers every heap shell by id;
// pass 2 resolves pointer fields by id (a miss => a dangling reference => the lowering raises
// UnresolvedReference), retaining/downgrading the boxed handle to wire `Shared`/`Weak` edges. Stores the
// pointer as a uint64 (fits on wasm32 and 64-bit alike). `claimed` is the give-once ledger: an `Owned` field
// records its target id here so a second `Owned` claim of the same id raises DuplicateId.
typedef struct kama_de_graph {
    kama_gmap objs;      // id -> (uintptr_t) boxed Shared<T>*
    kama_gmap claimed;   // id -> 1 once an Owned field has taken it (absent = unclaimed)
} kama_de_graph;

static inline void  kama_de_graph_init(kama_de_graph* g) { kama_gmap_init(&g->objs); kama_gmap_init(&g->claimed); }
static inline void  kama_de_graph_free(kama_de_graph* g) { kama_gmap_free(&g->objs); kama_gmap_free(&g->claimed); }
static inline void  kama_de_graph_register(kama_de_graph* g, uint64_t id, void* obj) {
    kama_gmap_put(&g->objs, id, (uint64_t)(uintptr_t)obj);
}
static inline void* kama_de_graph_lookup(const kama_de_graph* g, uint64_t id) {
    uint64_t v;
    return kama_gmap_get(&g->objs, id, &v) ? (void*)(uintptr_t)v : NULL;
}
// Give-once claim: returns 1 if `id` was already claimed (=> DuplicateId), else marks it and returns 0.
static inline int kama_de_graph_claim(kama_de_graph* g, uint64_t id) {
    uint64_t seen;
    if (kama_gmap_get(&g->claimed, id, &seen)) return 1;
    kama_gmap_put(&g->claimed, id, 1);
    return 0;
}

// A reconstructed graph node during the two-pass read: the zeroed heap pointee (`ptr`, cast to the concrete
// `T*` by the emitted code) plus its own control block (strong=1 "construction owner"). Pointer fields are
// wired by constructing `Shared<X>`/`Weak<X>`/`Owned<X>` values directly over (ptr, ctrl) and bumping the
// counts — no dependency on a `Shared<T>` kama instantiation existing for every node type (an `Owned`-only
// pointee never names one). Boxes are grouped per concrete type in a `kama_de_arena` so the driver can drop
// each with the right `T__dtor` after transferring ownership to the returned root.
// `type_id` records the pointee's concrete graph-node id (set by the pass-1 driver from the wire `__type`),
// so a polymorphic `Shared<Contract>` edge can recover the right conformance vtable in pass 2. 0 by default.
typedef struct kama_de_box { void* ptr; kama_ctrl* ctrl; uint32_t type_id; } kama_de_box;

typedef struct kama_de_arena { kama_de_box** items; size_t len; size_t cap; } kama_de_arena;
static inline void kama_de_arena_init(kama_de_arena* a) { a->items = NULL; a->len = 0; a->cap = 0; }
static inline void kama_de_arena_free(kama_de_arena* a) { kama_free(a->items); a->items = NULL; a->len = 0; a->cap = 0; }
// Allocate a fresh shell box (ptr=NULL until the caller callocs the pointee; ctrl strong=1) and record it.
static inline kama_de_box* kama_de_arena_new(kama_de_arena* a) {
    if (a->len == a->cap) {
        size_t nc = a->cap ? a->cap * 2 : 8;
        a->items = (kama_de_box**)kama_realloc(a->items, nc * sizeof(kama_de_box*));
        a->cap = nc;
    }
    kama_de_box* b = (kama_de_box*)kama_alloc(sizeof(kama_de_box));
    b->ptr = NULL; b->ctrl = kama_ctrl_new(); b->type_id = 0;
    a->items[a->len++] = b;
    return b;
}

#endif // KAMA_RUNTIME_H
