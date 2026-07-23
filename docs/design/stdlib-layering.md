# Standard-library layering — triage & principle

*Triage 2026-07-23. Prompted by "should `std::concurrent`/`std::fmt` live in the prelude?" — a question
that applies to every module. Verdict: the layering is already principled; a handful of low-priority
follow-ups are recorded below. This doc is the durable classification, not a work plan.*

## The principle (already followed)

> **Contracts + syntax + intrinsics live in the prelude (always-on, ships under `--no-std`).
> Backends / concrete implementations are opt-in `lib/std/*` modules (pay-for-what-you-use).**

The features that *look* like prelude candidates already have their load-bearing half in the prelude; only
the optional backend/sugar is an import:

| Feature | Primitive half — prelude / `kama_runtime.h` (always-on) | Opt-in `lib` half |
|---|---|---|
| Formatting | `Format` contract + `Formatter` (string interpolation `"{x}"` lowers into these) + `kama_fmt_*` number→string | `std::fmt` — `intStr`/`uintStr` helpers + `html`/`sql`/`stripIndent` tags |
| Serialization | `Serialize`/`Deserialize`/`Serializer`/`Deserializer` contracts + `@generate(Serialize/…)` synthesis | `std::serialization::{binary,json}` — the byte backends |
| Memory | `Shared`/`Owned`/`Weak` + `HeapOwner`/`Deref`/`Copyable` (drive `new`/`give`/`ref`) | *(none — fully prelude)* |
| Concurrency | `spawn`/`scope` keywords + sendability gate + `Atomic` borrow-exemption | `std::concurrent` — `Isolate`/`Channel`/`Atomic` over the C seams |

So **`fmt` is not mis-placed**: the part that string interpolation compiles into is already always-on (works
under `--no-std`); `lib/std/fmt` is genuinely-optional sugar. Same shape for serialization.

## Two tiers of "built-in"

- **Prelude (embedded in the compiler binary via `embed_prelude.sh`, always in scope, ships under `--no-std`):**
  `prelude/global.kama` (`Optional`/`Result`/`Unit`/`string` + ~24 contracts: `Deref`, `HeapOwner`,
  `Copyable`, `Iterator`, `Error`, `Serialize`, `Format`, …) and `prelude/std/memory/` (the `Shared`/`Owned`/
  `Weak` triad; `std::memory` is `provided`-satisfied, so `import std::memory` is a no-op).
- **On-disk `lib/std/*` (explicit `import`, absent under `--no-std`):** everything else.

## Compiler-coupling classification (all 13 modules)

| Tier | Modules | What the compiler knows |
|---|---|---|
| **Primitive** (syntax/synthesis, embedded) | prelude globals; `std::memory` triad | lowers syntax into them (`new`/`give`/`"{x}"`/`@generate`); template-keys `_sharedTmpl`/`_ownedTmpl`/`_weakTmpl` captured |
| **Library + privileged hooks** | `std::concurrent`, `std::serialization` | pure kama, but named: `_channelTmpl`/`_senderTmpl`/`_receiverTmpl`/`_atomicTmpl` + sendability gate; `@generate` targets `Serialize`/`Deserialize`/`Format` |
| **Pure library** (zero compiler knowledge) | `collections`, `fmt`, `io`, `net`, `fs`, `time`, `math`, `num`, `app` | ordinary generics; no gating, no name checks |

## Import-cost gates (why the non-prelude modules stay opt-in)

Pay-for-what-you-use: a module's link/runtime cost is triggered only when its seam header is actually
externed (see `kama.driver.cpp`): `kama_isolate.h`/`kama_channel.h` → native `-lpthread` / wasm
`-pthread -sPROXY_TO_PTHREAD`; `<math.h>` → `-lm`; `kama_gpu.h` → `-lwgpu_native` + GLFW + platform frameworks;
`kama_net_web.h` → wasm `--js-library`; Windows sockets → `-lws2_32`; `kama_app.h` → wasm `-sEXIT_RUNTIME=1`.
Folding any of these into the always-on prelude would tax every program (and `--no-std`/MCU) — hence the split.

## Placement verdict

Correct for every module. Prelude = `global` + `memory` (foundational, ~100% use, compiler-coupled);
everything else opt-in so its cost is pay-for-use. **`Atomic` is the one exception worth noting** — it needs
no pthread, yet rides in the pthread-heavy `std::concurrent`, so lock-free-without-threads isn't reachable
(see follow-ups).

## Open follow-ups (all LOW priority — none block anything today)

1. **`Atomic` granularity (MCU).** Split `Atomic` to a `std::concurrent::atomic` leaf, or MCU-promote it as an
   always-available seam, so a freestanding/multicore-MCU program gets lock-free cells without the isolate
   runtime. Relevant only when the MCU multicore track starts; atomics work fine via `std::concurrent` now.
   Ties to §5 (embedded) / §6 (concurrency).
2. **`std::gpu` has no kama module.** The `kama_gpu.h`/`.c` seam exists but there's no `std::gpu` surface —
   programs `extern` the WebGPU C API raw. An idiomatic wrapper is pure library work on the engine track (§8);
   no language blocker.
3. **`Array`/`List` vs `DynamicArray` naming.** Intrinsic `Array`/`List` and the library `DynamicArray`
   coexist (imported 27× vs 114×). GOALS favors *one way*; confirm whether both should survive before a real
   1.0 tag. Cheap to investigate, zero functional impact. Ties to the §5 collections revisit.

**Not doing (decided 2026-07-23):** a `std::prelude`-style convenience re-export of the common containers.
`DynamicArray` is imported 114× and the verbosity is real, but auto-importing them would blur the
prelude=foundational line and tax `--no-std`; explicit per-symbol imports stay. Revisit if it bites.
