# cstar — language goals & design philosophy

The durable "north star" for cstar. Decisions should be checked against these.

## What cstar is

A C-family language with C#-like syntax, **no garbage collector** (RAII / deterministic destruction),
and **explicit named parameters**. It **transpiles to portable C** — so it runs anywhere C runs (native
on every platform; the browser via WebAssembly) with no .NET/runtime baggage. Target use: a portable,
lightweight WebGPU game engine.

## Goals

1. **Self-hosting eventually.** Developers should not need extra tooling; the compiler should ultimately
   be written in cstar and bootstrap through the C transpiler. *Long-term — requires strings, collections,
   maps, file I/O, and tagged unions in the language first.*

2. **Fast compiles: single-pass, parallelizable.** Parsing stays essentially single-pass. Speed comes
   from per-file parallelism — parse + emit each translation unit independently, then compile the
   generated `.c` files in parallel. The module system should keep files independently parseable. *(True
   intra-file parallel parsing is not a goal — the win is per-file + parallel C compilation.)*

3. **Predictable allocation/deallocation, no GC.** Lifetimes are deterministic (RAII). Allocation is
   explicit in the generated C. Arena/pool allocators arrive as library types for the engine.

3a. **No unsafe code; no raw pointers in the language.** The cstar surface never exposes raw pointers or
   raw memory. Heap and buffers are reached only through safe abstractions: **collections** (`Array<T>`/
   `List<T>`/`String`) now, and a **smart-pointer family** (`Owned<T>` unique, `Shared<T>` ref-counted,
   `Weak<T>`) later. These are compiler-known intrinsics whose unsafe internals (raw pointers, `malloc`/
   `free`) live ONLY in `cstar_runtime.h` — the Rust-`Vec`/Swift-`Array` model: unsafe core, safe API.
   Indexing is **bounds-checked** (traps, not UB). A raw pointer / `unsafe` block is explicitly NOT a
   language goal. (Self-hosting the compiler in cstar — goal #1 — is the one case that may someday need a
   tightly-contained escape hatch; a separate, deferred decision.)

4. **One way to do a thing. Favor simplicity.** Unlike C++'s many syntaxes for one concept, cstar
   prefers a single, obvious construct. Resist redundant syntax. (Already: named args only — no
   positional; one form per construct.)

5. **Explicit over implicit.** Named parameters, explicit `ref`/`out`, explicit types, explicit casts
   for narrowing. Prefer surfacing intent over inferring it.

6. **Scripting / no-compile iteration eventually.** A `cstar run` that transpiles and runs instantly
   (e.g. via TinyCC) gives sub-second iteration while reusing the single C backend — preferred over a
   separate interpreter. A tree-walking interpreter / hot-reload are larger, later options.

7. **Self-describing.** The grammar (`cstar.y`) is the single source of truth — the compiler embodies
   the BNF. Generate machine-readable grammar/spec from it (`docs/grammar.bnf` via `tools/gen-grammar`)
   so the language always describes itself. A future `cstar describe --json` exposes the language surface
   (keywords, types, builtins, grammar) for tools.

8. **MIT licensed.** Permissive and embeddable — see `LICENSE`.

9. **LLM-discoverable on day 1.** AI assistants can read the current syntax, semantics, and library
   availability from a canonical, always-current entry point (`llms.txt` at the repo root → grammar,
   spec, goals, examples, runtime API). Docs are generated from / point at source of truth so they never
   drift.

## Production-grade build & debugging

- **Debug / Release configs.** `cstar build` defaults to debug (`-g -O0`, `#line` on, asserts on);
  `--release` opts into optimized (`-O2`/`-Oz`, `-DNDEBUG`, stripped, no `#line`).
- **IDE breakpoint debugging (VSCode first).** Set breakpoints in `.cstar`, step, and inspect the call
  stack + locals — enabled by the `#line` directives mapping generated C back to `.cstar` and by locals
  keeping their cstar names. The VSCode extension ships a CodeLLDB launch config + Build Debug/Release
  tasks. WASM debugging works in the browser via emscripten source maps.

## Working principles (see `.claude/skills/`)

Development of cstar follows two vendored guidance skills that reinforce goals #4 and #5:
- **karpathy-guidelines** — think before coding, simplicity first, surgical changes, goal-driven.
- **ponytail** — lazy-senior-dev YAGNI ladder: write the least code that works.

## Current status

See `README.md` and the milestone plan. Core language is being built up M0→M7; RAII (no-GC) is the
in-progress milestone (M5). Collections (`List<T>`/`Array<T>`, no raw arrays — by design) are the first
post-v1 workstream and the gating feature for real engine code.
