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
| 1 | **A package compiles every file under its source root** — whatever the import graph, so a native-only file still compiles on the wasm leg. **Ruled:** a file-level gate, `file @compileFor(FLAG);` (bison-measured; the attribute-first spelling costs a conflict) | M | [§2](ROADMAP_DETAIL.md#s2) |
| 2 | **`--no-heap` does not gate container allocation program-wide** — the `@noheap` ATTRIBUTE now does (`GlobalAllocator` is the analysis's leaf), but the flag applies that leaf to no body, so a `--no-heap` build still reaches `malloc` through a container. The last piece of "no allocation means no allocation", and the one the MCU target wants. Applying the leaf is one line; the open question is the DIAGNOSTIC, which needs a user-code/library distinction the emitter does not have | M | [§2](ROADMAP_DETAIL.md#s2) |
| 3 | **`@noheap` is not part of a `fnptr` type** — so a callback slot cannot *require* non-allocating of what is bound to it, and since a `fnptr` is a blind seam the call is now a hard error inside a no-heap region with **no escape hatch**. Unblocked: the transitivity it waited on has shipped | M | [§2](ROADMAP_DETAIL.md#s2) |
| 4 | **Running kama on a foreign OS thread** — module statics are `_Thread_local`, so a thread created by a C library (audio device, completion port, RTOS ISR) sees fresh zero-initialised statics. No escape hatch today. ⚠️ **Now measured, not argued:** the first consumer logged 326 audio underruns because the synth cannot run on the CoreAudio thread, and says moving it there "fixes the case completely, and nothing else does" | L | [§2](ROADMAP_DETAIL.md#s2) |
| 5 | **A panic in a real-time region kills the process** — `kama_bounds_fail` is `KAMA_NORETURN` and the panic hook is deliberately process-global, so a bounds miss in a mixer aborts from a thread the player cannot see. Wants a per-region policy so it glitches and degrades instead. Ships with the two rows above | M | [§2](ROADMAP_DETAIL.md#s2) |
| 6 | **BUG: `cflags`/`ldflags`/`link` are ignored on a DEPENDENCY** — every consumer must repeat the block and drift between them is silent. The first consumer repeats it in two packages, and copies kama's own window/framework link flags out of the driver because a project's own seam is not recognised | M | [§2](ROADMAP_DETAIL.md#s2) |
| 7 | **A project cannot hand its own C/C++ sources or emscripten settings to the build** — no `csources` key, so compiling your own C needs an out-of-band Makefile (the first consumer drives C, C++ and a third-party cmake project that way); and no `jsLibraries`/`emSettings`, so `--js-library` is smuggled through per-target `cflags` — where the stdlib's own `-sEXPORTED_RUNTIME_METHODS` is emitted AFTER them and wins a collision | M | [§2](ROADMAP_DETAIL.md#s2) |
| 8 | **No capturing closures** — a handler must be a `type resource` carrying its captures plus a `BindableFunctionPtr`, which is the shape a UI event table wants least. The first consumer works around it and says it will bite properly at their UI milestone. ⚠️ Rowed here for the first time: it was cited as already being on this list and was not | — | [§2](ROADMAP_DETAIL.md#s2) |
| 9 | **No incremental build** — every build recompiles everything. ⚠️ Largely already answered and never rowed: `zig cc` has a content-addressed per-TU object cache (measured 4.13 s cold, **0.11 s after editing one file**), so a bundled install is incremental today and kama's own object cache is mostly moot. What is left is the SLIM install, which uses clang and has no cache | — | [§9](ROADMAP_DETAIL.md#s9) |
| 10 | **Job system / event-loop scheduler** — libraries on the shipped concurrency primitives; the pool is sized, **scheduling** is what is missing | ? | [§6](ROADMAP_DETAIL.md#s6) |
| 11 | **Stdlib parity M2b** — fs + path, io handles + `lines()`, sleep + wall clock, DNS | — | [§1](ROADMAP_DETAIL.md#s1) |
| 12 | **Stdlib parity M2c** — `std::random`, `std::encoding` | — | [§1](ROADMAP_DETAIL.md#s1) |
| 13 | **Modular / opt-in stdlib** — whether emit-on-instantiation + `--gc-sections` pruning scales, or explicit per-module opt-in / dead-function elimination is wanted before the stdlib grows | — | [§3](ROADMAP_DETAIL.md#s3) |
| 14 | **`std::io` transform adapters** — compression et al., composing with serde and net | — | [§1](ROADMAP_DETAIL.md#s1) |
| 15 | **`std::net`** — IPv6, UDP multicast | — | [§2](ROADMAP_DETAIL.md#s2) |
| 16 | **`std::process`** — live/streaming child-stream reads | — | [§1](ROADMAP_DETAIL.md#s1) |
| 17 | **Serialization follow-ups** — deserialize breadth, more back ends, `@deprecated` | — | [§4](ROADMAP_DETAIL.md#s4) |
| 18 | **Windows** — long-path support; suite wall-clock (~906 s vs ~75 s in the container) | — | [§1](ROADMAP_DETAIL.md#s1) |
| 19 | **MCU toolchain packaging** — board presets, vendor-HAL glue, a real-hardware flash pass; AVR later | — | [§5](ROADMAP_DETAIL.md#s5) |
| 20 | **No way to give an `extern fn` or an `expose fn` a symbol name different from its kama name** — `@linkName("…")`, the peer of Rust's `#[link_name]`/`#[export_name]`. Distinct from the spelling problem contextual `type` closed: that was "my grammar cannot spell this name", this is "this symbol is named something else". Today an `extern` must be spelled exactly as C names it, so a C symbol that collides with a kama KEYWORD (not merely an identifier) has no binding at all, and `expose` cannot choose its exported name. ⚠️ **Do not call it `@cname`** — kama's only backend is C today, but the 2.0 bytecode VM has no C names; name it for the concept, not the target | S | [§2](ROADMAP_DETAIL.md#s2) |
| 21 | **Remaining language limitations** — no bound spells "an integer primitive", so `cast<T>` in a generic is checked per instantiation (the stdlib avoids it: pass by address, move `sizeof(T)` bytes); generic free fn calling a generic free fn; a generic call in a field's DEFAULT INITIALIZER (neither discovery pass walks one); generic `enum` members; unresolved type names inside generic arguments; an operator call's ARGUMENT TYPE is unchecked, so `Mat4 * Vec4` passes `kama check` and fails in the C compiler instead; `@compileFor` is whole-declaration only, so it cannot gate a single method or one `implements` block; contract-refinement thunks; `Fixed<B,const F>` implementing `Real`; `@align`/`@packed` not reaching a tagged `enum` (an outer `packed` misses the per-variant payload structs) | — | [§2](ROADMAP_DETAIL.md#s2) |
| 22 | **Collections knobs** — HashDoS-resistant keyed hashing; zero-size-field elision; thin smart-ptr handles | — | [§5](ROADMAP_DETAIL.md#s5) |
| 23 | **Performance** — bench cohort (add Zig), serialization benchmark track, devirtualization ladder, CPU-tuning knob | — | [§9](ROADMAP_DETAIL.md#s9) |
| 24 | **Hot-reload library** — `dlopen` + file-watch + fn-pointer rebind. Both compiler primitives already ship | — | [§8](ROADMAP_DETAIL.md#s8) |
| 25 | **Safe `std::gpu` binding wrapper** — wgpu handles→RAII `type resource`s (as `std::net` wraps sockets) + a typed acquire result naming `Occluded`; the seam's size accessor and discarded event queue. Scope: exactly the handles the seam already touches, nothing above them | M | [§8](ROADMAP_DETAIL.md#s8) |
| 26 | **`std::input`** — the seam pumps the window event queue and throws it away, on both targets: no keyboard, mouse, wheel, pointer-lock, resize, focus or gamepad. Peer of `std::gpu`, and the reason the first engine on kama derives its own window | M | [§8](ROADMAP_DETAIL.md#s8) |
| 27 | **wasm: `-fsanitize` blocks `-sWASM_WORKERS`** — so no AudioWorklet and no Wasm Workers at all. kama passes `-fsanitize-trap` (no sanitizer runtime), so emscripten's refusal looks over-broad; upstream fix, or a per-target opt-out | S | [§2](ROADMAP_DETAIL.md#s2) |

## LATER — tooling & ecosystem

Most of this gates on the repo going public.

| # | item | detail |
|---|---|---|
| 28 | **Registry — hosted deployment (M3.3)** + mandatory verification and the trust model | [§10](ROADMAP_DETAIL.md#s10) |
| 29 | **Editor/registry registrations** — Zed extension registry, nvim-treesitter, linguist, Helix upstreaming, Marketplace publish | [§10](ROADMAP_DETAIL.md#s10) |
| 30 | **LSP residuals** — one build configuration per server process; the prelude-analysis floor per keystroke | [§10](ROADMAP_DETAIL.md#s10) |
| 31 | **`kama fmt`** — a native formatter. Substrate settled: use the compiler's own front end, **not** tree-sitter | [§10](ROADMAP_DETAIL.md#s10) |
| 32 | **Debugger value formatting** — render `string`/`Optional`/collections as kama values, not their emitted-C form | [§10](ROADMAP_DETAIL.md#s10) |
| 33 | **`kama query` residuals** — no `callers-of`/`implementors-of`, no stdin/unsaved-buffer mode | [§10](ROADMAP_DETAIL.md#s10) |

## FUTURE — the big arcs, in this order

The 2.0 work. Nothing here starts before the NOW list is done.

| # | item | detail |
|---|---|---|
| 34 | **Editor tooling** — the front end as a reusable query API; everything later rides on it | [§10](ROADMAP_DETAIL.md#s10) |
| 35 | **Scripting / multimodal — the flagship 2.0.** First step: refactor the C emitter behind an abstract backend interface | [§7](ROADMAP_DETAIL.md#s7) |
| 36 | **Self-hosting — the capstone, LOWEST priority.** A maturity milestone, not an enabler | [§7](ROADMAP_DETAIL.md#s7) |
