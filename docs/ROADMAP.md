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

Recommended order: **M26e → M26f → M27 → M28 → M29 → Step 7 → tag.** Rationale: close the
borrow-safety arc first (M26e, small) and enable owned-interface storage (M26f — the engine needs
it); then the type-system push (generics → sum types — the dependency spine,
since `Optional<T>` *is* a generic tagged union — and generics is the biggest/riskiest piece, best
done early with full runway); then operators as a visible engine-math finale; then docs. M29 is
independent — pull it to right after M26e if validating engine-viability sooner beats de-risking
generics early (recommended against).

*Numbering note:* M0–M26 are historical (done/committed). The remaining work is numbered by build
order, so generics — long reserved as "M23" but never built — becomes **M27**, and operators (a few
notes back called "M27") becomes **M29**. Each may sub-decompose (M27a/b…) like M25/M26 did.

1. **M26e — borrow escape check** (second-class borrows). ✅ **DONE (v0.1.35).** Closed the last
   memory-safety hole; with the no-null work already shipped (M26a–d), fully delivers GOALS §3b.
   *(The `Weak` Try-pattern that was bundled here moves to M28, where `Optional` exists.)*
   - **Rule (DECIDED 2026-06-30):** **"borrow is parameter-only; storage requires ownership"** — no
     lifetime annotations (the simplicity bet). Finding: borrows are *already* second-class by
     construction (`ref`/`out` is only a param/arg modifier — no `ref` return/local/field). So M26e
     was tight: it turned the remaining escape vectors — a bare interface value (which borrows its
     object) **stored in a field, returned, or used as a collection element** — from ugly C errors /
     a silent `List<IShape>` hole into one clean, consistent cstar diagnostic guiding you to own the
     object (`Shared<IShape>`, explicit, never an implicit box). Interface *params/locals* (borrows)
     are unaffected. **No `ref` returns** (so no `ref T operator[]`); a returnable "reference" is
     always an owned smart-ptr **handle** (mutate via auto-deref), and value-container elements are
     mutated by the owner's own methods (tell-don't-ask; only *escaping* a borrow is banned). A
     future callback-lend (`grid.mutateAt(i:, with: (ref Cell c) => …)`) needs inline closures →
     post-1.0. Impl: `rejectStoredInterface` helper at emitStruct/prototype sites + registerCollection;
     xfail iface_field/iface_return/iface_collection. 110/110.

   **M26f — owned-interface storage** *(new; before 1.0, after M26e).* Smart-pointers over an
   interface element (`Shared<IShape>` / `Owned<IShape>` — a fat-pointer element, a real extension
   of the smart-ptr machinery) + interface fields / returns / collections, so polymorphism can be
   *stored* (the engine's `List<IDrawable>` scene). Bare stored `IShape` stays a clear error;
   ownership is explicit, never implicitly boxed. *(May instead fold into M27 generics — decide
   when we get there.)*

2. **M27 — generics** *(the long-reserved "M23", renumbered to its build order).* Full user-defined
   generics: `Map<K,V>`, multi-param + nested (`>>` lexing). The foundational type-system feature —
   unblocks `Optional<T>`, `Map`, and every future library type. *(Was slated to defer to 1.1;
   pulled back in for a language-complete 1.0.)*
   - **DECIDED (2026-06-30):**
     - **Monomorphization**, not erasure — one specialized copy per concrete type (elements inline,
       no boxing). **Zero runtime/memory cost**; identical layout to today's intrinsics. (Erasure
       would box every element — rejected.)
     - **Architecture = "C": one engine, pluggable bodies.** Generalize the existing intrinsic
       monomorphization machinery so a generic type's method bodies come *either* from a C runtime
       macro (the built-ins — `List`/`Owned`/… keep their hand-tuned bodies) *or* from cstar source
       (user types). Full surface unification (`List<T>` and a user `Stack<T>` declared/used
       identically), zero perf risk. Reimplementing the built-ins *in cstar* (option "B") is deferred
       to self-hosting and is a no-rework continuation — see GOALS §1.
     - **Constraints = interfaces only, inline syntax.** `class Map<K: IHashable + IComparable, V>`;
       `+` means **AND** (all listed interfaces; no disjunction — static dispatch needs the exact
       method set). An interface **bound** is compile-time only → calls monomorphize to **static
       direct calls (zero-cost)**, distinct from an interface **value** (runtime fat pointer). No
       base-class bounds (use an interface). **Generic functions** too, not just types.
     - **give/copy** composes for free — the markers dispatch on the *concrete* type at each
       monomorphized site (a `T` that resolves to `Owned` moves, a pod copies). Confirm in impl.
   - **DECIDED — the `This` keyword** (a contract's self-type). When a contract must name its *own*
     implementing type (`Map`'s key needs "K equals K"), use **`This`** — chosen over Rust/Swift's
     `Self` because it pairs with cstar's `this` value (`this : This` = value : type), follows the case
     convention (types PascalCase), and is self-documenting per GOALS §5/§7. `interface IEquatable { fn
     bool equals(other: This); }` → used as `K: IEquatable`; also enables self-returning methods (`fn
     This clone();`). Distinct from a generic-interface *param* (`ISequence<T>` = "some OTHER type") —
     `This` = "my OWN type"; both can coexist. Resolved by a substitution pass at `implements` + each
     monomorphization. Ship **named-method** contracts (`equals`/`compareTo`) in M27; they *become*
     operators at M29. *(Lands in SPEC/KEYWORDS when M27 builds — per-milestone docs.)*
   - **Tech:** `>>` token split for nested generics (`Map<K, List<V>>`, `List<Shared<T>>`).
   - **Forward-compat (M27 build notes, from M28/M29 — neither changes M27's design):** make the
     monomorphization engine general over type *declarations* (class **and** enum/variant), since
     `Optional<T>`/`Result<T,E>` (M28) are generic tagged unions and the first consumers. M29's
     operator-interfaces (generic math) will *reuse* this milestone's interface-bound + `This`
     mechanism — M27 lays the groundwork, no conflict.

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
     blessed mechanism (vs also `Result<T,E>`?). Depends on M27 (generics).

4. **M29 — operator overloading + full static methods.** Ergonomic `pod` math — `Vec2 + Vec2`,
   `Vec2::dot(left:, right:)` — the engine's Tier-0 dependency. The value model (M26) already
   treats `Vec2 c = a + b` as a cheap pod copy. Validated by a first Vec2/3/4 + Mat4 library.
   - **Design Qs:** Operator-method **syntax** — operators are the *sanctioned exception* to
     named-args-only (a binary op has exactly two operands, positional by nature); how do we spell
     it (`fn Vec2 operator+(Vec2 rhs)`? a special `operator` member? free-function form?). **Which
     operators** (arithmetic, comparison `== < >`, index `[]`, unary `-`/`!`, compound `+=`?). The
     **`static` method form** (`Type::method`, no `self` — `static` is only partial today, M19).
     Should `==` tie into a structural-equality default for `pod`s? **Operators-in-interfaces** for
     generic math (`interface IArithmetic { fn This operator+(This rhs); }`) reuse M27's interface-bound
     + `This` mechanism — so a generic `T: IArithmetic` gets `+`. (Does NOT impact M27's design; M27
     ships the named-method form, M29 makes the methods operators.)

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

A portable lightweight **WebGPU** game engine. Tiers: **math types** (Tier 0 — unblocked by M29)
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
