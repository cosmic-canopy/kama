# kama roadmap

**The order of work, and nothing else.** One row per item: what it is, and a link to the reasoning in
[ROADMAP_DETAIL.md](ROADMAP_DETAIL.md). The language's **history** lives in the git log; what the language
**is** lives in [SPEC.md](SPEC.md).

> **Keep this file short.** No reasoning here — it goes in [ROADMAP_DETAIL.md](ROADMAP_DETAIL.md), whose
> header carries the full maintenance rule (where a shipped item's record goes, and what may stay behind).
> This file drifted to 1,279 lines once by absorbing that prose, at which point "what is next" stopped
> being answerable without reading all of it. `tools/check-roadmap.sh` holds the split down.

## The shape

- **1.0 — language complete.** The surface is feature-complete; the tag is the API-stability point, so
  anything source-breaking lands before it or waits for 2.0.
- **1.x — systems & runtime.** Capabilities built ON the finished language: stdlib reach, serde back ends,
  MCU toolchain packaging, engine/GPU library work. Mostly library + codegen, little new syntax.
- **2.0 — dual-mode scripting** (flagship): the same language compiled OR scripted, via a shared IR feeding
  C, direct-wasm and a bytecode VM — [§7](ROADMAP_DETAIL.md#s7).
- **Concurrency** — the primitives are done ([SPEC.md](SPEC.md#concurrency-)); what is left is libraries on
  them, the job system and the event-loop scheduler — [§6](ROADMAP_DETAIL.md#s6).
- **Engine track** (product north star): a portable WebGPU game engine — a **separate product built on
  kama**, not part of it — [§8](ROADMAP_DETAIL.md#s8).

⚠️ **Performance invariant across all of the above:** kama is at C parity today, and the native/release tier
(`kama → C → clang/emcc`) stays exactly as fast — untouched. Multimodal is strictly additive.

---

## NOW — what is being worked on

**The priority list, in order.** Everything here is unblocked and fair game today.

**On the 1.0 tag:** it is the maintainer's call, not a checklist this file owns. This paragraph used to
say **explicit SIMD is the last source-visible gap** and must land before the tag. That row has now
**shipped in full** — the wasm `-msimd128` flag, the `Simd<T, comptime N>` type with shuffles, masks and
integer lanes, and the derived `SIMD128` flag ([SPEC.md](SPEC.md) *Explicit SIMD*; the measurements are
in [§2](ROADMAP_DETAIL.md#s2)) — and none of it was source-breaking. The language is complete and it
could be cut at any point; the current intent is to cut it once the stdlib rows below are done, so the
first stable release ships with the reach to match.

**The last source-breaking item has shipped.** Compile-time values left the generic list in `0.9.141`:
`<…>` holds types, `comptime(int32 N)` declares values, `#(4)` passes them ([SPEC.md](SPEC.md)
*Generics*). Everything remaining on this list is additive and can land in any 1.x, so the tag is no
longer waiting on a break — only on how much stdlib reach the maintainer wants in the first release.
Ordering the rows below is the maintainer's call.

**Size** is a batching hint, not a commitment: **S** fits beside others in one session · **M** is about a
session · **L** is several · **XL** wants its own design doc before any code. It is read off the linked
detail, so it is only as good as that reasoning: `?` marks a row the detail itself says is unprobed, and
**`—` means never scoped** — sizing work nobody has looked at would be invention, not estimation.

| # | item | size | detail |
|---|---|---|---|
| 1 | **BUG: a GENERIC resource reassigned from a fresh rvalue never drops the old value** — `slot = fresh();` on an `Owned<T>`, a `Shared<T>` or a user `Box<T>` leaks what the slot held, in SAFE kama, while the identical shape on a non-generic resource drops (measured: 503 MB vs 1.8 MB over 2,000 iterations; a dtor-counting user generic returns 0 where its non-generic twin returns 2). The fresh-rvalue path in the assignment emitter is restricted to a NON-generic resource on the claim that "the value-producing path further down already drops the old value" — that path is the COLLECTION one, and a generic resource falls through both into a plain store. `tests/give_assign.kama` states the correct rule in its comment and passes either way: it returns 42, and a leak is invisible to an exit code. ⚠️ A prerequisite of the owning-`static` row: the static's type IS a generic resource | S | [§2](ROADMAP_DETAIL.md#s2) |
| 2 | **`reproducible-float` does not propagate from a dependency** — a defect in `0.9.169` as shipped. The key was grouped with `no-heap`/`webgpu` as a whole-artifact decision, and it is not alike: those two can refuse a consumer's code or demand an SDK, this one can only turn contraction OFF. A dependency saying "my arithmetic must be reproducible" states a requirement of its own code, which the consumer compiles — and the first consumer's raw `-ffp-contract=off` cflag already propagates, so migrating to the first-class key would silently void the guarantee they built a tripwire for | S | [§2](ROADMAP_DETAIL.md#s2) |
| 3 | **BUG: a statement-form `match` over a width-pinned enum whose arms all `return` fails `-Werror,-Wreturn-type`** — kama's own return-path analysis accepts the function (correctly: it proved the match exhaustive), then the emitter closes the `switch` with `default: break;` and clang sees a fall-through. The VALUE-producing form was fixed for exactly this shape (the panic arm in `emitMatchDefaultArm`) and its comment declares the statement form correct byte-for-byte; the unpinned sibling builds. A check/build divergence, found by the first external project (their KB-12), who rewrite every such function to assign in the arms | S | [§2](ROADMAP_DETAIL.md#s2) |
| 4 | **The raw seam says nothing, three ways** — through an `UnsafePtr<T>` element: `p[0].m()` is "cannot resolve the receiver" (their KB-14), `p[0] = Big.sized()` silently leaks the old value (their KB-15, 527 MB), and `drop(value: p[0])` compiles to a literal `(void)0;`. All one seam, and NOT a bug in what `p[i]` means: a local raw element is deliberately untyped to ownership so `nd[i] = od[i]` in `DynamicArray.growTo` stays a bitwise relocate — measured, widening `exprClass` fixes KB-14 and breaks 45 fixtures with `cannot give out of a field/element` inside `dynamic_array.kama`, the same diagnostic the consumer hit. GOALS §3a/§3e settle it: a raw pointer is the FFI seam, never general-purpose escape, and to persist you OWN it — so the answer is the diagnostics, not the semantics. Refuse `drop` through a raw element instead of emitting nothing; have the method-call message name the two spellings (borrow it through a `ref T` parameter, or own it in an `Owned<T>`/a `static`); document the seam. Declines KB-14 as filed | S | [§2](ROADMAP_DETAIL.md#s2) |
| 5 | **A module `static` cannot own a destructible resource** — `static World g = World.make();` is refused ("no destructible resources yet", the gate's own words: "v1 has no static-dtor seam"), and it was UNTRACKED against §2's own policy. It is the root need behind the first consumer's calloc'd `World` and therefore behind both of their raw-seam bugs: on the web `main`'s frame is unwound while the rAF callback lives, so the one owner that outlives a frame is a static, and today it may not own. The goals' shape is `static Optional<Owned<World>> g;` — absence in the TYPE (§3b), `match` forces the dead case as `Weak.tryUpgrade` does, and the C callback receives a pointer BORROWED from the owner and turns it into a `ref World` parameter at one `unsafe fn` (the ref-parameter shape measured working and dropping today). Needs the static-dtor seam at both teardown sites (the synthesized `main` after `kama_main`, and the isolate trampoline — statics are per-isolate), the initializer rule to accept `Optional::None`, and the generic-resource reassignment row above | M | [§2](ROADMAP_DETAIL.md#s2) |
| 6 | **BUG: a file must import a type it never names** — declaring a local of a class whose FIELD is another user type (`Holder` with a `Kind k`) fires "`Kind` is declared in `decl.kama` and this file does not import it", because the definite-assignment walk re-resolves every field's type through `cType` at the READ site, so the declaring file's spelling is judged as the consumer's own reference. Never fires when the value is only produced or its field read inline; a resource holder reports it three times. The fixed `InlineArray`-size bug's family: collect-time is the fix, the read-site scope swap is the measured wrong one. ⚠️ Its position is the field's line in the DECLARING file under the consumer's path (a 6-line file blamed at 7:0) — the wrong-file class of the next row by a second mechanism, `site->line` of a foreign node rather than `diagFile()`. Found by the first external project (their KB-13): adding a typed field to a widely-held value type breaks every file that merely holds one | M | [§2](ROADMAP_DETAIL.md#s2) |
| 7 | **BUG: a generic type's destructor naming a module `static` is emitted before the static's declaration** — `~Box() { drops = drops + 1; }` passes `kama check` and fails in clang with `use of undeclared identifier`; the non-generic twin builds. The generic instance body lands in the header pass ahead of the statics. Found reducing the reassignment row; a check/build divergence of the family the harness's agreement phase exists for | S | [§2](ROADMAP_DETAIL.md#s2) |
| 8 | **A diagnostic can name the USER's file at a line that does not exist in it.** `diagFile()` falls back to the file being compiled for a body emitted in the header pass, so an error raised inside a prelude/stdlib body is stamped with the user's path and the LIBRARY's line number — an 8-line repro blamed at line 69. Found by the first external project (their KB-10 note) and worked around twice already inside the no-heap analysis, which stores no position rather than a wrong one | M | [§2](ROADMAP_DETAIL.md#s2) |
| 9 | **`exprClass` is unreliable inside a generic instantiation**, so the class-identity rule stops at the boundary of one. In `Owned<T, A>.adoptIn` the FIELD answers the substituted `BumpAllocator` while the `A allocator` PARAMETER still answers the default `GlobalAllocator`. Same family as the generic-scan drift. Until it is fixed, `Mat4 m = someVec4;` is caught everywhere EXCEPT inside a generic body | M | [§2](ROADMAP_DETAIL.md#s2) |
| 10 | **No capturing closures** — a handler must be a `type resource` carrying its captures plus a `BindableFunctionPtr`, which is the shape a UI event table wants least. The first consumer works around it and says it will bite properly at their UI milestone. ⚠️ Rowed here for the first time: it was cited as already being on this list and was not | — | [§2](ROADMAP_DETAIL.md#s2) |
| 11 | **No incremental build** — every build recompiles everything. ⚠️ Largely already answered and never rowed: `zig cc` has a content-addressed per-TU object cache (measured 4.13 s cold, **0.11 s after editing one file**), so a bundled install is incremental today and kama's own object cache is mostly moot. What is left is the SLIM install, which uses clang and has no cache | — | [§9](ROADMAP_DETAIL.md#s9) |
| 12 | **Job system / event-loop scheduler** — libraries on the shipped concurrency primitives; the pool is sized, **scheduling** is what is missing | ? | [§6](ROADMAP_DETAIL.md#s6) |
| 13 | **Stdlib parity M2b** — fs + path, io handles + `lines()`, sleep + wall clock, DNS | — | [§1](ROADMAP_DETAIL.md#s1) |
| 14 | **Stdlib parity M2c** — `std::random`, `std::encoding` | — | [§1](ROADMAP_DETAIL.md#s1) |
| 15 | **Modular / opt-in stdlib** — whether emit-on-instantiation + `--gc-sections` pruning scales, or explicit per-module opt-in / dead-function elimination is wanted before the stdlib grows | — | [§3](ROADMAP_DETAIL.md#s3) |
| 16 | **`std::io` transform adapters** — compression et al., composing with serde and net | — | [§1](ROADMAP_DETAIL.md#s1) |
| 17 | **`std::net`** — IPv6, UDP multicast | — | [§2](ROADMAP_DETAIL.md#s2) |
| 18 | **`std::process`** — live/streaming child-stream reads | — | [§1](ROADMAP_DETAIL.md#s1) |
| 19 | **Serialization follow-ups** — deserialize breadth, more back ends, `@deprecated` | — | [§4](ROADMAP_DETAIL.md#s4) |
| 20 | **Windows** — long-path support; suite wall-clock (~906 s vs ~75 s in the container) | — | [§1](ROADMAP_DETAIL.md#s1) |
| 21 | **MCU toolchain packaging** — board presets, vendor-HAL glue, a real-hardware flash pass; AVR later | — | [§5](ROADMAP_DETAIL.md#s5) |
| 22 | **No way to give an `extern fn` or an `expose fn` a symbol name different from its kama name** — `@linkName("…")`, the peer of Rust's `#[link_name]`/`#[export_name]`. Distinct from the spelling problem contextual `type` closed: that was "my grammar cannot spell this name", this is "this symbol is named something else". Today an `extern` must be spelled exactly as C names it, so a C symbol that collides with a kama KEYWORD (not merely an identifier) has no binding at all, and `expose` cannot choose its exported name. ⚠️ **Do not call it `@cname`** — kama's only backend is C today, but the 2.0 bytecode VM has no C names; name it for the concept, not the target | S | [§2](ROADMAP_DETAIL.md#s2) |
| 23 | **`csources` compiles C, not C++** — a `.cpp`/`.cc`/`.cxx`/`.mm` entry is refused BY NAME with the two obstacles: every input shares one flag prefix (`-std=c11` plus the C-only warning promotions), so a C++ TU needs its own, and a C++ link needs the target's C++ runtime library (`-lc++` vs `-lstdc++`), which varies per target and is a table kama does not have. A `.m` is a third case — Objective-C is one platform's language and `csources` is project-level with no per-target tier to exclude it, so this row carries per-target `csources` too. The first consumer drives C++ and a third-party cmake project from a Makefile; the cmake half is not ours, the C++ half is | M | [§2](ROADMAP_DETAIL.md#s2) |
| 24 | **Remaining language limitations** — no bound spells "an integer primitive", so `cast<T>` in a generic is checked per instantiation (the stdlib avoids it: pass by address, move `sizeof(T)` bytes); generic free fn calling a generic free fn; a generic call in a field's DEFAULT INITIALIZER (neither discovery pass walks one); generic `enum` members; unresolved type names inside generic arguments; `@compileFor` is whole-declaration only, so it cannot gate a single method or one `implements` block; contract-refinement thunks; `Fixed<B,const F>` implementing `Real`; `@align`/`@packed` not reaching a tagged `enum` (an outer `packed` misses the per-variant payload structs); an `expose fn` with neither `@callerThread` nor `@foreignEntry` is not a region, so a host-called body reading a mutable static is unchecked (a source break for 11 in-tree files if required) | — | [§2](ROADMAP_DETAIL.md#s2) |
| 25 | **Collections knobs** — HashDoS-resistant keyed hashing; zero-size-field elision; thin smart-ptr handles | — | [§5](ROADMAP_DETAIL.md#s5) |
| 26 | **Performance** — bench cohort (add Zig), serialization benchmark track, devirtualization ladder, CPU-tuning knob | — | [§9](ROADMAP_DETAIL.md#s9) |
| 27 | **Hot-reload library** — `dlopen` + file-watch + fn-pointer rebind. Both compiler primitives already ship | — | [§8](ROADMAP_DETAIL.md#s8) |
| 28 | **Safe `std::gpu` binding wrapper** — wgpu handles→RAII `type resource`s (as `std::net` wraps sockets) + a typed acquire result naming `Occluded`; the seam's size accessor and discarded event queue. Scope: exactly the handles the seam already touches, nothing above them | M | [§8](ROADMAP_DETAIL.md#s8) |
| 29 | **`std::input`** — the seam pumps the window event queue and throws it away, on both targets: no keyboard, mouse, wheel, pointer-lock, resize, focus or gamepad. Peer of `std::gpu`, and the reason the first engine on kama derives its own window | M | [§8](ROADMAP_DETAIL.md#s8) |

## LATER — tooling & ecosystem

Most of this gates on the repo going public.

| # | item | detail |
|---|---|---|
| 30 | **Registry — hosted deployment (M3.3)** + mandatory verification and the trust model | [§10](ROADMAP_DETAIL.md#s10) |
| 31 | **Editor/registry registrations** — Zed extension registry, nvim-treesitter, linguist, Helix upstreaming, Marketplace publish | [§10](ROADMAP_DETAIL.md#s10) |
| 32 | **tree-sitter accepts 78 of the 80 reserved words as a binding name** — `Thing else = …` renders as a valid declaration in every editor on this grammar, and the compiler then rejects it. The two reserve differently by construction: `kama.l` consults one table at every identifier, tree-sitter extracts keywords CONTEXTUALLY and a binding site expects `$.identifier`. ⚠️ `check-treesitter.sh` cannot see this class, and the one fixture that looks like it covers it passes on its USE site, not its declaration | [§10](ROADMAP_DETAIL.md#s10) |
| 33 | **LSP residuals** — one build configuration per server process; the prelude-analysis floor per keystroke | [§10](ROADMAP_DETAIL.md#s10) |
| 34 | **`kama fmt`** — a native formatter. Substrate settled: use the compiler's own front end, **not** tree-sitter | [§10](ROADMAP_DETAIL.md#s10) |
| 35 | **Debugger value formatting** — render `string`/`Optional`/collections as kama values, not their emitted-C form | [§10](ROADMAP_DETAIL.md#s10) |
| 36 | **`kama query` residuals** — no `callers-of`/`implementors-of`, no stdin/unsaved-buffer mode | [§10](ROADMAP_DETAIL.md#s10) |

## FUTURE — the big arcs, in this order

The 2.0 work. Nothing here starts before the NOW list is done.

| # | item | detail |
|---|---|---|
| 37 | **Editor tooling** — the front end as a reusable query API; everything later rides on it | [§10](ROADMAP_DETAIL.md#s10) |
| 38 | **Scripting / multimodal — the flagship 2.0.** First step: refactor the C emitter behind an abstract backend interface | [§7](ROADMAP_DETAIL.md#s7) |
| 39 | **Self-hosting — the capstone, LOWEST priority.** A maturity milestone, not an enabler | [§7](ROADMAP_DETAIL.md#s7) |
