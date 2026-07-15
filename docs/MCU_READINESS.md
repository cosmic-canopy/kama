# kama MCU / bare-metal readiness gap analysis

kama's no-GC, RAII, transpile-to-portable-C model is a natural fit for microcontrollers — deterministic,
no runtime, no allocator unless you ask for one. This document assesses what the **language** still needs to
target a **bare-metal MCU** (ARM Cortex-M / AVR / RISC-V, no OS, often no heap), and recommends a sequence.
Status: ✅ have · 🟡 partial · ❌ missing. The emphasis is **language surface** — most peripheral drivers are
ordinary library/FFI work once the core gaps below are closed.

Related: [ROADMAP.md](ROADMAP.md) §5 (embedded milestone) is the design of record; the allocator seam is shared
with [ENGINE_READINESS.md](ENGINE_READINESS.md) (frame arenas) and the collections campaign (M10/M11).

## What kama already has (the foundation)

A surprising amount of the bare-metal core is already in place:

- **Fixed-width scalar types** — `int8..int64`, `uint8..uint64`, `usize`, `float32/float64`, `bool`, `char` map
  straight to `<stdint.h>` types. No hidden width, no boxing.
- **A pointer-free *safe* surface + an explicit `unsafe { }` FFI boundary** — `Ptr<T>`, `addr(of: place)`,
  `extern` functions **and** `extern` structs, `extern "<header>"` includes. This is exactly the shape MMIO
  register access needs (an `extern value` register block + a `Ptr` to its base), with all raw access greppable.
- **No `null` in the safe surface** and **RAII destructors** — deterministic teardown with no GC, no finalizer
  thread, no hidden allocation on scope exit.
- **A no-heap value core** — `type value`, `InlineArray<T, N>` (a bounds-checked stack array, `struct { T v[N]; }`,
  no pointer decay), fixed `FixedArray`, and raw `Ptr` all work with **zero heap**. `sizeof(T)` is compile-time.
- **A runtime that leans only on freestanding headers** — `kama_runtime.h` includes just
  `<stdint.h>`/`<stdbool.h>`/`<stddef.h>`; the panic/bounds-check path writes to fd 2 and traps (no `<stdio.h>`
  pulled into user code).
- **A pluggable allocator seam** — the `Allocator` contract + `GlobalAllocator` (prelude), caller-owned `Arena`/
  `BumpAllocator`, custom-allocator containers (**M10**), and allocator-aware `new` / `Owned<T, A>` (**M11a**).
  A supplied allocator can already route container + boxed-object memory to a static region.
- **`--no-std`** — the prelude (Optional/Result, the contracts, primitive impls) and the smart-pointer triad are
  embedded in the compiler binary, so they survive an install with no `lib/` on disk.

That is most of a freestanding systems core. The gaps are the MCU-specific language surface: **module-level
mutable state, a `volatile`/MMIO qualifier, interrupt entry points, a freestanding build target, and the last
mile of the no-heap story.**

---

## Tier 0 — Hard blockers (can't flash a real MCU firmware without these)

| Feature | Status | Why an MCU needs it | Effort |
|---|---|---|---|
| **Module-level mutable statics** (`static` globals with deterministic zero/const init) | ❌ missing — no module-scope mutable variable exists in the grammar | Firmware *lives* on module state: peripheral handles, ISR-shared flags, ring buffers, flash lookup tables. An ISR and `main` must share a flag; today there is no place to put it. **New language surface** (declaration + guaranteed zero/const init at reset). ROADMAP §5. | **M** |
| **`hardware` / `volatile` qualifier for MMIO** | ❌ missing — `volatile` is a *reserved* token; the emitter hard-errors ("reserved (embedded/MMIO) but not yet implemented", `kama.cemit.cpp`) | A memory-mapped register read/write must not be optimized away or reordered. Plan: a `hardware Ptr<T>` → C `volatile T*` (mirroring how `const Ptr<T>` already lowers), explicitly **not** a concurrency primitive. ROADMAP §5. | **M** |
| **Interrupt handlers / ISR entry** | ❌ missing — no attribute or entry-point syntax; every `fn` is an ordinary C function | An ISR is a specific symbol (vector-table slot) with a target-specific calling convention (`__attribute__((interrupt))` / AVR `ISR()` / a naked reset handler). Needs an attribute to emit it and to keep it out of the normal `main`/argv path. | **M** |
| **Freestanding build target** (`--target embedded`: `-ffreestanding -nostdlib`, no `argc/argv` shim, `main` never returns) | 🟡 partial — the runtime is freestanding-friendly, but the driver always synthesizes a hosted `int main(int argc, char** argv)` wrapper that calls `kama_main()` and *returns* | Bare metal has no `argc`/`argv`, no `exit`, and `main` is an infinite loop (or a vendor `reset_handler`). The compiler must emit a freestanding entry (or none) and let a startup object/linker script own the vector table. | **M** |

## Tier 1 — Needed for a serious firmware (painful without)

| Feature | Status | Why | Effort |
|---|---|---|---|
| **The no-heap story: fallible allocation + a heap-free subset** | 🟡 partial — M10/M11a route allocations through a supplied `Allocator`, but `allocate` **panics** on OOM and `string`/`Shared`/`Weak` still assume a global heap | A no-heap target needs *every* allocation routed **and** a non-panic failure path: `allocate -> Optional<Ptr>` (ROADMAP, deferred with this milestone). Plus a documented, compiler-checkable "value + `InlineArray` + `Ptr` + stack" subset that rejects `string`/smart-pointers when you opt out of the heap. | **M–L** |
| **Linker-section / placement attributes** | ❌ missing | Const tables belong in flash (`.rodata`), ISR vectors in a fixed section, DMA buffers in a specific RAM bank; AVR needs `PROGMEM`. Needs a `@section("...")`-style attribute on statics/functions. | **M** |
| **Inline assembly / intrinsics** | ❌ missing — no `asm` in the grammar | `WFI`/`WFE`, memory barriers (`DMB`/`DSB`), `cpsid i` (disable interrupts), and cycle-exact delays need inline asm or compiler intrinsics (or a thin `extern` shim as a stopgap). | **S–M** |
| **Panic/trap policy hook** | 🟡 partial — the trap is already runtime-free (writes fd 2 + `__builtin_trap`/`abort`), but the destination is fixed | On an MCU there is no fd 2; a panic should be redirectable to a user handler (blink an LED, reset, breakpoint). Needs a weak/overridable panic hook rather than a hardcoded `abort`. | **S** |

## Tier 2 — Ergonomics & toolchain (nice-to-have; much is library/FFI, not language)

| Feature | Status | Why | Effort |
|---|---|---|---|
| **`alignof(T)`** | ❌ missing (`sizeof(T)` shipped) | Aligned DMA buffers, register-block layout asserts. The obvious sibling of `sizeof`. | **S** |
| **Compile-time evaluation (const-eval)** | 🟡 partial — `const` values + const generics + `sizeof` exist; arithmetic on const params / generated lookup tables do not | Baud-rate divisors, gamma/trig tables, permutation constants baked at build time. | **M** |
| **Soft-float mode / fixed-point** | 🟡 partial — float types exist; no soft-float intrinsics emitted, no fixed-point type | Cortex-M0/AVR have no FPU; today you pass `-msoft-float` to the C compiler and eat the libcall cost. A `Q15.16`-style fixed-point library `type value` is writable *today* (operator overloading) — this is a **library**, not a language gap. | **S (lib) / M (soft-float)** |
| **Toolchain integration** (target triples, linker scripts, startup objects, vendor HALs: pico-sdk / Arduino core / esp-idf) | ❌ missing — the driver emits C and defers to `clang`/`zig`; cross-compile flags are all manual via `--cc`/`--link` | Turnkey `kama build --target thumbv7em-none-eabi` with a linker script and a startup shim. Mostly driver/packaging work atop the C backend. | **M** |
| **MMIO register FFI** | ✅ have — `extern value` register structs + `Ptr` + `addr(of:)` + `unsafe` | The keystone for peripheral drivers — already shipped; a `hardware Ptr<T>` (Tier 0) makes it correct under optimization. | done |

---

## Recommended sequence

1. **Module-level statics with deterministic init** — the single biggest blocker; nothing real runs without
   ISR-shared state. Design the declaration + reset-time zero/const init first.
2. **`hardware` qualifier** (rename the reserved `volatile`) → `volatile T*` for MMIO — small, unblocks correct
   register access under `-O2`.
3. **`--target embedded`** — freestanding entry (no argv shim, `main` never returns), `-ffreestanding -nostdlib`,
   overridable panic hook. Now a blink-LED firmware links.
4. **ISR attribute** + **section placement** — real interrupt-driven drivers.
5. **Fallible `allocate -> Optional<Ptr>`** + the checkable no-heap subset — completes the no-heap story
   (shared with the embedded milestone in ROADMAP §5).
6. **Inline asm / intrinsics**, then **toolchain packaging** (target triples + linker scripts + vendor HALs)
   and the const-eval / `alignof` / soft-float polish.

**Bottom line:** the *systems core* (types, FFI, RAII, no-null, no-heap value subset, pluggable allocator) is
already here. Bare-metal readiness is a focused set of **language-surface** additions — statics, `hardware`,
ISRs, a freestanding target, fallible alloc — plus toolchain packaging. None require rethinking the model; they
extend it into the freestanding world the design already anticipates (ROADMAP §5).
