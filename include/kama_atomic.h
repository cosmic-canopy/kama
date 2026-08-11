#ifndef KAMA_ATOMIC_H
#define KAMA_ATOMIC_H

// kama atomic binding — the FFI boundary for `std::concurrent`'s `Atomic<T>` (M6).
//
// `Atomic<T>` is the ONE sanctioned cross-isolate shared-MUTABLE cell — the concurrency analog of
// `unsafe {}`/`Ptr` (opt-in, greppable, atomics-only). Everything else in the language is shared-nothing;
// this seam is where a value may be read/written concurrently by several isolates without a data race.
//
// The `Atomic<T>` surface is pure kama (a move-only `resource` holding one inline `T` cell); this header is
// the width-generic op layer it calls. Each op takes the cell address, a byte width (`sizeof(T)`, a
// compile-time constant at the kama call site), and a memory-order int, then dispatches on the width to a
// typed `__atomic_*_n` compiler builtin — so every op inlines to a lock-free machine instruction for the
// concrete scalar width (no libatomic dependency). The generic sized `__atomic_load`/`__atomic_store`
// forms would take a runtime size through this boundary and fall back to libatomic; the width switch keeps
// it inline. `T` is restricted (by the emitter) to an integer primitive or `Ptr`, so a cell is always a
// naturally-aligned lock-free scalar of width 1/2/4/8.
//
// Freestanding-friendly: only <stdint.h>. The `__atomic_*` builtins are GCC/Clang intrinsics (no header),
// and emscripten (clang) lowers them onto shared-memory Web-Worker atomics unchanged — one source, both
// legs, exactly like kama_isolate.h / kama_channel.h.
//
// Memory order ints match the compiler's __ATOMIC_* enum (relaxed 0 … seq_cst 5), so the kama-side
// `MemoryOrder` enum passes straight through. The `Atomic<T>` surface defaults every op to seq_cst (5).

#include <stdint.h>
#include <stddef.h>   // size_t (matches kama `usize`/`sizeof(T)`)

// ---- load: *out = atomic_load(cell) --------------------------------------------------------------------
static inline void kama_atomic_load(void* cell, void* out, size_t width, int32_t mo) {
    switch (width) {
        case 1: *(uint8_t *)out = __atomic_load_n((uint8_t *)cell, mo); break;
        case 2: *(uint16_t*)out = __atomic_load_n((uint16_t*)cell, mo); break;
        case 4: *(uint32_t*)out = __atomic_load_n((uint32_t*)cell, mo); break;
        case 8: *(uint64_t*)out = __atomic_load_n((uint64_t*)cell, mo); break;
    }
}

// ---- store: atomic_store(cell, *val) -------------------------------------------------------------------
static inline void kama_atomic_store(void* cell, void* val, size_t width, int32_t mo) {
    switch (width) {
        case 1: __atomic_store_n((uint8_t *)cell, *(uint8_t *)val, mo); break;
        case 2: __atomic_store_n((uint16_t*)cell, *(uint16_t*)val, mo); break;
        case 4: __atomic_store_n((uint32_t*)cell, *(uint32_t*)val, mo); break;
        case 8: __atomic_store_n((uint64_t*)cell, *(uint64_t*)val, mo); break;
    }
}

// ---- swap: *out = atomic_exchange(cell, *val) ----------------------------------------------------------
static inline void kama_atomic_swap(void* cell, void* val, void* out, size_t width, int32_t mo) {
    switch (width) {
        case 1: *(uint8_t *)out = __atomic_exchange_n((uint8_t *)cell, *(uint8_t *)val, mo); break;
        case 2: *(uint16_t*)out = __atomic_exchange_n((uint16_t*)cell, *(uint16_t*)val, mo); break;
        case 4: *(uint32_t*)out = __atomic_exchange_n((uint32_t*)cell, *(uint32_t*)val, mo); break;
        case 8: *(uint64_t*)out = __atomic_exchange_n((uint64_t*)cell, *(uint64_t*)val, mo); break;
    }
}

// ---- compare-exchange (strong): if *cell == *expected { *cell = *desired; true } else { *expected = *cell; false }
// On failure the observed value is written back through `expected` (the caller's `ref` cell), matching the
// C11 `atomic_compare_exchange_strong` contract, so a CAS-loop can retry against the fresh value.
static inline int32_t kama_atomic_cas(void* cell, void* expected, void* desired,
                                      size_t width, int32_t smo, int32_t fmo) {
    switch (width) {
        case 1: return __atomic_compare_exchange_n((uint8_t *)cell, (uint8_t *)expected, *(uint8_t *)desired, 0, smo, fmo);
        case 2: return __atomic_compare_exchange_n((uint16_t*)cell, (uint16_t*)expected, *(uint16_t*)desired, 0, smo, fmo);
        case 4: return __atomic_compare_exchange_n((uint32_t*)cell, (uint32_t*)expected, *(uint32_t*)desired, 0, smo, fmo);
        case 8: return __atomic_compare_exchange_n((uint64_t*)cell, (uint64_t*)expected, *(uint64_t*)desired, 0, smo, fmo);
    }
    return 0;
}

// ---- fetch-add / fetch-sub: return the PRIOR value, widened to 64 bits -----------------------------------
// Two's-complement add/sub is signedness-agnostic, so the unsigned-width op is correct for a signed `T`
// too; the kama surface truncates the returned prior value back to `T`. On a `Ptr` cell this is byte
// arithmetic over the raw bits — well-defined here, though rarely meaningful (use load/store/CAS instead).
static inline uint64_t kama_atomic_fetch_add(void* cell, uint64_t delta, size_t width, int32_t mo) {
    switch (width) {
        case 1: return (uint64_t)__atomic_fetch_add((uint8_t *)cell, (uint8_t )delta, mo);
        case 2: return (uint64_t)__atomic_fetch_add((uint16_t*)cell, (uint16_t)delta, mo);
        case 4: return (uint64_t)__atomic_fetch_add((uint32_t*)cell, (uint32_t)delta, mo);
        case 8: return (uint64_t)__atomic_fetch_add((uint64_t*)cell, (uint64_t)delta, mo);
    }
    return 0;
}

static inline uint64_t kama_atomic_fetch_sub(void* cell, uint64_t delta, size_t width, int32_t mo) {
    switch (width) {
        case 1: return (uint64_t)__atomic_fetch_sub((uint8_t *)cell, (uint8_t )delta, mo);
        case 2: return (uint64_t)__atomic_fetch_sub((uint16_t*)cell, (uint16_t)delta, mo);
        case 4: return (uint64_t)__atomic_fetch_sub((uint32_t*)cell, (uint32_t)delta, mo);
        case 8: return (uint64_t)__atomic_fetch_sub((uint64_t*)cell, (uint64_t)delta, mo);
    }
    return 0;
}

#endif  // KAMA_ATOMIC_H
