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
   - *Tied to this:* the built-in containers/smart-pointers are currently compiler intrinsics with
     hand-tuned **C** runtime bodies. Generics (M27) unify the *surface* but keep those C bodies
     (option "C"). **Reimplementing them as cstar generic library types** (option "B" — the Rust-`Vec`
     "unsafe core, safe API" model, in `unsafe`/`Ptr`) is a possible *later* step. It buys nothing for
     runtime performance (monomorphization makes both identical) and isn't needed for the language to
     be complete — its payoff is *this* goal, self-hosting (a cstar stdlib for a cstar compiler). The
     M27 engine is built so (B) is a no-rework continuation (swap a generic type's body source from
     C-macro to cstar), never a redo.

2. **Fast compiles: single-pass, parallelizable.** Parsing stays essentially single-pass. Speed comes
   from per-file parallelism — parse + emit each translation unit independently, then compile the
   generated `.c` files in parallel. The module system should keep files independently parseable. *(True
   intra-file parallel parsing is not a goal — the win is per-file + parallel C compilation.)*

3. **Predictable allocation/deallocation, no GC.** Lifetimes are deterministic (RAII). Allocation is
   explicit in the generated C. Arena/pool allocators arrive as library types for the engine.

3a. **No raw pointers in the *safe* surface.** The safe cstar surface never exposes raw pointers or raw
   memory. Heap and buffers are reached only through safe abstractions: **collections** (`Array<T>`/
   `List<T>`/`String`) and the **smart-pointer family** (`Owned<T>` unique, `Shared<T>` ref-counted,
   `Weak<T>`) — both shipped. These are compiler-known intrinsics whose unsafe internals (raw pointers,
   `malloc`/`free`) live ONLY in `cstar_runtime.h` — the Rust-`Vec`/Swift-`Array` model: unsafe core, safe
   API. Indexing is **bounds-checked** (traps, not UB). The one deliberately contained exception is the
   **`unsafe { }`** block + `Ptr<T>` at the **FFI boundary** (M17): a narrow, greppable seam for talking to C
   (GPU/OS APIs — the whole point of transpiling to C), never general-purpose escape, and the safe surface
   never sees it. (Self-hosting the compiler in cstar — goal #1 — is the other place a contained escape may
   matter.)

3b. **No null in the safe surface.** A stack value, an `Owned<T>`/`Shared<T>`, a `ref`/`out` borrow, and a
   contract value are **always valid** — there is nothing to null-check. Absence is encoded in the
   type/flow, never a sentinel you can forget to test: ownership is valid by construction, *use-after-move*
   is a compile error, a `Weak<T>` can only be reached through `tryUpgrade` (whose result forces you to
   handle the dead case), and (with the escape check) a borrow can't dangle. So **`== null` / `!= null` on a
   safe type is a compile error** with guidance — the C habit of null-checking a pointer is both unnecessary
   and checks the wrong thing here. The `null` literal and nullability are confined to **`Ptr<T>` at the FFI
   boundary** (checked inside `unsafe`), where you genuinely talk to C. This is the deliberate avoidance of
   the null-reference "billion-dollar mistake."

3c. **Ownership is the type axis — `value` / `resource` / `contract`.** cstar organizes types by
   *what they own*, not the C/C++ `class`/`struct`/`pod` legacy. Every declaration is `type <kind>
   Name` (the `type` marker, parallel to `fn`): a **`type value`** owns nothing (raw data, **copied**;
   the stricter cousin of a "value type" — no smuggled shared refs; may still encapsulate private
   fields to guard an invariant). A **`type resource`** owns something, or has identity (**moved**,
   RAII-dropped). A **`type contract`** is a public-only guarantee a type satisfies — cstar's word for
   an interface. Polymorphism's goal is **substitutability, not reuse**: inheritance bundles the two,
   so cstar unbundles them — **generics** give reuse (zero-cost monomorphization), **contracts** give
   substitutability, and `type virtual`/`abstract resource` is only for sharing *implementation* up an
   owned hierarchy. Hand-offs follow **"silent default, scream when ambiguous"**: a `give`/`copy`
   marker is required exactly when *both* move and copy are plausible (a `resource` that has opted into
   a copy contract), and silent otherwise. The kind words `value`/`resource`/`contract` are
   **contextual** (they name a kind only right after `type`), so they stay ordinary identifiers
   everywhere else. *(Shipped in **M26h**; full model in `docs/TYPE_MODEL.md`.)*

4. **One way to do a thing. Favor simplicity.** Unlike C++'s many syntaxes for one concept, cstar
   prefers a single, obvious construct. Resist redundant syntax. (Already: named args only — no
   positional; one form per construct.)

5. **Explicit over implicit.** Named parameters, explicit `ref`/`out`, explicit types, explicit casts
   for narrowing. Prefer surfacing intent over inferring it.
   - **Greppable / self-describing syntax.** Every declaration and dangerous operation is marked by a
     keyword or operator you can search for: `fn` on every function/method, `unsafe { }` for raw memory,
     the `…Ptr`/smart-pointer families for pointers, and `::` for scope resolution vs `.` for instance
     access. Intent is visible to a human, a tool, or an LLM at a glance — no guessing which `Type name(`
     is a declaration vs a call, or which access crosses a namespace vs an object.

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

The single source of truth for status is `README.md` (feature summary) and `docs/ROADMAP.md` (the
milestone plan). Through M26h, the core language, OO, RAII, collections, the smart-pointer family, the
full value/ownership model, `const`-correctness, access control, and the type-model vocabulary reframe
(`type value`/`type resource`/`type contract`, M26h) are all shipped; user-defined generics (M27), sum
types + pattern matching (M28), and operator overloading + full static methods (M31) are now shipped too —
so the **language feature set is complete**. What remains before 1.0 is Step 7 (doc/SPEC reconciliation +
a repo-wide naming/case pass), then the tag. This section is intentionally brief so it doesn't drift — see those files.
