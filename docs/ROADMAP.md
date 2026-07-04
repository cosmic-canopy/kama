# cstar roadmap

The forward plan. The **language feature set is complete**. History lives in the git
log; this file is only about what's next.

## The shape

- **1.0 — language complete.** Every *language* feature has landed: OO + RAII + the
  value/ownership model, generics, sum types + pattern matching, operator overloading. After
  1.0 the language surface is stable — you build *with* it, not *on* it.
- **1.x — systems & runtime.** Capabilities built ON the finished language: reflection +
  serialization, file I/O, networking, an embedded/MCU target. Mostly library + codegen, little
  new syntax.
- **2.0 — dual-mode scripting** (flagship): the *same* language usable compiled OR scripted,
  via a TinyCC-JIT / wasm REPL.
- **Engine track** (the product north star): a portable lightweight **WebGPU** game engine,
  woven through 1.x. Its Tier-0 math types are already unblocked by operator overloading.

## Remaining before 1.0

1. **Documentation & spec reconciliation** *(in progress)* — bring every doc current with the
   shipped language and settle the naming/case conventions (PascalCase types, lowerCamel
   methods, no `I`-prefix on contracts, lowercase `string`). 1.0 is the API-stability point,
   so naming is fixed here.
2. **Tag 1.0** — nothing else is deferred; the language is complete.

## Tracked limitations & non-goals

Policy: **no known limitation stays untracked** — each is either scheduled or a declared
non-goal.

- **`export` keyword** — reserved, hard-errors today; **tracked to 2.0** (the cstar→host
  boundary: WASM module exports for the browser engine + the scripting host interface). The
  engine's wasm build may pull a minimal `export` earlier.
- **`volatile` keyword** — reserved, hard-errors today; **tracked to 1.x → Embedded/MCU**
  (emit C `volatile` for ISR↔loop flags / MMIO registers).
- **`contract` refining a `contract`** (`type contract A : B`) parses, but deep multi-level
  contract inheritance is not yet lowered — revisit if a real case needs it.
- **By-value collection params/returns** — `give`-ing a collection into a variant payload
  works; general by-value collection params/returns (and deep-`copy` of a whole container into
  a variant) are **tracked to post-1.0** (same move-the-struct mechanism; not blocking).
- **Non-goal — function / constructor overloading.** Deliberately not planned: it conflicts
  with "one way to do a thing," and **named parameters** already cover the disambiguation
  overloading is usually reached for. **Operators are the sanctioned exception** — a type may
  carry several `operator*` distinguished by operand type (`mat*vec`, `mat*mat`, `v*s`, `s*v`),
  matching C++/C#/Rust. Reopen only if a concrete example shows named params can't express it.

## 1.x — systems & runtime (post-1.0)

Built ON the finished language; mostly library + codegen, not new syntax. These are also the
substrate the engine needs (asset I/O, scene serialization, networking).

- **Reflection + declarative serialization** — opt-in compile-time attributes/reflection →
  multi-format serialization (binary / json / yaml), little-endian canonical; scenegraph +
  serializer selection.
- **File I/O** — safe file APIs; gates serialization and engine asset loading.
- **Networking** — native UDP/TCP sockets vs browser **WebRTC DataChannels** (unreliable) /
  **WebSockets** (reliable), via FFI (the browser has no raw sockets — a real wasm nuance).
- **Embedded / MCU target** — globals/statics for ISR flags, `volatile` *emit*, ISR attributes,
  no-heap mode, avr/arm toolchains.
- **Native dispatch devirtualization** — on the `dispatch` bench cstar matches C on a real
  vtable indirect call, but Rust is ~5× faster because it devirtualizes/inlines the monomorphic
  case. A devirtualization / speculative-inlining pass, or `final`-method static-call lowering,
  would close the one real native gap.

## 2.0 — dual-mode: compiled + scripting/REPL (flagship)

The end goal is **one language, two modes** — the same cstar syntax usable both compiled and as
a scripting language with a full REPL. This rests on a **multi-backend emitter**: the shared
front end lowered to more than one target, not just today's single C backend.

- **Compiled (today):** cstar → C → clang (native) / emcc (wasm). The web target is wasm *via
  C* — there is no direct cstar→wasm; C is the portable middle.
- **Scripting / REPL (future):** the same front end + C emitter, compiled and run **in-memory**:
  - **Native fast path — TinyCC (TCC) JIT.** TCC compiles C in ~milliseconds in-process, so
    `cstar run foo.cstar` and the REPL are **JIT-compiled, not tree-walked** — near-native speed
    with instant startup.
  - **Web — run the wasm.** Browser scripting/REPL = compile to wasm (emcc) and run it.
  - A dedicated **bytecode VM** backend is the alternative if a more dynamic REPL is wanted; the
    design leans JIT/VM, never a slow tree-walker.
- **Why it beats other scripting languages:** Python/Ruby/Lua are bytecode interpreters;
  cstar-as-script runs **compiled** (TCC-JIT native, or wasm) — at or near native speed.

## Engine track (product north star)

A portable lightweight **WebGPU** game engine. Tiers: **math types** (Tier 0 — unblocked) →
buffers/bindings → first triangle → scene/material. Depends on the 1.x systems (file I/O for
assets, serialization for scenes). See [ENGINE_READINESS.md](ENGINE_READINESS.md).

## Performance (from the benchmark suite — [benchmarks/RESULTS.md](benchmarks/RESULTS.md))

cstar is at **C/C++ parity** on native compute and wins decisively on footprint (~2 MB RSS,
~66 KB binary) and the no-GC `alloc` workload. `cstar→wasm` (optimized) **beats hand-written JS
on fib/pi/collatz/fnptr (up to ~4.5×)** and ties on dispatch/alloc.

- **Native dispatch devirtualization** (above, under 1.x) is the one real native gap.
- **WASM tiering.** Measure at the optimizing tier (`node --no-liftoff` — what a real long-running
  app gets); the bench forces TurboFan for the wasm track so the numbers reflect steady-state, not
  V8's short-lived baseline (Liftoff) compiler.
- **Bench methodology (don't re-chase).** Short workloads are skewed by parallel load — run the
  bench with nothing else competing. The `/work` bind mount adds only ~0.3–1.7 ms (negligible);
  a named volume / tmpfs speeds up *builds* but not the measured numbers.

## Tooling / distribution (deferred)

- **VS Code Marketplace publish** — the `.vsix` is built + attached to releases; Marketplace
  publishing is deferred (pair with an eventual rename).
- **Brand rename** — "C*"/cstar is crowded; revisit before publish. Current name kept for now.
- **FreeBSD CI** — a non-blocking `vmactions/freebsd-vm` job once Windows is proven on a tag.
- **Browser-debug ergonomics** — richer wasm source maps / a no-extension flow.
