#ifndef KAMA_ATOMIC_H
#define KAMA_ATOMIC_H

// kama atomic binding — the FFI boundary for `std::concurrent`'s `Atomic<T>` (M6).
//
// `Atomic<T>` is the ONE sanctioned cross-isolate shared-MUTABLE cell — the concurrency analog of
// `unsafe {}`/`UnsafePtr` (opt-in, greppable, atomics-only). Everything else in the language is shared-nothing;
// this seam is where a value may be read/written concurrently by several isolates without a data race.
//
// The `Atomic<T>` surface is pure kama (a move-only `resource` holding one inline `T` cell); this header is
// the width-generic op layer it calls. Each op takes the cell address, a byte width (`sizeof(T)`, a
// compile-time constant at the kama call site), and a memory-order int, then dispatches on the width to a
// typed `__atomic_*_n` compiler builtin — so every op inlines to a lock-free machine instruction for the
// concrete scalar width (no libatomic dependency), or, on a bare-metal chip without one, to the same operation
// with interrupts masked (below). The generic sized `__atomic_load`/`__atomic_store`
// forms would take a runtime size through this boundary and fall back to libatomic; the width switch keeps
// it inline. `T` is restricted (by the emitter) to an integer primitive, `bool`, a float or `UnsafePtr`, so a
// cell is always a naturally-aligned lock-free scalar of width 1/2/4/8. A float cell is pure bit movement
// through the width switch — load/store/exchange/compare-exchange on its 4 or 8 bytes, with
// compare-exchange comparing BITS (C++20 `atomic<float>` semantics) — and the emitter refuses
// `fetchAdd`/`fetchSub` on one at the call, since the add below is an integer add of the bit pattern.
//
// Freestanding-friendly: only <stdint.h>. The `__atomic_*` builtins are GCC/Clang intrinsics (no header),
// and emscripten (clang) lowers them onto shared-memory Web-Worker atomics unchanged — one source, both
// legs, exactly like kama_isolate.h / kama_channel.h.
//
// Memory order ints match the compiler's __ATOMIC_* enum (relaxed 0 … seq_cst 5), so the kama-side
// `MemoryOrder` enum passes straight through. The `Atomic<T>` surface defaults every op to seq_cst (5).

#include <stdint.h>
#include <stddef.h>   // size_t (matches kama `usize`/`sizeof(T)`)

// ---- Each width, native or masked (KR-349) -------------------------------------------------------------
// A hosted target does every width with the compiler's builtins: a width it has no lock-free atomic for is refused at
// compile time (`Atomic<T>`'s instantiation in the emitter, backed by a `_Static_assert` per instantiated width), since
// there the builtin would be libatomic's lock, which `Atomic` promises it is not. A bare-metal target does a width
// natively where the C compiler reports it always lock-free, so a target's `-mcpu`/`-march` decide, never its triple: one
// `riscv32-unknown-none-elf` covers rv32imc and rv32imac. Any other width (a Cortex-M0's every read-modify-write, an
// M3's 8 bytes, an rv32imc's every read-modify-write) is done with interrupts masked, which is sound because kama runs on
// one core of a board (KR-346 replaces the mask when it starts a second). A width the core loads and stores in one access
// does those plainly, with the barrier LLVM emits for an atomic one (`ldr; dmb sy`), so no load reaches libatomic.
// The choice is the PREPROCESSOR's, per width: at -O0 a builtin in a dead C `if` is still emitted, as a libatomic call.
#if defined(KAMA_TARGET_EMBEDDED)
  #define KAMA__ATOMIC_NATIVE_1 (__GCC_ATOMIC_CHAR_LOCK_FREE == 2)
  #define KAMA__ATOMIC_NATIVE_2 (__GCC_ATOMIC_SHORT_LOCK_FREE == 2)
  #define KAMA__ATOMIC_NATIVE_4 (__GCC_ATOMIC_INT_LOCK_FREE == 2)
  #define KAMA__ATOMIC_NATIVE_8 (__GCC_ATOMIC_LLONG_LOCK_FREE == 2)
#else
  #define KAMA__ATOMIC_NATIVE_1 1
  #define KAMA__ATOMIC_NATIVE_2 1
  #define KAMA__ATOMIC_NATIVE_4 1
  #define KAMA__ATOMIC_NATIVE_8 1
#endif

#define KAMA__ATOMIC_NATIVE(N, T) \
static inline T       kama__atomic_load##N(T* c, int32_t mo)       { return __atomic_load_n(c, mo); } \
static inline void    kama__atomic_store##N(T* c, T v, int32_t mo) { __atomic_store_n(c, v, mo); } \
static inline T       kama__atomic_swap##N(T* c, T v, int32_t mo)  { return __atomic_exchange_n(c, v, mo); } \
static inline int32_t kama__atomic_cas##N(T* c, T* e, T d, int32_t smo, int32_t fmo) { \
    return __atomic_compare_exchange_n(c, e, d, 0, smo, fmo); } \
static inline T       kama__atomic_add##N(T* c, T v, int32_t mo)   { return __atomic_fetch_add(c, v, mo); } \
static inline T       kama__atomic_sub##N(T* c, T v, int32_t mo)   { return __atomic_fetch_sub(c, v, mo); }

#if !(KAMA__ATOMIC_NATIVE_1 && KAMA__ATOMIC_NATIVE_2 && KAMA__ATOMIC_NATIVE_4 && KAMA__ATOMIC_NATIVE_8)
// The mask saves the interrupt state, masks, and restores what it saved, so an operation inside a handler or inside a masked
// region leaves it masked. It costs the few instructions between the mask and the restore.
//  - Arm M-profile: `cpsid i` is IGNORED in unprivileged Thread mode, which is how an RTOS task runs (FreeRTOS-MPU, Zephyr's
//    user mode), and the operation would silently stop being atomic. So PRIMASK is read back after it, every time (an
//    `expose fn` may be called from such a task), and a zero panics: `Atomic` masks interrupts on this chip, which needs
//    privileged mode. The check is `mrs; cmp; beq`, 8 bytes on a Cortex-M0.
//  - RISC-V: `mstatus.MIE`, which needs M-mode, as an ESP32-C3's code runs. No check: a CSR access outside M-mode traps.
//  - Anything else: no mask is written. Xtensa's (`rsil`/`wsr.ps`) waits for a C compiler that generates Xtensa code
//    (clang 18 knows the backend but not the triple; zig has none). There a width the chip lacks traps: at -O0 every
//    `Atomic` program carries that arm, so an `#error` here would refuse a classic ESP32's `Atomic<uint32>` for the sake
//    of an `Atomic<uint64>` it never declared. Instead the emitter asks the C compiler, once per atomic cell type a
//    program instantiates, whether that width is lock-free wherever `KAMA__ATOMIC_MASKS` is not defined, which refuses
//    at build time exactly the programs that would reach the trap.
#if defined(__ARM_ARCH_PROFILE) && __ARM_ARCH_PROFILE == 'M'
#define KAMA__ATOMIC_MASKS 1
extern void kama_panic_handler(void) __attribute__((noreturn));
static inline unsigned long kama__atomic_mask(void) {
    unsigned long saved, now;
    __asm__ __volatile__("mrs %0, primask\n\tcpsid i\n\tmrs %1, primask" : "=r"(saved), "=r"(now) : : "memory");
    if (!now) kama_panic_handler();
    return saved;
}
static inline void kama__atomic_unmask(unsigned long saved) { __asm__ __volatile__("msr primask, %0" : : "r"(saved) : "memory"); }
#elif defined(__riscv)
#define KAMA__ATOMIC_MASKS 1
// GCC 12 on asks for `zicsr` by name before it assembles a CSR access, and a board's `-march=rv32imc` need not name it.
#define KAMA__ZICSR(insn) ".option push\n\t.option arch, +zicsr\n\t" insn "\n\t.option pop"
static inline unsigned long kama__atomic_mask(void) {
    unsigned long saved;
    __asm__ __volatile__(KAMA__ZICSR("csrrci %0, mstatus, 8") : "=r"(saved) : : "memory");
    return saved;
}
static inline void kama__atomic_unmask(unsigned long saved) {
    __asm__ __volatile__(KAMA__ZICSR("csrs mstatus, %0") : : "r"(saved & 8) : "memory");
}
#else
static inline unsigned long kama__atomic_mask(void) { __builtin_trap(); }
static inline void kama__atomic_unmask(unsigned long saved) { (void)saved; }
#endif

// A width that fits one register loads and stores in one access, so only its read-modify-writes mask; a wider one masks all.
#define KAMA__ATOMIC_MASKED(N, T) \
static inline T kama__atomic_load##N(T* c, int32_t mo) { \
    if (sizeof(T) <= sizeof(void*)) { T v = *(volatile T*)c; __atomic_thread_fence(mo); return v; } \
    unsigned long s = kama__atomic_mask(); T v = *(volatile T*)c; kama__atomic_unmask(s); return v; } \
static inline void kama__atomic_store##N(T* c, T v, int32_t mo) { \
    if (sizeof(T) <= sizeof(void*)) { \
        __atomic_thread_fence(mo); *(volatile T*)c = v; if (mo == __ATOMIC_SEQ_CST) __atomic_thread_fence(mo); return; } \
    unsigned long s = kama__atomic_mask(); *(volatile T*)c = v; kama__atomic_unmask(s); } \
static inline T kama__atomic_swap##N(T* c, T v, int32_t mo) { \
    (void)mo; unsigned long s = kama__atomic_mask(); T p = *c; *c = v; kama__atomic_unmask(s); return p; } \
static inline int32_t kama__atomic_cas##N(T* c, T* e, T d, int32_t smo, int32_t fmo) { \
    (void)smo; (void)fmo; unsigned long s = kama__atomic_mask(); T p = *c; int32_t ok = p == *e; if (ok) *c = d; \
    kama__atomic_unmask(s); if (!ok) *e = p; return ok; } \
static inline T kama__atomic_add##N(T* c, T v, int32_t mo) { \
    (void)mo; unsigned long s = kama__atomic_mask(); T p = *c; *c = (T)(p + v); kama__atomic_unmask(s); return p; } \
static inline T kama__atomic_sub##N(T* c, T v, int32_t mo) { \
    (void)mo; unsigned long s = kama__atomic_mask(); T p = *c; *c = (T)(p - v); kama__atomic_unmask(s); return p; }
#endif

#if KAMA__ATOMIC_NATIVE_1
KAMA__ATOMIC_NATIVE(1, uint8_t)
#else
KAMA__ATOMIC_MASKED(1, uint8_t)
#endif
#if KAMA__ATOMIC_NATIVE_2
KAMA__ATOMIC_NATIVE(2, uint16_t)
#else
KAMA__ATOMIC_MASKED(2, uint16_t)
#endif
#if KAMA__ATOMIC_NATIVE_4
KAMA__ATOMIC_NATIVE(4, uint32_t)
#else
KAMA__ATOMIC_MASKED(4, uint32_t)
#endif
#if KAMA__ATOMIC_NATIVE_8
KAMA__ATOMIC_NATIVE(8, uint64_t)
#else
KAMA__ATOMIC_MASKED(8, uint64_t)
#endif

// ---- load: *out = atomic_load(cell) --------------------------------------------------------------------
static inline void kama_atomic_load(void* cell, void* out, size_t width, int32_t mo) {
    switch (width) {
        case 1: *(uint8_t *)out = kama__atomic_load1((uint8_t *)cell, mo); break;
        case 2: *(uint16_t*)out = kama__atomic_load2((uint16_t*)cell, mo); break;
        case 4: *(uint32_t*)out = kama__atomic_load4((uint32_t*)cell, mo); break;
        case 8: *(uint64_t*)out = kama__atomic_load8((uint64_t*)cell, mo); break;
    }
}

// ---- store: atomic_store(cell, *val) -------------------------------------------------------------------
static inline void kama_atomic_store(void* cell, void* val, size_t width, int32_t mo) {
    switch (width) {
        case 1: kama__atomic_store1((uint8_t *)cell, *(uint8_t *)val, mo); break;
        case 2: kama__atomic_store2((uint16_t*)cell, *(uint16_t*)val, mo); break;
        case 4: kama__atomic_store4((uint32_t*)cell, *(uint32_t*)val, mo); break;
        case 8: kama__atomic_store8((uint64_t*)cell, *(uint64_t*)val, mo); break;
    }
}

// ---- swap: *out = atomic_exchange(cell, *val) ----------------------------------------------------------
static inline void kama_atomic_swap(void* cell, void* val, void* out, size_t width, int32_t mo) {
    switch (width) {
        case 1: *(uint8_t *)out = kama__atomic_swap1((uint8_t *)cell, *(uint8_t *)val, mo); break;
        case 2: *(uint16_t*)out = kama__atomic_swap2((uint16_t*)cell, *(uint16_t*)val, mo); break;
        case 4: *(uint32_t*)out = kama__atomic_swap4((uint32_t*)cell, *(uint32_t*)val, mo); break;
        case 8: *(uint64_t*)out = kama__atomic_swap8((uint64_t*)cell, *(uint64_t*)val, mo); break;
    }
}

// ---- compare-exchange (strong): if *cell == *expected { *cell = *desired; true } else { *expected = *cell; false }
// On failure the observed value is written back through `expected` (the caller's `ref` cell), matching the
// C11 `atomic_compare_exchange_strong` contract, so a CAS-loop can retry against the fresh value.
static inline int32_t kama_atomic_cas(void* cell, void* expected, void* desired,
                                      size_t width, int32_t smo, int32_t fmo) {
    switch (width) {
        case 1: return kama__atomic_cas1((uint8_t *)cell, (uint8_t *)expected, *(uint8_t *)desired, smo, fmo);
        case 2: return kama__atomic_cas2((uint16_t*)cell, (uint16_t*)expected, *(uint16_t*)desired, smo, fmo);
        case 4: return kama__atomic_cas4((uint32_t*)cell, (uint32_t*)expected, *(uint32_t*)desired, smo, fmo);
        case 8: return kama__atomic_cas8((uint64_t*)cell, (uint64_t*)expected, *(uint64_t*)desired, smo, fmo);
    }
    return 0;
}

// ---- fetch-add / fetch-sub: write the PRIOR value through `dst` ------------------------------------------
// Two's-complement add/sub is signedness-agnostic, so the unsigned-width op is correct for a signed `T`.
// On an `UnsafePtr` cell this is byte arithmetic over the raw bits — well-defined here, though rarely
// meaningful (use load/store/CAS instead).
//
// ⚠️ `delta` and `dst` are ADDRESSES of `width` bytes, not a `uint64`, and that is the whole point. They
// used to be a `uint64` in and a `uint64` out, with the kama surface converting on both sides — which was
// correct only while a narrowing `cast` truncated. Since `0.9.30` it TRAPS, so `Atomic<int8>` aborted on
// any negative prior value ("255 does not fit [-128, 127]") and any negative delta ("-5 does not fit
// [0, 18446744073709551615]"). Moving `width` bytes converts nothing and cannot trap, and it matches
// `kama_atomic_swap` directly above — same (cell, val, dst, width, mo) shape.
static inline void kama_atomic_fetch_add(void* cell, void* delta, void* dst, size_t width, int32_t mo) {
    switch (width) {
        case 1: *(uint8_t *)dst = kama__atomic_add1((uint8_t *)cell, *(uint8_t *)delta, mo); break;
        case 2: *(uint16_t*)dst = kama__atomic_add2((uint16_t*)cell, *(uint16_t*)delta, mo); break;
        case 4: *(uint32_t*)dst = kama__atomic_add4((uint32_t*)cell, *(uint32_t*)delta, mo); break;
        case 8: *(uint64_t*)dst = kama__atomic_add8((uint64_t*)cell, *(uint64_t*)delta, mo); break;
    }
}

static inline void kama_atomic_fetch_sub(void* cell, void* delta, void* dst, size_t width, int32_t mo) {
    switch (width) {
        case 1: *(uint8_t *)dst = kama__atomic_sub1((uint8_t *)cell, *(uint8_t *)delta, mo); break;
        case 2: *(uint16_t*)dst = kama__atomic_sub2((uint16_t*)cell, *(uint16_t*)delta, mo); break;
        case 4: *(uint32_t*)dst = kama__atomic_sub4((uint32_t*)cell, *(uint32_t*)delta, mo); break;
        case 8: *(uint64_t*)dst = kama__atomic_sub8((uint64_t*)cell, *(uint64_t*)delta, mo); break;
    }
}

#endif  // KAMA_ATOMIC_H
