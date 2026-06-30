# cstar roadmap

Living plan for where cstar is headed. The guiding shape (set 2026-06-30):

- **1.0 — language complete.** Every *language* feature lands: OO + RAII + the value/ownership
  model, **generics**, **sum types + pattern matching**, **operator overloading**. After 1.0 the
  language surface is stable — you build *with* it, not *on* it.
- **1.x — systems & runtime.** Capabilities built ON the finished language: reflection +
  declarative serialization, file I/O, networking, an embedded/MCU target. Mostly library +
  codegen work, little-to-no new syntax.
- **2.0 — dual-mode scripting** (flagship): the *same* language usable compiled OR scripted, via
  a TinyCC-JIT / wasm REPL.
- **Engine track** (the product north star): a portable lightweight **WebGPU** game engine, woven
  through 1.x. Its Tier-0 math types are unblocked by operator overloading in 1.0.

This file is the durable home for the plan + deferred decisions so they aren't lost. Near-term
milestones are also summarized in `CLAUDE.md` / `GOALS.md`.

## Road to 1.0 — language complete

Recommended order: **M26e → M23 → M28 → M27 → Step 7 → tag.** Rationale: finish the borrow-safety
arc (M26e, small), then the type-system push (generics → sum types, which is the dependency spine:
`Optional<T>` *is* a generic tagged union), then operators, then docs. M27 is independent — pull it
earlier if you want an engine-math demo sooner.

1. **M26e — borrow escape check** (second-class borrows). Flow analysis proving a `ref`/`out`
   borrow can't outlive its referent. Closes the last memory-safety hole; with the no-null work
   already shipped (M26a–d), fully delivers GOALS §3b. *(The `Weak` Try-pattern that was bundled
   here moves to M28, where `Optional` exists — so it ships in final form, no churn.)*
   - **Design Qs:** Is the rule strictly *second-class* (a borrow flows only DOWN the call stack —
     never returned, stored in a field, or captured in a `Bindable`/closure) with **no lifetime
     annotations** (the simplicity bet, à la Hylo mutable value semantics)? Or do we want a richer
     escape analysis? What's the exact forbidden set, and is checking purely intraprocedural?

2. **M23 — generics.** Full user-defined generics: `Map<K,V>`, multi-param + nested (`>>` lexing).
   The foundational type-system feature — unblocks `Optional<T>`, `Map`, and every future library
   type. *(Was slated to defer to 1.1; pulled back in for a language-complete 1.0.)*
   - **Design Qs:** **Monomorphization** (consistent with today's collection/smart-ptr intrinsics —
     zero-cost, some code bloat) vs type-erasure — almost certainly mono, confirm. **Generic
     constraints** via interface bounds (`<K: IHashable>`) — `Map` needs key hashing/equality, so
     some bound mechanism is required; what's the syntax? The `>>` token split for nested generics
     (`Map<K, List<V>>` — known lexer issue). How a generic type param composes with give/copy and
     the smart-ptr family. Generic functions vs only generic types?

3. **M28 — tagged unions + `match` + `Optional<T>`.** Sum types with exhaustive pattern matching;
   `Optional<T>` as a *library* tagged union (`enum Optional<T> { Some(T), None }`), **not** a
   compiler intrinsic — one way to do a thing. The "no forgotten case" capstone. Migrates
   `Weak.tryUpgrade() -> Optional<Shared<T>>` to its final form (no out-param). Every fallible op
   becomes compiler-checked.
   - **Design Qs:** Do payload variants **extend `enum`** (reuse the keyword — today it's C-style
     int constants; `enum E { A(int32), B(String) }`) or a **new keyword** (`union`/`variant`/`data`)?
     `match` as an **expression and/or statement**; exhaustiveness checking, payload binding, the
     `_` wildcard, guards? Memory layout (tag + union) and **per-variant RAII** (drop the right
     payload). **Flow-typing** the matched binding (and the `tryUpgrade` result) to the narrowed
     variant. This is where "absence" lives now that there's no null — confirm `Optional` is the one
     blessed mechanism (vs also `Result<T,E>`?). Depends on M23.

4. **M27 — operator overloading + full static methods.** Ergonomic `pod` math — `Vec2 + Vec2`,
   `Vec2::dot(left:, right:)` — the engine's Tier-0 dependency. The value model (M26) already
   treats `Vec2 c = a + b` as a cheap pod copy. Validated by a first Vec2/3/4 + Mat4 library.
   - **Design Qs:** Operator-method **syntax** — operators are the *sanctioned exception* to
     named-args-only (a binary op has exactly two operands, positional by nature); how do we spell
     it (`fn Vec2 operator+(Vec2 rhs)`? a special `operator` member? free-function form?). **Which
     operators** (arithmetic, comparison `== < >`, index `[]`, unary `-`/`!`, compound `+=`?). The
     **`static` method form** (`Type::method`, no `self` — `static` is only partial today, M19).
     Should `==` tie into a structural-equality default for `pod`s?

5. **Step 7 — doc/SPEC reconciliation + naming pass.** Bring SPEC/KEYWORDS/GOALS/README current
   (give/copy + by-value from M26c/d, generics, `match`/`Optional`, operators; GOALS §3a unsafe
   wording vs shipped `unsafe{}`/`Ptr`). **Fold in the repo-wide naming/case convention pass**
   (lower-camel methods, PascalCase types) — 1.0 is the API-stability point, and post-1.0 renames
   are breaking, so settle it *now*.

6. **Step 8 — tag 1.0.** Nothing deferred — the language is complete.

## 1.x — systems & runtime (post-1.0)

Built ON the finished language; mostly library + codegen, not new syntax. These are also the
substrate the engine needs (asset I/O, scene serialization, networking).

- **Reflection + declarative serialization** — opt-in compile-time attributes/reflection →
  multi-format serialization (binary / json / yaml), little-endian canonical; scenegraph +
  serializer selection. A flagship system the user has wanted since early on.
- **File I/O** — safe file APIs; gates serialization and engine asset loading.
- **Networking** — native UDP/TCP sockets vs browser **WebRTC DataChannels** (unreliable "UDP") /
  **WebSockets** (reliable), via FFI (the browser has no raw sockets — a real wasm nuance).
- **Embedded / MCU target** — globals/statics for ISR flags, `volatile` *emit* (re-reserved in
  M19 for exactly this), ISR attributes, no-heap mode, avr/arm toolchains.
- **Perf — native dispatch devirtualization.** On the `dispatch` bench cstar matches C (≈6.18 ms,
  a real vtable indirect call ×8M) but **Rust is ~5× faster (1.18 ms)** — it devirtualizes/inlines
  the monomorphic case. A devirtualization / speculative-inlining pass, or `final`-method
  static-call lowering (M25b already tracks `final`), would close it.

## 2.0 — dual-mode: compiled + scripting/REPL (flagship)

The end goal is **one language, two modes** — the *same* cstar syntax usable both as a compiled
language and as a scripting language with a full REPL. This rests on a **multi-backend
("polymorphic") emitter**: a shared Flex/Bison/AST front end lowered to more than one target, not
just the single C backend it has today.

- **Compiled (today):** cstar → C (`cstar.cemit`) → clang (native) / emcc (wasm). The **web
  target is wasm *via C*** (cstar→C→emcc) — there is no direct cstar→wasm; C is the portable
  middle.
- **Scripting / REPL (future):** the *same* front end + C emitter, but compiled and run
  **in-memory** instead of written to disk:
  - **Native fast path — TinyCC (TCC) JIT.** TCC compiles C in ~milliseconds in-process, so
    `cstar run foo.cstar` and the REPL are **JIT-compiled, not tree-walked** — near-native speed
    with instant startup. The REPL feeds each input through the front end, accumulates
    definitions, and TCC-compiles-and-runs. This is the answer to "fast interpreter on the native
    side": it's a JIT, not an interpreter.
  - **Web — run the wasm.** Browser scripting/REPL = compile to wasm (emcc) and run it in the
    browser's wasm runtime.
  - A dedicated **bytecode VM** backend is the alternative if a more dynamic REPL is wanted; the
    design leans **JIT/VM, never a slow tree-walker**.
- **Why it can be much faster than other scripting languages:** Python/Ruby/Lua are bytecode
  interpreters; cstar-as-script runs **compiled** (TCC-JIT native, or wasm) — at or near native
  speed, orders of magnitude faster than a bytecode interpreter. A future **cstar-script track**
  in the benchmark suite would measure this head-to-head (the suite today measures cstar
  *compiled* vs the scripting langs, which already wins handily).

## Engine track (product north star)

A portable lightweight **WebGPU** game engine. Tiers: **math types** (Tier 0 — unblocked by M27)
→ buffers/bindings → first triangle → scene/material. Depends on the 1.x systems (file I/O for
assets, serialization for scenes). See `docs/ENGINE_READINESS.md`.

## Performance (from the benchmark suite — `docs/benchmarks/RESULTS.md`)

cstar is at **C/C++ parity** on native compute and wins decisively on footprint (2 MB RSS,
66 KB binary) and the no-GC `alloc` workload. `cstar→wasm` (optimized — see below) **beats
hand-written JS on fib/pi/collatz/fnptr (up to ~4.5×)** and ties on dispatch/alloc.

- **Native dispatch — devirtualization** (tracked above under 1.x perf): the one real native gap.
- **WASM tiering (resolved — was an artifact, not a lag).** An earlier "cstar→wasm lags JS on
  pi/dispatch" finding was wrong: it was V8 running the short-lived wasm in its **baseline
  Liftoff** compiler (never tiering up to the optimizing **TurboFan** before the process exited,
  especially under load) while JS got its auto-JIT. Measured at the optimizing tier
  (`node --no-liftoff` — what a real long-running app gets automatically), cstar→wasm beats JS on
  fib/pi/collatz/fnptr and ties dispatch/alloc (strict IEEE). The bench now forces TurboFan for
  the wasm track. (Aside: `-ffast-math` is out — it would relax FP the baselines don't get.
  Isolated/cool, optimized wasm is ~10 ms — even further ahead — but the bench measures hot, so
  the committed numbers are the conservative, reproducible ones.)
- **Bench methodology (verified, don't re-chase).** Short workloads are skewed badly by
  **parallel load** — run the bench with nothing else competing. The `/work` bind mount
  (virtiofs/9p on macOS/Windows) adds only **~0.3–1.7 ms** for native *and* wasm under a
  controlled idle measurement — negligible. A named volume / tmpfs build dir would speed up
  *builds* on Mac/Windows but does **not** affect the measured numbers.

## Tooling / distribution (deferred)

- **VS Code Marketplace publish** — the `.vsix` is built + attached to GitHub Releases;
  Marketplace publishing is deferred (pair with the eventual rename).
- **Brand rename** — "C*"/cstar is crowded; revisit before 1.0/publish (celestial direction tied
  to Cosmic Canopy, e.g. *Canopus*/*Carina*). User prefers the current name for now.
- **FreeBSD CI** — a non-blocking `vmactions/freebsd-vm` job once Windows is proven on a tag.
- **Browser-debug ergonomics** — richer wasm source maps / a no-extension flow.
