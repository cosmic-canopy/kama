# cstar engine-readiness gap analysis

cstar is general-purpose, but the reason to reach for it is a **fast, deterministic runtime with no GC**
(RAII, transpiles to portable C → native + WASM). This document assesses what the language still needs to
write a **modern game engine**, and recommends a sequence. Status: ✅ have · 🟡 partial · ❌ missing.

## What cstar already has (the foundation)
Functions + named params + `ref`/`out`; full control flow (`if`/`while`/`do`/`for`/`switch`/`foreach`/
`break`/`continue`) + the full operator set + `cast<T>`; the **ownership type model** (`type value`/`type
resource`/`type contract`) with **RAII destructors**, single inheritance + **virtual dispatch** (`type
virtual`/`abstract resource`), **contracts**, **enums**; generic **collections** (`Array<T>`/
`List<T>`/owned `String`, bounds-checked, monomorphized); the full **smart-pointer family** (`Owned`/
`Shared`/`Weak`) with the **value/ownership model** (`give`/`copy` hand-off, by-value transfer, no null in
the safe surface, borrow-vs-storage rule); **`const`-correctness**; **enforced access control**
(private-by-default; `value`/`resource`/`contract` + `virtual`/`abstract`/`final` resource kinds; `friend`); **multi-file builds +
private-by-default namespaces**; **C FFI** (`extern` functions **and structs**, `extern "<header>"`
includes, opaque `Ptr<T>`, `addr(of:)`, `--link`) with raw pointers confined to an explicit **`unsafe { }`**
block at the FFI boundary; **function pointers** (`fnptr` free + `BindableFunctionPtr` bound); **native +
WASM**, debug/release, `#line` source debugging. The *safe* surface stays pointer-free; heap is reached only
through safe abstractions.

That's a solid systems-language core — and the **module system**, much of the **FFI keystone**, and now
**user generics** (M27 — monomorphized, with contract bounds + `This`) have landed. The remaining gaps are
mostly **the math layer** (operator overloading + vector/matrix types) and **sum types + pattern matching**
(M28), then **reach** (more C APIs, threading) and **scale**.

---

## Tier 0 — Hard blockers (can't build a real engine without these)

| Feature | Status | Why an engine needs it | Effort |
|---|---|---|---|
| **FFI depth**: `extern` structs, opaque handles, function-pointer params, pass structs by value/ptr to C, map/include C headers | ✅ mostly (M15–M18) — `extern` structs, `extern "<header>"` includes, opaque `Ptr<T>`, `addr(of:)` out-params, and C-callback fn-pointers all shipped; residual is richer struct-by-value ergonomics | WebGPU, SDL/GLFW, platform, audio are **C APIs**. **The keystone — largely landed.** | done |
| **Function pointers / delegates / closures** | 🟡 free `fnptr` ✅ + bound `BindableFunctionPtr` ✅ (M21–M22); inline closures ❌ | Callbacks (input, window events, GPU completion), ECS system fns, job functions. Pervasive. | S (closures) |
| **Module system**: multi-file builds, `using`/imports, namespaces actually linked | ✅ (M13–M14) — multi-file builds, `namespace`/`using`/aliases, private-by-default, shared header + per-module `.c` | An engine is hundreds of files. | done |
| **Math layer**: vector/matrix/quaternion types, fixed-size value arrays (`float[4]`, `Vec3 pos`), SIMD | ❌ (no array types, no math types, no SIMD) | Transforms, physics, culling, shading — the numeric core. Start as structs + operator overload; SIMD after. | M (types) → L (SIMD) |

## Tier 1 — Core ergonomics (painful without; needed soon after Tier 0)

| Feature | Status | Why | Effort |
|---|---|---|---|
| **Operator overloading** | 🟡 parses, **deferred** in the emitter | `a + b` for the math types above; without it the math layer is unusable. | M |
| **General user generics** (`type value Foo<T>`, generic fns, nested `>>`, contract bounds `<K: I + J>`, `This`) | ✅ **done (M27, v0.1.54)** — monomorphized, zero-cost, ASan-clean | Every future library type. | done |
| **`Map<K,V>` / hash maps** | 🟡 now *buildable* as a library type on M27 (not yet written) | Entity/resource/asset registries, caches, string→handle lookup. | M (library) |
| **Slices / spans** (non-owning views over `Array`/`List`/buffers) | ❌ | Iterate a subrange, pass a buffer to a system or a GPU upload without copying or transferring ownership. | M |
| **Tagged unions / sum types + pattern matching** | ❌ | Events, messages, render commands, animation/state machines, asset variants. | L |
| **Allocator control**: arenas / pools / frame & stack allocators | ❌ (GOALS #3 wants these as library types) | Deterministic per-frame perf, zero mid-frame `malloc`, bump-reset allocators. Needs a placement-construct hook + the runtime unsafe core. | M–L |
| **Error model**: `Result`/`Option` (or a chosen scheme) | ❌ | File/asset/GPU/shader-compile failures need a first-class, non-exception path (fits no-GC/deterministic). | M |

## Tier 2 — Systems & scale

| Feature | Status | Why | Effort |
|---|---|---|---|
| **Threading / atomics / memory model** | ❌ | Job system, parallel ECS, async asset streaming. (WASM threads = SharedArrayBuffer caveats.) | XL |
| **Bit/byte manipulation**: reinterpret/bitcast, byte buffers, endianness | 🟡 partial (bitwise ops only) | (De)serialization, networking, binary asset/scene formats. | M |
| **String formatting / interpolation + I/O** (file, stdout, logging) | ❌ (String type only) | Logging, config, text assets, tooling. | M |
| **comptime / const-eval** | 🟡 partial (`const` values; no compile-time eval) | Lookup tables, shader/permutation specialization, asserts. | L |
| **Reflection / metadata** | ❌ | Auto-serialization, editor property panels, ECS introspection. | L–XL |

## Tier 3 — Ecosystem & polish
Package manager / multi-module build & deps (L); **LSP** + formatter + debugger polish (L); variadics (S);
`defer`/scope-guards (S — RAII already covers most); enum methods / flags (S). *(Const-correctness on
params/methods — once a Tier-3 item — shipped in M24.)*

## The actual goal
| **cstar-level WebGPU bindings** → first triangle → the engine spine | ❌ | Depends entirely on **Tier-0 FFI** (extern structs + function pointers). Once FFI lands, this is a binding layer + a thin idiomatic wrapper. | L (after FFI) |

---

## Recommended sequence (fastest path to a rendering engine)
1. ~~**FFI depth** (extern structs + function pointers + handles)~~ — ✅ landed (M15–M18).
2. ~~**Module system**~~ — ✅ landed (M13–M14).
3. **Finish the type system** (1.0): ~~user generics (M27)~~ ✅ done → sum types + `match`/`Optional`
   (M28, next) → **operator overloading + full static methods** (M29) — the last unlocks ergonomic math types.
4. **Math types** (`Vec2/3/4`, `Mat4`) as `type value`s (public fields) with operators + `::` static methods — the numeric core
   (Tier-0 math, unblocked by step 3's operators).
5. **WebGPU bindings** (FFI is ready) → **a triangle on screen** → the engine spine + a real demo.
6. Iterate as the engine grows: `Map`/slices/arenas/tagged unions/error model, then threading.

The classic "triangle rendering" milestone now depends on finishing the 1.0 type system (for math
ergonomics), not on FFI or the module system — those keystones have landed. Each item is independently
shippable (a tagged release), matching the milestone cadence so far. The fast/deterministic-runtime thesis
is already true today — the remaining gaps are about reach (hardware/OS) and scale (large codebase), not the
runtime model.
