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

⚠️ **Row 2 is the one source-breaking item outstanding, and its cost only rises.** Everything else in
this list is additive and can land in any 1.x; moving compile-time values out of `<…>` cannot. Decided
2026-08-31, so what is left is a migration ([design/comptime-params.md](design/comptime-params.md)).
Row 1 sits above it deliberately: it is what the first external project is **blocked on**, and a
blocked user outranks a migration with no deadline. That is the only reason row 2 slipped, and it
should not slip again. (The other two blockers have shipped: the platform seam in `0.9.131`, and
`@noheap` transitivity in `0.9.132`.)

**Size** is a batching hint, not a commitment: **S** fits beside others in one session · **M** is about a
session · **L** is several · **XL** wants its own design doc before any code. It is read off the linked
detail, so it is only as good as that reasoning: `?` marks a row the detail itself says is unprobed, and
**`—` means never scoped** — sizing work nobody has looked at would be invention, not estimation.

| # | item | size | detail |
|---|---|---|---|
| 1 | **A package compiles every file under its source root** — whatever the import graph, so a native-only file still compiles on the wasm leg. **Ruled:** a file-level gate, `file @compileFor(FLAG);` (bison-measured; the attribute-first spelling costs a conflict) | M | [§2](ROADMAP_DETAIL.md#s2) |
| 2 | **Compile-time arguments — `#(…)`** — compile-time VALUES leave the generic list; `<…>` becomes types-only. Decided; what remains is a lexer token, grammar arms and a ~300-site migration. **Source-breaking — before the tag** | XL | [design/comptime-params.md](design/comptime-params.md) |
| 3 | **BUG: a contract member returning a type declared BESIDE the contract cannot be implemented from another file** — the implementer is told to import a type it already imports on line 1. Only a member returning a USER type fails; `int32`/`void` are fine, and moving the type to a third file both import builds clean. The diagnostic also misattributes: one file's path, another's line number, a third named in the summary | M | [§2](ROADMAP_DETAIL.md#s2) |
| 4 | **BUG: a module `comptime` cannot be interpolated** — `"${WIDTH}"` on a `comptime int32 WIDTH` gives `method call on unresolved receiver` at line 1 col 0, naming neither the constant nor the interpolation; binding to a local first works. **The diagnostic is the more valuable half** — an unresolved-receiver error should name the receiver and its real line | M | [§2](ROADMAP_DETAIL.md#s2) |
| 5 | **BUG: a `fnptr` held in a FIELD or a module `static` cannot be called** — `r.h(x: 1)` says `` `Reg` has no method `h` `` and `g_h(x: 1)` says `call to unknown function`. Binding one works, so this is the call site not accepting a `fnptr`-typed PLACE as a target. Rules out every callback-registry shape | M | [§2](ROADMAP_DETAIL.md#s2) |
| 6 | **`--no-heap` does not gate container allocation program-wide** — the `@noheap` ATTRIBUTE now does (`GlobalAllocator` is the analysis's leaf), but the flag applies that leaf to no body, so a `--no-heap` build still reaches `malloc` through a container. The last piece of "no allocation means no allocation", and the one the MCU target wants. Applying the leaf is one line; the open question is the DIAGNOSTIC, which needs a user-code/library distinction the emitter does not have | M | [§2](ROADMAP_DETAIL.md#s2) |
| 7 | **`@noheap` is not part of a `fnptr` type** — so a callback slot cannot *require* non-allocating of what is bound to it, and since a `fnptr` is a blind seam the call is now a hard error inside a no-heap region with **no escape hatch**. Unblocked: the transitivity it waited on has shipped | M | [§2](ROADMAP_DETAIL.md#s2) |
| 8 | **Running kama on a foreign OS thread** — module statics are `_Thread_local`, so a thread created by a C library (audio device, completion port, RTOS ISR) sees fresh zero-initialised statics. No escape hatch today. ⚠️ **Now measured, not argued:** the first consumer logged 326 audio underruns because the synth cannot run on the CoreAudio thread, and says moving it there "fixes the case completely, and nothing else does" | L | [§2](ROADMAP_DETAIL.md#s2) |
| 9 | **A panic in a real-time region kills the process** — `kama_bounds_fail` is `KAMA_NORETURN` and the panic hook is deliberately process-global, so a bounds miss in a mixer aborts from a thread the player cannot see. Wants a per-region policy so it glitches and degrades instead. Ships with the two rows above | M | [§2](ROADMAP_DETAIL.md#s2) |
| 10 | **BUG: `cflags`/`ldflags`/`link` are ignored on a DEPENDENCY** — every consumer must repeat the block and drift between them is silent. The first consumer repeats it in two packages, and copies kama's own window/framework link flags out of the driver because a project's own seam is not recognised | M | [§2](ROADMAP_DETAIL.md#s2) |
| 11 | **A project cannot hand its own C/C++ sources or emscripten settings to the build** — no `csources` key, so compiling your own C needs an out-of-band Makefile (the first consumer drives C, C++ and a third-party cmake project that way); and no `jsLibraries`/`emSettings`, so `--js-library` is smuggled through per-target `cflags` — where the stdlib's own `-sEXPORTED_RUNTIME_METHODS` is emitted AFTER them and wins a collision | M | [§2](ROADMAP_DETAIL.md#s2) |
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
| 22 | **Remaining language limitations** — no bound spells "an integer primitive", so `cast<T>` in a generic is checked per instantiation (the stdlib avoids it: pass by address, move `sizeof(T)` bytes); generic free fn calling a generic free fn; a generic call in a field's DEFAULT INITIALIZER (neither discovery pass walks one); generic `enum` members; unresolved type names inside generic arguments; an operator call's ARGUMENT TYPE is unchecked, so `Mat4 * Vec4` passes `kama check` and fails in the C compiler instead; `@compileFor` is whole-declaration only, so it cannot gate a single method or one `implements` block; contract-refinement thunks; `Fixed<B,const F>` implementing `Real`; `@align`/`@packed` not reaching a tagged `enum` (an outer `packed` misses the per-variant payload structs) | — | [§2](ROADMAP_DETAIL.md#s2) |
| 23 | **Collections knobs** — HashDoS-resistant keyed hashing; zero-size-field elision; thin smart-ptr handles | — | [§5](ROADMAP_DETAIL.md#s5) |
| 24 | **Performance** — bench cohort (add Zig), serialization benchmark track, devirtualization ladder, CPU-tuning knob | — | [§9](ROADMAP_DETAIL.md#s9) |
| 25 | **Hot-reload library** — `dlopen` + file-watch + fn-pointer rebind. Both compiler primitives already ship | — | [§8](ROADMAP_DETAIL.md#s8) |
| 26 | **Safe `std::gpu` binding wrapper** — wgpu handles→RAII `type resource`s (as `std::net` wraps sockets) + a typed acquire result naming `Occluded`; the seam's size accessor and discarded event queue. Scope: exactly the handles the seam already touches, nothing above them | M | [§8](ROADMAP_DETAIL.md#s8) |
| 27 | **`std::input`** — the seam pumps the window event queue and throws it away, on both targets: no keyboard, mouse, wheel, pointer-lock, resize, focus or gamepad. Peer of `std::gpu`, and the reason the first engine on kama derives its own window | M | [§8](ROADMAP_DETAIL.md#s8) |
| 28 | **wasm: `-fsanitize` blocks `-sWASM_WORKERS`** — so no AudioWorklet and no Wasm Workers at all. kama passes `-fsanitize-trap` (no sanitizer runtime), so emscripten's refusal looks over-broad; upstream fix, or a per-target opt-out | S | [§2](ROADMAP_DETAIL.md#s2) |

## LATER — tooling & ecosystem

Most of this gates on the repo going public.

| # | item | detail |
|---|---|---|
| 29 | **Registry — hosted deployment (M3.3)** + mandatory verification and the trust model | [§10](ROADMAP_DETAIL.md#s10) |
| 30 | **Editor/registry registrations** — Zed extension registry, nvim-treesitter, linguist, Helix upstreaming, Marketplace publish | [§10](ROADMAP_DETAIL.md#s10) |
| 31 | **LSP residuals** — one build configuration per server process; the prelude-analysis floor per keystroke | [§10](ROADMAP_DETAIL.md#s10) |
| 32 | **`kama fmt`** — a native formatter. Substrate settled: use the compiler's own front end, **not** tree-sitter | [§10](ROADMAP_DETAIL.md#s10) |
| 33 | **Debugger value formatting** — render `string`/`Optional`/collections as kama values, not their emitted-C form | [§10](ROADMAP_DETAIL.md#s10) |
| 34 | **`kama query` residuals** — no `callers-of`/`implementors-of`, no stdin/unsaved-buffer mode | [§10](ROADMAP_DETAIL.md#s10) |

## FUTURE — the big arcs, in this order

The 2.0 work. Nothing here starts before the NOW list is done.

| # | item | detail |
|---|---|---|
| 35 | **Editor tooling** — the front end as a reusable query API; everything later rides on it | [§10](ROADMAP_DETAIL.md#s10) |
| 36 | **Scripting / multimodal — the flagship 2.0.** First step: refactor the C emitter behind an abstract backend interface | [§7](ROADMAP_DETAIL.md#s7) |
| 37 | **Self-hosting — the capstone, LOWEST priority.** A maturity milestone, not an enabler | [§7](ROADMAP_DETAIL.md#s7) |
