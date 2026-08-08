# kama — language goals & design philosophy

The durable "north star" for kama. Decisions should be checked against these.

## What kama is

A C-family language with C#-like syntax, **no garbage collector** (RAII / deterministic destruction),
and **explicit named parameters**. It **transpiles to portable C** — so it runs anywhere C runs (native
on every platform; the browser via WebAssembly) with no .NET/runtime baggage.

## Goals

1. **Self-hosting eventually.** Developers should not need extra tooling; the compiler should ultimately
   be written in kama and bootstrap through the C transpiler. *Long-term — requires strings, collections,
   maps, file I/O, and tagged unions in the language first.*
   - *Tied to this:* the built-in containers/smart-pointers are compiler intrinsics with hand-tuned
     **C** runtime bodies. Generics unify the *surface* but keep those C bodies. **Reimplementing them
     as kama generic library types** (the Rust-`Vec` "unsafe core, safe API" model, in `unsafe`/`Ptr`)
     is a possible *later* step. It buys nothing for runtime performance (monomorphization makes both
     identical) and isn't needed for the language to be complete — its payoff is *this* goal,
     self-hosting (a kama stdlib for a kama compiler). The generics engine is built so this is a
     no-rework continuation (swap a generic type's body source from C-macro to kama), never a redo.

2. **Fast compiles: single-pass, parallelizable.** Parsing stays essentially single-pass. Speed comes
   from per-file parallelism — parse + emit each translation unit independently, then compile the
   generated `.c` files in parallel. The module system should keep files independently parseable. *(True
   intra-file parallel parsing is not a goal — the win is per-file + parallel C compilation.)*

3. **Predictable allocation/deallocation, no GC.** Lifetimes are deterministic (RAII). Allocation is
   explicit in the generated C. Arena/pool allocators arrive as library types for the engine.

3a. **No raw pointers in the *safe* surface.** The safe kama surface never exposes raw pointers or raw
   memory. Heap and buffers are reached only through safe abstractions: **collections** (`FixedArray<T>`/
   `DynamicArray<T>`/`string`) and the **smart-pointer family** (`Owned<T>` unique, `Shared<T>` ref-counted,
   `Weak<T>`). These are compiler-known intrinsics whose unsafe internals (raw pointers,
   `malloc`/`free`) live ONLY in `kama_runtime.h` — the Rust-`Vec`/Swift-`FixedArray` model: unsafe core, safe
   API. Indexing is **bounds-checked** (traps, not UB). The one deliberately contained exception is the
   **`unsafe { }`** block + `Ptr<T>` at the **FFI boundary**: a narrow, greppable seam for talking to C
   (GPU/OS APIs — the whole point of transpiling to C), never general-purpose escape, and the safe surface
   never sees it. (Self-hosting the compiler in kama — goal #1 — is the other place a contained escape may
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

3c. **Ownership is the type axis — `value` / `resource` / `view` / `contract`.** kama organizes types by
   *what they own*, not the C/C++ `class`/`struct`/`pod` legacy. Every declaration is `type <kind>
   Name` (the `type` marker, parallel to `fn`): a **`type value`** owns nothing (raw data, **copied**;
   the stricter cousin of a "value type" — no smuggled shared refs; may still encapsulate private
   fields to guard an invariant). A **`type resource`** owns something, or has identity (**moved**,
   RAII-dropped). A **`type view`** *borrows* a range of memory it does not own — a non-owning,
   **stack-only** slice/span (the flagship is `View<T>`; C# `ref struct`). It copies like a value but is
   a *second-class borrow*: the escape check forbids it from being a field, a collection element, or an
   escaping return, so — with no borrow checker — it can never dangle. A **`type contract`** is a
   public-only guarantee a type satisfies — kama's word for an interface (also a borrow, of one object).
   Polymorphism's goal is **substitutability, not reuse**: inheritance bundles the two,
   so kama unbundles them — **generics** give reuse (zero-cost monomorphization), **contracts** give
   substitutability, and `type virtual`/`abstract resource` is only for sharing *implementation* up an
   owned hierarchy. Hand-offs follow **"every kind is movable; the default is declared, a marker
   overrides"**: each kind has a natural bare hand-off (move for `Owned`/`resource`, retain for
   `Shared`/`Weak`), a `Copyable` resource declares its bare default at opt-in (`Copyable(bare: give|copy)`),
   and `give`/`copy` override it. The kind words `value`/`resource`/`view`/`contract` are
   **contextual** (they name a kind only right after `type`), so they stay ordinary identifiers
   everywhere else. **`type enum`** is the fifth kind — a plain variant set or a tagged union — and takes
   the same `implements` clause as the rest; `enum` is the one kind word that is a reserved keyword,
   because it predates the marker. **`type intrinsic`** is the sixth — the kind a *primitive* is, which
   exists so `int32` and `string` can declare their own conformances (`type intrinsic <int8, …, int64>
   implements Comparable<This>`, one body for a whole set of widths) instead of having them reached in
   from outside. Two hidden kinds had no spelling, and giving them one deleted the mechanism that had been
   papering over the gap rather than fencing it. *(Full model in `docs/TYPE_MODEL.md`.)*

3d. **No exceptions — fallibility is a value.** There is no `throw`/`try`/`catch` and no stack unwinding.
   A operation that can fail returns its outcome as a value: **`Optional<T>`** (absence) or **`Result<T, E>`**
   (failure), which `match` forces the caller to handle. This has a direct consequence for construction:
   **constructors are infallible** — trivial, in-place field setup that cannot fail (which is also why the
   ctor keeps its in-place `void ctor(T*)` ABI: there is nothing to signal). **Fallible resource acquisition
   is a `static fn` factory returning `Result<T, E>`** (`Buffer::create(size:) -> Result<Owned<Buffer>, E>`):
   the fallible work lives there, and on failure it returns `Err` *before* the resource exists — so no
   half-constructed object can escape and the invariant "if you hold one, it's valid" holds by construction.
   A type with a *meaningful* inert state may instead start valid-but-inert and expose a
   `bring_up(): Result<…>`. (This composes static methods + `Result` + `Owned` + RAII;
   see `tests/fallible_factory`.)

3e. **No lifetime tracking — no borrow checker.** kama does not track lifetimes or prove at compile time
   that a borrow outlives its referent (the machinery Rust pays for `&`-safety). Safety comes from
   *ownership* instead: what you keep is owned (`Owned`/`Shared`/`Weak`, RAII), and a **borrow is
   scope-local** — a `ref`/`out` or a bare `contract` value may be a parameter or a local but may **never
   escape** (no storing it in a field, returning it, or putting it in a collection; the escape check rejects
   those, so it can't dangle). To persist or share, **own it**: widen a concrete/derived into an owning
   `Shared<Contract>` / `Shared<Base>` handle — the upcast (destruction stays virtual, so no slicing). The
   deliberate trade: Rust-grade non-null + ownership discipline **without a GC and without a borrow checker**
   (unlike the GC/ARC of Kotlin/Swift/Dart, or Rust's lifetime annotations).

4. **One way to do a thing. Favor simplicity.** Unlike C++'s many syntaxes for one concept, kama
   prefers a single, obvious construct. Resist redundant syntax. (Already: named args only — no
   positional; one form per construct.)

5. **Explicit over implicit.** Named parameters, explicit `ref`/`out`, explicit types, explicit casts
   for narrowing. Prefer surfacing intent over inferring it.
   - **Greppable / self-describing syntax.** Every declaration and dangerous operation is marked by a
     keyword or operator you can search for: `fn` on every function/method, `unsafe { }` for raw memory,
     the `…Ptr`/smart-pointer families for pointers, and `::` for scope resolution vs `.` for instance
     access. Intent is visible to a human, a tool, or an LLM at a glance — no guessing which `Type name(`
     is a declaration vs a call, or which access crosses a namespace vs an object.

6. **Scripting / no-compile iteration eventually.** A `kama run` that transpiles and runs instantly
   (e.g. via TinyCC) gives sub-second iteration while reusing the single C backend — preferred over a
   separate interpreter. A tree-walking interpreter / hot-reload are larger, later options.

7. **Self-describing.** The grammar (`kama.y`) is the single source of truth — the compiler embodies
   the BNF. Generate machine-readable grammar/spec from it (`docs/grammar.bnf` via `tools/gen-grammar`)
   so the language always describes itself. A future `kama describe --json` exposes the language surface
   (keywords, types, builtins, grammar) for tools.

8. **MIT licensed.** Permissive and embeddable — see `LICENSE`.

9. **LLM-discoverable on day 1.** AI assistants can read the current syntax, semantics, and library
   availability from a canonical, always-current entry point (`llms.txt` at the repo root → grammar,
   spec, goals, examples, runtime API). Docs are generated from / point at source of truth so they never
   drift.

10. **Polymorphic backends — one frontend, many renderers.** kama is not wedded to the C transpiler; that
   is *one* rendering of a shared, semantically-lowered IR. The same front end (parse → type-check →
   ownership/move analysis, proven **once**) feeds multiple **opt-in** backends: portable **C** (the
   portability moat, kept always), **direct WASM** (self-contained web/scripting), a **bytecode VM + REPL**
   (self-contained native, zero external toolchain), and — only if a real hot path demands it — **native
   codegen** (the original LLVM ambition, revisited via a *stable* backend, not LLVM's moving API). Each
   backend is opt-in *tooling*, not a monolith — you build in the renderers you want. This is what makes
   goal #6 (scripting) and goal #1 (self-hosting) the **same** project: port the runtime into kama once,
   every backend inherits it. *The plan — and the IR-refactor that is the real work — live in
   `docs/ROADMAP.md` §7.*

## Production-grade build & debugging

- **Debug / Release configs.** `kama build` defaults to debug (`-g -O0`, `#line` on, asserts on);
  `--release` opts into optimized (`-O2`/`-Oz`, `-DNDEBUG`, stripped, no `#line`).
- **IDE breakpoint debugging (VSCode first).** Set breakpoints in `.kama`, step, and inspect the call
  stack + locals — enabled by the `#line` directives mapping generated C back to `.kama` and by locals
  keeping their kama names. The VSCode extension ships a CodeLLDB launch config + Build Debug/Release
  tasks. WASM debugging works in the browser via emscripten source maps.

## Working principles (see `.claude/skills/`)

Development of kama follows two vendored guidance skills that reinforce goals #4 and #5:
- **karpathy-guidelines** — think before coding, simplicity first, surgical changes, goal-driven.
- **ponytail** — lazy-senior-dev YAGNI ladder: write the least code that works.

## Current status

**The language feature set is complete.** What remains before the 1.0 tag is documentation and
release polish, not language work. This section is intentionally brief so it doesn't drift — the
feature reference is `docs/SPEC.md` and the forward plan is `docs/ROADMAP.md`.
