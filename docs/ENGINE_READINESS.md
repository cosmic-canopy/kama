# kama engine-readiness gap analysis

kama is general-purpose, but the reason to reach for it is a **fast, deterministic runtime with no GC**
(RAII, transpiles to portable C → native + WASM). This document assesses what the language still needs to
write a **modern game engine**, and recommends a sequence. Status: ✅ have · 🟡 partial · ❌ missing.

## What kama already has (the foundation)

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
  self-type, turbofish `f::<T>()`) — plus **const generics** and the **`Fixed<T,N>` safe fixed array**
  (a bounds-checked value array; `Mat4 = Fixed<float32,16>` / `Fixed<Vec4,4>` is available today).
- **Place-indexing**: an indexed element is an lvalue, so `m[i][j] = v`, `arr[i].x = v`, `a[i] += x`,
  `ref a[i]`, and `foreach (ref T e in c)` all work — and a **user type can define its own
  `operator[]`** (and `fn ref T at(i)`), so a `Vec`/matrix can be written *in* kama.
- **Stdlib-prerequisite builtins**: `sizeof(T)` (compile-time, monomorphizes), `panic`/`assert` (a
  clean abort trap), so a heap collection can be written in kama (proven by `tests/opindex_vec`).
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
language features: a math layer, more C bindings, slices, allocators, and threading (already *designed* — the
shared-nothing model in ROADMAP.md).

---

## Tier 0 — Hard blockers (can't build a real engine without these)

| Feature | Status | Why an engine needs it | Effort |
|---|---|---|---|
| **FFI depth**: `extern` structs, opaque handles, function-pointer params, pass structs by value/ptr to C, map/include C headers | ✅ — `extern` structs, `extern "<header>"` includes, opaque `Ptr<T>`, `addr(of:)` out-params, and C-callback fn-pointers all shipped; residual is richer struct-by-value ergonomics | WebGPU, SDL/GLFW, platform, audio are **C APIs**. The keystone — landed. | done |
| **Function pointers / delegates** | ✅ free `fnptr` + bound `BindableFunctionPtr` (captures a receiver); with a `this`/userdata pointer these cover callbacks (input, window, GPU completion), ECS system fns, and job functions | Pervasive — and satisfied. Inline *capturing closures* are a separate ergonomic nice-to-have (Tier 3; M+ under the ownership/RAII/move model), not a blocker. | done |
| **Module system**: multi-file builds, `using`/imports, namespaces actually linked | ✅ — multi-file builds, `namespace`/`using`/aliases, private-by-default, shared header + per-module `.c` | An engine is hundreds of files. | done |
| **Math layer**: vector/matrix/quaternion types + SIMD | 🟡 — every *language* primitive is in place (operators + static methods + `Fixed<T,N>` storage + `m[i][j]` place-indexing), so `Vec2/3/4`/`Mat4`/`Quat` are writable today as `type value`s; the **library types themselves** aren't written yet, and SIMD is later | Transforms, physics, culling, shading — the numeric core. A pure library layer now. | S (types) → L (SIMD) |
| **Fixed-size value arrays** (`float[4]`, matrix storage) | ✅ — **`Fixed<T,N>`** shipped: a bounds-checked value array (`struct{T v[N];}`, monomorphized per (T,N), value-copy, no pointer decay), with array literals `[a,b,c]`/`[v;N]`, `.length()`, `foreach`, nested `Fixed<Fixed<..>,..>`, and place-indexing. `Mat4 = Fixed<float32,16>` or `Fixed<Vec4,4>`. Const OOB is a compile error; dynamic OOB traps. | Compact stack numeric storage, SIMD lanes, ergonomic matrices. | done |

## Tier 1 — Core ergonomics (painful without; needed soon after Tier 0)

| Feature | Status | Why | Effort |
|---|---|---|---|
| **Operator overloading** | ✅ — full set, arity-picked forms, type-based dispatch, chaining, compound assignment, operators-in-contracts for generic math | `a + b` for the math types above; without it the math layer is unusable. | done |
| **General user generics** (`type value Foo<T>`, generic fns, nested `>>`, contract bounds `<K: A + B>`, `This`, turbofish) | ✅ — monomorphized, zero-cost, ASan-clean | Every future library type. | done |
| **Error model**: `Result`/`Optional` | ✅ — `Optional<T>`/`Result<T,E>` prelude tagged unions, consumed by exhaustive `match` (no exceptions, no `null`) | File/asset/GPU/shader-compile failures need a first-class, non-exception path (fits no-GC/deterministic). | done |
| **Tagged unions / sum types + pattern matching** | ✅ — `enum` payloads/generic enums + value-producing exhaustive `match` (plain enums too) | Events, messages, render commands, animation/state machines, asset variants. | done |
| **`Map<K,V>` / hash maps** | 🟡 generics + bounds *machinery* ready, but the standard vocabulary is not: no prelude `Hashable`/`Comparable`/`Ordering` contracts, no hashing convention (only `Equatable`, and only in a test). Needs that small stdlib-contract + hashing foundation, then the container. | Entity/resource/asset registries, caches, string→handle lookup. | M (contracts + hashing + container) |
| **Slices / spans** (non-owning views over `Array`/`List`/buffers) | ❌ | Iterate a subrange, pass a buffer to a system or a GPU upload without copying or transferring ownership. | M |
| **Allocator control**: arenas / pools / frame & stack allocators | ❌ (GOALS #3 wants these as library types) | Deterministic per-frame perf, zero mid-frame `malloc`, bump-reset allocators. Needs a placement-construct hook + the runtime unsafe core. | M–L |

## Tier 2 — Systems & scale

| Feature | Status | Why | Effort |
|---|---|---|---|
| **Threading / atomics / memory model** | ❌ built · ✅ *designed* — the **shared-nothing model** (isolates + ownership-transferring channels + `Atomic<T>`, mapping 1:1 onto WASM Web Workers) is specified in [ROADMAP.md](ROADMAP.md) (§ Concurrency). That model **is** the engine's job-system / parallel-ECS substrate; only the runtime remains. | Job system, parallel ECS, async asset streaming. | XL (build) |
| **Bit/byte manipulation**: reinterpret/bitcast, byte buffers, endianness | 🟡 partial (bitwise ops only) | (De)serialization, networking, binary asset/scene formats. | M |
| **String formatting / interpolation + I/O** (file, stdout, logging) | ❌ (the `string` type + `concat`/compare/`length` only) | Logging, config, text assets, tooling. | M |
| **comptime / const-eval** | 🟡 partial — `const` values, **const generics** (`Fixed<T,N>`, integer type params), and **`sizeof(T)`** (a monomorphizing compile-time builtin) are shipped; general compile-time *evaluation* (arithmetic on const params, lookup-table generation) is not. `alignof(T)` is the obvious missing sibling for allocators. | Lookup tables, shader/permutation specialization, asserts. | M |
| **Reflection / metadata** | ❌ built · **mostly codegen, not a language gap** — the compiler's `ClassInfo` already holds every field's name/type/order + sum-type variants, so a serializer generator can walk it; the only *possible* new language surface is opt-in **attributes** on types/fields (needed only for per-field control like rename/skip). | Auto-serialization, editor property panels, ECS introspection. | M (codegen) + S (opt-in attrs, if field-level) |

## Tier 3 — Ecosystem & polish

Package manager / multi-module build & deps (L); **LSP** + formatter + debugger polish (L); variadics (S);
`defer`/scope-guards (S — RAII already covers most); enum methods / flags (S); inline **capturing closures**
(M+); a **`foreach` iteration protocol** for user types (S–M — today `foreach` is built-in-collections only;
matters once `Map` / custom containers land).

## The actual goal

| Goal | Status | Notes |
|---|---|---|
| **kama-level WebGPU bindings** → first triangle → the engine spine | ❌ | The Tier-0 FFI keystone (extern structs + function pointers) is in place, **and a `--webgpu` build flag already links Emscripten's `emdawnwebgpu` port** (`kama.driver.cpp`, `README.md`) — so the toolchain path is wired; only the kama-side `extern` bindings against `webgpu.h` + a thin idiomatic wrapper remain. | L (binding layer) |

---

## Recommended sequence (fastest path to a rendering engine)

The language is complete; the path is now entirely library + platform work.

1. **Math types** (`Vec2/3/4`, `Mat4`, quaternion) as `type value`s (public fields) with operators + `::`
   static methods — the numeric core (Tier-0 math). **All the language machinery it needs is shipped** —
   operators, static methods, and now `Fixed<T,N>` for matrix storage + `m[i][j]` place-indexing — so
   this is a pure library layer (a natural first opt-in **stdlib module**, per ROADMAP).
2. **WebGPU bindings** (FFI is ready) → **a triangle on screen** → the engine spine + a real demo.
3. Iterate as the engine grows: `Map`/slices/arenas as library types, then bit/byte + I/O, then threading.

Each item is independently shippable (a tagged release), matching the milestone cadence so far. The
fast/deterministic-runtime thesis is already true today — the remaining gaps are about reach (hardware/OS)
and scale (large codebase), not the runtime model or the language.
