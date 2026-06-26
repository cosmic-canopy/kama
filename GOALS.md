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

4. **One way to do a thing. Favor simplicity.** Unlike C++'s many syntaxes for one concept, cstar
   prefers a single, obvious construct. Resist redundant syntax. (Already: named args only — no
   positional; one form per construct.)

5. **Explicit over implicit.** Named parameters, explicit `ref`/`out`, explicit types, explicit casts
   for narrowing. Prefer surfacing intent over inferring it.

6. **Scripting / no-compile iteration eventually.** A `cstar run` that transpiles and runs instantly
   (e.g. via TinyCC) gives sub-second iteration while reusing the single C backend — preferred over a
   separate interpreter. A tree-walking interpreter / hot-reload are larger, later options.

## Working principles (see `.claude/skills/`)

Development of cstar follows two vendored guidance skills that reinforce goals #4 and #5:
- **karpathy-guidelines** — think before coding, simplicity first, surgical changes, goal-driven.
- **ponytail** — lazy-senior-dev YAGNI ladder: write the least code that works.

## Current status

See `README.md` and the milestone plan. Core language is being built up M0→M7; RAII (no-GC) is the
in-progress milestone (M5). Collections (`List<T>`/`Array<T>`, no raw arrays — by design) are the first
post-v1 workstream and the gating feature for real engine code.
