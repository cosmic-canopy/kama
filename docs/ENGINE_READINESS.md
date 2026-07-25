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
- Generic **collections** (`FixedArray<T>`/`DynamicArray<T>`/`string`, bounds-checked, monomorphized) and **user
  generics** (monomorphized types + functions, contract bounds `<K: Hashable + Comparable>`, the `This`
  self-type, turbofish `f::<T>()`) — plus **const generics** and the **`InlineArray<T,N>` safe fixed array**
  (a bounds-checked value array; `Mat4 = InlineArray<float32,16>` / `InlineArray<Vec4,4>` is available today).
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

That is a complete systems-language core. The math layer (`std::math`), native file/socket I/O
(`std::fs`/`std::io`/`std::net`), the **`Map<K,V>`/`Set<K>`** hash containers, opt-in **serialization**
(`@generate`, intrinsic + json backend), and the **`expose` + `kama build --shared`** kama→host C-ABI
boundary are all **shipped**; the remaining engine work is further **library and platform reach**, not
language features: WebGPU bindings, more C bindings, and allocators. **Threading is now shipped too** — the
shared-nothing model (isolates + ownership-transferring channels + structured-concurrency `scope` +
`Atomic<T>` + disjoint-slice `parallel_for`), native **and** wasm, so the job-system / parallel-ECS
*substrate* exists and only the higher-level scheduler *library* remains (ROADMAP §6). (Slices/spans
**shipped** — the `type view` kind + `View<T>`.)

---

## Tier 0 — Hard blockers (can't build a real engine without these)

| Feature | Status | Why an engine needs it | Effort |
|---|---|---|---|
| **FFI depth**: `extern` structs, opaque handles, function-pointer params, pass structs by value/ptr to C, map/include C headers | ✅ — `extern` structs, `extern "<header>"` includes, opaque `Ptr<T>`, `addr(of:)` out-params, and C-callback fn-pointers all shipped; residual is richer struct-by-value ergonomics | WebGPU, SDL/GLFW, platform, audio are **C APIs**. The keystone — landed. | done |
| **Function pointers / delegates** | ✅ free `fnptr` + bound `BindableFunctionPtr` (captures a receiver); with a `this`/userdata pointer these cover callbacks (input, window, GPU completion), ECS system fns, and job functions | Pervasive — and satisfied. Inline *capturing closures* are a separate ergonomic nice-to-have (Tier 3; M+ under the ownership/RAII/move model), not a blocker. | done |
| **Module system**: multi-file builds, `using`/imports, namespaces actually linked | ✅ — multi-file builds, `namespace`/`using`/aliases, private-by-default, shared header + per-module `.c` | An engine is hundreds of files. | done |
| **Math layer**: vector/matrix/quaternion types + SIMD | ✅ **DONE** — **`std::math` shipped**: concrete float32 `Vec2/3/4`, `Mat2/3/4`, `Quat` + scalar helpers (`import std::math::{…}`). Column-major, WebGPU 0..1-depth `perspective`/`ortho`/`lookAt`, Mat4 `inverse`, full `Quat` (Hamilton/`rotate`/slerp/`toMat4`), method chaining. Native + ASan + wasm, exact-value fixtures. **SIMD ✅**: the SIMD-ready layout auto-vectorizes at `-O3`; `--release` unity-TU builds inline the ops so hot math runs **at C parity** (no explicit vector types/intrinsics needed — see SPEC *Math*). | Transforms, physics, culling, shading — the numeric core. | done |
| **InlineArray-size value arrays** (`float[4]`, matrix storage) | ✅ — **`InlineArray<T,N>`** shipped: a bounds-checked value array (`struct{T v[N];}`, monomorphized per (T,N), value-copy, no pointer decay), with array literals `[a,b,c]`/`[v;N]`, `.length()`, `foreach`, nested `InlineArray<InlineArray<..>,..>`, and place-indexing. `Mat4 = InlineArray<float32,16>` or `InlineArray<Vec4,4>`. Const OOB is a compile error; dynamic OOB traps. | Compact stack numeric storage, SIMD lanes, ergonomic matrices. | done |

## Tier 1 — Core ergonomics (painful without; needed soon after Tier 0)

| Feature | Status | Why | Effort |
|---|---|---|---|
| **Operator overloading** | ✅ — full set, arity-picked forms, type-based dispatch, chaining, compound assignment, operators-in-contracts for generic math | `a + b` for the math types above; without it the math layer is unusable. | done |
| **General user generics** (`type value Foo<T>`, generic fns, nested `>>`, contract bounds `<K: A + B>`, `This`, turbofish) | ✅ — monomorphized, zero-cost, ASan-clean | Every future library type. | done |
| **Error model**: `Result`/`Optional` | ✅ — `Optional<T>`/`Result<T,E>` prelude tagged unions, consumed by exhaustive `match` (no exceptions, no `null`) | File/asset/GPU/shader-compile failures need a first-class, non-exception path (fits no-GC/deterministic). | done |
| **Tagged unions / sum types + pattern matching** | ✅ — `enum` payloads/generic enums + value-producing exhaustive `match` (plain enums too) | Events, messages, render commands, animation/state machines, asset variants. | done |
| **`Map<K,V>` / hash maps + sorted maps** | ✅ — **`Map<K,V>`/`Set<K>`** (open-addressing/tombstoned, owning keys+values, deep `copy` + key iteration, ASan-clean) over prelude **`Hashable`/`Equatable`** (`hash()` = a cheap CONTENT hash — identity per integer width, FNV-1a for `string`; floats `Equatable`-only) with a **pluggable finalizer** — `Map<K,V,H: Hasher = DefaultHasher>`/`Set<K,H>` compute `H::finish(k.hash()) & (cap-1)`, default **`DefaultHasher`** (splitmix64) swappable to **`FastHasher`** (single Fibonacci mul) for trusted-key hot loops — plus `reserve`/`withCapacity` preallocation. **AND `SortedMap<K,V>`/`SortedSet<K>`** — a **B-tree** (min-degree 6) over prelude **`Comparable`/`Ordering`**: ordered iteration + `first`/`last`/`floor`/`ceil`/`range` + `getRef` in-place borrow + deep `copy` + JSON serde, move-only keys/values ASan-clean through splits/borrows/merges. **Every container now takes a custom allocator** (`A: Allocator = GlobalAllocator`) — the direct-heap containers (M10) and the boxed `SortedMap<K,V,A>`/`SortedSet<K,A>` B-tree (M11b, interior nodes drawn from `A` via allocator-aware `new`); see the Allocator-control row. | Entity/resource/asset registries, caches, string→handle lookup; timelines / z-order / spatial-sort / range scans (sorted). | done |
| **Slices / spans** (non-owning views over `FixedArray`/`DynamicArray`/buffers) | ✅ — **`type view` kind + `View<T>`**: a stack-only borrow (C# `ref struct`), `arr.view()`/`arr.slice(from:,count:)`, index + mutate-through + `foreach`, `const View<T>` for read-only; escape-checked (never a field/collection-element/escaping-return), zero-copy. Engine can author its own (`type view StridedView<T>`/`Grid2D<T>`). | Iterate a subrange, pass a buffer to a system or a GPU upload without copying or transferring ownership. | done |
| **Allocator control**: arenas / pools / frame & stack allocators | ✅ — **M10a + M10b shipped**: containers take a copyable-value-handle allocator param `A: Allocator = GlobalAllocator` (à la C++ `pmr` / Zig `std.mem.Allocator`); **every direct-heap container** — `DynamicArray`/`Map`/`Set` (M10a) + `Deque`/`FixedArray`/`BitSet`/`SlotMap`/`PriorityQueue` (M10b) — routes every buffer through it via a **direct monomorphized call**, and a caller-owned **`Arena`** + **`BumpAllocator`** give O(1) bump-reset per frame (`DynamicArray::withAllocator(allocator: arena.handle())`). **M11a shipped allocator-aware `new`**: a placement `new(allocator: a) T(args)` draws a heap-*boxed* object from `a` and yields `Owned<T, A>` (the box stores the handle; its dtor releases through the same allocator), so a system can now arena-back boxed objects too and bulk-reset. **M11b threaded `A` through the `SortedMap<K,V,A>`/`SortedSet<K,A>` B-tree** — interior node boxes placement-`new` from `A`, so `arena.reset()` reclaims the whole tree (one box per tree, the always-live root, stays `GlobalAllocator` — freed by RAII, a documented wart). **M11c completed the smart pointers**: concrete `Shared<T, A>` / `Weak<T, A>` draw BOTH the pointee and the shared control block from `A` (placement `new(allocator: a) T(...)`), so `arena.reset()` reclaims a whole ref-counted graph — each handle carries its own copyable `A` value, so a `Weak` that outlives its `Shared` still frees the ctrl through the right allocator (no type-erased ctrl-block channel needed). **M11d finished the campaign**: the type-erased *interface*-element handles (`Owned/Shared/Weak<Contract, A>`) now carry a by-value `A alloc` + `objsize` in the fat handle and draw the pointee + ctrl from `A`, so **every** box — concrete and contract-erased — is arena-backable; default-`GlobalAllocator` interface boxes stay byte-identical. The one carve-out: object-graph `@generate` `Shared`/`Weak`/`Owned` edges are `GlobalAllocator`-only (a stateful edge is a compile error). **Fallible `allocate -> Optional<Ptr>` shipped (MCU step 5)** on the same seam — a frame/pool allocator now stops **gracefully** at `None` on exhaustion instead of trapping (`try new -> Optional<Owned<T>>` is the non-panic construction entry), and a per-region **`@noheap`** attribute / whole-program `--no-heap` flag makes a frame tick or audio callback **provably allocation-free** (`tests/alloc_frame_arena.kama` demos both). See ROADMAP §5.3. | Deterministic per-frame perf, zero mid-frame `malloc`, bump-reset allocators, provably heap-free hot paths. | ✅ done |

## Tier 2 — Systems & scale

| Feature | Status | Why | Effort |
|---|---|---|---|
| **Threading / atomics / memory model** | ✅ **shipped** (concurrency campaign complete, 2026-07-23) — the **shared-nothing model** is built and TSan/ASan-proven on native **and** wasm (Web Workers over SharedArrayBuffer) from one source: isolates (`spawn`) + ownership-transferring `channel<T>` + structured-concurrency `scope` (join-before-drop) + `Atomic<T>` (the one shared-mutable seam) + immutable-`Shared` cross-isolate reads + **disjoint-slice `parallel_for`**. **`parallel_for` IS the data-parallel / parallel-ECS fan-out** (splits a buffer into non-overlapping mutable sub-`View`s, one per worker, safe by disjointness). The engine's higher-level **job-system / work-stealing scheduler is now an ordinary library** on these primitives — no language work left. | Job system, parallel ECS, async asset streaming. | ✅ done (lang/runtime; job-system lib on top) |
| **Bit/byte manipulation**: reinterpret/bitcast, byte buffers, endianness | ✅ — **`bitcast<T>(x)` shipped**: a same-width numeric-scalar reinterpret (`float32↔uint32`, `float64↔uint64`, `intN↔uintN`) lowering to a no-UB ISO-C11 union type-pun; equal-width + numeric-scalar checks at compile time (native + wasm, `tests/bitcast_basic`, xfail `bitcast_{width,nonscalar}`). Byte buffers are `DynamicArray<uint8>`; endianness = **`std::num` `byteswapU16/U32/U64` + `byteswapF32/F64`** (the float swaps ride `bitcast`), `tests/endian_byteswap`. (Host-order `htole`/`htobe` await a compile-time endianness flag — small follow-up; every current target is little-endian.) | (De)serialization, networking, binary asset/scene formats. | ✅ done |
| **File / network I/O** (files, sockets) | ✅ — **`std::fs`/`std::io`/`std::net` shipped** (RAII `File`, `readFile`/`writeFile`/`stat`/`readDir`; blocking TCP `TcpListener`/`TcpStream`; `Result<…,IoError>`), POSIX + Windows, over the bundled `kama_os.h` FFI boundary. `examples/httpd/` is a real static-file server on it. Follow-ups (UDP/DNS, buffered readers) tracked in ROADMAP §1. | Asset/scene loading, config, tooling, networking. | done |
| **String formatting / interpolation** (`"${x}"`, number→string, logging) | ✅ — **shipped**: the `Format` contract + `Formatter` sink + `toString<T>` (prelude), plus `${expr}` interpolation (identifier + `.field`/`[index]` holes) lowering to a compile-time, statically-checked `Formatter` build — one buffer, no O(n²) concat. On top of the already-rich `string` (ops, `split`/`trim`/…, UTF-8 + `.chars()`). Follow-ups (additive): format specifiers `${x:.2f}`, a `@generate` debug derive, tagged strings (`sql"…"`/`html"…"`) — see [SPEC.md](SPEC.md) "Formatting & string interpolation". | Logging, text assets, tooling. | done (specifiers/tags follow) |
| **comptime / const-eval** | 🟢 shipped — `const` values, **const generics** (`InlineArray<T,N>`, integer type params), **`sizeof(T)`/`alignof(T)`**, **const-param arithmetic (6b-1)**, **named `comptime` constants at local/module/type scope (6b-2)**, and **compile-time *functions* `comptime fn` (6b-3)** all ship. A `comptime fn` RUNS at build time and bakes its scalar/table result into a `static const` — a lookup table / permutation constant computed once by the compiler, zero runtime cost. Bounded + pure; comptime-only; both top-level and type-associated (`Type::name()`, member-visibility-controlled). Flagship `comptime_fn_crc` (baked CRC-8 LUT). | Lookup tables, shader/permutation specialization, asserts. | M |
| **Reflection / metadata** | ✅ opt-in **serialization shipped** — `@generate(Serialize, Deserialize)` on a type/enum drives the compiler's `ClassInfo` (field name/type/order + variants). User surface = contracts + attributes (+ hand-written override); the field walk **and** the object-graph rebuild are a **compiler intrinsic** (lowering to C), with swappable library wire backends (`std::serialization::json` now). Two modes gated on `reachesPointer`: pointer-free ⇒ by-value (`decode::<T> -> T`); reaches a pointer ⇒ heap graph (`decode::<Shared<T>>`), incl. polymorphic `Shared/Weak/Owned<Contract>` + a `DeError` set. Remaining is additive library work (more wire back ends) — see [ROADMAP.md](ROADMAP.md) §4 / [SPEC.md](SPEC.md) "Serialization". | Auto-serialization, editor property panels, ECS introspection. | done (json; more back ends follow) |

## Tier 3 — Ecosystem & polish

Package manager / multi-module build & deps (L); **LSP** + formatter + debugger polish (L); variadics (S);
`defer`/scope-guards (S — RAII already covers most); enum methods / flags (S); inline **capturing closures**
(M+); a **`foreach` iteration protocol** for user types (S–M — today `foreach` is built-in-collections only;
now that `Map`/`Set` have landed, the open piece is entry-wise iteration — a generic `Entry<K,V>` yielded
through `Optional` doesn't monomorphize yet, ROADMAP §5).

## The actual goal

| Goal | Status | Notes |
|---|---|---|
| **kama-level WebGPU bindings** → first triangle → (engine spine) | 🟡 — **triangle shipped, browser/WASM *and* native desktop** (`8db3408`). [`examples/webgpu/`](../examples/webgpu/) is a **pure-Kama** WebGPU triangle (instance→adapter→device→queue→surface→render pass→draw), no C glue: opaque handles are `Ptr`, C descriptors are `type extern value` (header owns layout), the async adapter/device handshake rides `fnptr` callbacks + `userdata`, the frame loop is `std::app`. The `std::gpu` platform seam ([`lib/std/gpu/kama_gpu.h`](../lib/std/gpu/kama_gpu.h)) abstracts surface/present/pump so one source builds web (canvas) and native (GLFW + Metal/X11/Wayland/HWND); `examples/webgpu/build-native.sh`. **kama-scoped remainder:** at most a thin safe `std::gpu` binding wrapper (handles→RAII, as `std::net` wraps sockets) — *optional stdlib polish, not a 1.0 blocker*. **The engine *spine* (buffers/bindings/pipelines/renderer beyond a triangle) is the engine *product* built on kama, not part of kama — see [ROADMAP.md](ROADMAP.md) §8; out of scope for the language.** | seam shipped; wrapper = optional polish |

---

## Recommended sequence (fastest path to a rendering engine)

The language is complete; the path is now entirely library + platform work.

1. **Math types** (`Vec2/3/4`, `Mat4`, quaternion) as `type value`s (public fields) with operators + `::`
   static methods — the numeric core (Tier-0 math). **All the language machinery it needs is shipped** —
   operators, static methods, and now `InlineArray<T,N>` for matrix storage + `m[i][j]` place-indexing — so
   this is a pure library layer (a natural first opt-in **stdlib module**, per ROADMAP).
2. **WebGPU bindings** (FFI is ready) → **a triangle on screen** → the engine spine + a real demo.
3. Iterate as the engine grows: `Map`/slices/arenas as library types, then bit/byte + I/O. **Threading is
   now shipped** — build the job-system / parallel-ECS library on the isolate + `parallel_for` primitives
   when the engine needs it (no language gap remains).

Each item is independently shippable (a tagged release), matching the milestone cadence so far. The
fast/deterministic-runtime thesis is already true today — the remaining gaps are about reach (hardware/OS)
and scale (large codebase), not the runtime model or the language.
