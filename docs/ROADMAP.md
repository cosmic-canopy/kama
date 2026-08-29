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

## NOW — the 1.0 gate

**The tag needs exactly this list and nothing below it.** The gate is: *no source-breaking change left, no
ordinary safe-kama construct miscompiles or emits invalid C, the diagnostics can be trusted — and the
engine can be built on it without reaching outside the language.*

That last clause is why the **engine-unblock** rows are in this list rather than after it. The engine is a
separate consumer project, but it is the north star that decides whether the surface is actually finished:
a hot math kernel with no explicit SIMD, or a GUI binary that opens a stray console, is the language coming
up short, and both are **source-visible** — a SIMD surface and a `subsystem` manifest key are API. That
makes them 1.0 business by the first clause too.

**Size** is a batching hint, not a commitment: **S** fits beside others in one session · **M** is about a
session · **L** is several · **XL** wants its own design doc before any code. It is read off the linked
detail, so it is only as good as that reasoning — `?` marks one the detail itself says is unprobed, and
`+` marks two rows that are one piece of work and should be taken together. **LATER rows carry no size on
purpose:** sizing work nobody has scoped yet would be invention, not estimation.

| # | item | size | why it gates the tag | detail |
|---|---|---|---|---|
| 1 | **Test-infra holes** — no MSan leg; an `xfail` never links so never reaches ASan; `tests/trap/` skipped under SAN/WASM/Windows | L | three independent holes; what a green suite is allowed to mean | [design/analysis-gap.md](design/analysis-gap.md) |
| 2 | **Explicit SIMD** — auto-vectorization is all there is; needed for shuffles, dot/cross, packed compare/select | ? | the first hot math kernel — and a SIMD surface is API | [§2](ROADMAP_DETAIL.md#s2) |
| 3 | **Windows subsystem knob** — every binary is console-subsystem, so a GUI program opens a stray console. Decided: `subsystem` manifest key + CLI flag, default `console`, `AttachConsole` on the GUI path | S | ship day, and the manifest key is source-visible | [§2](ROADMAP_DETAIL.md#s2) |
| 4 | **Runtime-N task fan-out has no spelling** — a bare `spawn` must be a direct statement of its `scope`, so "one child per work item" is unwritable when N is not static | M | a job graph the fixed pool cannot express | [§6](ROADMAP_DETAIL.md#s6) |

## LATER — stdlib & platform reach

| # | item | detail |
|---|---|---|
| 5 | **Stdlib parity M2b** — fs + path, io handles + `lines()`, sleep + wall clock, DNS | [§1](ROADMAP_DETAIL.md#s1) |
| 6 | **Stdlib parity M2c** — `std::random`, `std::encoding` | [§1](ROADMAP_DETAIL.md#s1) |
| 7 | **Modular / opt-in stdlib** — whether emit-on-instantiation + `--gc-sections` pruning scales, or explicit per-module opt-in / dead-function elimination is wanted before the stdlib grows | [§3](ROADMAP_DETAIL.md#s3) |
| 8 | **`std::io` transform adapters** — compression et al., composing with serde and net | [§1](ROADMAP_DETAIL.md#s1) |
| 9 | **`std::net`** — IPv6, UDP multicast | [§2](ROADMAP_DETAIL.md#s2) |
| 10 | **`std::process`** — live/streaming child-stream reads | [§1](ROADMAP_DETAIL.md#s1) |
| 11 | **Serialization follow-ups** — deserialize breadth, more back ends, `@deprecated` | [§4](ROADMAP_DETAIL.md#s4) |
| 12 | **Windows** — long-path support; suite wall-clock (~906 s vs ~75 s in the container) | [§1](ROADMAP_DETAIL.md#s1) |
| 13 | **MCU toolchain packaging** — board presets, vendor-HAL glue, a real-hardware flash pass; AVR later | [§5](ROADMAP_DETAIL.md#s5) |
| 14 | **Remaining language limitations** — no bound spells "an integer primitive", so `cast<T>` in a generic is checked per instantiation (the stdlib avoids it: pass by address, move `sizeof(T)` bytes); generic free fn calling a generic free fn; a generic call in a field's DEFAULT INITIALIZER (neither discovery pass walks one); generic `enum` members; unresolved type names inside generic arguments; `--no-heap` not gating container allocation; contract-refinement thunks; `Fixed<B,const F>` implementing `Real`; `@align`/`@packed` not reaching a tagged `enum` (an outer `packed` misses the per-variant payload structs) | [§2](ROADMAP_DETAIL.md#s2) |
| 15 | **Collections knobs** — HashDoS-resistant keyed hashing; zero-size-field elision; thin smart-ptr handles | [§5](ROADMAP_DETAIL.md#s5) |
| 16 | **Performance** — bench cohort (add Zig), serialization benchmark track, devirtualization ladder, CPU-tuning knob | [§9](ROADMAP_DETAIL.md#s9) |

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
| 23 | **Hot-reload library** — `dlopen` + file-watch + fn-pointer rebind. Both compiler primitives already ship | [§8](ROADMAP_DETAIL.md#s8) |
| 24 | **Job system / event-loop scheduler** — ordinary libraries on the shipped concurrency primitives | [§6](ROADMAP_DETAIL.md#s6) |

## LATER — the big arcs, in this order

| # | item | detail |
|---|---|---|
| 25 | **Editor tooling** — the front end as a reusable query API; everything later rides on it | [§10](ROADMAP_DETAIL.md#s10) |
| 26 | **Scripting / multimodal — the flagship 2.0.** First step: refactor the C emitter behind an abstract backend interface | [§7](ROADMAP_DETAIL.md#s7) |
| 27 | **Self-hosting — the capstone, LOWEST priority.** A maturity milestone, not an enabler | [§7](ROADMAP_DETAIL.md#s7) |
