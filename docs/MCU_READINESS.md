# kama MCU / bare-metal readiness gap analysis

kama's no-GC, RAII, transpile-to-portable-C model is a natural fit for microcontrollers — deterministic,
no runtime, no allocator unless you ask for one. This document assesses what the **language** still needs to
target a **bare-metal MCU** (ARM Cortex-M / AVR / RISC-V, no OS, often no heap), and recommends a sequence.
Status: ✅ have · 🟡 partial · ❌ missing. The emphasis is **language surface** — most peripheral drivers are
ordinary library/FFI work once the core gaps below are closed.

Related: [ROADMAP.md](ROADMAP.md) §5 (embedded milestone) is the design of record; the allocator seam is shared
with [ENGINE_READINESS.md](ENGINE_READINESS.md) (frame arenas) and the collections campaign (M10/M11).

> **Post-concurrency triage (2026-07-23): this track is the front-runner.** With the concurrency campaign
> complete, the readiness re-triage (ROADMAP §6 "Forward sequencing") leans MCU: it is the **only** track with
> real **language-surface** work queued (Engine and Web are now library/platform work with no language gap),
> and its #1 blocker builds directly onto the just-shipped concurrency model (see the statics row below). The
> recommended sequence at the bottom is the actionable starting point.

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
  pulled into user code). The `std::fmt`/`Format` number formatters live here too and stay header-clean:
  integer/char/bool/string formatting is a pure digit/encode loop (no libc), and float formatting declares
  `snprintf` at **block scope** (like `malloc`/`memcpy`) — so no header leaks, though a `-nostdlib` build that
  *formats a float* still needs a `snprintf` symbol (integer/string formatting is fully freestanding).
- **A pluggable allocator seam** — the `Allocator` contract + `GlobalAllocator` (prelude), caller-owned `Arena`/
  `BumpAllocator`, custom-allocator containers (**M10**), and allocator-aware `new` / `Owned<T, A>` (**M11a**).
  A supplied allocator can already route container + boxed-object memory to a static region.
- **`--no-std`** — the prelude (Optional/Result, the contracts, primitive impls) and the smart-pointer triad are
  embedded in the compiler binary, so they survive an install with no `lib/` on disk.

That is most of a freestanding systems core. The gaps are the MCU-specific language surface: **module-level
mutable state, a `hardware`/MMIO qualifier, interrupt entry points, a freestanding build target, and the last
mile of the no-heap story.**

---

## Tier 0 — Hard blockers (can't flash a real MCU firmware without these)

| Feature | Status | Why an MCU needs it | Effort |
|---|---|---|---|
| **Module-level mutable statics** (`static` globals with deterministic zero/const init) | ✅ **SHIPPED (step 1)** — `static T name = const;`, per-isolate by construction (`KAMA_ISOLATE_LOCAL`); value/`Ptr`/`InlineArray` + const-init only in v1; TSan-clean | Firmware *lives* on module state: peripheral handles, ISR-shared flags, ring buffers, flash lookup tables. An ISR and `main` must share a flag; today there is no place to put it. **New language surface** (declaration + guaranteed zero/const init at reset). ROADMAP §5. **Build it *per-isolate by construction*** (plain C `static` on a single-core MCU → zero cost; `_Thread_local` on multicore native; automatic on wasm) so the same declaration is race-free under the threading model — cross-isolate sharing stays on the `Atomic<T>` seam. The concurrency model that pins this rule is now **shipped** (campaign complete 2026-07-23: isolates + channels + `scope` + `Atomic<T>` + `parallel_for`, [design/concurrency.md](design/concurrency.md) §"three sharing seams"), so this is **settled, proven ground** — statics are a targeted addition onto working code, not a co-design with an unbuilt system. **This makes MCU the lowest-risk next track.** | **M** |
| **`hardware` qualifier for MMIO** | ✅ **SHIPPED (step 2).** `hardware Ptr<T>` → C `volatile T*`, `hardware` on a module `static` → `volatile T`/`volatile T*` (ISR↔loop flag/handle), `const hardware Ptr<T>` → `const volatile T*` (read-only register); mirrors the shipped `const Ptr<T>` lowering. `volatile` is no longer a keyword. Explicitly **not** a concurrency primitive. ROADMAP §5. Fixtures: `tests/hardware_*`. | done |
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

1. **Module-level statics with deterministic init** — ✅ **DONE.** `static T name = const;`, per-isolate by
   construction (`static KAMA_ISOLATE_LOCAL T name`; `_Thread_local` native + wasm-pthreads, plain `static` on
   `--target embedded`), value/`Ptr`/`InlineArray` + const-init only in v1, zero-init when the initializer is
   omitted. Race-free proven TSan-clean. Fixtures: `tests/module_static_*` (+ `xfail/module_static_*`).
2. **`hardware` qualifier** — ✅ **DONE.** `hardware Ptr<T>` → `volatile T*` for MMIO and `hardware` module
   statics → `volatile T`/`volatile T*` for ISR↔loop flags/handles (`const hardware Ptr<T>` → `const volatile T*`).
   `volatile` de-reserved (no longer a keyword). Correct register access under `-O2`. Fixtures: `tests/hardware_*`.
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
