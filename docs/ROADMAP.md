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

⚠️ **Row 3 is the one source-breaking item outstanding, and its cost only rises.** Everything else in
this list is additive and can land in any 1.x; moving compile-time values out of `<…>` cannot. Decided
2026-08-31, so what is left is a migration ([design/comptime-params.md](design/comptime-params.md)).
Rows 1-2 sit above it deliberately: they are what the first external project is **blocked on**, and a
blocked user outranks a migration with no deadline. That is the only reason row 3 slipped, and it
should not slip again. (The third blocker, the platform seam itself, shipped in `0.9.131`.)

**Size** is a batching hint, not a commitment: **S** fits beside others in one session · **M** is about a
session · **L** is several · **XL** wants its own design doc before any code. It is read off the linked
detail, so it is only as good as that reasoning: `?` marks a row the detail itself says is unprobed, and
**`—` means never scoped** — sizing work nobody has looked at would be invention, not estimation.

| # | item | size | detail |
|---|---|---|---|
| 1 | **`@noheap` is not transitive** — one un-annotated callee silently reintroduces allocation into a "proven" region, and SPEC promises a proof the compiler does not deliver | M | [§2](ROADMAP_DETAIL.md#s2) |
| 2 | **A package compiles every file under its source root** — whatever the import graph, so a native-only file still compiles on the wasm leg. **Ruled:** a file-level gate, `file @compileFor(FLAG);` (bison-measured; the attribute-first spelling costs a conflict) | M | [§2](ROADMAP_DETAIL.md#s2) |
| 3 | **Compile-time arguments — `#(…)`** — compile-time VALUES leave the generic list; `<…>` becomes types-only. Decided; what remains is a lexer token, grammar arms and a ~300-site migration. **Source-breaking — before the tag** | XL | [design/comptime-params.md](design/comptime-params.md) |
| 4 | **Running kama on a foreign OS thread** — module statics are `_Thread_local`, so a thread created by a C library (audio device, completion port, RTOS ISR) sees fresh zero-initialised statics. No escape hatch today | L | [§2](ROADMAP_DETAIL.md#s2) |
| 5 | **Job system / event-loop scheduler** — libraries on the shipped concurrency primitives; the pool is sized, **scheduling** is what is missing | ? | [§6](ROADMAP_DETAIL.md#s6) |
| 6 | **Stdlib parity M2b** — fs + path, io handles + `lines()`, sleep + wall clock, DNS | — | [§1](ROADMAP_DETAIL.md#s1) |
| 7 | **Stdlib parity M2c** — `std::random`, `std::encoding` | — | [§1](ROADMAP_DETAIL.md#s1) |
| 8 | **Modular / opt-in stdlib** — whether emit-on-instantiation + `--gc-sections` pruning scales, or explicit per-module opt-in / dead-function elimination is wanted before the stdlib grows | — | [§3](ROADMAP_DETAIL.md#s3) |
| 9 | **`std::io` transform adapters** — compression et al., composing with serde and net | — | [§1](ROADMAP_DETAIL.md#s1) |
| 10 | **`std::net`** — IPv6, UDP multicast | — | [§2](ROADMAP_DETAIL.md#s2) |
| 11 | **`std::process`** — live/streaming child-stream reads | — | [§1](ROADMAP_DETAIL.md#s1) |
| 12 | **Serialization follow-ups** — deserialize breadth, more back ends, `@deprecated` | — | [§4](ROADMAP_DETAIL.md#s4) |
| 13 | **Windows** — long-path support; suite wall-clock (~906 s vs ~75 s in the container) | — | [§1](ROADMAP_DETAIL.md#s1) |
| 14 | **MCU toolchain packaging** — board presets, vendor-HAL glue, a real-hardware flash pass; AVR later | — | [§5](ROADMAP_DETAIL.md#s5) |
| 15 | **Remaining language limitations** — no bound spells "an integer primitive", so `cast<T>` in a generic is checked per instantiation (the stdlib avoids it: pass by address, move `sizeof(T)` bytes); generic free fn calling a generic free fn; a generic call in a field's DEFAULT INITIALIZER (neither discovery pass walks one); generic `enum` members; unresolved type names inside generic arguments; an operator call's ARGUMENT TYPE is unchecked, so `Mat4 * Vec4` passes `kama check` and fails in the C compiler instead; `--no-heap` not gating container allocation; contract-refinement thunks; `Fixed<B,const F>` implementing `Real`; `@align`/`@packed` not reaching a tagged `enum` (an outer `packed` misses the per-variant payload structs) | — | [§2](ROADMAP_DETAIL.md#s2) |
| 16 | **Collections knobs** — HashDoS-resistant keyed hashing; zero-size-field elision; thin smart-ptr handles | — | [§5](ROADMAP_DETAIL.md#s5) |
| 17 | **Performance** — bench cohort (add Zig), serialization benchmark track, devirtualization ladder, CPU-tuning knob | — | [§9](ROADMAP_DETAIL.md#s9) |
| 18 | **Hot-reload library** — `dlopen` + file-watch + fn-pointer rebind. Both compiler primitives already ship | — | [§8](ROADMAP_DETAIL.md#s8) |
| 19 | **Safe `std::gpu` binding wrapper** — wgpu handles→RAII `type resource`s (as `std::net` wraps sockets) + a typed acquire result naming `Occluded`; the seam's size accessor and discarded event queue. Scope: exactly the handles the seam already touches, nothing above them | M | [§8](ROADMAP_DETAIL.md#s8) |
| 20 | **`std::input`** — the seam pumps the window event queue and throws it away, on both targets: no keyboard, mouse, wheel, pointer-lock, resize, focus or gamepad. Peer of `std::gpu`, and the reason the first engine on kama derives its own window | M | [§8](ROADMAP_DETAIL.md#s8) |
| 21 | **wasm: `-fsanitize` blocks `-sWASM_WORKERS`** — so no AudioWorklet and no Wasm Workers at all. kama passes `-fsanitize-trap` (no sanitizer runtime), so emscripten's refusal looks over-broad; upstream fix, or a per-target opt-out | S | [§2](ROADMAP_DETAIL.md#s2) |

## LATER — tooling & ecosystem

Most of this gates on the repo going public.

| # | item | detail |
|---|---|---|
| 22 | **Registry — hosted deployment (M3.3)** + mandatory verification and the trust model | [§10](ROADMAP_DETAIL.md#s10) |
| 23 | **Editor/registry registrations** — Zed extension registry, nvim-treesitter, linguist, Helix upstreaming, Marketplace publish | [§10](ROADMAP_DETAIL.md#s10) |
| 24 | **LSP residuals** — one build configuration per server process; the prelude-analysis floor per keystroke | [§10](ROADMAP_DETAIL.md#s10) |
| 25 | **`kama fmt`** — a native formatter. Substrate settled: use the compiler's own front end, **not** tree-sitter | [§10](ROADMAP_DETAIL.md#s10) |
| 26 | **Debugger value formatting** — render `string`/`Optional`/collections as kama values, not their emitted-C form | [§10](ROADMAP_DETAIL.md#s10) |
| 27 | **`kama query` residuals** — no `callers-of`/`implementors-of`, no stdin/unsaved-buffer mode | [§10](ROADMAP_DETAIL.md#s10) |

## FUTURE — the big arcs, in this order

The 2.0 work. Nothing here starts before the NOW list is done.

| # | item | detail |
|---|---|---|
| 28 | **Editor tooling** — the front end as a reusable query API; everything later rides on it | [§10](ROADMAP_DETAIL.md#s10) |
| 29 | **Scripting / multimodal — the flagship 2.0.** First step: refactor the C emitter behind an abstract backend interface | [§7](ROADMAP_DETAIL.md#s7) |
| 30 | **Self-hosting — the capstone, LOWEST priority.** A maturity milestone, not an enabler | [§7](ROADMAP_DETAIL.md#s7) |
