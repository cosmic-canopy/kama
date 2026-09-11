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
| 1 | **`std::time` calendar** — civil-from-days, ISO-8601 format/parse, no zone database; whole or not at all | — | [§1](ROADMAP_DETAIL.md#s1) |
| 2 | **No incremental build** — every build recompiles everything. ⚠️ Largely already answered and never rowed: `zig cc` has a content-addressed per-TU object cache (measured 4.13 s cold, **0.11 s after editing one file**), so a bundled install is incremental today and kama's own object cache is mostly moot. What is left is the SLIM install, which uses clang and has no cache | — | [§9](ROADMAP_DETAIL.md#s9) |
| 3 | **Job system / event-loop scheduler** — libraries on the shipped concurrency primitives; the pool is sized, **scheduling** is what is missing | ? | [§6](ROADMAP_DETAIL.md#s6) |
| 4 | **Modular / opt-in stdlib** — whether emit-on-instantiation + `--gc-sections` pruning scales, or explicit per-module opt-in / dead-function elimination is wanted before the stdlib grows | — | [§3](ROADMAP_DETAIL.md#s3) |
| 5 | **`std::io` transform adapters** — compression et al., composing with serde and net | — | [§1](ROADMAP_DETAIL.md#s1) |
| 6 | **`std::net`** — IPv6, UDP multicast | — | [§2](ROADMAP_DETAIL.md#s2) |
| 7 | **`std::process`** — live/streaming child-stream reads | — | [§1](ROADMAP_DETAIL.md#s1) |
| 8 | **Restricted-private contract members** — *to consider*, not scheduled: a contract may declare a member non-public, with a `friend` grant naming who may reach it, and an implementer then matches that visibility instead of being forced `public`. It is no longer a serde blocker — that justification was written before the container seam was measured, and the seam turned out to need no contract at all. What stands on its own is expressiveness: binding types together with a guarantee that is not part of the public surface. ⚠️ **`private` on a contract member is ACCEPTED AND INERT today** (measured `0.9.269`): it parses and the marker is silently dropped, so with a `public` implementer a call through a contract-typed value reaches it. That is the sibling `0.9.266` left behind when it refused `friend` on a contract on the premise that a contract *cannot* have a private member. Deliberately **not fixed** — it is inert, it blocks nothing, and whether it is refused or made real is this row's decision. Both spellings already parse, so the work is emitter-only | M | [§4](ROADMAP_DETAIL.md#s4) |
| 9 | **Graph edges in a keyed container — the two containers still short of it** — `Map<K, Shared<V>>` round-trips as a graph now (`0.9.289`, `tests/ser_graph_map`), via `@serializedGraphEdges` on its `valuesMut()`. Two gaps remain, and NEITHER is about the marking. **(a) `SortedMap`** — its element write happens inside `BTreeNode`, a nested helper the graph discovery never reaches, so the leaves are written INLINE and duplicated and the read then says *"unresolved reference"*. `Map`, which writes its elements in its own body, works — which is what isolated it. **(b) `SlotMap`** — no `Serializable` half at all, so it cannot carry a graph regardless; pre-existing and unrelated. ⚠️ Written down with them: a keyed container's edges live in its VALUES only, since a key cannot be rewired in place without moving it in the ordering or the hash — so `Map<Shared<K>, V>` is a NON-GOAL, not a "not yet" | M | [§2](ROADMAP_DETAIL.md#s2) |
| 10 | **Two defects found building the graph-edge marking** — (a) an inline generic-instance ctor as the RHS of an element store bypasses the `__set` lowering and emits `__get(…) = …`, not assignable C; reduced to 11 lines, and `InlineArray<P>` works where `InlineArray<Frame<P>>` does not, with no generics needed on the holder. Fails LOUDLY; binding the RHS to a local is the workaround, used twice in `sorted_map.kama`. (b) **consumer KB-23** — an `InlineArray<Struct>#(N)` degrades to a RAW POINTER when `N` is a local `comptime` aliasing an IMPORTED one, and neither diagnostic names the declaration at fault (one says the ctor never assigns a field it fully assigns). ⚠️ **MEASURED and not in the consumer's report: FILE ORDER is the trigger**, as in KB-22 — sorting the exporting file first makes the same source build. ⚠️ `refreshStaleParamTypes` does NOT extend to it (attempted): the fold of the constant is what is early, not the field's type, so the fix is re-folding module `comptime` constants after collection. ⚠️ A running repro plus the full truth table is kept at `.scratch/kb23/` (gitignored; its README carries the file-order flip and the reverted attempt) | M | [§2](ROADMAP_DETAIL.md#s2) |
| 11 | **Consumer KB-24 — a `fnptr`'s threading contract is enforced on a PARAMETER and not on a DESCRIPTOR-STRUCT FIELD**, which is the shape real C APIs use. Handing an unannotated `fnptr` straight to an `extern fn` is refused with a good diagnostic; assigning the identical `fnptr` into a field of a `type extern value` and handing THAT to C compiles, links and runs, silently. ⚠️ REPRODUCED at `0.9.292` in 24 lines — the parameter path refuses, the descriptor path returns 0. ⚠️ It reopens the gap `@callerThread`/`@foreignEntry` exist to close at exactly the family where it matters: WebGPU, CoreAudio and miniaudio all take their callback inside a descriptor struct, which is where a callback runs on a thread kama did not create. The consumer found it because SPEC claimed the compiler checked their three WebGPU callbacks and their tree carries ZERO annotations and builds clean. **Where to look:** `checkForeignCrossing` is per-ARGUMENT and keys on the parameter's own type, so an extern struct is not an `fnptr` and passes — walk an extern struct's fields (transitively) at the crossing, which catches every construction route rather than only the assignment | M | [§2](ROADMAP_DETAIL.md#s2) |
| 12 | **Compiler defects found building AND reviewing the serde split** — ⚠️ **RE-TRIAGED 2026-09-10 against `0.9.282`; every item below was re-probed, two were dropped as fixed and two sharpened.** ✅ The stdlib `Weak<Contract>` still does not monomorphize (`weak.kama:36`, *"cannot tell which `Shared` to construct"*). ✅ A TEMPORARY passed to a contract-typed parameter still emits `&` of an rvalue — and the repro is NARROWER than recorded: `&(compound literal)` IS an lvalue in C99, so a bare `T.of(…)` argument passes; it needs a FUNCTION-CALL temporary (`use(g: makeG(k: 4))` → *"cannot take the address of an rvalue"*). The old `ObjectGraph` repro is gone with the type. ✅ A generic `ctor` in ARGUMENT position still does not infer. ✅ A `when [...]` gate naming a FREE-PARAMETER contract with a concrete argument still never holds (*"`Wrap<P>` has no method `pull`"*). ✅ An enum PAYLOAD edge still reports *"not `@generate`"* about a type that IS. ✅ An internal payload name still leaks: a read-side refusal on an enum payload says ``field `__p_w` `` where the write-side one says `` `w` ``. ❌ **DROPPED — the collection-of-edges field that "blames the container with advice that cannot be followed" is gone**: that shape compiles now (`0.9.277`/`0.9.278`). ❌ **DROPPED — "the message calls a smart pointer a collection" is FIXED at `0.9.282`**, and it had rotted twice over: the test matched any derive-less generic instance, and the text still said "a collection OF nodes is not walked yet" after collections started being walked. `tests/xfail/graph_owned_of_shared` pins the replacement. Each remaining item fails loudly; each wants a fixture or an `xfail` | — | [§4](ROADMAP_DETAIL.md#s4) |
| 13 | **Paths with spaces** — two of the C compiler's `-I` entries are unquoted while everything else on the line is quoted, so a project under `C:\Users\John Smith\` (or `/home/x/my project/`) likely hands clang a torn include path, on every platform. Found reading the command builder; not yet measured — the probe is one build from a `mktemp -d` with a space in it | ? | [§2](ROADMAP_DETAIL.md#s2) |
| 14 | **Windows path residuals after `0.9.222`** — `CreateProcessW`'s cwd/executable ≤ 260 (non-goal, a Win32 limit; document "pass absolute paths" on `Command.cwd`), the two 260-byte cosmetic buffers (`selfExePath`, `relativizeToCwd`), the long-`~/.kama` store with no guard, `longPathAware` (non-goal, registry-gated), a volume with 8dot3 names off (the linker fails as before; `lld` is the answer), and a non-ASCII `%TEMP%` breaking clang's own one-step link (toolchain; `targets.md` should say so). Verdicts written in the detail | S | [§2](ROADMAP_DETAIL.md#s2) |
| 15 | **Windows suite wall-clock** — ~1819 s on the box, 2026-09-06, against a ~75 s container figure that is older ([windows.md](platforms/windows.md) carries both dates and the caveat) | — | [§1](ROADMAP_DETAIL.md#s1) |
| 16 | **MCU toolchain packaging** — board presets, vendor-HAL glue, a real-hardware flash pass; AVR later | — | [§5](ROADMAP_DETAIL.md#s5) |
| 17 | **`csources` compiles C, not C++** — a `.cpp`/`.cc`/`.cxx`/`.mm` entry is refused BY NAME with the two obstacles: every input shares one flag prefix (`-std=c11` plus the C-only warning promotions), so a C++ TU needs its own, and a C++ link needs the target's C++ runtime library (`-lc++` vs `-lstdc++`), which varies per target and is a table kama does not have. A `.m` is a third case — Objective-C is one platform's language and `csources` is project-level with no per-target tier to exclude it, so this row carries per-target `csources` too. The first consumer drives C++ and a third-party cmake project from a Makefile; the cmake half is not ours, the C++ half is | M | [§2](ROADMAP_DETAIL.md#s2) |
| 18 | **Remaining language limitations** — no bound spells "an integer primitive", so `cast<T>` in a generic is checked per instantiation (the stdlib avoids it: pass by address, move `sizeof(T)` bytes); a generic free fn calling a generic free fn ⚠️ **RE-PROBED 2026-09-10: the CALL works with a turbofish (`idA::<T>(x: x)`); it is INFERENCE that fails** (*"argument 'x' is not a literal or a locally-typed value"*), which is narrower than this row said; a generic call in a field's DEFAULT INITIALIZER (neither discovery pass walks one); generic `enum` members (so `Optional`/`Result` cannot declare `Sendable` and are judged by payload); unresolved type names inside generic arguments ⚠️ (re-probed: the diagnostic is *"cannot tell which `DynamicArray` to construct — give the type arguments"* even when the turbofish IS written, so it advises the thing the author already did); `@compileFor` is whole-declaration only, so it cannot gate a single method or one `implements` block; contract-refinement thunks; `Fixed<B,const F>` implementing `Real`; ⚠️ **`@align`/`@packed` on an `enum` is NOT a silent miss — re-probed 2026-09-10, it is a clean refusal** naming the reason and the fix (`type enum E : IntType`), so what is left here is only whether a tagged enum should honor a layout attribute at all; an `expose fn` with neither `@callerThread` nor `@foreignEntry` is not a region, so a host-called body reading a mutable static is unchecked (a source break for 11 in-tree files if required) | — | [§2](ROADMAP_DETAIL.md#s2) |
| 19 | **Collections knobs** — HashDoS-resistant keyed hashing; zero-size-field elision; thin smart-ptr handles | — | [§5](ROADMAP_DETAIL.md#s5) |
| 20 | **Performance** — bench cohort (add Zig), serialization benchmark track, devirtualization ladder, CPU-tuning knob | — | [§9](ROADMAP_DETAIL.md#s9) |
| 21 | **Hot-reload library** — `dlopen` + file-watch + fn-pointer rebind. Both compiler primitives already ship | — | [§8](ROADMAP_DETAIL.md#s8) |
| 22 | **Safe `std::gpu` binding wrapper** — wgpu handles→RAII `type resource`s (as `std::net` wraps sockets) + a typed acquire result naming `Occluded`; the seam's size accessor and discarded event queue. Scope: exactly the handles the seam already touches, nothing above them | M | [§8](ROADMAP_DETAIL.md#s8) |
| 23 | **`std::input`** — the seam pumps the window event queue and throws it away, on both targets: no keyboard, mouse, wheel, pointer-lock, resize, focus or gamepad. Peer of `std::gpu`, and the reason the first engine on kama derives its own window | M | [§8](ROADMAP_DETAIL.md#s8) |

## LATER — tooling & ecosystem

Most of this gates on the repo going public.

| # | item | detail |
|---|---|---|
| 24 | **Registry — hosted deployment (M3.3)** + mandatory verification and the trust model | [§10](ROADMAP_DETAIL.md#s10) |
| 25 | **Editor/registry registrations** — Zed extension registry, nvim-treesitter, linguist, Helix upstreaming, Marketplace publish | [§10](ROADMAP_DETAIL.md#s10) |
| 26 | **tree-sitter accepts 78 of the 84 reserved words as a binding name** — `Thing else = …` renders as a valid declaration in every editor on this grammar, and the compiler then rejects it. The two reserve differently by construction: `kama.l` consults one table at every identifier, tree-sitter extracts keywords CONTEXTUALLY and a binding site expects `$.identifier`. ⚠️ `check-treesitter.sh` cannot see this class, and the one fixture that looks like it covers it passes on its USE site, not its declaration | [§10](ROADMAP_DETAIL.md#s10) |
| 27 | **LSP residuals** — one build configuration per server process; the prelude-analysis floor per keystroke | [§10](ROADMAP_DETAIL.md#s10) |
| 28 | **`kama fmt`** — a native formatter. Substrate settled: use the compiler's own front end, **not** tree-sitter | [§10](ROADMAP_DETAIL.md#s10) |
| 29 | **Debugger value formatting** — render `string`/`Optional`/collections as kama values, not their emitted-C form | [§10](ROADMAP_DETAIL.md#s10) |
| 30 | **`kama query` residuals** — no `callers-of`/`implementors-of`, no stdin/unsaved-buffer mode | [§10](ROADMAP_DETAIL.md#s10) |

## FUTURE — the big arcs, in this order

The 2.0 work. Nothing here starts before the NOW list is done.

| # | item | detail |
|---|---|---|
| 31 | **Editor tooling** — the front end as a reusable query API; everything later rides on it | [§10](ROADMAP_DETAIL.md#s10) |
| 32 | **Scripting / multimodal — the flagship 2.0.** First step: refactor the C emitter behind an abstract backend interface | [§7](ROADMAP_DETAIL.md#s7) |
| 33 | **Self-hosting — the capstone, LOWEST priority.** A maturity milestone, not an enabler | [§7](ROADMAP_DETAIL.md#s7) |
| 34 | **Boxing a `string` into a contract emits an undefined vtable** — `serializeJsonBuffer(v: aString)` references `kama_string__as_Serializable` that nothing defines: `kama check` passes clean and clang reports it. Narrow (every other primitive boxes fine) and pre-existing. ⚠️ Cause, measured: a `type intrinsic` conformance is recorded **static-dispatch-only**, so no fat-pointer vtable is emitted. A scalar target escapes through `emitPrimWidenVtables`' by-value thunks; `kama_string`'s methods already take a pointer, so it needs the ordinary class vtable — which no emitter reaches, because the per-unit sweep walks declaration nodes and an intrinsic has none. Exempting it there pulls in every other contract `string` implements ("unknown contract in implements"), so this wants the narrow fix: the ONE contract actually being boxed. `tests/ser_scalar_root` carries the line to re-add | S | [§4](ROADMAP_DETAIL.md#s4) |
| 35 | **A refusal that fires from stdlib source cannot name the author's file** — three serde diagnostics report against `json.kama`/`dynamic_array.kama` at a line the author never wrote, and two carry a `-` row in `tests/xfail/DIAGNOSTIC_LINES` because there is no position in the fixture to assert. ⚠️ It is not "report the bound instead": measured twice this session that a graph fact (`reachesPointer`, `graphDeserialize`) is settled AFTER collection while a generic bound is judged DURING it, so the refusal genuinely cannot move earlier. What is wanted is attribution — a diagnostic raised inside a monomorphized stdlib body naming the INSTANTIATION site | ? | [§4](ROADMAP_DETAIL.md#s4) |
| 36 | **`@noheap` cannot cross a contract slot even when the callee is knowable** — a `@noheap` caller cannot use serde at all: the refusal is `Deserializer.readI32`, a contract member not declared `@noheap`, so nothing about scalar reads is provable. Two answers, and the second is the better one. Declaring the scalar members `@noheap` is the shipped mechanism (every backend is then checked against it, and `readString`/`writeString` stay unmarked because they genuinely allocate) — but `serializeJsonBuffer` CONSTRUCTS its backend and boxes it, so the concrete implementation is statically known and the compiler could prove the callee and check that body directly, with nothing declared on the contract. That is the devirtualization ladder (row 19) reaching a real consumer | M | [§4](ROADMAP_DETAIL.md#s4) |
| 37 | **Binding comments** — documentation syntax that cannot drift from the declaration it references. Motivating case: a contract member with no caller in kama source (the compiler emits the call), where a reader asks "why is this here?" and a comment is the only answer — and comments are not binding. Wanted at the declaration, checked against what it names, so a rename or a removal is an error rather than silent rot. Unscoped: needs a design pass on what a binding comment may assert before any syntax is proposed | — | [§1](ROADMAP_DETAIL.md#s1) |
