# kama MCU / bare-metal readiness gap analysis

kama's no-GC, RAII, transpile-to-portable-C model is a natural fit for microcontrollers — deterministic,
no runtime, no allocator unless you ask for one. This document assesses what the **language** still needs to
target a **bare-metal MCU** (ARM Cortex-M / AVR / RISC-V, no OS, often no heap), and recommends a sequence.
Status: ✅ have · 🟡 partial · ❌ missing. The emphasis is **language surface** — most peripheral drivers are
ordinary library/FFI work once the core gaps below are closed.

Related: [ROADMAP_DETAIL.md](ROADMAP_DETAIL.md) §5 (embedded milestone) is the design of record; the allocator seam is shared
with [ENGINE_READINESS.md](ENGINE_READINESS.md) (frame arenas) and the collections campaign (M10/M11).

> **Post-concurrency triage (2026-07-23): this track is the front-runner.** With the concurrency campaign
> complete, the readiness re-triage (ROADMAP_DETAIL §1, "the decided big-arc sequence") leans MCU: it is the **only** track with
> real **language-surface** work queued (Engine and Web are now library/platform work with no language gap),
> and its #1 blocker builds directly onto the just-shipped concurrency model (see the statics row below). The
> recommended sequence at the bottom is the actionable starting point.

## What kama already has (the foundation)

A surprising amount of the bare-metal core is already in place:

- **Fixed-width scalar types** — `int8..int64`, `uint8..uint64`, `usize`, `float32/float64`, `bool`, `char` map
  straight to `<stdint.h>` types. No hidden width, no boxing.
- **A pointer-free *safe* surface + an explicit `unsafe fn` FFI boundary** — `UnsafePtr<T>`, `addr(of: place)`,
  `extern` functions **and** `extern` structs, `extern "<header>"` includes. This is exactly the shape MMIO
  register access needs (an `extern value` register block + an `UnsafePtr` to its base), with all raw access greppable.
- **No `null` in the safe surface** and **RAII destructors** — deterministic teardown with no GC, no finalizer
  thread, no hidden allocation on scope exit.
- **A no-heap value core** — `type value`, `InlineArray<T>#(N)` (a bounds-checked stack array, `struct { T v[N]; }`,
  no pointer decay), fixed `FixedArray`, and raw `UnsafePtr` all work with **zero heap**. `sizeof(T)` is compile-time.
- **A runtime that leans only on freestanding headers** — `kama_runtime.h` includes just
  `<stdint.h>`/`<stdbool.h>`/`<stddef.h>`; the panic/bounds-check path writes to fd 2 and traps (no `<stdio.h>`
  pulled into user code). The `std::fmt`/`Formattable` number formatters live here too and stay header-clean:
  integer/char/bool/string formatting is a pure digit/encodeJsonBuffer loop (no libc), and float formatting declares
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
| **Module-level mutable statics** (`static` globals with deterministic zero/const init) | ✅ **SHIPPED (step 1)** — `static T name = const;`, per-isolate by construction (`KAMA_ISOLATE_LOCAL`); value/`UnsafePtr`/`InlineArray` + const-init only in v1; TSan-clean | Firmware *lives* on module state: peripheral handles, ISR-shared flags, ring buffers, flash lookup tables. An ISR and `main` must share a flag; today there is no place to put it. **New language surface** (declaration + guaranteed zero/const init at reset). ROADMAP_DETAIL §5. **Build it *per-isolate by construction*** (plain C `static` on a single-core MCU → zero cost; `_Thread_local` on multicore native; automatic on wasm) so the same declaration is race-free under the threading model — cross-isolate sharing stays on the `Atomic<T>` seam. The concurrency model that pins this rule is now **shipped** (campaign complete 2026-07-23: isolates + channels + `scope` + `Atomic<T>` + `parallel_for`, [SPEC.md](SPEC.md#the-three-sharing-seams-) § "The three sharing seams"), so this is **settled, proven ground** — statics are a targeted addition onto working code, not a co-design with an unbuilt system. **This makes MCU the lowest-risk next track.** | **M** |
| **`hardware` qualifier for MMIO** | ✅ **SHIPPED (step 2).** `hardware UnsafePtr<T>` → C `volatile T*`, `hardware` on a module `static` → `volatile T`/`volatile T*` (ISR↔loop flag/handle), `const hardware UnsafePtr<T>` → `const volatile T*` (read-only register); mirrors the shipped `const UnsafePtr<T>` lowering. `volatile` is no longer a keyword. Explicitly **not** a concurrency primitive. ROADMAP_DETAIL §5. Fixtures: `tests/hardware_*`. | done |
| **Interrupt handlers / ISR entry** | ✅ **SHIPPED (step 4).** `@interrupt expose fn void h()` → `__attribute__((interrupt, used))` (the Cortex-M / RISC-V / classic-ARM ISR calling convention). Enforced `void f(void)` signature; `expose` required so the vector table can name the bare symbol; `used` survives `--gc-sections`. AVR's `@interrupt("VECTOR")` → `ISR(VECTOR)` macro is a deliberately separate later step. Fixture `tests/support/embedded_isr.kama` (transpile-grep in `tools/check-embedded.sh`). | done |
| **Freestanding build target** (`--target embedded`: `-ffreestanding -nostdlib`, no `argc/argv` shim, `main` never returns) | ✅ **DONE (step 3)** — `--target embedded` compiles to a `-ffreestanding -nostdlib` object; the emitter emits a guarded `int main(void){ kama_main(); for(;;){} }` (no argv, never returns) selected by `KAMA_TARGET_EMBEDDED`. Triple-agnostic (via `--cc`); the startup object + linker script own the vector table at the user's link step. | **M** |

## Tier 1 — Needed for a serious firmware (painful without)

| Feature | Status | Why | Effort |
|---|---|---|---|
| **The no-heap story: fallible allocation + a heap-free subset** | ✅ **SHIPPED (step 5).** The `Allocator` seam is **fallible** (`allocate -> Optional<UnsafePtr>`, `None` on OOM — never panics); `new`/collections unwrap-or-panic (prelude `unwrapPtr`), **`try new -> Optional<Owned<T>>`** is the non-panic construction entry, and direct `allocate` callers `match` on `None`. The compiler-checkable subset is a per-region **`@noheap`** fn attribute + a whole-program **`--no-heap`** flag: every emitter-visible allocation is a compile error via one gate (`rejectIfNoHeap`). Fixtures: `tests/alloc_frame_arena.kama` (graceful arena exhaustion + `@noheap` hot loop), `tests/noheap_ok.kama`, `tests/xfail/noheap_{new,try_new,interp}.kama`, `tools/check-noheap.sh`. | done |
| **Linker-section / placement attributes** | ✅ **SHIPPED (step 4).** `@section(".name")` on a module static or a function → `__attribute__((section(".name")))` — const tables in flash, ISR vectors in a fixed section, DMA buffers in a RAM bank. (AVR `PROGMEM` is `@section` + the AVR toolchain, later.) Fixtures `tests/support/embedded_section.kama` (freestanding-object build) + `embedded_isr.kama`. | done |
| **Struct layout control** | ✅ **SHIPPED.** `@align(N)` / `@packed` on a `type value`/`type resource` → `__attribute__((aligned(N)))` / `((packed))` on the emitted struct — a packed MMIO register block or wire struct, and cache-line/DMA alignment. Passthrough by design: kama states the constraint, `sizeof`/`alignof`/`comptime assert` verify what the toolchain did. `N` is held to a power of two because gcc/clang round a non-power-of-two up rather than refusing it. Fixture `tests/layout_align_packed.kama` + four `tests/xfail/`. | done |
| **Inline assembly / intrinsics** | ✅ **SHIPPED (step 6a)** — `asm("…")` inside an `unsafe fn` lowers to `__asm__ __volatile__("…" : : : "memory")` (always volatile + a full compiler memory barrier). Fixture `tests/asm_nop.kama` (runs on the host); ARM mnemonics (`wfi`/`cpsid i`/`dsb`) transpile-grep-verified in `tools/check-embedded.sh`. | `WFI`/`WFE`, memory barriers (`DMB`/`DSB`), `cpsid i` (disable interrupts), cycle-exact delays. Curated named helpers (`wfi()`, `disable_interrupts()`) are a thin follow-on library over the primitive. | done |
| **Panic/trap policy hook** | ✅ **DONE (step 3)** — under `KAMA_TARGET_EMBEDDED`, bounds/panic/OOM route through one overridable weak `kama_panic_handler` (default `for(;;) __builtin_trap()`); a strong user symbol redirects to blink/reset/breakpoint. No fd 2 / `abort` dependency. | **S** |

## Tier 2 — Ergonomics & toolchain (nice-to-have; much is library/FFI, not language)

| Feature | Status | Why | Effort |
|---|---|---|---|
| **`alignof(T)`** | ✅ **SHIPPED** (alongside `sizeof(T)`) — `alignof(T)` → C `_Alignof(cType)`, monomorphizes under substitution, and folds in const-init contexts. Fixture `tests/alignof_basic.kama`. | Aligned DMA buffers, register-block layout asserts. | done |
| **Compile-time evaluation (const-eval)** | 🟢 shipped — `const` values + comptime parameters + `sizeof`/`alignof`, **comptime-param arithmetic (6b-1)**, **named `comptime` constants at local/module/type scope (6b-2)**, and **compile-time *functions* `comptime fn` (6b-3)** all ship. A `comptime fn` RUNS at build time and bakes a `static const` scalar or **table** into `.rodata`/flash — a baud divisor, gamma/trig/CRC LUT, or permutation constant computed once by the compiler, zero runtime cost. Bounded (step budget) + pure (deterministic → reproducible builds); comptime-only; both top-level and type-associated (`Type::name()`). Fixtures `comptime_fn_crc` (baked 256-entry CRC-8 LUT, asserted bit-identical to a runtime recompute) + `tools/check-comptime.sh`, `comptime_fn_scalar`/`_type_assoc`. | Baud-rate divisors, gamma/trig tables, permutation constants baked at build time. | **M** |
| **Soft-float mode / fixed-point** | ✅ **SHIPPED** — both halves land, neither a language gap. *Fixed-point* = the pure **library** `type value` `std::num::Fixed<B> comptime(int32 F)` (signed binary fixed-point; `Fixed<int32>#(16)` is Q16.16) over the shipped operator overloading: `+ - * /` (mul/div widen through `int64` and re-scale), `fromInt`/`toInt`/`fromFloat`/`toFloat`, saturating `satAdd`/`satSub`/`satMul` — base ops trap on overflow (language default), `sat*` clamp. Fixture `tests/num_fixed.kama` (exact-value oracle). *Soft-float* = a turnkey **no-FPU board preset**: `--board microbit` (QEMU nRF51822, Cortex-M0, no FPU; triple `thumbv6m-none-eabi`) added to `mcu/build.sh` + `mcu/run-qemu.sh` + `mcu/boards/microbit/linker.ld`. The emitted C's `float`/`double` ops lower to compiler-rt/libgcc soft-float libcalls automatically (no `-mfloat-abi` flag needed on M0). `tools/check-softfloat.sh` builds `tests/mcu_softfloat.kama` into M0 firmware, **boots it on QEMU** and asserts exit 33. (A latent emitter bug surfaced + fixed en route: whole-number `float64` literals like `100.0f64` now keep their decimal point, so `100.0f64 / 8.0f64` is a `double` divide, not integer `100 / 8` — guarded by `tests/float64_literal_div.kama`.) | Cortex-M0/AVR have no FPU. | ✅ done |
| **Toolchain integration** (target triples, linker scripts, startup objects, vendor HALs: pico-sdk / Arduino core / esp-idf) | 🟢 **turnkey Cortex-M path shipped + QEMU-proven** — an opt-in cross image (`tools/Dockerfile.mcu`: arm-none-eabi-gcc + qemu; `tools/cdev build-image-mcu`), a bundled reference startup/vector-table (`mcu/startup.c`) + board linker script (`mcu/boards/lm3s6965evb/linker.ld`), and one-command `mcu/build.sh <in.kama>` (kama object → linked ELF) + `mcu/run-qemu.sh`. `tools/check-mcu.sh` builds `embedded_blink` into Cortex-M firmware, **boots it on QEMU** and asserts exit 22 (skips when the cross toolchain is absent). See [mcu.md](mcu.md). **Remaining:** more boards (STM32/Pico presets) + vendor-HAL glue + a real-hardware flash pass (docs cover the integration recipe). | Turnkey `kama build` → flashable image. Driver/packaging atop the C backend. | ✅ core done (QEMU); real-HW + more boards follow |
| **AVR (Harvard) target family** — the deliberately-deferred bucket (Arduino Uno/Nano/Mega, ATmega/ATtiny) | ❌ deferred (Cortex-M/RISC-V shipped first) | Three AVR-specific pieces, each distinct from the shipped path: **(1) ISR** — `@interrupt("VECTOR")` → the `ISR(VECTOR)` macro (`<avr/interrupt.h>`), not the parameterless `__attribute__((interrupt))` (step 4); **(2) Harvard `PROGMEM`** — flash const data needs `PROGMEM` + `pgm_read_*` accessors (a flash pointer can't be plain-deref'd), so `@section` alone doesn't cover it; **(3) toolchain** — `avr-gcc`-only (clang/zig don't target AVR cleanly), so the `--cc` triple-agnostic story misses it (needs avr-gcc + `-mmcu=`). Cortex-M/RISC-V cover the common hobbyist boards (RP2040/Pico, STM32, ESP32-C3); AVR is a bounded follow-on when demand warrants. | **M** |
| **MMIO register FFI** | ✅ have — `extern value` register structs + `UnsafePtr` + `addr(of:)` + `unsafe` | The keystone for peripheral drivers — already shipped; a `hardware UnsafePtr<T>` (Tier 0) makes it correct under optimization. | done |

---

## Recommended sequence

1. **Module-level statics with deterministic init** — ✅ **DONE.** `static T name = const;`, per-isolate by
   construction (`static KAMA_ISOLATE_LOCAL T name`; `_Thread_local` native + wasm-pthreads, plain `static` on
   `--target embedded`), value/`UnsafePtr`/`InlineArray` + const-init only in v1, zero-init when the initializer is
   omitted. Race-free proven TSan-clean. Fixtures: `tests/module_static_*` (+ `xfail/module_static_*`).
2. **`hardware` qualifier** — ✅ **DONE.** `hardware UnsafePtr<T>` → `volatile T*` for MMIO and `hardware` module
   statics → `volatile T`/`volatile T*` for ISR↔loop flags/handles (`const hardware UnsafePtr<T>` → `const volatile T*`).
   `volatile` de-reserved (no longer a keyword). Correct register access under `-O2`. Fixtures: `tests/hardware_*`.
3. **`--target embedded`** — ✅ **DONE.** Compiles a value program to a `-ffreestanding -nostdlib` **object**:
   guarded freestanding entry (`int main(void){ kama_main(); for(;;){} }` — no argv shim, `main` never returns),
   overridable weak `kama_panic_handler`. Triple-agnostic (CPU triple via `--cc`); the board link (crt0 + linker
   script) is the user's step. Now a value-only blink object compiles freestanding and libc-free. Fixture:
   `tests/embedded_blink.kama`; freestanding guard `tools/check-embedded.sh`.
4. **ISR attribute** + **section placement** — ✅ **DONE.** `@interrupt expose fn void h()` →
   `__attribute__((interrupt, used))` (Cortex-M/RISC-V/classic-ARM; enforced `void()` + `expose`);
   `@section(".x")` on statics/functions → `__attribute__((section(".x")))`. Reuses the existing
   `@name(args)` attribute mechanism (now drives codegen, previously serialization-only). AVR
   `@interrupt("VECTOR")` → `ISR()` deferred. Fixtures: `tests/support/embedded_{isr,section}.kama`,
   xfail `tests/xfail/{isr_*,section_nonstring,interrupt_on_static}.kama`.
5. **Fallible `allocate -> Optional<UnsafePtr>`** + `try new` + the checkable `@noheap`/`--no-heap` subset ✅
   **SHIPPED** — completes the no-heap story (shared with the embedded milestone in ROADMAP_DETAIL §5). Also serves
   game-engine frame allocators / real-time audio, not only MCU.
6. **Inline asm / intrinsics** — ✅ **SHIPPED (step 6a, `4811d23`).** `asm("...")` inside an `unsafe fn`
   lowers to `__asm__ __volatile__("..." : : : "memory")` (always volatile + a full compiler memory
   barrier); fixtures `tests/asm_nop.kama` + `tests/support/embedded_asm.kama` (transpile-grep in
   `tools/check-embedded.sh`). See [KEYWORDS.md](KEYWORDS.md) (`asm`) and [SPEC.md](SPEC.md).
   **Toolchain packaging — turnkey Cortex-M path ✅ shipped + QEMU-proven** (opt-in `kama-mcu` cross image +
   bundled startup/linker script + `mcu/build.sh`/`run-qemu.sh` + `tools/check-mcu.sh` booting firmware on
   emulated silicon; see [mcu.md](mcu.md)). **Soft-float + fixed-point ✅ shipped** — `std::num::Fixed<B> comptime(int32 F)` (library)
   + a no-FPU Cortex-M0 board preset (`--board microbit`) QEMU-proven by `tools/check-softfloat.sh` (see the
   Tier-2 row). **Remaining (all build/library, not language):** more board presets (STM32/Pico) + vendor-HAL
   glue + a real-hardware flash pass, then the AVR follow-on. (`alignof` already ships — see the Tier-2 row.) Related, shipped separately: decl-level conditional compilation via
   **`@compileFor(FLAG)`** (`68b41ff`) — the platform/build-mode gate for per-target driver code (see
   [SPEC.md](SPEC.md#conditional-compilation--compileforflag-)).

**Bottom line:** the *systems core* (types, FFI, RAII, no-null, no-heap value subset, pluggable allocator) is
already here. Bare-metal readiness is a focused set of **language-surface** additions — statics, `hardware`,
ISRs, a freestanding target, fallible alloc — plus toolchain packaging. None require rethinking the model; they
extend it into the freestanding world the design already anticipates (ROADMAP_DETAIL §5).
