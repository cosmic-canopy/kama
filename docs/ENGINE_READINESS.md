# cstar engine-readiness gap analysis

cstar is general-purpose, but the reason to reach for it is a **fast, deterministic runtime with no GC**
(RAII, transpiles to portable C → native + WASM). This document assesses what the language still needs to
write a **modern game engine**, and recommends a sequence. Status: ✅ have · 🟡 partial · ❌ missing.

## What cstar already has (the foundation)
Functions + named params + `ref`/`out`; full control flow (`if`/`while`/`do`/`for`/`switch`/`foreach`/
`break`/`continue`) + the full operator set + `cast<T>`; classes with **RAII destructors**, single
inheritance + **virtual dispatch**, **interfaces**, **enums**; generic **collections** (`Array<T>`/
`List<T>`/owned `String`, bounds-checked, monomorphized); the full **smart-pointer family** (`Owned`/
`Shared`/`Weak`); `extern` C functions (the FFI seam); **native + WASM**, debug/release, `#line` source
debugging. No raw pointers / no `unsafe` — heap is reached only through safe abstractions.

That's a solid systems-language core. The gaps below are mostly about **talking to the outside world**
(the GPU/OS are C APIs), **the math layer**, and **scaling to a real codebase**.

---

## Tier 0 — Hard blockers (can't build a real engine without these)

| Feature | Status | Why an engine needs it | Effort |
|---|---|---|---|
| **FFI depth**: `extern` structs, opaque handles, function-pointer params, pass structs by value/ptr to C, map/include C headers | 🟡 partial (only `extern` *functions* today) | WebGPU, SDL/GLFW, platform, audio are **C APIs**. Without struct/handle/callback interop you can't touch the GPU. **The keystone.** | L |
| **Function pointers / delegates / closures** | ❌ | Callbacks (input, window events, GPU completion), ECS system fns, job functions. Pervasive. | M–L |
| **Module system**: multi-file builds, `using`/imports, namespaces actually linked | 🟡 partial (syntax parses; compiler builds **one file**) | An engine is hundreds of files. Today everything must live in a single `.cstar`. | L |
| **Math layer**: vector/matrix/quaternion types, fixed-size value arrays (`float[4]`, `Vec3 pos`), SIMD | ❌ (no array types, no math types, no SIMD) | Transforms, physics, culling, shading — the numeric core. Start as structs + operator overload; SIMD after. | M (types) → L (SIMD) |

## Tier 1 — Core ergonomics (painful without; needed soon after Tier 0)

| Feature | Status | Why | Effort |
|---|---|---|---|
| **Operator overloading** | 🟡 parses, **deferred** in the emitter | `a + b` for the math types above; without it the math layer is unusable. | M |
| **`Map<K,V>` / hash maps** + **general user generics** (`class Foo<T>`) | 🟡 partial (built-in generics only) | Entity/resource/asset registries, caches, string→handle lookup. | M (Map) + L (general monomorphization) |
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
Package manager / multi-module build & deps (L); **LSP** + formatter + debugger polish (L); const-correctness
on params/methods (S–M); variadics (S); `defer`/scope-guards (S — RAII already covers most); enum methods /
flags (S).

## The actual goal
| **cstar-level WebGPU bindings** → first triangle → the engine spine | ❌ | Depends entirely on **Tier-0 FFI** (extern structs + function pointers). Once FFI lands, this is a binding layer + a thin idiomatic wrapper. | L (after FFI) |

---

## Recommended sequence (fastest path to a rendering engine)
1. **FFI depth** (extern structs + function pointers + handles) — the keystone; unlocks every C API.
2. **Module system** — so the engine can be more than one file.
3. **Math types + operator overloading + fixed value arrays** — the numeric core.
4. **WebGPU bindings** (now possible) → **a triangle on screen** → the engine spine + a real demo.
5. Iterate as the engine grows: `Map`/slices/arenas/tagged unions/error model, then threading.

This gets the classic "triangle rendering" milestone by step 4, then grows feature-by-feature against a
real engine. Each item is independently shippable (a tagged release), matching the milestone cadence so
far. The fast/deterministic-runtime thesis is already true today — these gaps are about reach (hardware/OS)
and scale (large codebase), not about the runtime model.
