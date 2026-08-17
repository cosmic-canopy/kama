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

**The tag needs exactly this list and nothing below it.** The gate is: *no source-breaking change left, and
no ordinary safe-kama construct miscompiles or emits invalid C.*

| # | item | why it gates the tag | detail |
|---|---|---|---|
| 1 | **Contextual literal typing** — a literal takes its type from its destination, not always `int32` | prerequisite for 2: **measured**, it is 219 of the 322 sites strict conversion would break. Scoped into three halves, probed | [design/analysis-gap.md](design/analysis-gap.md) |
| 2 | **Strict numeric conversion** — no implicit conversion (Rust/Swift/Go) | **source-breaking**, so it lands before the tag or never. Measured: 103 residual sites, one shape, one subsystem | [design/analysis-gap.md](design/analysis-gap.md) |
| 3 | **Runtime narrowing casts truncate in silence** — decided: **trap**, with `try cast<T>` as the `Optional` form | source-visible; `try` is already the non-panicking modality (`try new`). The CONSTANT half already rejects | [§2](ROADMAP_DETAIL.md#s2) |
| 4 | **Uninstantiated generic bodies get no analysis at all** — four distinct errors build clean | hits package authors hardest: ship `check`-green, consumers get the errors | [design/analysis-gap.md](design/analysis-gap.md) |
| 5 | **C keyword collisions** — `int32 switch = 3;` emits invalid C (25 of C11's 44 keywords are legal kama identifiers) | a live correctness bug, not polish; exposure is locals, params, struct fields | [§10](ROADMAP_DETAIL.md#s10) |
| 6 | **File-private C symbols are POSITIONAL** — `_F4__Holder` vs `_F5__Holder` by argument order | `--keep-c` is non-reproducible, which is what the README's "drops into an existing C codebase" rests on. One campaign with 5 — the halves want opposite naming rules | [§10](ROADMAP_DETAIL.md#s10) |
| 7 | **Docs/naming reconcile** | 1.0 fixes naming and case conventions | [§1](ROADMAP_DETAIL.md#s1) |
| 8 | **Repoint the Zed grammar pin at the tag** | Zed installs the grammar by fetching a pinned commit, currently behind | [§1](ROADMAP_DETAIL.md#s1) |

## NEXT — engine unblock

The engine is a **separate consumer project**. Its first phase (math, ECS, windowing, renderer spine) is
buildable today. These are the kama-side gaps it hits, ordered by when a renderer actually reaches them.

| # | item | bites at | detail |
|---|---|---|---|
| 9 | **Layout control `@align(N)` / `@packed`** — passthrough lowering, the shape `@section` already has | **week 2** — the first vertex buffer or `std140` uniform block | [§2](ROADMAP_DETAIL.md#s2) |
| 10 | **`foreach` protocol for user types** — today built-in collections only | the first custom container | [§5](ROADMAP_DETAIL.md#s5) |
| 11 | **Explicit SIMD** — auto-vectorization is all there is; needed for shuffles, dot/cross, packed compare/select. Wants 9 first | the first hot math kernel | [§2](ROADMAP_DETAIL.md#s2) |
| 12 | **`spawn` disjointness: ROOT → PLACE granularity** — `w.bodies` + `w.springs` are rejected as "the same root `w`" | the first parallel system | [§3](ROADMAP_DETAIL.md#s3) |
| 13 | **Value-producing `match` over `enum X : IntType` does not compile** | render-command enums | [§2](ROADMAP_DETAIL.md#s2) |
| 14 | **Fallible `new` is concrete-only**; a fallible ctor on a generic instance has no static result type | asset loading | [§2](ROADMAP_DETAIL.md#s2) |
| 15 | **Windows subsystem knob** — every binary is console-subsystem, so a GUI program opens a stray console. Decided: `subsystem` manifest key + CLI flag, default `console`, `AttachConsole` on the GUI path | ship day | [§2](ROADMAP_DETAIL.md#s2) |

## NEXT — diagnostics you can trust

Nothing here miscompiles; all of it costs every user every day.

| # | item | detail |
|---|---|---|
| 16 | **Nothing in the repo checks a diagnostic's line number** — all 439 xfail fixtures would pass with every line wrong. The guard *is* the work, and it is the one 17–18 need | [§2](ROADMAP_DETAIL.md#s2) |
| 17 | **A lib/prelude diagnostic is attributed to the user's file** — right line, wrong file | [§2](ROADMAP_DETAIL.md#s2) |
| 18 | **A top-level `fn`'s diagnostics point at the PREVIOUS declaration** | [§2](ROADMAP_DETAIL.md#s2) |
| 19 | **A prelude generic bound failure cascades at `<user file>:<prelude line>`** | [§10](ROADMAP_DETAIL.md#s10) |
| 20 | **~42 negative doc claims have no machine-readable link to an xfail fixture** | [§2](ROADMAP_DETAIL.md#s2) |
| 21 | **Test-infra holes** — no MSan leg; an `xfail` never links so never reaches ASan; `tests/trap/` skipped under SAN/WASM/Windows | [design/analysis-gap.md](design/analysis-gap.md) |

## LATER — stdlib & platform reach

| # | item | detail |
|---|---|---|
| 22 | **Stdlib parity M2b** — fs + path, io handles + `lines()`, sleep + wall clock, DNS | [§1](ROADMAP_DETAIL.md#s1) |
| 23 | **Stdlib parity M2c** — `std::random`, `std::encoding` | [§1](ROADMAP_DETAIL.md#s1) |
| 24 | **`std::io` transform adapters** — compression et al., composing with serde and net | [§1](ROADMAP_DETAIL.md#s1) |
| 25 | **`std::net`** — IPv6, UDP multicast | [§2](ROADMAP_DETAIL.md#s2) |
| 26 | **`std::process`** — live/streaming child-stream reads | [§1](ROADMAP_DETAIL.md#s1) |
| 27 | **Serialization follow-ups** — deserialize breadth, more back ends, `@deprecated` | [§4](ROADMAP_DETAIL.md#s4) |
| 28 | **Windows** — long-path support; suite wall-clock (~906 s vs ~75 s in the container) | [§1](ROADMAP_DETAIL.md#s1) |
| 29 | **MCU toolchain packaging** — board presets, vendor-HAL glue, a real-hardware flash pass; AVR later | [§5](ROADMAP_DETAIL.md#s5) |
| 30 | **Remaining language limitations** — generic free fn calling a generic free fn; generic `enum` members; unresolved type names inside generic arguments; `--no-heap` not gating container allocation; contract-refinement thunks; `Fixed<B,const F>` implementing `Real` | [§2](ROADMAP_DETAIL.md#s2) |
| 31 | **Collections knobs** — HashDoS-resistant keyed hashing; zero-size-field elision; thin smart-ptr handles | [§5](ROADMAP_DETAIL.md#s5) |
| 32 | **Performance** — bench cohort (add Zig), serialization benchmark track, devirtualization ladder, CPU-tuning knob | [§9](ROADMAP_DETAIL.md#s9) |

## LATER — tooling & ecosystem

Most of this gates on the repo going public.

| # | item | detail |
|---|---|---|
| 33 | **Registry — hosted deployment (M3.3)** + mandatory verification and the trust model | [§10](ROADMAP_DETAIL.md#s10) |
| 34 | **Editor/registry registrations** — Zed extension registry, nvim-treesitter, linguist, Helix upstreaming, Marketplace publish | [§10](ROADMAP_DETAIL.md#s10) |
| 35 | **LSP residuals** — one build configuration per server process; the prelude-analysis floor per keystroke | [§10](ROADMAP_DETAIL.md#s10) |
| 36 | **`kama fmt`** — a native formatter. Substrate settled: use the compiler's own front end, **not** tree-sitter | [§10](ROADMAP_DETAIL.md#s10) |
| 37 | **Debugger value formatting** — render `string`/`Optional`/collections as kama values, not their emitted-C form | [§10](ROADMAP_DETAIL.md#s10) |
| 38 | **`kama query` residuals** — no `callers-of`/`implementors-of`, no stdin/unsaved-buffer mode | [§10](ROADMAP_DETAIL.md#s10) |
| 39 | **Hot-reload library** — `dlopen` + file-watch + fn-pointer rebind. Both compiler primitives already ship | [§8](ROADMAP_DETAIL.md#s8) |
| 40 | **Job system / event-loop scheduler** — ordinary libraries on the shipped concurrency primitives | [§6](ROADMAP_DETAIL.md#s6) |

## LATER — the big arcs, in this order

| # | item | detail |
|---|---|---|
| 41 | **Editor tooling** — the front end as a reusable query API; everything later rides on it | [§10](ROADMAP_DETAIL.md#s10) |
| 42 | **Scripting / multimodal — the flagship 2.0.** First step: refactor the C emitter behind an abstract backend interface | [§7](ROADMAP_DETAIL.md#s7) |
| 43 | **Self-hosting — the capstone, LOWEST priority.** A maturity milestone, not an enabler | [§7](ROADMAP_DETAIL.md#s7) |
