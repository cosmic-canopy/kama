# cstar roadmap

Living list of what's planned but not yet done. Near-term work to 1.0 is tracked as
milestones (see `CLAUDE.md` / `GOALS.md`); this file is the durable home for **deferred
decisions and post-1.0 backlog** so they aren't lost.

## Toward 1.0 (in progress)

- **M26 — value & ownership model** (in progress): `give`/`copy` hand-off markers (M26c),
  by-value smart-pointer params (M26d), second-class borrows / escape check + the `Weak`
  `tryUpgrade(out:) -> bool` Try-pattern with flow-typed result (M26e).
- **Step 7** — doc/SPEC reconciliation.
- **Step 8** — formally defer M23 generics to 1.1, then tag **1.0**.

## Dual-mode: compiled + scripting/REPL (flagship)

The end goal is **one language, two modes** — the *same* cstar syntax usable both as a
compiled language and as a scripting language with a full REPL. This rests on a
**multi-backend ("polymorphic") emitter**: a shared Flex/Bison/AST front end lowered to more
than one target, not just the single C backend it has today.

- **Compiled (today):** cstar → C (`cstar.cemit`) → clang (native) / emcc (wasm). The **web
  target is wasm *via C*** (cstar→C→emcc) — there is no direct cstar→wasm; C is the portable
  middle.
- **Scripting / REPL (future):** the *same* front end + C emitter, but compiled and run
  **in-memory** instead of written to disk:
  - **Native fast path — TinyCC (TCC) JIT.** TCC compiles C in ~milliseconds in-process, so
    `cstar run foo.cstar` and the REPL are **JIT-compiled, not tree-walked** — near-native
    speed with instant startup. The REPL feeds each input through the front end, accumulates
    definitions, and TCC-compiles-and-runs. This is the answer to "fast interpreter on the
    native side": it's a JIT, not an interpreter.
  - **Web — run the wasm.** Browser scripting/REPL = compile to wasm (emcc) and run it in the
    browser's wasm runtime.
  - A dedicated **bytecode VM** backend is the alternative if a more dynamic REPL is wanted;
    the design leans **JIT/VM, never a slow tree-walker**.
- **Why it can be much faster than other scripting languages:** Python/Ruby/Lua are bytecode
  interpreters; cstar-as-script runs **compiled** (TCC-JIT native, or wasm) — at or near
  native speed, i.e. orders of magnitude faster than a bytecode interpreter. A future
  **cstar-script track** in the benchmark suite would measure this head-to-head (the suite
  today measures cstar *compiled* vs the scripting langs, which already wins handily).

## Performance (from the benchmark suite — `docs/benchmarks/RESULTS.md`)

cstar is at **C/C++ parity** on native compute and wins decisively on footprint (2 MB RSS,
66 KB binary) and the no-GC `alloc` workload. Two measured gaps are tracked here:

- **Native dispatch — devirtualization.** On the `dispatch` workload cstar matches C
  (≈6.15 ms, a real vtable indirect call ×8M) but **Rust is ~5× faster (1.22 ms)** because
  its optimizer devirtualizes/inlines the monomorphic case. cstar does not yet. A
  devirtualization / speculative-inlining pass (or `final`-method static-call lowering —
  M25b already tracks `final`) would close it. Post-1.0 optimization.
- **WASM float64 + indirect calls.** `cstar→wasm` beats hand-written JS on fib (1.4×),
  collatz (2.2×), fnptr (3.1×) and ties alloc, but **lags JS on `pi` (float64 loop, ~1.6×)
  and `dispatch` (indirect calls, ~1.4×)** — V8's JIT outdoes emcc's AOT wasm on those two
  patterns. Native cstar `pi` is at C parity, so this is specifically the wasm lane.
  Investigate emcc tuning (`-msimd128`, FP scheduling/reassociation without breaking the
  fairness checksum, `call_indirect` lowering). Pairs with the WebGPU/engine track.

## Language backlog (post-1.0)

- **Operator overloading + full static methods** — unblocks `pod` math (`Vec2 + Vec2`,
  `Vec2::dot(left:, right:)`); the value/ownership model (M26) already reserves the place.
- **Generics (M23)** — `Map<K,V>`, multi-param/nested generics (`>>` lexing); deferred to 1.1.
- **Tagged unions + `match` → `Optional<T>`** — the general "forced unwrap" mechanism; makes
  `Weak::tryUpgrade` (and every fallible op) compiler-checked without an out-param.
- **Naming/case convention pass** — settle the repo-wide convention (methods are lower-camel
  today, e.g. `tryUpgrade`; types PascalCase) deliberately rather than ad hoc.
- **Reflection + declarative serialization** (binary/json/yaml, scenegraph + selectors),
  file I/O, networking (native UDP/TCP vs browser WebRTC DataChannels), dual-mode
  compiled+scripting/REPL, embedded/MCU target (globals/statics, ISR attrs, no-heap).

## Engine track

The end goal is a portable lightweight **WebGPU** game engine. Tiers: math types →
buffers/bindings → first triangle → scene/material. See `docs/ENGINE_READINESS.md`.

## Tooling / distribution (deferred)

- **VS Code Marketplace publish** — the `.vsix` is built + attached to GitHub Releases;
  Marketplace publishing is deferred (pair with the eventual rename).
- **Brand rename** — "C*"/cstar is crowded; revisit before 1.0/publish (celestial direction
  tied to Cosmic Canopy, e.g. *Canopus*/*Carina*). User prefers the current name for now.
- **FreeBSD CI** — a non-blocking `vmactions/freebsd-vm` job once Windows is proven on a tag.
- **Browser-debug ergonomics** — richer wasm source maps / a no-extension flow.
