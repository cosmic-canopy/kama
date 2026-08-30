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
say **explicit SIMD is the last source-visible gap** and must land before the tag; the design that row
finally got ([design/simd.md](design/simd.md)) measured the premise and weakened it. `std::math` *does*
auto-vectorize on native, rebuilding it on a vector type is rejected on measurement, and what is left —
a wasm flag and an additive `Simd<T, N>` type — breaks nothing that compiles today. **On the evidence
nothing source-visible now gates the tag**, though row 1 (wasm has no SIMD at all) is a poor thing to
ship a first stable release with. Past that the language is complete and the tag could be cut at any
point; the current intent is to cut it once the stdlib rows below are done, so the first stable release
ships with the reach to match.

**Size** is a batching hint, not a commitment: **S** fits beside others in one session · **M** is about a
session · **L** is several · **XL** wants its own design doc before any code. It is read off the linked
detail, so it is only as good as that reasoning: `?` marks a row the detail itself says is unprobed, and
**`—` means never scoped** — sizing work nobody has looked at would be invention, not estimation.

| # | item | size | detail |
|---|---|---|---|
| 1 | **wasm has no SIMD** — the release path passes no `-msimd128`, so a `.wasm` holds zero v128 ops; the flag alone vectorizes the *already-shipped* `std::math`. Wants the guard the claim never had | S | [§2](ROADMAP_DETAIL.md#s2) |
| 2 | **Explicit SIMD surface** — an additive `Simd<T, const N>` for what auto-vectorization cannot spell: shuffles, a lane mask as a value; plus a derived `SIMD128` flag | L | [§2](ROADMAP_DETAIL.md#s2) |
| 3 | **Job system / event-loop scheduler** — libraries on the shipped concurrency primitives; the pool is sized, **scheduling** is what is missing | ? | [§6](ROADMAP_DETAIL.md#s6) |
| 4 | **Stdlib parity M2b** — fs + path, io handles + `lines()`, sleep + wall clock, DNS | — | [§1](ROADMAP_DETAIL.md#s1) |
| 5 | **Stdlib parity M2c** — `std::random`, `std::encoding` | — | [§1](ROADMAP_DETAIL.md#s1) |
| 6 | **Modular / opt-in stdlib** — whether emit-on-instantiation + `--gc-sections` pruning scales, or explicit per-module opt-in / dead-function elimination is wanted before the stdlib grows | — | [§3](ROADMAP_DETAIL.md#s3) |
| 7 | **`std::io` transform adapters** — compression et al., composing with serde and net | — | [§1](ROADMAP_DETAIL.md#s1) |
| 8 | **`std::net`** — IPv6, UDP multicast | — | [§2](ROADMAP_DETAIL.md#s2) |
| 9 | **`std::process`** — live/streaming child-stream reads | — | [§1](ROADMAP_DETAIL.md#s1) |
| 10 | **Serialization follow-ups** — deserialize breadth, more back ends, `@deprecated` | — | [§4](ROADMAP_DETAIL.md#s4) |
| 11 | **Windows** — long-path support; suite wall-clock (~906 s vs ~75 s in the container) | — | [§1](ROADMAP_DETAIL.md#s1) |
| 12 | **MCU toolchain packaging** — board presets, vendor-HAL glue, a real-hardware flash pass; AVR later | — | [§5](ROADMAP_DETAIL.md#s5) |
| 13 | **Remaining language limitations** — no bound spells "an integer primitive", so `cast<T>` in a generic is checked per instantiation (the stdlib avoids it: pass by address, move `sizeof(T)` bytes); generic free fn calling a generic free fn; a generic call in a field's DEFAULT INITIALIZER (neither discovery pass walks one); generic `enum` members; unresolved type names inside generic arguments; an operator call's ARGUMENT TYPE is unchecked, so `Mat4 * Vec4` passes `kama check` and fails in the C compiler instead; `--no-heap` not gating container allocation; contract-refinement thunks; `Fixed<B,const F>` implementing `Real`; `@align`/`@packed` not reaching a tagged `enum` (an outer `packed` misses the per-variant payload structs) | — | [§2](ROADMAP_DETAIL.md#s2) |
| 14 | **Collections knobs** — HashDoS-resistant keyed hashing; zero-size-field elision; thin smart-ptr handles | — | [§5](ROADMAP_DETAIL.md#s5) |
| 15 | **Performance** — bench cohort (add Zig), serialization benchmark track, devirtualization ladder, CPU-tuning knob | — | [§9](ROADMAP_DETAIL.md#s9) |
| 16 | **Hot-reload library** — `dlopen` + file-watch + fn-pointer rebind. Both compiler primitives already ship | — | [§8](ROADMAP_DETAIL.md#s8) |

## LATER — tooling & ecosystem

Most of this gates on the repo going public.

| # | item | detail |
|---|---|---|
| 17 | **Registry — hosted deployment (M3.3)** + mandatory verification and the trust model | [§10](ROADMAP_DETAIL.md#s10) |
| 18 | **Editor/registry registrations** — Zed extension registry, nvim-treesitter, linguist, Helix upstreaming, Marketplace publish | [§10](ROADMAP_DETAIL.md#s10) |
| 19 | **LSP residuals** — one build configuration per server process; the prelude-analysis floor per keystroke | [§10](ROADMAP_DETAIL.md#s10) |
| 20 | **`kama fmt`** — a native formatter. Substrate settled: use the compiler's own front end, **not** tree-sitter | [§10](ROADMAP_DETAIL.md#s10) |
| 21 | **Debugger value formatting** — render `string`/`Optional`/collections as kama values, not their emitted-C form | [§10](ROADMAP_DETAIL.md#s10) |
| 22 | **`kama query` residuals** — no `callers-of`/`implementors-of`, no stdin/unsaved-buffer mode | [§10](ROADMAP_DETAIL.md#s10) |

## FUTURE — the big arcs, in this order

The 2.0 work. Nothing here starts before the NOW list is done.

| # | item | detail |
|---|---|---|
| 23 | **Editor tooling** — the front end as a reusable query API; everything later rides on it | [§10](ROADMAP_DETAIL.md#s10) |
| 24 | **Scripting / multimodal — the flagship 2.0.** First step: refactor the C emitter behind an abstract backend interface | [§7](ROADMAP_DETAIL.md#s7) |
| 25 | **Self-hosting — the capstone, LOWEST priority.** A maturity milestone, not an enabler | [§7](ROADMAP_DETAIL.md#s7) |
