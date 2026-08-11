#ifndef KAMA_RUNTIME_H
#define KAMA_RUNTIME_H

// Minimal kama runtime. Dependency-light and WASM-safe: it leans only on the
// freestanding-friendly C headers so generated code compiles for native and
// for clang/emscripten wasm targets alike. Generated translation units include
// this first, then their own module header.

#include <stdint.h>    // int32_t … (types only — no callable C functions)
#include <stdbool.h>   // bool       (type only)
#include <stddef.h>    // size_t, NULL (types only)

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
static inline void kama_u64_to_buf(char* buf, size_t* p, size_t v) {
    char tmp[20]; int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
    while (t) buf[(*p)++] = tmp[--t];
}
#if defined(KAMA_TARGET_EMBEDDED)
// Freestanding trap policy (MCU campaign step 3). On a bare-metal target there is no fd 2 to write
// to and no `abort` under `-nostdlib`, so every fatal condition (bounds/slice/panic/OOM) funnels
// through one OVERRIDABLE weak hook. The default spins in a `__builtin_trap` loop (a debugger breaks
// here / the MCU resets); a firmware author provides a strong `kama_panic_handler` to blink an SOS,
// reset, or log over a peripheral. Weak symbols are supported by clang/gcc = the embedded toolchains.
__attribute__((weak)) KAMA_NORETURN void kama_panic_handler(void) { for (;;) __builtin_trap(); }
static inline KAMA_NORETURN void kama_bounds_fail(size_t i, size_t len) {
    (void)i; (void)len;
    kama_panic_handler();
    for (;;) {}   // kama_panic_handler must not return; belt-and-suspenders if a user override does
}
static inline KAMA_NORETURN void kama_utf8_split_fail(size_t off) {
    (void)off;
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
// TERMINATES after it (it is not a resume point — recovery is `Result`, not panic). Covers every hosted
// fatal path (bounds / panic / assert), matching the embedded "one hook for all fatal conditions" policy.
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

// InlineArray<T,N> — a fixed-size, bounds-checked VALUE array (`struct { T v[N]; }`). It owns no heap:
// it copies by value (a plain struct blit), has no destructor, and never decays to a raw pointer.
// The element must be a `value` (owns nothing), so there is no per-element dtor/copy. This is how
// kama reintroduces raw arrays SAFELY — indexing is bounds-checked (a runtime trap), the size is
// part of the type (monomorphized per (T,N)), and the whole thing is a first-class value.
#define KAMA_FIXED_TYPE(T, N, NAME) typedef struct NAME { T v[N]; } NAME;
#define KAMA_FIXED_FUNCS(T, N, NAME)                                           \
static inline T      NAME##__get(const NAME* self, size_t i) {                  \
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
static inline size_t NAME##__length(const NAME* self) { (void)self; return (size_t)(N); } \
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

// Format-specifier `flags` bitmask shared by the width/precision helpers below: bit0 zero-pad, bit1
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

// --- Fatal diagnostics with source location (panic / assert) -----------------
// Compose a message + " (file:line)" into a stack buffer, write it to stderr, and terminate — the same
// clean-abort discipline as kama_panic (no <stdio.h>, no UB, never returns). On embedded there is no
// stderr/abort, so route through the overridable weak kama_panic_handler (see kama_bounds_fail). Every
// append is bounded by `cap`, so an over-long condition/message/path truncates rather than overruns.
static inline KAMA_NORETURN void kama_fail_emit(const char* buf, size_t n) {
#if defined(KAMA_TARGET_EMBEDDED)
    (void)buf; (void)n;
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
static inline void kama_args_init(int argc, char** argv) { kama_argc = argc; kama_argv = argv; }
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
static inline int kama_program_path(kama_string* out) {
#if defined(__EMSCRIPTEN__)
    (void)out; *out = kama_string_lit("", 0); return 0;   // wasm host: no executable path
#elif defined(_WIN32)
    extern int _get_pgmptr(char**);
    extern size_t strlen(const char*);
    char* p = 0;
    if (_get_pgmptr(&p) == 0 && p) { *out = kama_string_from_raw((const uint8_t*)p, 0, (int32_t)strlen(p)); return 1; }
    *out = kama_string_lit("", 0); return 0;
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
    extern char* getenv(const char*);
    extern size_t strlen(const char*);
    const char* v = getenv(name);
    if (!v) { *out = kama_string_lit("", 0); return 0; }
    *out = kama_string_from_raw((const uint8_t*)v, 0, (int32_t)strlen(v));
    return 1;
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
