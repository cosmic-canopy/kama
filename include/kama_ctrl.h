#ifndef KAMA_CTRL_H
#define KAMA_CTRL_H

// kama reference-count control-block ops — the seam behind `Shared<T>`/`Weak<T>` (M6.2).
//
// A `Shared`/`Weak` handle refers to a heap `kama_ctrl { size_t kama_strong; size_t kama_weak; }` (kama_runtime.h).
// The library (concrete-element) `Shared`/`Weak` in lib/std/memory/{shared,weak}.kama route EVERY
// strong/weak count mutation through the functions here instead of poking the counter inline, so the two
// refcount FLAVORS live in one audited place:
//
//   * `atomic == 0` (the default `Rc`): plain non-atomic counter ops — zero overhead, single-isolate.
//   * `atomic == 1` (a `Shared<immutable T>`): Arc-correct atomics — a deeply-immutable payload is
//     race-free even when several isolates clone/drop the handle concurrently (M6.2's safe-sharing seam).
//
// `atomic` is a COMPILE-TIME constant at every call site (the emitter passes `__kama_ctrl_atomic()`, which
// lowers to 0/1 per Shared/Weak instance), so the `if (atomic)` branch folds away — the plain path keeps
// `c->kama_strong++` and never pays for atomics it doesn't use. The `__atomic_*` builtins lower to lock-free
// instructions on native AND emscripten from one source, exactly like kama_atomic.h.
//
// Ordering follows Rust's `Arc`: retains are RELAXED (no data published by acquiring a reference); the
// last strong release is RELEASE + an ACQUIRE fence before the pointee is destroyed (so the destroying
// isolate sees every prior write). A deeply-immutable graph is ACYCLIC by construction, so the plain
// path's "keep strong at 1 during the pointee dtor" cycle dance is unnecessary on the atomic path.
//
// This header is included by kama_runtime.h right after the `kama_ctrl` typedef (it needs that type); it is
// not self-contained and is not meant to be included on its own.

// Retain one strong reference (a `Shared` clone / bare copy).
static inline void kama_ctrl_retain_strong(kama_ctrl* c, int atomic) {
    if (atomic) __atomic_fetch_add(&c->kama_strong, (size_t)1, __ATOMIC_RELAXED);
    else        c->kama_strong += 1;
}

// Retain one weak reference (a `Weak` clone / `downgrade`).
static inline void kama_ctrl_retain_weak(kama_ctrl* c, int atomic) {
    if (atomic) __atomic_fetch_add(&c->kama_weak, (size_t)1, __ATOMIC_RELAXED);
    else        c->kama_weak += 1;
}

// Release one strong reference. Returns 1 iff this was the LAST strong ref — the caller then destroys the
// pointee and calls kama_ctrl_finish_strong. The PLAIN path deliberately does NOT decrement on the last
// ref: it leaves `strong == 1` while the pointee dtor runs (cycle-safe — a `Weak` back-edge may re-enter
// this same ctrl), matching the historical prelude/macro semantics. The ATOMIC path fetch_sub(release)s
// and, if it was last, issues an acquire fence (an immutable graph is acyclic, so no dance is needed).
static inline int kama_ctrl_release_strong(kama_ctrl* c, int atomic) {
    if (atomic) {
        if (__atomic_fetch_sub(&c->kama_strong, (size_t)1, __ATOMIC_RELEASE) == 1) {
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            return 1;
        }
        return 0;
    }
    if (c->kama_strong == 1) return 1;   // last: keep it at 1 across the pointee dtor
    c->kama_strong -= 1;
    return 0;
}

// Called after the last-strong pointee dtor has run. Plain: zero `strong` (it was kept at 1). Atomic: the
// fetch_sub already reached 0. Returns 1 iff the ctrl block should now be freed (no weak refs remain).
static inline int kama_ctrl_finish_strong(kama_ctrl* c, int atomic) {
    if (atomic) return __atomic_load_n(&c->kama_weak, __ATOMIC_ACQUIRE) == 0;
    c->kama_strong = 0;
    return c->kama_weak == 0;
}

// Release one weak reference. Returns 1 iff the ctrl block should be freed (weak hit 0 AND no strong).
static inline int kama_ctrl_release_weak(kama_ctrl* c, int atomic) {
    if (atomic) {
        if (__atomic_fetch_sub(&c->kama_weak, (size_t)1, __ATOMIC_RELEASE) == 1) {
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            return __atomic_load_n(&c->kama_strong, __ATOMIC_ACQUIRE) == 0;
        }
        return 0;
    }
    return (--c->kama_weak == 0 && c->kama_strong == 0);
}

// Try to acquire a strong reference from a `Weak` (`tryUpgrade`). Returns 1 on success (strong bumped).
// The atomic path is a CAS loop — a plain "if strong>0 then strong++" would race a concurrent last drop.
static inline int kama_ctrl_try_upgrade(kama_ctrl* c, int atomic) {
    if (atomic) {
        size_t s = __atomic_load_n(&c->kama_strong, __ATOMIC_RELAXED);
        while (s != 0) {
            if (__atomic_compare_exchange_n(&c->kama_strong, &s, s + 1, 1,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                return 1;
            // s was reloaded with the current value; retry unless it hit 0
        }
        return 0;
    }
    if (c->kama_strong > 0) { c->kama_strong += 1; return 1; }
    return 0;
}

#endif  // KAMA_CTRL_H
