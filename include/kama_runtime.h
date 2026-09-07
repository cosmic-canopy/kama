#ifndef KAMA_RUNTIME_H
#define KAMA_RUNTIME_H

// Minimal kama runtime. Dependency-light and WASM-safe: it leans only on the
// freestanding-friendly C headers so generated code compiles for native and
// for clang/emscripten wasm targets alike. Generated translation units include
// this first, then their own module header.

// ⚠️ The ONE feature-test macro this build asks for, and it has to be HERE — before the first standard
// header — because glibc's <features.h> latches its `__USE_*` set the first time it is read, and this
// file is the first include of every generated translation unit.
//
// Why at all: kama compiles its C with `-std=c11`, which sets `__STRICT_ANSI__`, and glibc then hides
// anything past ISO C. `clock_gettime` and `getaddrinfo` are both on the far side of that line, along with
// `struct addrinfo` — and a seam header cannot buy its way out by declaring the symbol itself, because
// that struct's member ORDER is not fixed by POSIX (glibc and musl put `ai_canonname` after `ai_addr`,
// macOS before). `_DEFAULT_SOURCE` asks for exactly the namespace this build had before `-std=c11`, so it
// widens what is visible and changes nothing that was.
//
// ⚠️ Asking LATER does not work, and the failure is instructive: setting the macro inside kama_os.h and
// clearing <features.h>'s include guard to force a recompute really does expose `getaddrinfo` — and
// breaks `WIFEXITED`, because <sys/wait.h> defines the `W*` macros only when <stdlib.h> has not already
// been read under a feature set that would have defined them, and <stdlib.h> HAD been, under the old one.
// Eighty fixtures. A feature set is a property of a translation unit, not of a header.
#if defined(__linux__) && !defined(_GNU_SOURCE) && !defined(_DEFAULT_SOURCE)
#  define _DEFAULT_SOURCE 1
#endif

#include <stdint.h>    // int32_t … (types only — no callable C functions)
#include <stdbool.h>   // bool       (type only)
#include <stddef.h>    // size_t, ptrdiff_t, NULL (types only) — ptrdiff_t IS kama's `isize`, so this is
                       // load-bearing for every length/index in the runtime, not just for NULL
#include <limits.h>    // CHAR_BIT   (freestanding header — C11 §4.6, so MCU-safe)

// The premises kama's scalar sizes rest on, checked by the C compiler for the ACTUAL target on every
// build. kama folds `sizeof` for fixed-width scalars (CEmitter::scalarByteSize) and hardcodes the same
// widths in two other places — `emitBitcast`'s union type-pun table and the kama_f32_bits/kama_f64_bits
// reinterprets below, which the binary serializer's wire format depends on. Those were silent
// assumptions; these three lines make them verified facts. kama accepts arbitrary triples and any `--cc`
// you hand it, so a per-build assert is the only thing that can police a wrong flag or a changed
// toolchain default — a curated target list cannot.
_Static_assert(CHAR_BIT == 8,       "kama: a byte must be 8 bits — scalar sizeof folding assumes it "
                                    "(ISO C fixes int32_t at 32 BITS; sizeof counts chars)");
_Static_assert(sizeof(float)  == 4, "kama: float32 maps to C float and must be 4 bytes");
_Static_assert(sizeof(double) == 8, "kama: float64 maps to C double and must be 8 bytes "
                                    "(avr-gcc defaults to -mdouble=32 — build that target with -mdouble=64)");

// A fatal path NEVER RETURNS, and the C compiler has to be told so — otherwise a kama function whose last
// statement is `panic(msg: …)` looks like it falls off the end, and clang's -Werror=return-type rejects it
// even though kama's own fall-off-the-end analysis (CEmitter::alwaysExits) correctly accepted it. The two
// analyses have to agree; this is what makes them.
#ifndef KAMA_NORETURN
#  if defined(__GNUC__) || defined(__clang__)
#    define KAMA_NORETURN __attribute__((noreturn))
#  elif defined(_MSC_VER)
#    define KAMA_NORETURN __declspec(noreturn)
#  else
#    define KAMA_NORETURN _Noreturn
#  endif
#endif


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

// KAMA_ISOLATE_LOCAL — the per-isolate storage class for a module-level `static` (MCU campaign step 1).
// A module `static` is per-isolate BY CONSTRUCTION (concurrency spec, "three sharing seams"): it cannot be
// seen by another isolate, so it cannot race; cross-isolate sharing stays on the `Atomic<T>` seam.
//  - native   : `_Thread_local` → each isolate (pthread) gets its own const/zero-initialized copy. C11 does
//               the per-isolate init automatically — no synthesized startup hook, because the init is const.
//  - wasm      : `_Thread_local` too. kama's wasm isolates are emscripten pthreads (`-sPROXY_TO_PTHREAD
//               -pthread`) that SHARE one linear memory (SharedArrayBuffer), exactly like native pthreads
//               share an address space — so a plain `static` would be shared/racy; TLS makes it per-isolate.
//  - embedded  : empty → one core = one isolate; a plain C `static`, zero cost (set when `--target embedded` lands).
// ⚠️ A thread kama did NOT create (a C callback) is a fresh isolate too: it sees every static at its
// declared initialiser. The compiler checks that at compile time inside `@foreignEntry` regions — SPEC,
// "Foreign entry points" — so the state a callback needs travels through the pointer the C API hands it.
#if defined(KAMA_TARGET_EMBEDDED)
  #define KAMA_ISOLATE_LOCAL
#else
  #define KAMA_ISOLATE_LOCAL _Thread_local
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

// IEEE-754 bit reinterpretation (for the binary serializer's exact float encoding — a value cast would
// round, these preserve the bit pattern). memcpy is the portable, strict-aliasing-safe reinterpret.
static inline uint32_t kama_f32_bits(float v)        { uint32_t b; kama_copy(&b, &v, 4); return b; }
static inline float    kama_f32_from_bits(uint32_t b){ float v;    kama_copy(&v, &b, 4); return v; }
static inline uint64_t kama_f64_bits(double v)       { uint64_t b; kama_copy(&b, &v, 8); return b; }
static inline double   kama_f64_from_bits(uint64_t b){ double v;   kama_copy(&v, &b, 8); return v; }

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

// Allocator-aware Owned<I> (M11d): the fat handle carries its own copy of the caller's allocator value
// `alloc` (a lightweight value handle over externally-owned state, e.g. an arena) + the concrete pointee's
// `objsize`, so the drop frees `obj` through THAT allocator instead of libc. Selected only for a stateful
// allocator; a default GlobalAllocator box keeps the plain macros above (byte-identical).
#define KAMA_OWNED_IFACE_ALLOC_TYPE(NAME, VTBL, ATYPE) \
    typedef struct NAME { void* obj; const VTBL* vtbl; ATYPE alloc; size_t objsize; } NAME;
#define KAMA_OWNED_IFACE_ALLOC_FUNCS(NAME, ATYPE)                             \
static inline void NAME##__dtor(NAME* self) {                                  \
    if (self->obj) {                                                           \
        if (self->vtbl && self->vtbl->__dtor) self->vtbl->__dtor(self->obj);   \
        ATYPE##__deallocate(&self->alloc, self->obj, self->objsize);           \
        self->obj = NULL;                                                      \
    }                                                                          \
}

// Shared<T> — ref-counted shared ownership (shared_ptr / Rc). Copy retains;
// drop releases; the pointee is destroyed + freed when the last strong handle
// goes away. The control block (counts) is a SEPARATE allocation so a future
// Weak<T> can outlive the T. `ptr` mirrors Owned's, so auto-deref is identical.
typedef struct kama_ctrl { size_t strong; size_t weak; } kama_ctrl;   // weak: reserved for Weak<T>
#include "kama_ctrl.h"   // M6.2: the strong/weak count ops (plain + atomic flavor) — needs kama_ctrl above
// kama_ctrl_new is defined further down, next to kama_panic: it has to CHECK its allocation, and the
// panic path (and `kama_string`) is declared below this point. Its only in-header caller is well past it.
static inline kama_ctrl* kama_ctrl_new(void);
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

// Allocator-aware Shared<I> (M11d): fat handle carries its own `alloc` value copy + pointee `objsize`.
// Both the pointee AND the ctrl block are drawn from `alloc` at the new-site, so the last strong drop frees
// both through it (cycle-safe order preserved: release AFTER the pointee dtor). A Weak that outlives the
// Shared frees the ctrl through its OWN equal `alloc` copy (all copies are equal — a value handle over
// externally-owned state). Default GlobalAllocator boxes keep the plain macros above (byte-identical).
#define KAMA_SHARED_IFACE_ALLOC_TYPE(NAME, VTBL, ATYPE) \
    typedef struct NAME { void* obj; const VTBL* vtbl; kama_ctrl* ctrl; ATYPE alloc; size_t objsize; } NAME;
#define KAMA_SHARED_IFACE_ALLOC_FUNCS(NAME, ATYPE)                            \
static inline void NAME##__dtor(NAME* self) {                                  \
    if (self->ctrl) {                                                          \
        if (self->ctrl->strong == 1) {                                        \
            if (self->vtbl && self->vtbl->__dtor) self->vtbl->__dtor(self->obj); \
            ATYPE##__deallocate(&self->alloc, self->obj, self->objsize);       \
            self->ctrl->strong = 0;                                           \
            if (self->ctrl->weak == 0)                                        \
                ATYPE##__deallocate(&self->alloc, (void*)self->ctrl, sizeof(kama_ctrl)); \
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

// Allocator-aware Weak<I> (M11d): same fat layout as Shared + `alloc`/`objsize`; counts `weak`, never
// touches the concrete object. Frees the ctrl (through its own `alloc` copy) when BOTH counts reach 0.
// `__upgrade` MUST propagate `alloc`+`objsize` into the returned Shared so the upgraded strong handle frees
// through the right allocator (init `= {0}` so the empty/expired case leaves them zeroed).
#define KAMA_WEAK_IFACE_ALLOC_TYPE(NAME, VTBL, ATYPE) \
    typedef struct NAME { void* obj; const VTBL* vtbl; kama_ctrl* ctrl; ATYPE alloc; size_t objsize; } NAME;
#define KAMA_WEAK_IFACE_ALLOC_FUNCS(NAME, ATYPE, SHARED_NAME)                 \
static inline void NAME##__dtor(NAME* self) {                                  \
    if (self->ctrl) {                                                          \
        if (--self->ctrl->weak == 0 && self->ctrl->strong == 0)              \
            ATYPE##__deallocate(&self->alloc, (void*)self->ctrl, sizeof(kama_ctrl)); \
        self->obj = NULL; self->ctrl = NULL;                                  \
    }                                                                          \
}                                                                              \
static inline bool NAME##__expired(NAME* self) {                              \
    return self->ctrl == NULL || self->ctrl->strong == 0;                     \
}                                                                              \
static inline SHARED_NAME NAME##__upgrade(NAME* self) {                        \
    SHARED_NAME s = {0};                                                      \
    if (self->ctrl && self->ctrl->strong > 0) {                              \
        self->ctrl->strong++; s.obj = self->obj; s.vtbl = self->vtbl;         \
        s.ctrl = self->ctrl; s.alloc = self->alloc; s.objsize = self->objsize; \
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

// Raw byte write to a standard fd with NO <stdio.h> — the one place that spells the platform's
// write syscall. Everything below (bounds trap, panic, assert, print/log floor) goes through here.
// Not compiled on the freestanding path: every caller's KAMA_TARGET_EMBEDDED branch routes to the
// weak kama_panic_handler / kama_log_sink instead, because a bare-metal target has no fd.
//   - POSIX/emscripten spell it `write`; Windows (MSVC/UCRT/MinGW) spell it `_write`.
//   - macOS needs the __asm("_write") label: <unistd.h> declares write with __DARWIN_ALIAS_C, and a
//     later include of it (kama_isolate.h pulls <unistd.h> for sysconf) would otherwise be "cannot
//     apply asm label to function after its first use". Carrying the SAME label here makes that a
//     consistent redeclaration. (The Windows `_write` spelling does NOT substitute: macOS `write`
//     mangles to object symbol `_write`, so declaring `_write` in C would mangle to `__write`.)
#if !defined(KAMA_TARGET_EMBEDDED)
static inline long kama_raw_write(int fd, const void* bytes, size_t n) {
#if defined(_WIN32)
    extern int _write(int, const void*, unsigned int);
    return _write(fd, bytes, (unsigned int)n);
#elif defined(__APPLE__)
    extern long write(int, const void*, size_t) __asm("_write");
    return write(fd, bytes, n);
#else
    extern long write(int, const void*, size_t);
    return write(fd, bytes, n);
#endif
}
#endif

// Bounds-check trap: a clean panic (not undefined behavior) on out-of-range.
// Formats its own message and writes to stderr (fd 2) so it needs no <stdio.h>.
// `unsigned long long`, not `size_t`: the widest value these messages carry is a 64-bit one, and a
// `size_t` parameter would silently truncate it to 32 bits on wasm32 / thumbv6m. 20 digits is UINT64_MAX.
static inline void kama_u64_to_buf(char* buf, size_t* p, unsigned long long v) {
    char tmp[20]; int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
    while (t) buf[(*p)++] = tmp[--t];
}
// The signed twin. Negating LLONG_MIN would itself overflow, so the magnitude is taken in unsigned math.
static inline void kama_i64_to_buf(char* buf, size_t* p, long long v) {
    unsigned long long m;
    if (v < 0) { buf[(*p)++] = '-'; m = (unsigned long long)(-(v + 1)) + 1ULL; }
    else       { m = (unsigned long long)v; }
    kama_u64_to_buf(buf, p, m);
}
// ── Recoverable regions: `@onPanic(recover: <literal>)` (ROADMAP row 2) ─────────────────────────────
// A panic inside a region declared `@onPanic` does not terminate the process: the fault leaf (bounds,
// narrowing, arithmetic, division, shift, float cast, `panic`, `assert`, OOM) writes its message, then
// `kama_try_recover` longjmps to the region's prologue, which returns the literal the region named. The
// slot is PER THREAD (`KAMA_ISOLATE_LOCAL`), so a foreign audio thread's region never sees another
// isolate's; regions nest (each saves the previous slot). The process-level panic hook is NOT run for a
// recovered panic — the region's declared recovery IS the handling, and the hook keeps its contract of
// running only when the process is about to terminate.
//
// The compiler admits a region only when it is `@noheap` and no frame it reaches owns a destructible
// local, so the longjmp skips no destructor. `<setjmp.h>` is hosted-only (not in C11's freestanding
// list), so the emitted C includes it — and defines KAMA_ONPANIC — only for a program that declares a
// region; every other program keeps this header dependency-light and the recovery path empty.
#if defined(KAMA_ONPANIC)
typedef struct kama_recover { jmp_buf jb; struct kama_recover* prev; } kama_recover_t;
extern KAMA_ISOLATE_LOCAL kama_recover_t* kama_recover_top;   /* defined in the entry TU */
static inline void kama_try_recover(void) {
    kama_recover_t* r = kama_recover_top;
    if (r) { kama_recover_top = r->prev; longjmp(r->jb, 1); }
}
#else
static inline void kama_try_recover(void) { }
#endif

#if defined(KAMA_TARGET_EMBEDDED)
// Freestanding trap policy (MCU campaign step 3). On a bare-metal target there is no fd 2 to write
// to and no `abort` under `-nostdlib`, so every fatal condition (bounds/slice/panic/OOM) funnels
// through one OVERRIDABLE weak hook. The default spins in a `__builtin_trap` loop (a debugger breaks
// here / the MCU resets); a firmware author provides a strong `kama_panic_handler` to blink an SOS,
// reset, or log over a peripheral. Weak symbols are supported by clang/gcc = the embedded toolchains.
__attribute__((weak)) KAMA_NORETURN void kama_panic_handler(void) { for (;;) __builtin_trap(); }
static inline KAMA_NORETURN void kama_bounds_fail(size_t i, size_t len) {
    (void)i; (void)len;
    kama_try_recover();
    kama_panic_handler();
    for (;;) {}   // kama_panic_handler must not return; belt-and-suspenders if a user override does
}
static inline KAMA_NORETURN void kama_narrow_fail_s(long long v, long long lo, unsigned long long hi) {
    (void)v; (void)lo; (void)hi;
    kama_try_recover();
    kama_panic_handler();
    for (;;) {}
}
static inline KAMA_NORETURN void kama_narrow_fail_u(unsigned long long v, long long lo, unsigned long long hi) {
    (void)v; (void)lo; (void)hi;
    kama_try_recover();
    kama_panic_handler();
    for (;;) {}
}
static inline KAMA_NORETURN void kama_arith_fail(long long v, long long lo, unsigned long long hi) {
    (void)v; (void)lo; (void)hi;
    kama_try_recover();
    kama_panic_handler();
    for (;;) {}
}
static inline KAMA_NORETURN void kama_simd_arith_fail(int lane) {
    (void)lane;
    kama_try_recover();
    kama_panic_handler();
    for (;;) {}
}
static inline KAMA_NORETURN void kama_sdiv_fail(void) {
    kama_try_recover();
    kama_panic_handler();
    for (;;) {}
}
static inline KAMA_NORETURN void kama_div_zero_fail(void) {
    kama_try_recover();
    kama_panic_handler();
    for (;;) {}
}
static inline KAMA_NORETURN void kama_shift_fail(long long n, int width) {
    (void)n; (void)width;
    kama_try_recover();
    kama_panic_handler();
    for (;;) {}
}
static inline KAMA_NORETURN void kama_fcast_fail(void) {
    kama_try_recover();
    kama_panic_handler();
    for (;;) {}
}
static inline KAMA_NORETURN void kama_wide_arith_fail(const char* op, int width) {
    (void)op; (void)width;
    kama_try_recover();
    kama_panic_handler();
    for (;;) {}
}
static inline KAMA_NORETURN void kama_utf8_split_fail(size_t off) {
    (void)off;
    kama_try_recover();
    kama_panic_handler();
    for (;;) {}
}
// On embedded the fatal handler IS the weak `kama_panic_handler` symbol (firmware provides a strong
// override at link time), so a runtime setter doesn't apply — this stub lets the prelude `setPanicHandler`
// surface still compile on --target embedded (a no-op; use the weak-symbol mechanism instead).
static inline void kama_set_panic_handler(void (*h)(void)) { (void)h; }
#else
// Hosted analogue of the embedded weak `kama_panic_handler`: a settable fatal handler for cleanup/exhibition
// (a shipped game/GUI with no terminal shows a dialog / flushes a save instead of a bare stderr `abort`).
// Contract (baked in): SET-ONCE (first registration wins — register at startup before spawning isolates,
// same rule as the argv stash, so the read-only slot is race-free); RE-ENTRANCY-GUARDED (a panic while
// already handling one skips the hook and hard-aborts — no infinite recursion); and the runtime ALWAYS
// TERMINATES after it (it is not a resume point — recovery is `Result`, or a declared `@onPanic` region,
// which recovers BEFORE the hook and never reaches it). Covers every hosted fatal path (bounds / panic /
// assert), matching the embedded "one hook for all fatal conditions" policy.
//
// EXTERNAL LINKAGE (single definition in the entry TU, emitted by the compiler — see `isEntry` in
// kama.cemit.cpp). This handler is PROCESS-GLOBAL by contract, not per-isolate, so it must be ONE object: a
// panic path is `static inline` and inlined into EVERY TU, and `setPanicHandler` writes the slot from the
// entry TU — a per-TU `static` slot would leave a panic that ORIGINATES in another TU (e.g. a bounds-check
// failure in stdlib/library code) reading its own empty copy and silently taking the default abort instead
// of the user's handler. The re-entrancy flag is likewise one global (a re-entrant panic from any TU during
// handling skips the hook). (Genuinely isolate-local state uses KAMA_ISOLATE_LOCAL, not plain `static`.)
extern void (*kama_panic_hook)(void);
extern int kama_in_panic_hook;
static inline void kama_set_panic_handler(void (*h)(void)) { if (!kama_panic_hook) kama_panic_hook = h; }
static inline void kama_run_panic_hook(void) {
    kama_try_recover();   /* an armed `@onPanic` region recovers here and never reaches the hook */
    if (kama_panic_hook && !kama_in_panic_hook) { kama_in_panic_hook = 1; kama_panic_hook(); }
}
static inline KAMA_NORETURN void kama_bounds_fail(size_t i, size_t len) {
    extern void abort(void);
    char buf[96]; size_t p = 0;
    const char* a = "kama: index ";              while (*a) buf[p++] = *a++;
    kama_u64_to_buf(buf, &p, i);
    const char* b = " out of bounds (length ";    while (*b) buf[p++] = *b++;
    kama_u64_to_buf(buf, &p, len);
    const char* c = ")\n";                        while (*c) buf[p++] = *c++;
    (void)kama_raw_write(2, buf, p);
    kama_run_panic_hook();   // custom exhibition (dialog / telemetry); runtime still terminates
    abort();
}
// A `cast<T>` whose runtime value does not FIT `T`. Distinct from kama_bounds_fail: nothing is indexed,
// so the message names the value and the range it missed. Same clean abort, same overridable hook.
static inline KAMA_NORETURN void kama_narrow_fail_s(long long v, long long lo, unsigned long long hi) {
    extern void abort(void);
    char buf[160]; size_t p = 0;
    const char* a = "kama: value ";                 while (*a) buf[p++] = *a++;
    kama_i64_to_buf(buf, &p, v);
    const char* b = " does not fit the target range [";  while (*b) buf[p++] = *b++;
    kama_i64_to_buf(buf, &p, lo);
    const char* c = ", ";                           while (*c) buf[p++] = *c++;
    kama_u64_to_buf(buf, &p, hi);
    const char* d = "]\n";                          while (*d) buf[p++] = *d++;
    (void)kama_raw_write(2, buf, p);
    kama_run_panic_hook();   // custom exhibition (dialog / telemetry); runtime still terminates
    abort();
}
// The unsigned-source twin: the value cannot be printed as signed (it may exceed LLONG_MAX), while the
// target's floor still can be. Splitting the two is what keeps both ends of the message exact.
static inline KAMA_NORETURN void kama_narrow_fail_u(unsigned long long v, long long lo, unsigned long long hi) {
    extern void abort(void);
    char buf[160]; size_t p = 0;
    const char* a = "kama: value ";                 while (*a) buf[p++] = *a++;
    kama_u64_to_buf(buf, &p, v);
    const char* b = " does not fit the target range [";  while (*b) buf[p++] = *b++;
    kama_i64_to_buf(buf, &p, lo);
    const char* c = ", ";                           while (*c) buf[p++] = *c++;
    kama_u64_to_buf(buf, &p, hi);
    const char* d = "]\n";                          while (*d) buf[p++] = *d++;
    (void)kama_raw_write(2, buf, p);
    kama_run_panic_hook();
    abort();
}
// A sub-`int` SIGNED arithmetic result outside its own type's range. Distinct from kama_narrow_fail_s:
// a narrowing `cast<int8>(300)` is a conversion the author WROTE, while this is an arithmetic result the
// author did not — so the message names the operation, not a target.
static inline KAMA_NORETURN void kama_arith_fail(long long v, long long lo, unsigned long long hi) {
    extern void abort(void);
    char buf[160]; size_t p = 0;
    const char* a = "kama: arithmetic overflow -- the result ";  while (*a) buf[p++] = *a++;
    kama_i64_to_buf(buf, &p, v);
    const char* b = " does not fit the operand type [";          while (*b) buf[p++] = *b++;
    kama_i64_to_buf(buf, &p, lo);
    const char* c = ", ";                                        while (*c) buf[p++] = *c++;
    kama_u64_to_buf(buf, &p, hi);
    const char* d = "]\n";                                       while (*d) buf[p++] = *d++;
    (void)kama_raw_write(2, buf, p);
    kama_run_panic_hook();
    abort();
}
// A signed integer LANE that overflowed. Named separately from kama_arith_fail because the lane index is
// the thing a reader needs — the other lanes are fine, and "which one" is not otherwise recoverable.
static inline KAMA_NORETURN void kama_simd_arith_fail(int lane) {
    extern void abort(void);
    char buf[96]; size_t p = 0;
    const char* a = "kama: arithmetic overflow in Simd lane ";  while (*a) buf[p++] = *a++;
    kama_i64_to_buf(buf, &p, (long long)lane);
    const char* b = "\n";                                       while (*b) buf[p++] = *b++;
    (void)kama_raw_write(2, buf, p);
    kama_run_panic_hook();
    abort();
}
// `TYPE_MIN / -1`. Named separately because it can report no value: the result is `-TYPE_MIN`, the one
// number the type cannot hold, and at 64 bits no wider signed type can hold it either.
static inline KAMA_NORETURN void kama_sdiv_fail(void) {
    extern void abort(void);
    const char* m = "kama: arithmetic overflow -- TYPE_MIN / -1 has no representable result\n";
    size_t n = 0; while (m[n]) ++n;
    (void)kama_raw_write(2, m, n);
    kama_run_panic_hook();
    abort();
}
// The three faults that used to be `-fsanitize-trap` — a bare `ud2` that printed nothing, ran no hook and
// could not be recovered from. Each is now kama's own check (KAMA_DIV/KAMA_MOD/KAMA_SHL/KAMA_SHR and
// kama_f2i_chk below), so every fault a kama program can raise takes ONE path: a message, the panic hook,
// and — inside an `@onPanic` region — recovery. It is also what lets the driver pass no `-fsanitize` at
// all, which is what emscripten's Wasm Workers (an AudioWorklet) required.
static inline KAMA_NORETURN void kama_div_zero_fail(void) {
    extern void abort(void);
    const char* m = "kama: division by zero\n";
    size_t n = 0; while (m[n]) ++n;
    (void)kama_raw_write(2, m, n);
    kama_run_panic_hook();
    abort();
}
static inline KAMA_NORETURN void kama_shift_fail(long long n, int width) {
    extern void abort(void);
    char buf[96]; size_t p = 0;
    const char* a = "kama: shift by ";                        while (*a) buf[p++] = *a++;
    kama_i64_to_buf(buf, &p, n);
    const char* b = " is outside the ";                       while (*b) buf[p++] = *b++;
    kama_i64_to_buf(buf, &p, (long long)width);
    const char* c = "-bit width\n";                           while (*c) buf[p++] = *c++;
    (void)kama_raw_write(2, buf, p);
    kama_run_panic_hook();
    abort();
}
static inline KAMA_NORETURN void kama_fcast_fail(void) {
    extern void abort(void);
    const char* m = "kama: float -> int conversion is out of range (or NaN)\n";
    size_t n = 0; while (m[n]) ++n;
    (void)kama_raw_write(2, m, n);
    kama_run_panic_hook();
    abort();
}
// A signed `+ - *` or negation that overflowed a 32/64-bit type (debug tier). Like kama_sdiv_fail it can
// report no result — the result is the one number the type cannot hold — so it names the operation.
static inline KAMA_NORETURN void kama_wide_arith_fail(const char* op, int width) {
    extern void abort(void);
    char buf[96]; size_t p = 0;
    const char* a = "kama: arithmetic overflow -- int";          while (*a) buf[p++] = *a++;
    kama_i64_to_buf(buf, &p, (long long)width);
    const char* b = " ";                                        while (*b) buf[p++] = *b++;
    while (*op) buf[p++] = *op++;
    const char* c = " overflowed\n";                            while (*c) buf[p++] = *c++;
    (void)kama_raw_write(2, buf, p);
    kama_run_panic_hook();
    abort();
}
// A byte offset that lands INSIDE a UTF-8 character. Distinct from kama_bounds_fail: the offset is in
// range, so "out of bounds" would name the wrong problem.
static inline KAMA_NORETURN void kama_utf8_split_fail(size_t off) {
    extern void abort(void);
    char buf[96]; size_t p = 0;
    const char* a = "kama: byte offset ";                     while (*a) buf[p++] = *a++;
    kama_u64_to_buf(buf, &p, off);
    const char* b = " splits a UTF-8 character\n";            while (*b) buf[p++] = *b++;
    (void)kama_raw_write(2, buf, p);
    kama_run_panic_hook();
    abort();
}
#endif

// A NARROWING CAST TRAPS. `cast<T>(x)` preserves the VALUE, so a runtime value that does not fit `T` is
// not a conversion but a different number — the same reasoning that already rejects the constant
// `cast<int8>(300)` at compile time, and the same policy C's out-of-range float->int conversion already
// gets (`-fsanitize-trap=float-cast-overflow`, every build). The escapes are `truncate<T>(x)` (keep the
// low bits) and `try cast<T>(x)` (`Optional<T>`), which emit no check.
//
// TWO checkers, not one per target: the target's bounds arrive as ARGUMENTS the way `kama_bounds_fail`
// takes a length, so the range table stays in the compiler (`cNumRangeText`) and is stated once. They
// split on the SOURCE's signedness, which is what a single checker cannot do: a `uint64_t` above
// LLONG_MAX cannot be examined as `long long`, and a negative source cannot be examined as unsigned.
//
// `lo`/`hi` are compile-time constants at every call site, so the comparison that cannot fail folds away
// -- an `int32 -> int64` widening costs nothing and a `uint64 -> int32` narrowing costs one branch. That
// is why there is no hand-written one-sided variant: `-O2` already emits it.
static inline long long kama_narrow_chk_s(long long v, long long lo, unsigned long long hi) {
    if (v < lo) kama_narrow_fail_s(v, lo, hi);
    if (v >= 0 && (unsigned long long)v > hi) kama_narrow_fail_s(v, lo, hi);
    return v;
}
// No `lo` test: every numeric target in the language has a floor of 0 or below, so an unsigned source is
// never under it. `lo` is still carried, for the message.
static inline unsigned long long kama_narrow_chk_u(unsigned long long v, long long lo, unsigned long long hi) {
    if (v > hi) kama_narrow_fail_u(v, lo, hi);
    return v;
}

// -- sub-`int` SIGNED arithmetic overflow -------------------------------------------------------------
//
// `int32`/`int64` get their overflow trap from `-fsanitize=signed-integer-overflow`. `int8`/`int16` do
// NOT, and not because the flag is missing: C promotes both operands to `int`, so `100 + 100` is
// computed as 200 where nothing overflows, and the narrowing back to `int8` is a *conversion*, which
// that check does not watch. The value silently became -56 while `int32 MAX + 1` trapped, and SPEC
// documented the split as a ⚠️ rather than the rule being wrong. It is the rule being wrong: 200 is not
// representable in the type kama says the expression has.
//
// The message says OVERFLOW rather than reusing `kama_narrow_fail_s`'s "does not fit": a narrowing
// `cast<int8>(300)` is a conversion the author asked for, while this is an arithmetic result the author
// did not. Same clean abort, same overridable hook.
static inline long long kama_arith_chk(long long v, long long lo, unsigned long long hi) {
    if (v < lo) kama_arith_fail(v, lo, hi);
    if (v >= 0 && (unsigned long long)v > hi) kama_arith_fail(v, lo, hi);
    return v;
}
// -- TYPE_MIN / -1, the one signed overflow that is NOT `-fwrapv`'s to define ------------------------
//
// `-fwrapv` defines signed `+ - *` as two's-complement wrapping, and that is the whole of what it
// promises — division is not on its list, and `INT32_MIN / -1` stays undefined under it. On aarch64 the
// hardware quietly yields `INT32_MIN`; on x86 the same expression raises SIGFPE. SPEC says it traps in
// EVERY build, so it is checked here rather than left to a flag.
//
// This is why the driver was passing `-fsanitize=signed-integer-overflow` in RELEASE as well as debug —
// that one case was the only thing it was buying there. ⚠️ The price was much larger than the purchase:
// Apple clang 21 does not let `-fwrapv` suppress the sanitizer (Ubuntu clang 18.1.3 and gcc 13.3 do), so
// a macOS release build carried `adds; b.vs; brk` on every signed add and `smull; cmp; b.ne; brk` on
// every multiply — a compare and a branch on arithmetic that kama promises is at C parity. Checking the
// one case explicitly costs one predictable branch per signed DIVISION (already a 20-40 cycle
// instruction) and lets the release tier drop the sanitizer entirely.
//
// The test is on the OPERANDS, not the result: `INT64_MIN / -1` overflows the very `long long` a
// result-based check would have to compute it in. Divide-by-zero is the OTHER fault a division can raise,
// and the same helpers check it, in every build (it was `-fsanitize-trap` until 0.9.160).
// ⚠️ It reports NO value, and that is forced rather than lazy: the result of `TYPE_MIN / -1` is
// `-TYPE_MIN`, which is exactly the one number the type cannot hold — and at 64 bits it cannot be held
// by `long long` either, so there is no width to pass it in. The first version of this macro computed
// the bounds inline and made clang warn "overflow in expression" on its own definition, in every
// program. The message names the operation instead, which is the whole of what a reader needs.
// Division and remainder with no undefined behaviour, in EVERY build. Two faults: a zero divisor, and
// `TYPE_MIN / -1` (the one signed division that overflows; `TYPE_MIN % -1` is 0 by kama's rule, where C
// traps computing it). One predictable branch each on an instruction that already costs 20-40 cycles,
// then a cold noreturn leaf. `KAMA_DIV`/`KAMA_MOD` dispatch on the PROMOTED operand type — `_Generic`
// reads the type of `(a) / (b)` and never evaluates it — so a sub-`int` operand takes the `int` helper
// exactly as C promotes it, and the associations are C's builtin types, never the <stdint.h> typedefs
// (`long` and `unsigned long` are what `int64_t`/`size_t` spell on Linux). Float division is IEEE, takes
// no check, and keeps its own type. No `default:` on purpose: a type nobody listed is a compile error
// here, not a silently unchecked division.
#define KAMA_DIV_CHK(NAME, T, TMIN, SIGNED)                                     \
static inline T NAME(T a, T b) {                                                \
    if (b == 0) kama_div_zero_fail();                                           \
    if ((SIGNED) && b == (T)-1 && a == (TMIN)) kama_sdiv_fail();                \
    return a / b;                                                               \
}
#define KAMA_MOD_CHK(NAME, T, SIGNED)                                           \
static inline T NAME(T a, T b) {                                                \
    if (b == 0) kama_div_zero_fail();                                           \
    if ((SIGNED) && b == (T)-1) return 0;                                       \
    return a % b;                                                               \
}
KAMA_DIV_CHK(kama_div_i32, int32_t,  INT32_MIN, 1)
KAMA_DIV_CHK(kama_div_i64, int64_t,  INT64_MIN, 1)
KAMA_DIV_CHK(kama_div_u32, uint32_t, 0,         0)
KAMA_DIV_CHK(kama_div_u64, uint64_t, 0,         0)
KAMA_MOD_CHK(kama_mod_i32, int32_t,  1)
KAMA_MOD_CHK(kama_mod_i64, int64_t,  1)
KAMA_MOD_CHK(kama_mod_u32, uint32_t, 0)
KAMA_MOD_CHK(kama_mod_u64, uint64_t, 0)
static inline float  kama_div_f32(float a, float b)   { return a / b; }
static inline double kama_div_f64(double a, double b) { return a / b; }
#define KAMA_DIV(a, b) _Generic((a) / (b),                                      \
    int: kama_div_i32, long: kama_div_i64, long long: kama_div_i64,             \
    unsigned int: kama_div_u32, unsigned long: kama_div_u64,                    \
    unsigned long long: kama_div_u64,                                           \
    float: kama_div_f32, double: kama_div_f64)((a), (b))
#define KAMA_MOD(a, b) _Generic((a) % (b),                                      \
    int: kama_mod_i32, long: kama_mod_i64, long long: kama_mod_i64,             \
    unsigned int: kama_mod_u32, unsigned long: kama_mod_u64,                    \
    unsigned long long: kama_mod_u64)((a), (b))

// A shift amount at or past the PROMOTED width, or negative, is UB in C and was `-fsanitize-trap`. Now one
// branch against the width, then the shift. A signed LEFT shift is done in the unsigned peer, because a
// shift into the sign bit is DEFINED in kama ("computed in the unsigned type — a defined bit pattern"),
// so `1 << 31` is legal and `3i8 << 7i8` is -128. The amount is widened to `long long`, so a huge unsigned
// amount reads as negative and fails the same test.
#define KAMA_SHL_CHK(NAME, T, UT, W)                                            \
static inline T NAME(T a, long long n) {                                        \
    if (n < 0 || n >= (W)) kama_shift_fail(n, (W));                             \
    return (T)((UT)a << n);                                                     \
}
#define KAMA_SHR_CHK(NAME, T, W)                                                \
static inline T NAME(T a, long long n) {                                        \
    if (n < 0 || n >= (W)) kama_shift_fail(n, (W));                             \
    return a >> n;                                                              \
}
KAMA_SHL_CHK(kama_shl_i32, int32_t,  uint32_t, 32)
KAMA_SHL_CHK(kama_shl_i64, int64_t,  uint64_t, 64)
KAMA_SHL_CHK(kama_shl_u32, uint32_t, uint32_t, 32)
KAMA_SHL_CHK(kama_shl_u64, uint64_t, uint64_t, 64)
KAMA_SHR_CHK(kama_shr_i32, int32_t,  32)
KAMA_SHR_CHK(kama_shr_i64, int64_t,  64)
KAMA_SHR_CHK(kama_shr_u32, uint32_t, 32)
KAMA_SHR_CHK(kama_shr_u64, uint64_t, 64)
#define KAMA_SHL(a, b) _Generic((a) + 0,                                        \
    int: kama_shl_i32, long: kama_shl_i64, long long: kama_shl_i64,             \
    unsigned int: kama_shl_u32, unsigned long: kama_shl_u64,                    \
    unsigned long long: kama_shl_u64)((a), (long long)(b))
#define KAMA_SHR(a, b) _Generic((a) + 0,                                        \
    int: kama_shr_i32, long: kama_shr_i64, long long: kama_shr_i64,             \
    unsigned int: kama_shr_u32, unsigned long: kama_shr_u64,                    \
    unsigned long long: kama_shr_u64)((a), (long long)(b))

// An out-of-range (or NaN) float -> int conversion is UB in C and was `-fsanitize-trap`. The check is a
// range test on the double; the truncation toward zero is still C's own cast, applied by the caller to
// the value handed back. `hi + 1.0` is exact for every target width — 2^31, 2^63 and 2^64 are all
// representable — and a NaN fails both comparisons.
static inline double kama_f2i_chk(double v, long long lo, unsigned long long hi) {
    if (!(v >= (double)lo && v < (double)hi + 1.0)) kama_fcast_fail();
    return v;
}


// Signed overflow TRAPS in debug and WRAPS in release — the same two-tier rule `int32`/`int64` get from
// `-fsanitize=signed-integer-overflow` (debug only, since 0.9.125) + `-fwrapv`. `NDEBUG` is the release
// tier's marker; the driver passes it with `-O3`. The release arm is a plain truncation, which IS the
// defined two's-complement wrap, so this costs exactly nothing in a release build.
//
// ⚠️ NOT used for `/`. The only division that can overflow is `TYPE_MIN / -1`, which SPEC promises traps
// in EVERY build, so the emitter calls `kama_arith_chk` (sub-`int`, where the narrowing catches it) or
// `kama_sdiv_i32`/`_i64` (above) directly rather than through this macro.
#ifdef NDEBUG
#  define KAMA_ARITH_NARROW(T, LO, HI, V)  ((T)(V))
#else
#  define KAMA_ARITH_NARROW(T, LO, HI, V)  ((T)kama_arith_chk((long long)(V), (LO), (HI)))
#endif
// The emitter calls a checker DIRECTLY when it knows the source's signedness. When it does not — a type
// parameter, a `foreach` binding, an intrinsic with no recorded return type — it emits this instead, and
// C answers the question it could not: `_Generic` selects on the operand's static type and evaluates ONLY
// the selected branch, so a side-effecting operand (`cast<int8>(f())`) is still evaluated exactly once
// (`KAMA_DIV`/`KAMA_SHL` above lean on the same property). A statement expression would be the obvious
// alternative and is not available: kama emits strict ISO C11, where `({ … })` is a GNU extension.
//
// ⚠️ The associations are C's BUILTIN types, never the <stdint.h> typedefs: on Linux x86_64 `size_t` and
// `uint64_t` are both `unsigned long`, and two `_Generic` associations for one type does not compile.
// The 10 integer builtins + plain `char` cover every numeric type kama can emit.
//
// A float/double source takes the range test (kama_f2i_chk) and hands the VALUE back unchanged, so the
// caller's cast still truncates toward zero (`cast<int32>(3.9f64) == 3`).
#define KAMA_NARROW(x, LO, HI) _Generic((x),                                    \
    signed char:        kama_narrow_chk_s((long long)(x), (LO), (HI)),          \
    char:               kama_narrow_chk_s((long long)(x), (LO), (HI)),          \
    short:              kama_narrow_chk_s((long long)(x), (LO), (HI)),          \
    int:                kama_narrow_chk_s((long long)(x), (LO), (HI)),          \
    long:               kama_narrow_chk_s((long long)(x), (LO), (HI)),          \
    long long:          kama_narrow_chk_s((long long)(x), (LO), (HI)),          \
    unsigned char:      kama_narrow_chk_u((unsigned long long)(x), (LO), (HI)), \
    unsigned short:     kama_narrow_chk_u((unsigned long long)(x), (LO), (HI)), \
    unsigned int:       kama_narrow_chk_u((unsigned long long)(x), (LO), (HI)), \
    unsigned long:      kama_narrow_chk_u((unsigned long long)(x), (LO), (HI)), \
    unsigned long long: kama_narrow_chk_u((unsigned long long)(x), (LO), (HI)), \
    _Bool:              kama_narrow_chk_u((unsigned long long)(x), (LO), (HI)), \
    float:  kama_f2i_chk((double)(x), (LO), (HI)),                              \
    double: kama_f2i_chk((double)(x), (LO), (HI)),                              \
    default: (x))

// Signed `+ - *` and negation at 32/64 bits: TRAP in debug, WRAP in release — kama's own check, not the
// toolchain's. Until 0.9.161 the debug half was `-fsanitize=signed-integer-overflow` with
// `-fsanitize-trap`: a bare `ud2` that named nothing and could not be recovered from, and the last
// `-fsanitize` flag standing between a wasm build and `-sWASM_WORKERS`. The release half is unchanged and
// costs nothing: under NDEBUG every macro below is the plain C operator, and `-fwrapv` (both tiers now,
// so the runtime's own C is defined too) makes that the defined two's-complement wrap —
// tools/check-release-arith.sh reads the release asm to keep it true.
//
// VALUE operators (`KAMA_ADD(a, b)`) dispatch on the promoted result type: signed `int`/`long`/`long long`
// take the checked helper; unsigned and float take an identity helper OF THEIR OWN TYPE, so the expression
// keeps its type. A sub-`int` operand never reaches these — it takes KAMA_ARITH_NARROW (range-checked
// against its own width) from the emitter, as before.
#define KAMA_WIDE_ARITH(SUF, T, TMIN, W)                                        \
static inline T kama_add_##SUF(T a, T b) {                                      \
    T o; if (__builtin_add_overflow(a, b, &o)) kama_wide_arith_fail("+", (W)); return o; } \
static inline T kama_sub_##SUF(T a, T b) {                                      \
    T o; if (__builtin_sub_overflow(a, b, &o)) kama_wide_arith_fail("-", (W)); return o; } \
static inline T kama_mul_##SUF(T a, T b) {                                      \
    T o; if (__builtin_mul_overflow(a, b, &o)) kama_wide_arith_fail("*", (W)); return o; } \
static inline T kama_neg_##SUF(T a) {                                           \
    if (a == (TMIN)) kama_wide_arith_fail("negation", (W)); return (T)(-a); }
#define KAMA_PLAIN_ARITH(SUF, T)                                                \
static inline T kama_add_##SUF(T a, T b) { return (T)(a + b); }                 \
static inline T kama_sub_##SUF(T a, T b) { return (T)(a - b); }                 \
static inline T kama_mul_##SUF(T a, T b) { return (T)(a * b); }                 \
static inline T kama_neg_##SUF(T a)      { return (T)(-a); }
KAMA_WIDE_ARITH(i32, int,                INT32_MIN, 32)
KAMA_WIDE_ARITH(l,   long,               LONG_MIN,  (int)(sizeof(long) * 8))
KAMA_WIDE_ARITH(ll,  long long,          LLONG_MIN, 64)
KAMA_PLAIN_ARITH(u32, unsigned int)
KAMA_PLAIN_ARITH(ul,  unsigned long)
KAMA_PLAIN_ARITH(ull, unsigned long long)
KAMA_PLAIN_ARITH(f32, float)
KAMA_PLAIN_ARITH(f64, double)
#define KAMA_VAL_SEL(OP, x) _Generic((x),                                       \
    int: kama_##OP##_i32, long: kama_##OP##_l, long long: kama_##OP##_ll,       \
    unsigned int: kama_##OP##_u32, unsigned long: kama_##OP##_ul,               \
    unsigned long long: kama_##OP##_ull, float: kama_##OP##_f32, double: kama_##OP##_f64)
#ifdef NDEBUG
#  define KAMA_ADD(a, b) ((a) + (b))
#  define KAMA_SUB(a, b) ((a) - (b))
#  define KAMA_MUL(a, b) ((a) * (b))
#  define KAMA_NEG(a)    (-(a))
#else
#  define KAMA_ADD(a, b) KAMA_VAL_SEL(add, (a) + (b))((a), (b))
#  define KAMA_SUB(a, b) KAMA_VAL_SEL(sub, (a) - (b))((a), (b))
#  define KAMA_MUL(a, b) KAMA_VAL_SEL(mul, (a) * (b))((a), (b))
#  define KAMA_NEG(a)    KAMA_VAL_SEL(neg, (a) + 0)((a))
#endif

// PLACE operators — compound assignment and `++`/`--` — take the place BY ADDRESS, so `a[idx()] += 1`
// evaluates `idx()` exactly once (a rewrite to `x = x + 1` would not), and `x++` in value position hands
// back the old value. One helper per (op, storage type); `_Generic` keys on the POINTER type, which keeps
// a `volatile` (`static hardware`) place distinct and reachable. Three classes of arithmetic: a signed
// sub-`int` place computes in `long long` and range-checks against its own width (debug) or truncates
// (release) — the two-tier rule of KAMA_ARITH_NARROW, and the check C's `x += y` never had (it promoted,
// added and truncated silently — the one shape the D-arith rule missed); a wide signed place uses the
// value operators above; unsigned and float are the plain operator. `/= %= <<= >>=` check in EVERY tier.
#define KAMA_OPSYM_add +
#define KAMA_OPSYM_sub -
#define KAMA_OPSYM_mul *
#define KAMA_VAL_add(a, b) KAMA_ADD(a, b)
#define KAMA_VAL_sub(a, b) KAMA_SUB(a, b)
#define KAMA_VAL_mul(a, b) KAMA_MUL(a, b)
#define KAMA_KIND_NARROW(NAME, T, LO, HI, a, b) KAMA_ARITH_NARROW(T, LO, HI, (long long)(a) KAMA_OPSYM_##NAME (long long)(b))
#define KAMA_KIND_WIDE(NAME, T, LO, HI, a, b)   KAMA_VAL_##NAME((a), (b))
#define KAMA_KIND_PLAIN(NAME, T, LO, HI, a, b)  ((a) KAMA_OPSYM_##NAME (b))
#define KAMA_PLACE_ARITH(SUF, Q, T, LO, HI, KIND)                               \
static inline T kama_addeq_##SUF(Q T* p, T b) { T v = (T)KIND(add, T, LO, HI, *p, b); *p = v; return v; } \
static inline T kama_subeq_##SUF(Q T* p, T b) { T v = (T)KIND(sub, T, LO, HI, *p, b); *p = v; return v; } \
static inline T kama_muleq_##SUF(Q T* p, T b) { T v = (T)KIND(mul, T, LO, HI, *p, b); *p = v; return v; } \
static inline T kama_postadd_##SUF(Q T* p, T b) { T o = *p; kama_addeq_##SUF(p, b); return o; }          \
static inline T kama_postsub_##SUF(Q T* p, T b) { T o = *p; kama_subeq_##SUF(p, b); return o; }
#define KAMA_PLACE_INT(SUF, Q, T)                                               \
static inline T kama_diveq_##SUF(Q T* p, T b) { T v = (T)KAMA_DIV(*p, b); *p = v; return v; }            \
static inline T kama_modeq_##SUF(Q T* p, T b) { T v = (T)KAMA_MOD(*p, b); *p = v; return v; }            \
static inline T kama_shleq_##SUF(Q T* p, long long n) { T v = (T)KAMA_SHL(*p, n); *p = v; return v; }    \
static inline T kama_shreq_##SUF(Q T* p, long long n) { T v = (T)KAMA_SHR(*p, n); *p = v; return v; }
#define KAMA_PLACE_FLOAT(SUF, Q, T)                                             \
static inline T kama_diveq_##SUF(Q T* p, T b) { T v = (T)(*p / b); *p = v; return v; }
#define KAMA_PLACE_TYPES(Q, V)                                                                          \
KAMA_PLACE_ARITH(i8##V,  Q, signed char,        -128,       127,        KAMA_KIND_NARROW) KAMA_PLACE_INT(i8##V,  Q, signed char)        \
KAMA_PLACE_ARITH(i16##V, Q, short,              -32768,     32767,      KAMA_KIND_NARROW) KAMA_PLACE_INT(i16##V, Q, short)              \
KAMA_PLACE_ARITH(i32##V, Q, int,                0, 0,                   KAMA_KIND_WIDE)   KAMA_PLACE_INT(i32##V, Q, int)                \
KAMA_PLACE_ARITH(l##V,   Q, long,               0, 0,                   KAMA_KIND_WIDE)   KAMA_PLACE_INT(l##V,   Q, long)               \
KAMA_PLACE_ARITH(ll##V,  Q, long long,          0, 0,                   KAMA_KIND_WIDE)   KAMA_PLACE_INT(ll##V,  Q, long long)          \
KAMA_PLACE_ARITH(u8##V,  Q, unsigned char,      0, 0,                   KAMA_KIND_PLAIN)  KAMA_PLACE_INT(u8##V,  Q, unsigned char)      \
KAMA_PLACE_ARITH(u16##V, Q, unsigned short,     0, 0,                   KAMA_KIND_PLAIN)  KAMA_PLACE_INT(u16##V, Q, unsigned short)     \
KAMA_PLACE_ARITH(u32##V, Q, unsigned int,       0, 0,                   KAMA_KIND_PLAIN)  KAMA_PLACE_INT(u32##V, Q, unsigned int)       \
KAMA_PLACE_ARITH(ul##V,  Q, unsigned long,      0, 0,                   KAMA_KIND_PLAIN)  KAMA_PLACE_INT(ul##V,  Q, unsigned long)      \
KAMA_PLACE_ARITH(ull##V, Q, unsigned long long, 0, 0,                   KAMA_KIND_PLAIN)  KAMA_PLACE_INT(ull##V, Q, unsigned long long) \
KAMA_PLACE_ARITH(f32##V, Q, float,              0, 0,                   KAMA_KIND_PLAIN)  KAMA_PLACE_FLOAT(f32##V, Q, float)            \
KAMA_PLACE_ARITH(f64##V, Q, double,             0, 0,                   KAMA_KIND_PLAIN)  KAMA_PLACE_FLOAT(f64##V, Q, double)
KAMA_PLACE_TYPES(, )
KAMA_PLACE_TYPES(volatile, v)
#define KAMA_PSEL(OP, SUF, T) T*: kama_##OP##_##SUF, volatile T*: kama_##OP##_##SUF##v
#define KAMA_PLACE_SEL_INT(OP, p) _Generic((p),                                 \
    KAMA_PSEL(OP, i8, signed char), KAMA_PSEL(OP, i16, short), KAMA_PSEL(OP, i32, int),          \
    KAMA_PSEL(OP, l, long), KAMA_PSEL(OP, ll, long long),                                        \
    KAMA_PSEL(OP, u8, unsigned char), KAMA_PSEL(OP, u16, unsigned short),                        \
    KAMA_PSEL(OP, u32, unsigned int), KAMA_PSEL(OP, ul, unsigned long),                          \
    KAMA_PSEL(OP, ull, unsigned long long))
#define KAMA_PLACE_SEL(OP, p) _Generic((p),                                     \
    KAMA_PSEL(OP, i8, signed char), KAMA_PSEL(OP, i16, short), KAMA_PSEL(OP, i32, int),          \
    KAMA_PSEL(OP, l, long), KAMA_PSEL(OP, ll, long long),                                        \
    KAMA_PSEL(OP, u8, unsigned char), KAMA_PSEL(OP, u16, unsigned short),                        \
    KAMA_PSEL(OP, u32, unsigned int), KAMA_PSEL(OP, ul, unsigned long),                          \
    KAMA_PSEL(OP, ull, unsigned long long),                                                      \
    KAMA_PSEL(OP, f32, float), KAMA_PSEL(OP, f64, double))
#define KAMA_ADDEQ(p, b)   KAMA_PLACE_SEL(addeq, p)((p), (b))
#define KAMA_SUBEQ(p, b)   KAMA_PLACE_SEL(subeq, p)((p), (b))
#define KAMA_MULEQ(p, b)   KAMA_PLACE_SEL(muleq, p)((p), (b))
#define KAMA_DIVEQ(p, b)   KAMA_PLACE_SEL(diveq, p)((p), (b))
#define KAMA_MODEQ(p, b)   KAMA_PLACE_SEL_INT(modeq, p)((p), (b))
#define KAMA_SHLEQ(p, n)   KAMA_PLACE_SEL_INT(shleq, p)((p), (long long)(n))
#define KAMA_SHREQ(p, n)   KAMA_PLACE_SEL_INT(shreq, p)((p), (long long)(n))
#define KAMA_POSTADD(p, b) KAMA_PLACE_SEL(postadd, p)((p), (b))
#define KAMA_POSTSUB(p, b) KAMA_PLACE_SEL(postsub, p)((p), (b))

// InlineArray<T,N> — a fixed-size, bounds-checked VALUE array (`struct { T v[N]; }`). It owns no heap:
// it copies by value (a plain struct blit), has no destructor, and never decays to a raw pointer.
// The element must be a `value` (owns nothing), so there is no per-element dtor/copy. This is how
// kama reintroduces raw arrays SAFELY — indexing is bounds-checked (a runtime trap), the size is
// part of the type (monomorphized per (T,N)), and the whole thing is a first-class value.
#define KAMA_FIXED_TYPE(T, N, NAME) typedef struct NAME { T v[N]; } NAME;
// Indices are `ptrdiff_t` (kama's `isize`), not `size_t`: an index is a SIZE, and kama's size type is
// signed so that `len - 1` on an empty container is -1 rather than SIZE_MAX. That makes the negative case
// REACHABLE here, where an unsigned index made it merely unrepresentable, so each check tests it
// explicitly — the same `i < 0 || i >= len` shape the kama-side collections use.
#define KAMA_FIXED_FUNCS(T, N, NAME)                                           \
static inline T      NAME##__get(const NAME* self, ptrdiff_t i) {               \
    if (i < 0 || i >= (ptrdiff_t)(N)) kama_bounds_fail((size_t)i, (size_t)(N)); \
    return self->v[i];                                                          \
}                                                                               \
static inline void   NAME##__set(NAME* self, ptrdiff_t i, T x) {                \
    if (i < 0 || i >= (ptrdiff_t)(N)) kama_bounds_fail((size_t)i, (size_t)(N)); \
    self->v[i] = x;                                                             \
}                                                                               \
static inline T*     NAME##__at(NAME* self, ptrdiff_t i) {                      \
    if (i < 0 || i >= (ptrdiff_t)(N)) kama_bounds_fail((size_t)i, (size_t)(N)); \
    return &self->v[i];                                                         \
}                                                                               \
static inline ptrdiff_t NAME##__length(const NAME* self) { (void)self; return (ptrdiff_t)(N); } \
/* The safe InlineArray->pointer bridge, matching FixedArray/DynamicArray: OBTAINING the buffer      \
   pointer is safe, DEREFERENCING it needs an `unsafe fn` — a rule that falls out of the `*`-suffixed \
   return type, not from anything special-cased here. This is the one container that is stack-        \
   allocated, fixed-size and allocation-free, so it is what a `@noheap` region has to hand to C.     */\
static inline T const* NAME##__dataPtr(NAME* self) { return self->v; }                          \
static inline T*     NAME##__dataPtrMut(NAME* self) { return self->v; }                         \
static inline NAME   NAME##__fill(T x) {                                        \
    NAME r; for (size_t i = 0; i < (size_t)(N); ++i) r.v[i] = x; return r;      \
}
#define KAMA_FIXED_DEFINE(T, N, NAME) KAMA_FIXED_TYPE(T, N, NAME) KAMA_FIXED_FUNCS(T, N, NAME)


// ---- Simd<T, comptime N> — a LANE BATCH, which is not an array ---------------------------------------
//
// `InlineArray<T,N>` above and this are both N values of T in 16 bytes, and they are different tools. An
// InlineArray is a container: lanes are addressed one at a time, indexing is bounds-checked, and `foreach`
// is the point. A Simd is a single VALUE the CPU operates on whole — `a + b` is one instruction, lanes are
// interchangeable, and iterating it scalar-at-a-time is the thing it exists to avoid. So this has no
// `foreach`, no `operator[]`, and no `set`.
//
// ⚠️ `vector_size`, never `ext_vector_type`. gcc IGNORES `ext_vector_type` with a warning and leaves a
// one-lane scalar, which under `--cc gcc` is a silent miscompile (three of four lanes vanish). Measured on
// gcc 13.3, clang 18.1.3, Apple clang 21 and emcc: `vector_size` gives sizeof 16 / alignof 16 on all four,
// with `+ - * /`, compound-literal init, lane subscript, compare-to-mask and `__builtin_shufflevector`
// behaving identically. That intersection is what this header is allowed to use.
//
// A target with no SIMD unit is NOT an error: `vector_size` lowers to scalar operations that are still
// correct, which is the whole reason kama exposes a type rather than per-ISA intrinsics.
#define KAMA_SIMD_TYPE(T, N, NAME) typedef T NAME __attribute__((vector_size(sizeof(T) * (N))));

// The lane index is a `ptrdiff_t` (kama's `isize`) and IS bounds-checked, like every other kama index —
// a vector subscript past the end is UB in C, and "fast but occasionally nonsense" is not a trade kama
// makes. The check costs nothing where it matters: `v.lane(index: 2)` folds both operands at `-O2` and
// the branch disappears entirely (measured).
// ⚠️ Every operation here is written as a PER-LANE LOOP over the vector, and none of them calls libm.
// Both are deliberate, and both were measured (2026-08-31, gcc 13.3 / clang 18.1.3 / Apple clang 21):
//
//   * the loop is not a fallback — the backends fold it to the branchless vector form. `abs` becomes
//     `fcmlt.4s; fneg.4s; bit.16b` and `min`/`max` become `fcmgt.4s; bif.16b`, identically on all three.
//     Writing them with per-compiler builtins would buy nothing and cost a seam, because clang has
//     `__builtin_elementwise_*` and gcc has NO equivalent — and gcc treats the unknown name as an
//     *implicit function declaration*, a warning, which is the same silent-miscompile shape that
//     disqualified `ext_vector_type`.
//   * no libm, because this header is FREESTANDING (stdint/stdbool/stddef/limits only — see the top) and
//     an MCU target has no `<math.h>`. That is what confines this set to what arithmetic and comparison
//     can express. `sqrt`/`floor`/`ceil` genuinely need libm and are NOT here for that reason; they want
//     the `kama_math.h` seam that `lib/std/math/scalar.kama` already uses, which an intrinsic cannot
//     reach today (ROADMAP_DETAIL §2).
//     ⚠️ They also need `-fno-math-errno` to vectorize at all: WITH it a per-lane `sqrtf` loop folds to
//     `fsqrt v0.4s` on both gcc and clang; WITHOUT it neither folds, because the errno side effect makes
//     the call unsinkable. Measured — do not re-derive.
#define KAMA_SIMD_FUNCS(T, N, NAME, ARR)                                        \
static inline NAME NAME##__splat(T x) {                                         \
    NAME r; for (int i = 0; i < (N); ++i) r[i] = x; return r;                    \
}                                                                               \
static inline T NAME##__lane(const NAME* self, ptrdiff_t i) {                   \
    if (i < 0 || i >= (ptrdiff_t)(N)) kama_bounds_fail((size_t)i, (size_t)(N)); \
    return (*self)[i];                                                          \
}                                                                               \
static inline ARR NAME##__toArray(const NAME* self) {                           \
    ARR r; for (int i = 0; i < (N); ++i) r.v[i] = (*self)[i]; return r;         \
}                                                                               \
static inline NAME NAME##__abs(const NAME* self) {                              \
    NAME r; for (int i = 0; i < (N); ++i) {                                      \
        T x = (*self)[i]; r[i] = x < (T)0 ? (T)-x : x;                           \
    } return r;                                                                 \
}                                                                               \
static inline NAME NAME##__min(const NAME* self, NAME o) {                      \
    NAME r; for (int i = 0; i < (N); ++i) r[i] = (*self)[i] < o[i] ? (*self)[i] : o[i]; \
    return r;                                                                   \
}                                                                               \
static inline NAME NAME##__max(const NAME* self, NAME o) {                      \
    NAME r; for (int i = 0; i < (N); ++i) r[i] = (*self)[i] > o[i] ? (*self)[i] : o[i]; \
    return r;                                                                   \
}

#define KAMA_SIMD_DEFINE(T, N, NAME, ARR) KAMA_SIMD_TYPE(T, N, NAME) KAMA_SIMD_FUNCS(T, N, NAME, ARR)


// ---- Mask<T, N> — a lane mask as a VALUE, which is the other thing scalars cannot say ----------------
//
// ⚠️ A mask must carry its element type `T`, not just the lane count, and that is a MEASURED constraint
// rather than a stylistic one. A vector comparison yields an integer vector whose lane width follows the
// OPERAND's — identical on gcc 13.3 and clang 18:
//
//     f32x4 > f32x4  ->  4-byte lanes      i16x8 > i16x8  ->  2-byte lanes
//     f64x2 > f64x2  ->  8-byte lanes
//
// So a bare `Mask<4>` has no C type at all: it is one thing from a `float32` compare and another from an
// `int16` one. `Mask<T, N>` lowers to the signed integer vector of T's own width, which is exactly what
// the comparison produces. (Rust's portable_simd reached the same conclusion — its mask is parameterised
// by the element's mask type.)
//
// Lanes are all-ones or all-zeros BIT PATTERNS, not booleans, which is why a mask is its own type rather
// than a `Simd<bool, N>`: `*`, `/` and `reduceAdd` are meaningless on it, and `select` must not accept a
// data vector by mistake.
#define KAMA_MASK_TYPE(IT, N, NAME) typedef IT NAME __attribute__((vector_size(sizeof(IT) * (N))));

// `select` is spelled as mask AND/OR rather than the ternary `m ? a : b`, because GCC REJECTS a ternary
// on vector operands — clang accepts it, and taking clang's spelling would have made `--cc gcc` fail to
// build rather than silently miscompile, but failing to build is still failing. The memcpy round-trips
// are how a float lane batch is bit-manipulated without type-punning UB; both compilers fold them to
// nothing at -O2 (the object is register-sized).
#define KAMA_MASK_FUNCS(N, NAME, MNAME)                                         \
static inline NAME MNAME##__select(const MNAME* m, NAME ifTrue, NAME ifFalse) { \
    MNAME ti, fi, r; NAME out;                                                   \
    __builtin_memcpy(&ti, &ifTrue,  sizeof(NAME));                               \
    __builtin_memcpy(&fi, &ifFalse, sizeof(NAME));                               \
    r = (*m & ti) | (~*m & fi);                                                  \
    __builtin_memcpy(&out, &r, sizeof(NAME));                                    \
    return out;                                                                 \
}                                                                               \
static inline MNAME MNAME##__and(const MNAME* self, MNAME o) { return *self & o; } \
static inline MNAME MNAME##__or (const MNAME* self, MNAME o) { return *self | o; } \
static inline MNAME MNAME##__not(const MNAME* self)          { return ~*self;    } \
static inline bool  MNAME##__anyTrue(const MNAME* self) {                       \
    for (int i = 0; i < (N); ++i) if ((*self)[i]) return true;                   \
    return false;                                                               \
}                                                                               \
static inline bool  MNAME##__allTrue(const MNAME* self) {                       \
    for (int i = 0; i < (N); ++i) if (!(*self)[i]) return false;                 \
    return true;                                                                \
}

// ---- signed integer lanes obey kama's OVERFLOW rule, lane by lane -----------------------------------
//
// `int32 MAX + 1` traps in a debug build and wraps in release. A `Simd<int32,4>` whose lane 0 does the
// same arithmetic must behave the same way, or the language has two different answers for one operation
// depending on how the numbers were packed. Nothing in the toolchain provides this: UBSan's
// `signed-integer-overflow` check does NOT instrument vector arithmetic (measured — the scalar add
// trapped and the lane add wrapped to INT32_MIN, same build, same flags), so the check is emitted here.
//
// `__builtin_{add,sub,mul}_overflow` is the per-lane test: both gcc and clang have it, it is exact at
// every width including `int64` (where a widening check has no wider type to use), and it costs nothing
// in release because the whole thing compiles to the bare vector op there. UNSIGNED lanes are not
// checked — unsigned wrapping is defined and the language says so at every width.
#define KAMA_SIMD_ICHK(T, UT, N, NAME)                                          \
static inline NAME NAME##__chkAdd(NAME a, NAME b) {                             \
    NAME r; for (int i = 0; i < (N); ++i) { T o;                                 \
        if (__builtin_add_overflow(a[i], b[i], &o)) kama_simd_arith_fail(i);     \
        r[i] = o; } return r;                                                   \
}                                                                               \
static inline NAME NAME##__chkSub(NAME a, NAME b) {                             \
    NAME r; for (int i = 0; i < (N); ++i) { T o;                                 \
        if (__builtin_sub_overflow(a[i], b[i], &o)) kama_simd_arith_fail(i);     \
        r[i] = o; } return r;                                                   \
}                                                                               \
static inline NAME NAME##__chkMul(NAME a, NAME b) {                             \
    NAME r; for (int i = 0; i < (N); ++i) { T o;                                 \
        if (__builtin_mul_overflow(a[i], b[i], &o)) kama_simd_arith_fail(i);     \
        r[i] = o; } return r;                                                   \
}                                                                               \
/* A signed left shift INTO the sign bit is UB in C, for a vector lane exactly as for a scalar. kama  */\
/* defines it (SPEC: "computed in the unsigned type - a defined bit pattern"), and `KAMA_SHL` does */\
/* that for scalars via `_Generic` - which cannot see a vector type, and produced a hard C error the  */\
/* first time a `Simd` met `<<`. This is the same rule, per lane, with the unsigned peer passed in.   */\
static inline NAME NAME##__shl(NAME a, NAME b) {                                \
    NAME r; for (int i = 0; i < (N); ++i) r[i] = (T)((UT)a[i] << b[i]);          \
    return r;                                                                   \
}

// Debug traps, release wraps — the same two-tier split the scalar types get, and the release arm is the
// bare vector operator, so a `--release` build's codegen is byte-identical to having no check at all.
#ifdef NDEBUG
#  define KAMA_SIMD_ADD(NAME, A, B) ((A) + (B))
#  define KAMA_SIMD_SUB(NAME, A, B) ((A) - (B))
#  define KAMA_SIMD_MUL(NAME, A, B) ((A) * (B))
#else
#  define KAMA_SIMD_ADD(NAME, A, B) NAME##__chkAdd((A), (B))
#  define KAMA_SIMD_SUB(NAME, A, B) NAME##__chkSub((A), (B))
#  define KAMA_SIMD_MUL(NAME, A, B) NAME##__chkMul((A), (B))
#endif

// The comparisons live with the DATA vector (they are `a.greaterThan(rhs: b)`) but produce the mask, so
// they need both names. A vector compare already yields the right lane width; the cast names the type.
#define KAMA_SIMD_CMP(T, N, NAME, MNAME)                                        \
static inline MNAME NAME##__greaterThan(const NAME* self, NAME o) { return (MNAME)(*self >  o); } \
static inline MNAME NAME##__lessThan   (const NAME* self, NAME o) { return (MNAME)(*self <  o); } \
static inline MNAME NAME##__atLeast    (const NAME* self, NAME o) { return (MNAME)(*self >= o); } \
static inline MNAME NAME##__atMost     (const NAME* self, NAME o) { return (MNAME)(*self <= o); } \
static inline MNAME NAME##__equals     (const NAME* self, NAME o) { return (MNAME)(*self == o); } \
static inline MNAME NAME##__notEquals  (const NAME* self, NAME o) { return (MNAME)(*self != o); }

// Horizontal reductions — the one place a lane batch is deliberately collapsed to a scalar. Written as a
// per-lane fold; the backends turn `reduceAdd` into `faddp`/`addv`-style pair reductions where the ISA
// has them. This is the ONLY operation here that is cheaper as a scalar loop over memory, which is why
// `docs/design/simd.md` §1c warns against reaching for `Simd` when a horizontal dot product is the goal.
#define KAMA_SIMD_REDUCE(T, N, NAME)                                            \
static inline T NAME##__reduceAdd(const NAME* self) {                           \
    T a = (*self)[0]; for (int i = 1; i < (N); ++i) a += (*self)[i]; return a;   \
}                                                                               \
static inline T NAME##__reduceMul(const NAME* self) {                           \
    T a = (*self)[0]; for (int i = 1; i < (N); ++i) a *= (*self)[i]; return a;   \
}                                                                               \
static inline T NAME##__reduceMin(const NAME* self) {                           \
    T a = (*self)[0]; for (int i = 1; i < (N); ++i) { T x = (*self)[i]; if (x < a) a = x; } return a; \
}                                                                               \
static inline T NAME##__reduceMax(const NAME* self) {                           \
    T a = (*self)[0]; for (int i = 1; i < (N); ++i) { T x = (*self)[i]; if (x > a) a = x; } return a; \
}


// kama `char` is ONE UNICODE CODEPOINT, not a byte and not a number — `s[i]` is a `uint8`, and
// codepoints are reached only through `.chars()`. Its representation is a 32-bit unsigned integer, but it
// is a DISTINCT TYPE, and this typedef is what makes that true for kama's own checker.
//
// It used to lower straight to `uint32_t`, and the consequence was not cosmetic: every rule in the
// compiler decides type identity by comparing lowered C spellings, so with `char` spelled `uint32_t`
// there was nothing left to compare and `uint32 n = c;`, `char d = u;` and `c == u` all crossed in
// silence. The codepoint/integer distinction was documented, believed, and unenforced — the same shape
// as the byte/codepoint bug AGENTS.md opens with. Giving it a name of its own is what enforces it; C
// behaviour is byte-for-byte unchanged, since this is a typedef and not a wrapper.
typedef uint32_t kama_char;

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
// `len`/`cap` stay `size_t` in the struct — they are ALLOCATION sizes, and this layout is the C-facing
// one. The ACCESSOR returns `ptrdiff_t`, because a length is kama's `isize`. The narrowing is safe by
// construction: no allocation may exceed PTRDIFF_MAX (the same bound Rust puts on a single allocation,
// for the same reason — a byte offset between two points in one object must be representable).
static inline ptrdiff_t kama_string__length(kama_string* self) { return (ptrdiff_t)self->len; }
// FFI: the underlying NUL-terminated bytes, for passing to a C `const char*`.
static inline const char* kama_string__cstr(kama_string* self) { return self->data; }
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
// True when `off` is a character boundary: `len` is one (a range's exclusive end), and any other offset
// is a boundary unless it names a CONTINUATION byte (10xxxxxx). O(1), no decoding.
static inline bool kama_utf8_is_boundary(const kama_string* self, size_t off) {
    return off >= self->len || ((unsigned char)self->data[off] & 0xC0) != 0x80;
}
// The greatest character boundary <= `off` (Rust's `floor_char_boundary`). Total: never traps, clamps
// past-the-end to `len`. At most 3 bytes back, since a UTF-8 sequence is at most 4 bytes long. This is
// THE primitive that makes an arithmetic offset safe — at either end of a range and at any position — so
// `substring` stays the one slicing operation instead of growing a safe twin.
static inline size_t kama_string__floorCharBoundary(kama_string* self, size_t off) {
    if (off >= self->len) return self->len;
    while (off > 0 && ((unsigned char)self->data[off] & 0xC0) == 0x80) --off;
    return off;
}
// Owned byte-range copy of `[start, end)` — a fresh heap-owned string (cap>0). Traps on
// `start > end || end > len` (kama_bounds_fail, the same clean abort as __get) and on an offset that
// SPLITS a character (kama_utf8_split_fail).
//
// The split check is what keeps `string`'s UTF-8 invariant total. Every other string operation preserves
// it by construction — literals are valid, `concat` of two valid strings is valid, `split`/`find` cut on
// whole needles, `trim` removes only ASCII, and casing leaves bytes >= 0x80 alone — so this was the one
// place the SAFE surface could produce an ill-formed string from well-formed input, with no `unsafe` and
// no error. Trapping matches how indexing already behaves (a bad index aborts rather than invoking UB)
// and how Rust's `&s[0..2]` panics on a non-char-boundary. For an offset from arithmetic rather than from
// a search, snap it with `floorCharBoundary` (or use `truncate`) — both are total.
static inline kama_string kama_string__substring(kama_string* self, size_t start, size_t end) {
    if (start > end || end > self->len) kama_bounds_fail(end, self->len);
    if (!kama_utf8_is_boundary(self, start)) kama_utf8_split_fail(start);
    if (!kama_utf8_is_boundary(self, end))   kama_utf8_split_fail(end);
    size_t n = end - start;
    kama_string r; r.len = n;
    if (n == 0) { r.data = NULL; r.cap = 0; return r; }
    char* buf = (char*)kama_alloc(n + 1);
    kama_copy(buf, self->data + start, n);
    buf[n] = '\0';
    r.data = buf; r.cap = n + 1;   // heap-owned
    return r;
}
// At most `maxBytes` bytes from the start, never splitting a character — the named form of the common
// case where an offset comes from a BUDGET (a wire field, a column limit, a log cap) rather than from a
// search. Total: never traps. One line over the primitive, kept because a caller who never discovers
// `floorCharBoundary` still writes correct code by reaching for the obvious name.
static inline kama_string kama_string__truncate(kama_string* self, size_t maxBytes) {
    return kama_string__substring(self, 0, kama_string__floorCharBoundary(self, maxBytes));
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
// iterator hold a borrowed `UnsafePtr<uint8>` (so it stays a POD `value` type, like Chars) yet yield OWNED
// pieces, without exposing raw allocation to kama source. Declared in the prelude as
// `extern fn string kama_string_from_raw(UnsafePtr<uint8> base, isize start, isize len);`. len<=0 -> "".
// The offsets are `ptrdiff_t`, not `int32_t`: they index a string, and a string's length is an `isize`
// (kama's size type), so taking them narrower would put a cast on every caller of the one runtime helper
// that exists precisely so `.split()` need not open-code allocation.
static inline kama_string kama_string_from_raw(const uint8_t* base, ptrdiff_t start, ptrdiff_t len) {
    kama_string r;
    if (start < 0 || len <= 0) { r.data = NULL; r.len = 0; r.cap = 0; return r; }   // defensive: caller (Split) always passes >=0
    char* buf = (char*)kama_alloc((size_t)len + 1);
    kama_copy(buf, base + start, (size_t)len);
    buf[len] = '\0';
    r.data = buf; r.len = (size_t)len; r.cap = (size_t)len + 1;
    return r;
}

// ---- Number / char -> string -----------------------------------------------
// The formatting primitives behind `std::fmt` AND the prelude `Display`/`Formatter`. They live HERE (not
// in the opt-in kama_fmt.h) because the prelude — which is emitted into EVERY program and is NOT tree-shaken
// — binds them via `extern fn`, exactly like `kama_string_from_raw` above. Integer formatting is a pure
// digit loop (no libc). Float formatting delegates to `snprintf` at round-trip precision (`%.17g`/`%.9g`);
// `snprintf` is declared at BLOCK scope (the same pattern as `malloc`/`memcpy` at the top of this header),
// so this stays `<stdio.h>`-free and the freestanding property holds — a program links `snprintf` from libc
// only if it actually formats a float. Every result is a fresh heap-owned `kama_string`.

// Reversed digit loop for an unsigned 64-bit value into `buf` (no NUL); returns the digit count.
// `0` renders as a single '0'. buf must hold >= 20 bytes.
static inline int kama_fmt_u64_digits(char* buf, uint64_t v) {
    char tmp[20]; int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v) { tmp[t++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
    int n = t, i = 0;
    while (t) buf[i++] = tmp[--t];
    return n;
}

// Unsigned decimal -> fresh heap-owned kama_string.
static inline kama_string kama_fmt_u64(uint64_t v) {
    char buf[24];
    int n = kama_fmt_u64_digits(buf, v);
    return kama_string_from_raw((const uint8_t*)buf, 0, (int32_t)n);
}

// Signed decimal -> fresh heap-owned kama_string. INT64_MIN is handled by taking the magnitude in the
// unsigned domain (`0u - (uint64_t)v`), which is well-defined two's-complement and never overflows.
static inline kama_string kama_fmt_i64(int64_t v) {
    char buf[24]; int n = 0;
    if (v < 0) {
        buf[n++] = '-';
        n += kama_fmt_u64_digits(buf + n, (uint64_t)0 - (uint64_t)v);
    } else {
        n = kama_fmt_u64_digits(buf, (uint64_t)v);
    }
    return kama_string_from_raw((const uint8_t*)buf, 0, (int32_t)n);
}

// float64 -> fresh heap-owned kama_string, at shortest-round-trip precision (%.17g).
static inline kama_string kama_fmt_f64(double v) {
    extern int snprintf(char*, size_t, const char*, ...);
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%.17g", v);
    if (n < 0) n = 0;
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    return kama_string_from_raw((const uint8_t*)buf, 0, (int32_t)n);
}

// float32 -> fresh heap-owned kama_string, at shortest-round-trip precision for single (%.9g).
static inline kama_string kama_fmt_f32(float v) {
    extern int snprintf(char*, size_t, const char*, ...);
    char buf[24];
    int n = snprintf(buf, sizeof buf, "%.9g", (double)v);
    if (n < 0) n = 0;
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    return kama_string_from_raw((const uint8_t*)buf, 0, (int32_t)n);
}

// UTF-8 encode a single Unicode scalar (codepoint) -> fresh heap-owned kama_string of 1–4 bytes. The
// inverse of the prelude `Chars` decoder. A surrogate (0xD800..0xDFFF) or out-of-range value yields the
// U+FFFD replacement character rather than emitting ill-formed UTF-8.
static inline kama_string kama_fmt_char(uint32_t cp) {
    unsigned char b[4]; int n;
    if (cp <= 0x7Fu) { b[0] = (unsigned char)cp; n = 1; }
    else if (cp <= 0x7FFu) { b[0] = (unsigned char)(0xC0u | (cp >> 6)); b[1] = (unsigned char)(0x80u | (cp & 0x3Fu)); n = 2; }
    else if ((cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu) { cp = 0xFFFDu; b[0] = (unsigned char)(0xE0u | (cp >> 12)); b[1] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu)); b[2] = (unsigned char)(0x80u | (cp & 0x3Fu)); n = 3; }
    else if (cp <= 0xFFFFu) { b[0] = (unsigned char)(0xE0u | (cp >> 12)); b[1] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu)); b[2] = (unsigned char)(0x80u | (cp & 0x3Fu)); n = 3; }
    else { b[0] = (unsigned char)(0xF0u | (cp >> 18)); b[1] = (unsigned char)(0x80u | ((cp >> 12) & 0x3Fu)); b[2] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu)); b[3] = (unsigned char)(0x80u | (cp & 0x3Fu)); n = 4; }
    return kama_string_from_raw(b, 0, (int32_t)n);
}

// Formattable-specifier `flags` bitmask shared by the width/precision helpers below: bit0 zero-pad, bit1
// left-align (`-`), bit2 force a leading `+` on non-negatives. Assembled into a printf conversion — `0` is
// dropped when left-aligning (printf makes `-` win), matching C semantics. `+` is only emitted for the signed
// helpers (it is a no-op / ill-formed for `%u`).
#define KAMA_FMT_ZERO 1
#define KAMA_FMT_LEFT 2
#define KAMA_FMT_PLUS 4
// Write the flag chars for `%[-][+][0]` into `f` (after a leading '%' the caller already placed); `allowPlus`
// gates the `+` for unsigned conversions. Returns the number of chars written.
static inline int kama_fmt_flagchars(char* f, int32_t flags, int allowPlus) {
    int i = 0;
    if (flags & KAMA_FMT_LEFT) f[i++] = '-';
    if (allowPlus && (flags & KAMA_FMT_PLUS)) f[i++] = '+';
    if ((flags & KAMA_FMT_ZERO) && !(flags & KAMA_FMT_LEFT)) f[i++] = '0';
    return i;
}

// Fixed-precision float -> fresh heap-owned kama_string (the `${x:.N}` / `${x:W.N}` interpolation specifiers,
// with optional `+`/`-`/`0` flags). `prec` is clamped to [0,64] and `width` to [0,256]. `snprintf` is declared
// at block scope so this stays `<stdio.h>`-free (the same pattern as kama_fmt_f64). Buffer covers width 256.
static inline kama_string kama_fmt_f64_prec(double v, int32_t prec, int32_t width, int32_t flags) {
    extern int snprintf(char*, size_t, const char*, ...);
    if (prec < 0) prec = 0; if (prec > 64) prec = 64;
    if (width < 0) width = 0; if (width > 256) width = 256;
    char fmt[12]; int fi = 0; fmt[fi++] = '%';
    fi += kama_fmt_flagchars(fmt + fi, flags, 1);
    fmt[fi++] = '*'; fmt[fi++] = '.'; fmt[fi++] = '*'; fmt[fi++] = 'f'; fmt[fi] = '\0';
    char buf[512];
    int n = snprintf(buf, sizeof buf, fmt, (int)width, (int)prec, v);
    if (n < 0) n = 0;
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    return kama_string_from_raw((const uint8_t*)buf, 0, (int32_t)n);
}

// Decimal integer, minimum field width + `+`/`-`/`0` flags (the `${n:W}` / `${n:0W}` / `${n:+W}` specifiers).
// `width` is clamped to [0,256]; signed vs unsigned pick `lld`/`llu` (the `u` helper never emits `+`). The
// zero-pad keeps the sign ahead of the zeros (printf `%+0*lld`). Block-scope `snprintf` keeps this stdio-free.
static inline kama_string kama_fmt_i64_width(int64_t v, int32_t width, int32_t flags) {
    extern int snprintf(char*, size_t, const char*, ...);
    if (width < 0) width = 0; if (width > 256) width = 256;
    char fmt[12]; int fi = 0; fmt[fi++] = '%';
    fi += kama_fmt_flagchars(fmt + fi, flags, 1);
    fmt[fi++] = '*'; fmt[fi++] = 'l'; fmt[fi++] = 'l'; fmt[fi++] = 'd'; fmt[fi] = '\0';
    char buf[300];
    int n = snprintf(buf, sizeof buf, fmt, (int)width, (long long)v);
    if (n < 0) n = 0;
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    return kama_string_from_raw((const uint8_t*)buf, 0, (int32_t)n);
}
static inline kama_string kama_fmt_u64_width(uint64_t v, int32_t width, int32_t flags) {
    extern int snprintf(char*, size_t, const char*, ...);
    if (width < 0) width = 0; if (width > 256) width = 256;
    char fmt[12]; int fi = 0; fmt[fi++] = '%';
    fi += kama_fmt_flagchars(fmt + fi, flags, 0);
    fmt[fi++] = '*'; fmt[fi++] = 'l'; fmt[fi++] = 'l'; fmt[fi++] = 'u'; fmt[fi] = '\0';
    char buf[300];
    int n = snprintf(buf, sizeof buf, fmt, (int)width, (unsigned long long)v);
    if (n < 0) n = 0;
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    return kama_string_from_raw((const uint8_t*)buf, 0, (int32_t)n);
}

// Integer rendered in a non-decimal base -> fresh heap-owned kama_string (the `${n:0x}` / `${n:x}` etc.
// interpolation specifiers). `v` is masked to `width_bits` (8/16/32/64) first, so the value is shown as the
// unsigned bit pattern of its declared width — a signed negative round-trips (`${x:0x}` on `-1i8` -> `0xff`).
// `base` is 16/8/2; `upper` uppercases the hex digits; `prefix` emits the matching `0x`/`0o`/`0b` marker so
// the output is itself a valid Kama literal. A base-16 uint64 is at most 16 digits + a 2-char prefix.
static inline kama_string kama_fmt_u64_radix(uint64_t v, int32_t base, int32_t width_bits, int32_t upper, int32_t prefix) {
    if (width_bits > 0 && width_bits < 64) v &= ((uint64_t)1 << width_bits) - 1u;
    const char* digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[64]; int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v) { tmp[t++] = digs[(int)(v % (uint64_t)base)]; v /= (uint64_t)base; }
    char buf[72]; int i = 0;
    if (prefix) {
        buf[i++] = '0';
        buf[i++] = (base == 16) ? (upper ? 'X' : 'x') : (base == 8 ? 'o' : 'b');
    }
    while (t) buf[i++] = tmp[--t];
    return kama_string_from_raw((const uint8_t*)buf, 0, (int32_t)i);
}

// Amortized-growth append of `n` bytes onto an owned kama_string buffer used as a builder — the backing of
// the prelude `Formatter`. Repeated appends double the capacity, so building a string is amortized O(1) per
// byte (unlike `kama_string__concat`, which reallocates the WHOLE string every call → O(n²) for a chain). A
// fresh/empty buffer (`cap==0`, possibly a borrowed literal) is upgraded to a heap allocation on first push;
// an existing heap buffer (`cap>0`) is `realloc`-grown. The result stays a well-formed `kama_string` (NUL
// terminated, `cap>len`), so ordinary string RAII frees it — the `Formatter` needs no custom destructor.
static inline void kama_str_push(kama_string* s, const kama_string* add) {
    size_t n = add->len;
    if (n == 0) return;
    size_t need = s->len + n + 1;                         // +1 for the NUL
    if (s->cap == 0 || need > s->cap) {
        size_t ncap = s->cap ? s->cap : 16;
        while (ncap < need) ncap *= 2;
        char* nb = (char*)kama_alloc(ncap);
        if (s->len) kama_copy(nb, s->data, s->len);
        if (s->cap) kama_free(s->data);                  // free the old heap buffer; a cap==0 literal isn't freed
        s->data = nb; s->cap = ncap;
    }
    kama_copy(s->data + s->len, add->data, n);
    s->len += n;
    s->data[s->len] = '\0';
}

// Move a builder's buffer OUT as an owned string, leaving the field empty (`{NULL,0,0}` — a safe no-op to
// drop). The zero-copy `Formatter.finish`: it hands off the accumulated heap buffer rather than copying it.
// An untouched buffer is still the empty literal (`cap==0`), which transfers harmlessly as a borrowed "".
static inline kama_string kama_str_take(kama_string* s) {
    kama_string r = *s;
    s->data = NULL; s->len = 0; s->cap = 0;
    return r;
}

// User-triggerable trap for `panic(msg: …)` and a failed `assert(cond: …)`. Writes
// "kama: panic: <msg>" to stderr and `abort()`s — the same clean-abort mechanism as the bounds
// trap (no <stdio.h>, no undefined behavior). Never returns.
static inline KAMA_NORETURN void kama_panic(kama_string msg) {
#if defined(KAMA_TARGET_EMBEDDED)
    // Freestanding: no stderr, no `abort`. Route through the overridable weak hook (see kama_bounds_fail).
    (void)msg;
    kama_try_recover();
    kama_panic_handler();
    for (;;) {}
#else
    extern void abort(void);
    (void)kama_raw_write(2, "kama: panic: ", 13);
    if (msg.len) (void)kama_raw_write(2, msg.data, msg.len);
    (void)kama_raw_write(2, "\n", 1);
    kama_run_panic_hook();   // custom exhibition (dialog / telemetry); runtime still terminates
    abort();
#endif
}

// A `Shared<T>`'s control block (declared up beside `kama_ctrl`). ⚠️ It CHECKS its allocation: it used to
// write `c->strong` straight through whatever kama_alloc returned, so the one path that is supposed to
// answer OOM with kama's documented panic answered it with a null dereference instead. The fallible verb
// `try new` does not come through here at all — it allocates its ctrl inline so a failure can become
// `None` (see emitTryNewBox); this is the infallible path, where panic IS the contract.
static inline kama_ctrl* kama_ctrl_new(void) {
    kama_ctrl* c = (kama_ctrl*)kama_alloc(sizeof(kama_ctrl));
    if (!c) kama_panic(kama_string_lit("out of memory", 13));
    c->strong = 1; c->weak = 0;
    return c;
}

// --- Fatal diagnostics with source location (panic / assert) -----------------
// Compose a message + " (file:line)" into a stack buffer, write it to stderr, and terminate — the same
// clean-abort discipline as kama_panic (no <stdio.h>, no UB, never returns). On embedded there is no
// stderr/abort, so route through the overridable weak kama_panic_handler (see kama_bounds_fail). Every
// append is bounded by `cap`, so an over-long condition/message/path truncates rather than overruns.
static inline KAMA_NORETURN void kama_fail_emit(const char* buf, size_t n) {
#if defined(KAMA_TARGET_EMBEDDED)
    (void)buf; (void)n;
    kama_try_recover();
    kama_panic_handler();
    for (;;) {}
#else
    extern void abort(void);
    (void)kama_raw_write(2, buf, n);
    (void)kama_raw_write(2, "\n", 1);
    kama_run_panic_hook();   // custom exhibition (dialog / telemetry); runtime still terminates
    abort();
#endif
}
static inline void kama_fail_puts(char* buf, size_t* p, size_t cap, const char* s) {
    while (s && *s && *p < cap) buf[(*p)++] = *s++;
}
static inline void kama_fail_loc(char* buf, size_t* p, size_t cap, const char* file, int line) {
    kama_fail_puts(buf, p, cap, " (");
    kama_fail_puts(buf, p, cap, file);
    if (*p < cap) buf[(*p)++] = ':';
    char lb[24]; size_t lp = 0; kama_u64_to_buf(lb, &lp, (size_t)(line < 0 ? 0 : line));
    for (size_t i = 0; i < lp && *p < cap; ++i) buf[(*p)++] = lb[i];
    if (*p < cap) buf[(*p)++] = ')';
}
// `panic(msg:)` → "kama: panic: <msg> (file:line)".
static inline KAMA_NORETURN void kama_panic_at(kama_string msg, const char* file, int line) {
    char buf[1024]; size_t p = 0; const size_t cap = sizeof buf;
    kama_fail_puts(buf, &p, cap, "kama: panic: ");
    for (size_t i = 0; i < msg.len && p < cap; ++i) buf[p++] = ((const char*)msg.data)[i];
    kama_fail_loc(buf, &p, cap, file, line);
    kama_fail_emit(buf, p);
}
// A failed `assert`/`debugAssert` → "assertion failed: <cond>[ — <msg>] (file:line)". `cond` is the
// auto-stringified condition text (may be ""); `msg` is the user message (may be "").
static inline void kama_assert_fail(const char* cond, kama_string msg, const char* file, int line) {
    char buf[1024]; size_t p = 0; const size_t cap = sizeof buf;
    kama_fail_puts(buf, &p, cap, "assertion failed");
    if (cond && *cond) { kama_fail_puts(buf, &p, cap, ": "); kama_fail_puts(buf, &p, cap, cond); }
    if (msg.len) {
        kama_fail_puts(buf, &p, cap, " \xE2\x80\x94 ");   // em dash (U+2014), UTF-8
        for (size_t i = 0; i < msg.len && p < cap; ++i) buf[p++] = ((const char*)msg.data)[i];
    }
    kama_fail_loc(buf, &p, cap, file, line);
    kama_fail_emit(buf, p);
}

// --- Floor console output (print / println / eprint / eprintln) --------------
// Raw unbuffered byte write to a standard fd (1 = stdout, 2 = stderr), no <stdio.h> — the same discipline as
// the panic path. On --target embedded there is no fd, so BOTH streams route to an OVERRIDABLE weak
// `kama_log_sink` that defaults to a no-op (zero cost on-chip; a firmware author overrides it once to pipe
// out a UART/RTT). Mirrors the weak kama_panic_handler. Unbuffered line writes in v1.
#if defined(KAMA_TARGET_EMBEDDED)
__attribute__((weak)) void kama_log_sink(const uint8_t* bytes, size_t n) { (void)bytes; (void)n; }
static inline void kama_print_write(int fd, const void* bytes, size_t n) {
    (void)fd;
    kama_log_sink((const uint8_t*)bytes, n);
}
#else
static inline void kama_print_write(int fd, const void* bytes, size_t n) {
    (void)kama_raw_write(fd, bytes, n);
}
#endif
// Write a single newline to `fd` — the `println`/`eprintln` tail (avoids a second kama_string round-trip).
static inline void kama_print_nl(int fd) { kama_print_write(fd, "\n", 1); }

// Tiny tracing hook for tests/debugging: a folding accumulator that records a
// sequence of integer events (e.g. constructor/destructor order). Declare in
// kama with `extern void kama_trace(int code);` / `extern int kama_trace_get();`.
// Single-TU builds only (definition lives in this header).
static int kama_trace_acc = 0;
static inline void kama_trace(int code) { kama_trace_acc = kama_trace_acc * 31 + code; }
static inline int  kama_trace_get(void) { return kama_trace_acc; }

// ---- Command-line arguments + environment (prelude floor) -------------------
// argv/argc are stashed once by the synthesized `main` (kama.cemit) via kama_args_init BEFORE kama_main
// runs, then read-only — so PLAIN globals (NOT KAMA_ISOLATE_LOCAL/_Thread_local): argv is process-wide,
// every isolate must see the same vector, and it's written once on the main thread before any spawn, so
// there is no race. The prelude binds these via `extern fn`, exactly like kama_string_from_raw. The
// invocation (argv[0]) is NOT part of args(): kama_args_count/kama_args_at index argv[1..argc). Three separate
// program-identity accessors: kama_program_invocation (argv[0] VERBATIM — exactly how it was launched),
// kama_program_name (basename of argv[0] — "what was I invoked as", for usage text / applet dispatch,
// spoofable but conventional), and kama_program_path (the OS-RESOLVED absolute executable path — reliable
// for finding sibling files / re-exec). getenv/strlen/readlink/… are declared at
// BLOCK scope (the malloc/memcpy/snprintf pattern), so <stdlib.h>/<string.h>/<unistd.h> never leak to user
// code and the header stays dependency-light + `--no-std`-clean.
#if !defined(KAMA_TARGET_EMBEDDED)
// EXTERNAL LINKAGE (single definition in the entry TU — see `isEntry` in kama.cemit.cpp). argv must be ONE
// object program-wide: the reader accessors (kama_args_count/at, kama_program_*) are `static inline` and
// inlined into EVERY TU, but `kama_args_init` runs only in `main` (the entry TU). A per-TU `static` would
// leave the prelude floor `args()`/`programName()`/`programPath()` reading an EMPTY argv when called from any
// non-entry TU (a library/stdlib module) — the same multi-TU-static hazard as the panic hook / log sink. This
// realizes the "every isolate sees the same vector" intent documented above (it is written once on the main
// thread before any spawn, so a single shared object is race-free — not KAMA_ISOLATE_LOCAL/per-isolate).
extern int    kama_argc;
extern char** kama_argv;
static inline void kama_args_init(int argc, char** argv) {
    kama_argc = argc; kama_argv = argv;
#if defined(_WIN32)
    // argv as UTF-8. The CRT hands `main` the UTF-16 command line re-encoded through the process ANSI
    // code page, so a non-ASCII argument — a path the user typed — arrives as mojibake, which is the same
    // defect the filesystem seam had (kama_os.h; utf8everywhere.org). Re-read the command line wide and
    // convert it here, once. On any failure the narrow argv stays, which is still right for ASCII.
    // Block-scope declarations, like _setmode below, so <windows.h> never leaks: CP_UTF8 is 65001;
    // CommandLineToArgvW lives in shell32, which `kama build` links for a Windows target.
    {
        extern wchar_t*  __stdcall GetCommandLineW(void);
        extern wchar_t** __stdcall CommandLineToArgvW(const wchar_t*, int*);
        extern void*     __stdcall LocalFree(void*);
        extern int       __stdcall WideCharToMultiByte(unsigned, unsigned long, const wchar_t*, int,
                                                       char*, int, const char*, int*);
        int wargc = 0;
        wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
        if (wargv) {
            char** u = (char**)kama_calloc((size_t)wargc + 1, sizeof(char*));
            int ok = u != NULL && wargc > 0;
            for (int i = 0; ok && i < wargc; ++i) {
                int n = WideCharToMultiByte(65001u, 0, wargv[i], -1, (char*)0, 0, (const char*)0, (int*)0);
                u[i] = n > 0 ? (char*)kama_alloc((size_t)n) : (char*)0;
                if (!u[i] || WideCharToMultiByte(65001u, 0, wargv[i], -1, u[i], n, (const char*)0, (int*)0) <= 0) ok = 0;
            }
            if (ok) { kama_argc = wargc; kama_argv = u; }
            else if (u) { for (int i = 0; i < wargc; ++i) kama_free(u[i]); kama_free(u); }
            LocalFree(wargv);
        }
    }
#endif
#if defined(_WIN32) && defined(KAMA_SUBSYSTEM_WINDOWS)
    // A GUI-subsystem PE (`--subsystem windows`) is given NO console, so `print`/`eprintln` write to
    // handles that go nowhere. That is the cost that stops windows-subsystem being the default. Undo the
    // half that matters: if this program was launched FROM a terminal, borrow that terminal.
    //
    // ⚠️ AttachConsole ALONE IS NOT ENOUGH, and this is the whole trap. It gives the process a console,
    // but the CRT bound stdout/stderr to nothing back at startup — before `main` — so the FILE* streams
    // stay pointed at nothing and `print` is still silent. The streams have to be reopened onto the
    // console device by name. Double-clicked from Explorer there is no parent console, AttachConsole
    // fails, and the freopens are skipped: output goes nowhere, which is exactly right for a GUI app.
    //
    // ⚠️ AND IT IS THE FILE DESCRIPTORS THAT MUST BE REBOUND, NOT stdout/stderr. `print` goes through
    // kama_raw_write -> _write(fd, …) (see above); it never touches a FILE*. So the obvious spelling —
    // freopen("CONOUT$", "w", stdout) — fixes a stream kama does not use and leaves `print` silent. Open
    // the console device, wrap the HANDLE in a CRT fd, and _dup2 it over fd 1 and 2.
    //
    // SetStdHandle as well, because the two namespaces are independent: _dup2 moves the CRT fd and leaves
    // the Win32 std handle alone, so anything a user links that calls GetStdHandle/WriteConsole (a C
    // library, wgpu's logging) would still be writing nowhere.
    //
    // Ordered BEFORE the _setmode block below so the binary-mode calls act on the rebound descriptors.
    // ATTACH_PARENT_PROCESS is (DWORD)-1; the STD_*_HANDLE ids are -10/-11/-12; the CreateFileA constants
    // are GENERIC_READ|GENERIC_WRITE and FILE_SHARE_READ|FILE_SHARE_WRITE with OPEN_EXISTING. All declared
    // at block scope, like _setmode/_write below, so <windows.h> never leaks into user code.
    {
        extern int   __stdcall AttachConsole(unsigned long);
        extern void* __stdcall CreateFileA(const char*, unsigned long, unsigned long, void*,
                                           unsigned long, unsigned long, void*);
        extern int   __stdcall SetStdHandle(unsigned long, void*);
        extern int   _open_osfhandle(intptr_t, int);
        extern int   _dup2(int, int);
        extern int   _close(int);
        if (AttachConsole((unsigned long)-1)) {
            void* hOut = CreateFileA("CONOUT$", 0x80000000u | 0x40000000u, 0x1u | 0x2u,
                                     (void*)0, 3u, 0u, (void*)0);
            void* hIn  = CreateFileA("CONIN$",  0x80000000u | 0x40000000u, 0x1u | 0x2u,
                                     (void*)0, 3u, 0u, (void*)0);
            if (hOut != (void*)(intptr_t)-1) {
                int fd = _open_osfhandle((intptr_t)hOut, 0);
                if (fd >= 0) { _dup2(fd, 1); _dup2(fd, 2); _close(fd); }
                SetStdHandle((unsigned long)-11, hOut);
                SetStdHandle((unsigned long)-12, hOut);
            }
            if (hIn != (void*)(intptr_t)-1) {
                int fd = _open_osfhandle((intptr_t)hIn, 0);
                if (fd >= 0) { _dup2(fd, 0); _close(fd); }
                SetStdHandle((unsigned long)-10, hIn);
            }
        }
    }
#endif
#if defined(_WIN32)
    // ...and, on Windows, put the standard streams in BINARY mode before a single byte moves.
    //
    // kama_raw_write is a BYTE writer — `print` means "these bytes, on this fd". The Windows CRT opens
    // fd 0/1/2 in TEXT mode, where _write silently rewrites every 0x0A to 0x0D 0x0A, so `println` emitted
    // CRLF and any program piping non-text (an image, a protocol frame, a tarball) had its 0x0A bytes
    // corrupted on the way out. check-print caught the visible half of that and printed two lines that
    // looked identical, because the only difference was the carriage returns.
    //
    // LF-only is also what the neighbours do: Go's os.Stdout and Rust's println! both write the bytes
    // given and translate nothing. Console hosts (conhost, Windows Terminal, cmd, PowerShell) render a
    // bare LF as a newline, so this costs nothing on the display side.
    //
    // `main` is the one place this can go — it must happen before any output, exactly once — and this is
    // what already runs first there. _setmode/_O_BINARY are declared at block scope rather than pulled in
    // with <io.h>/<fcntl.h>, the same way _write is above, so the header stays dependency-light and
    // `--no-std`-clean. _O_BINARY is 0x8000 in every Microsoft CRT (msvcrt, UCRT) and in mingw-w64.
    {
        extern int _setmode(int, int);
        _setmode(0, 0x8000); _setmode(1, 0x8000); _setmode(2, 0x8000);
    }
#endif
}
static inline int  kama_args_count(void) { return kama_argc > 1 ? kama_argc - 1 : 0; }   // drop argv[0]
// The i-th user arg (0-based over argv[1..argc)) as a FRESH owned kama_string; out-of-range -> "".
static inline kama_string kama_args_at(int i) {
    if (i < 0 || i >= kama_args_count()) return kama_string_lit("", 0);
    extern size_t strlen(const char*);
    const char* a = kama_argv[i + 1];
    return kama_string_from_raw((const uint8_t*)a, 0, (int32_t)strlen(a));
}
// The raw invocation — argv[0] VERBATIM (exactly how the program was launched: `./app`, `/usr/bin/app`,
// or a bare `app`), as a fresh owned copy; 1 if present, else 0 + "". The unmodified string, for
// fidelity/logging or code ported from Go's os.Args[0] / Rust's args().next(). basename -> program_name;
// resolved path -> program_path.
static inline int kama_program_invocation(kama_string* out) {
    if (kama_argc < 1 || !kama_argv[0]) { *out = kama_string_lit("", 0); return 0; }
    extern size_t strlen(const char*);
    *out = kama_string_from_raw((const uint8_t*)kama_argv[0], 0, (int32_t)strlen(kama_argv[0]));
    return 1;
}
// The program name — the basename of argv[0] (the last path segment, splitting on BOTH '/' and '\\' so it
// is correct cross-platform), as a fresh owned copy; 1 if present, else 0 + "". This is "the name the
// program was invoked as" (usage messages, busybox-style applet dispatch): `/usr/bin/app` -> `app`,
// `.\app.exe` -> `app.exe`, a bare `app` -> `app`.
static inline int kama_program_name(kama_string* out) {
    if (kama_argc < 1 || !kama_argv[0]) { *out = kama_string_lit("", 0); return 0; }
    extern size_t strlen(const char*);
    const char* a = kama_argv[0];
    size_t len = strlen(a), start = 0;
    for (size_t i = 0; i < len; ++i) { if (a[i] == '/' || a[i] == '\\') start = i + 1; }
    *out = kama_string_from_raw((const uint8_t*)(a + start), 0, (int32_t)(len - start));
    return 1;
}
// The OS-RESOLVED absolute path to the running executable (NOT argv[0] — that is unreliable: a PATH launch
// passes a bare name and a caller can spoof it). Reliable on the hosted desktop/server platforms; returns 0
// (-> None in kama) where there is no such notion or no portable query — wasm (runs in a JS/browser host),
// bare metal (the embedded stub below), and any platform without a branch here (e.g. a console port adds its
// own). Models the driver's selfExePath (kama.driver.cpp): _get_pgmptr / readlink /proc/self/exe /
// _NSGetExecutablePath, each declared at block scope so no platform header leaks.
#if defined(_WIN32)
struct HINSTANCE__;   // HMODULE's pointee — the tag <windows.h> defines, so a later include agrees with the declaration below
#endif
static inline int kama_program_path(kama_string* out) {
#if defined(__EMSCRIPTEN__)
    (void)out; *out = kama_string_lit("", 0); return 0;   // wasm host: no executable path
#elif defined(_WIN32)
    // GetModuleFileNameW, converted to UTF-8 — the narrow _get_pgmptr is the ANSI re-encoding of this
    // path (and its wide twin _wget_pgmptr is not in mingw-w64's UCRT import library at all). 32767 is
    // the NT path ceiling, so one heap buffer of that size never truncates.
    extern unsigned long __stdcall GetModuleFileNameW(struct HINSTANCE__*, wchar_t*, unsigned long);
    extern int __stdcall WideCharToMultiByte(unsigned, unsigned long, const wchar_t*, int, char*, int, const char*, int*);
    wchar_t* w = (wchar_t*)kama_alloc(32768u * sizeof(wchar_t));
    unsigned long got = GetModuleFileNameW((struct HINSTANCE__*)0, w, 32768u);
    int n = (got > 0 && got < 32768u) ? WideCharToMultiByte(65001u, 0, w, -1, (char*)0, 0, (const char*)0, (int*)0) : 0;
    if (n > 1) {
        char* s = (char*)kama_alloc((size_t)n);
        WideCharToMultiByte(65001u, 0, w, -1, s, n, (const char*)0, (int*)0);
        out->data = s; out->len = (size_t)n - 1; out->cap = (size_t)n;                     // the from_raw shape
        kama_free(w); return 1;
    }
    kama_free(w); *out = kama_string_lit("", 0); return 0;
#elif defined(__linux__)
    extern long readlink(const char*, char*, size_t);   // ssize_t; does not NUL-terminate
    char buf[4096];
    long n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0 && n < (long)sizeof buf) { *out = kama_string_from_raw((const uint8_t*)buf, 0, (int32_t)n); return 1; }
    *out = kama_string_lit("", 0); return 0;
#elif defined(__APPLE__)
    extern int _NSGetExecutablePath(char*, unsigned int*);   // <mach-o/dyld.h>; NUL-terminates on success
    extern size_t strlen(const char*);
    char buf[4096]; unsigned int sz = sizeof buf;
    if (_NSGetExecutablePath(buf, &sz) == 0) { *out = kama_string_from_raw((const uint8_t*)buf, 0, (int32_t)strlen(buf)); return 1; }
    *out = kama_string_lit("", 0); return 0;
#else
    (void)out; *out = kama_string_lit("", 0); return 0;   // no portable query on this platform
#endif
}
// Look up env var `name` (a NUL-terminated C string). Writes an OWNED copy to *out and returns 1 if set,
// else leaves *out = "" and returns 0.
static inline int kama_env_lookup(const char* name, kama_string* out) {
#if defined(_WIN32)
    // Wide, then UTF-8 — getenv's value is the ANSI re-encoding of the same block (see kama_args_init).
    extern int __stdcall MultiByteToWideChar(unsigned, unsigned long, const char*, int, wchar_t*, int);
    extern int __stdcall WideCharToMultiByte(unsigned, unsigned long, const wchar_t*, int, char*, int, const char*, int*);
    extern unsigned long __stdcall GetEnvironmentVariableW(const wchar_t*, wchar_t*, unsigned long);
    *out = kama_string_lit("", 0);
    int wn = MultiByteToWideChar(65001u, 0, name, -1, (wchar_t*)0, 0);
    if (wn <= 0) return 0;
    wchar_t* wname = (wchar_t*)kama_alloc((size_t)wn * sizeof(wchar_t));
    MultiByteToWideChar(65001u, 0, name, -1, wname, wn);
    unsigned long need = GetEnvironmentVariableW(wname, (wchar_t*)0, 0);            // incl. NUL; 0 = unset
    if (need == 0) { kama_free(wname); return 0; }
    wchar_t* wval = (wchar_t*)kama_alloc((size_t)need * sizeof(wchar_t));
    unsigned long got = GetEnvironmentVariableW(wname, wval, need);
    kama_free(wname);
    int n = (got > 0 && got < need) ? WideCharToMultiByte(65001u, 0, wval, -1, (char*)0, 0, (const char*)0, (int*)0) : 0;
    if (n > 0) {
        char* s = (char*)kama_alloc((size_t)n);
        WideCharToMultiByte(65001u, 0, wval, -1, s, n, (const char*)0, (int*)0);
        out->data = s; out->len = (size_t)n - 1; out->cap = (size_t)n;                 // the from_raw shape
    }
    kama_free(wval);
    return n > 0;
#else
    extern char* getenv(const char*);
    extern size_t strlen(const char*);
    const char* v = getenv(name);
    if (!v) { *out = kama_string_lit("", 0); return 0; }
    *out = kama_string_from_raw((const uint8_t*)v, 0, (int32_t)strlen(v));
    return 1;
#endif
}
#else
// Freestanding: no argv, no environ, no executable path. Stubs so the prelude surface still COMPILES on
// --target embedded (args() -> empty, programInvocation()/programName()/programPath()/env() -> None) with
// zero libc linkage — same discipline as kama_panic's embedded arm.
static inline void        kama_args_init(int argc, char** argv) { (void)argc; (void)argv; }
static inline int         kama_args_count(void) { return 0; }
static inline kama_string kama_args_at(int i) { (void)i; return kama_string_lit("", 0); }
static inline int         kama_program_invocation(kama_string* out) { *out = kama_string_lit("", 0); return 0; }
static inline int         kama_program_name(kama_string* out) { *out = kama_string_lit("", 0); return 0; }
static inline int         kama_program_path(kama_string* out) { *out = kama_string_lit("", 0); return 0; }
static inline int         kama_env_lookup(const char* name, kama_string* out) { (void)name; *out = kama_string_lit("", 0); return 0; }
#endif

// ---- Serialization graph context ------------------------------------------
// Pure-C substrate for the compiler's object-graph serialization (a `@generate` type that transitively
// reaches a Shared/Weak/Owned — see `reachesPointer`). No std::collections dependency: the compiler's own
// graph machinery can't lean on kama library types (that circularity is why serialization became a compiler
// intrinsic). This is the id-table / worklist / registry substrate; the reserve/intern/drain sequencing and
// the two-pass read are emitted C driven onto it by the lowering. Inert until then. See docs/SPEC.md
// "Serialization" and ROADMAP_DETAIL §4. (Replaces the generated-kama `std::serialization::graph` SerContext.)

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
