# cstar engine-readiness gap analysis

cstar is general-purpose, but the reason to reach for it is a **fast, deterministic runtime with no GC**
(RAII, transpiles to portable C → native + WASM). This document assesses what the language still needs to
write a **modern game engine**, and recommends a sequence. Status: ✅ have · 🟡 partial · ❌ missing.

## What cstar already has (the foundation)

The entire language feature set is complete. In place today:

- **Functions + named params + `ref`/`out`**; full control flow (`if`/`while`/`do`/`for`/`foreach`/`break`/
  `continue`) — enum branching via **`match`** — the full operator set + `cast<T>`.
- The **ownership type model** (`type value`/`type resource`/`type contract`) with **RAII destructors**,
  single inheritance + **virtual dispatch** (`type virtual`/`abstract resource`), **contracts**, and
  **enums** (plain + tagged unions).
- The full **smart-pointer family** (`Owned`/`Shared`/`Weak`) with the **value/ownership model** (`give`/
  `copy` hand-off, by-value transfer, no null in the safe surface, borrow-vs-storage rule).
- Generic **collections** (`Array<T>`/`List<T>`/`string`, bounds-checked, monomorphized) and **user
  generics** (monomorphized types + functions, contract bounds `<K: Hashable + Comparable>`, the `This`
  self-type, turbofish `f::<T>()`).
- **Operator overloading** + **full static methods** (`Type::method()`) — including type-based dispatch, so
  ergonomic math types (`mat*vec` + `mat*mat`) are writable today.
- **Tagged unions + pattern matching** (`match`, exhaustive) and the **error model** (`Optional<T>`/
  `Result<T,E>` — no exceptions, no `null`).
- **`const`-correctness**; **enforced access control** (private-by-default; `value`/`resource`/`contract` +
  `virtual`/`abstract`/`final` resource kinds; `friend`); **multi-file builds + private-by-default
  namespaces**.
- **C FFI** (`extern` functions **and** structs, `extern "<header>"` includes, opaque `Ptr<T>`, `addr(of:)`,
  `--link`) with raw pointers confined to an explicit **`unsafe { }`** block at the FFI boundary; **function
  pointers** (`fnptr` free + `BindableFunctionPtr` bound).
- **Native + WASM**, debug/release, `#line` source debugging. The *safe* surface stays pointer-free; heap is
  reached only through safe abstractions.

That is a complete systems-language core. The remaining engine work is **library and platform reach**, not
language features: a math layer, more C bindings, slices, allocators, and eventually threading.

---

## Tier 0 — Hard blockers (can't build a real engine without these)

| Feature | Status | Why an engine needs it | Effort |
|---|---|---|---|
| **FFI depth**: `extern` structs, opaque handles, function-pointer params, pass structs by value/ptr to C, map/include C headers | ✅ — `extern` structs, `extern "<header>"` includes, opaque `Ptr<T>`, `addr(of:)` out-params, and C-callback fn-pointers all shipped; residual is richer struct-by-value ergonomics | WebGPU, SDL/GLFW, platform, audio are **C APIs**. The keystone — landed. | done |
| **Function pointers / delegates / closures** | 🟡 free `fnptr` ✅ + bound `BindableFunctionPtr` ✅; inline closures ❌ | Callbacks (input, window events, GPU completion), ECS system fns, job functions. Pervasive. | S (closures) |
| **Module system**: multi-file builds, `using`/imports, namespaces actually linked | ✅ — multi-file builds, `namespace`/`using`/aliases, private-by-default, shared header + per-module `.c` | An engine is hundreds of files. | done |
| **Math layer**: vector/matrix/quaternion types, fixed-size value arrays (`float[4]`, `Vec3 pos`), SIMD | 🟡 — operator overloading + static methods are done (math types are now *writable* as `type value`s); no built-in math types or SIMD yet | Transforms, physics, culling, shading — the numeric core. Write as `value` structs + operators; SIMD after. | M (types) → L (SIMD) |

## Tier 1 — Core ergonomics (painful without; needed soon after Tier 0)

| Feature | Status | Why | Effort |
|---|---|---|---|
| **Operator overloading** | ✅ — full set, arity-picked forms, type-based dispatch, chaining, compound assignment, operators-in-contracts for generic math | `a + b` for the math types above; without it the math layer is unusable. | done |
| **General user generics** (`type value Foo<T>`, generic fns, nested `>>`, contract bounds `<K: A + B>`, `This`, turbofish) | ✅ — monomorphized, zero-cost, ASan-clean | Every future library type. | done |
| **Error model**: `Result`/`Optional` | ✅ — `Optional<T>`/`Result<T,E>` prelude tagged unions, consumed by exhaustive `match` (no exceptions, no `null`) | File/asset/GPU/shader-compile failures need a first-class, non-exception path (fits no-GC/deterministic). | done |
| **Tagged unions / sum types + pattern matching** | ✅ — `enum` payloads/generic enums + value-producing exhaustive `match` (plain enums too) | Events, messages, render commands, animation/state machines, asset variants. | done |
| **`Map<K,V>` / hash maps** | 🟡 now *buildable* as a library type on the generics + bounds infrastructure (not yet written) | Entity/resource/asset registries, caches, string→handle lookup. | M (library) |
| **Slices / spans** (non-owning views over `Array`/`List`/buffers) | ❌ | Iterate a subrange, pass a buffer to a system or a GPU upload without copying or transferring ownership. | M |
| **Allocator control**: arenas / pools / frame & stack allocators | ❌ (GOALS #3 wants these as library types) | Deterministic per-frame perf, zero mid-frame `malloc`, bump-reset allocators. Needs a placement-construct hook + the runtime unsafe core. | M–L |

## Tier 2 — Systems & scale

| Feature | Status | Why | Effort |
|---|---|---|---|
| **Threading / atomics / memory model** | ❌ | Job system, parallel ECS, async asset streaming. (WASM threads = SharedArrayBuffer caveats.) | XL |
| **Bit/byte manipulation**: reinterpret/bitcast, byte buffers, endianness | 🟡 partial (bitwise ops only) | (De)serialization, networking, binary asset/scene formats. | M |
| **String formatting / interpolation + I/O** (file, stdout, logging) | ❌ (the `string` type + `concat`/compare/`length` only) | Logging, config, text assets, tooling. | M |
| **comptime / const-eval** | 🟡 partial (`const` values; no compile-time eval) | Lookup tables, shader/permutation specialization, asserts. | L |
| **Reflection / metadata** | ❌ | Auto-serialization, editor property panels, ECS introspection. | L–XL |

## Tier 3 — Ecosystem & polish

Package manager / multi-module build & deps (L); **LSP** + formatter + debugger polish (L); variadics (S);
`defer`/scope-guards (S — RAII already covers most); enum methods / flags (S).

## The actual goal

| Goal | Status | Notes |
|---|---|---|
| **cstar-level WebGPU bindings** → first triangle → the engine spine | ❌ | The Tier-0 FFI keystone (extern structs + function pointers) is in place, so this is now a binding layer + a thin idiomatic wrapper. | L (binding layer) |

---

## Recommended sequence (fastest path to a rendering engine)

The language is complete; the path is now entirely library + platform work.

1. **Math types** (`Vec2/3/4`, `Mat4`, quaternion) as `type value`s (public fields) with operators + `::`
   static methods — the numeric core (Tier-0 math). All the language machinery it needs is in place.
2. **WebGPU bindings** (FFI is ready) → **a triangle on screen** → the engine spine + a real demo.
3. Iterate as the engine grows: `Map`/slices/arenas as library types, then bit/byte + I/O, then threading.

Each item is independently shippable (a tagged release), matching the milestone cadence so far. The
fast/deterministic-runtime thesis is already true today — the remaining gaps are about reach (hardware/OS)
and scale (large codebase), not the runtime model or the language.
