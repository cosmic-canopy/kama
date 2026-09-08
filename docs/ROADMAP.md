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
| 1 | **`Simd` `sqrt`/`floor`/`ceil`** — a `kama_math.h`-backed intrinsic arm on a lane batch; wants the wasm leg to prove the `-msimd128` lowering | S | [§2](ROADMAP_DETAIL.md#s2) |
| 2 | **23 `tests/xfail` fixtures use the retired class-named constructor** and pass only because their `.msg` still appears among 2–13 errors; rewrite each to `ctor make` + the brace import and confirm ONE error. Tests only | S | [§1](ROADMAP_DETAIL.md#s1) |
| 3 | **`std::time` calendar** — civil-from-days, ISO-8601 format/parse, no zone database; whole or not at all | — | [§1](ROADMAP_DETAIL.md#s1) |
| 4 | **No incremental build** — every build recompiles everything. ⚠️ Largely already answered and never rowed: `zig cc` has a content-addressed per-TU object cache (measured 4.13 s cold, **0.11 s after editing one file**), so a bundled install is incremental today and kama's own object cache is mostly moot. What is left is the SLIM install, which uses clang and has no cache | — | [§9](ROADMAP_DETAIL.md#s9) |
| 5 | **Job system / event-loop scheduler** — libraries on the shipped concurrency primitives; the pool is sized, **scheduling** is what is missing | ? | [§6](ROADMAP_DETAIL.md#s6) |
| 6 | **Modular / opt-in stdlib** — whether emit-on-instantiation + `--gc-sections` pruning scales, or explicit per-module opt-in / dead-function elimination is wanted before the stdlib grows | — | [§3](ROADMAP_DETAIL.md#s3) |
| 7 | **`std::io` transform adapters** — compression et al., composing with serde and net | — | [§1](ROADMAP_DETAIL.md#s1) |
| 8 | **`std::net`** — IPv6, UDP multicast | — | [§2](ROADMAP_DETAIL.md#s2) |
| 9 | **`std::process`** — live/streaming child-stream reads | — | [§1](ROADMAP_DETAIL.md#s1) |
| 10 | **KBIN field-name interning** — the binary backend writes every field NAME in full in every object: measured 24 bytes of data, 91 on the wire. A backend-only fix (write each name once behind a "new name" tag, then a small index), no compiler change, no schema, and `skipValue`/forward-compat survive | S | [§4](ROADMAP_DETAIL.md#s4) |
| 11 | **Serialization follow-ups** — deserialize breadth, more back ends, `@deprecated` | — | [§4](ROADMAP_DETAIL.md#s4) |
| 12 | **Paths with spaces** — two of the C compiler's `-I` entries are unquoted while everything else on the line is quoted, so a project under `C:\Users\John Smith\` (or `/home/x/my project/`) likely hands clang a torn include path, on every platform. Found reading the command builder; not yet measured — the probe is one build from a `mktemp -d` with a space in it | ? | [§2](ROADMAP_DETAIL.md#s2) |
| 13 | **Windows path residuals after `0.9.222`** — `CreateProcessW`'s cwd/executable ≤ 260 (non-goal, a Win32 limit; document "pass absolute paths" on `Command.cwd`), the two 260-byte cosmetic buffers (`selfExePath`, `relativizeToCwd`), the long-`~/.kama` store with no guard, `longPathAware` (non-goal, registry-gated), a volume with 8dot3 names off (the linker fails as before; `lld` is the answer), and a non-ASCII `%TEMP%` breaking clang's own one-step link (toolchain; `targets.md` should say so). Verdicts written in the detail | S | [§2](ROADMAP_DETAIL.md#s2) |
| 14 | **Windows suite wall-clock** — ~1819 s on the box, 2026-09-06, against a ~75 s container figure that is older ([windows.md](platforms/windows.md) carries both dates and the caveat) | — | [§1](ROADMAP_DETAIL.md#s1) |
| 15 | **MCU toolchain packaging** — board presets, vendor-HAL glue, a real-hardware flash pass; AVR later | — | [§5](ROADMAP_DETAIL.md#s5) |
| 16 | **`csources` compiles C, not C++** — a `.cpp`/`.cc`/`.cxx`/`.mm` entry is refused BY NAME with the two obstacles: every input shares one flag prefix (`-std=c11` plus the C-only warning promotions), so a C++ TU needs its own, and a C++ link needs the target's C++ runtime library (`-lc++` vs `-lstdc++`), which varies per target and is a table kama does not have. A `.m` is a third case — Objective-C is one platform's language and `csources` is project-level with no per-target tier to exclude it, so this row carries per-target `csources` too. The first consumer drives C++ and a third-party cmake project from a Makefile; the cmake half is not ours, the C++ half is | M | [§2](ROADMAP_DETAIL.md#s2) |
| 17 | **Remaining language limitations** — no bound spells "an integer primitive", so `cast<T>` in a generic is checked per instantiation (the stdlib avoids it: pass by address, move `sizeof(T)` bytes); generic free fn calling a generic free fn; a generic call in a field's DEFAULT INITIALIZER (neither discovery pass walks one); generic `enum` members (so `Optional`/`Result` cannot declare `Sendable` and are judged by payload); unresolved type names inside generic arguments; `@compileFor` is whole-declaration only, so it cannot gate a single method or one `implements` block; contract-refinement thunks; `Fixed<B,const F>` implementing `Real`; `@align`/`@packed` not reaching a tagged `enum` (an outer `packed` misses the per-variant payload structs); an `expose fn` with neither `@callerThread` nor `@foreignEntry` is not a region, so a host-called body reading a mutable static is unchecked (a source break for 11 in-tree files if required) | — | [§2](ROADMAP_DETAIL.md#s2) |
| 18 | **Collections knobs** — HashDoS-resistant keyed hashing; zero-size-field elision; thin smart-ptr handles | — | [§5](ROADMAP_DETAIL.md#s5) |
| 19 | **Performance** — bench cohort (add Zig), serialization benchmark track, devirtualization ladder, CPU-tuning knob | — | [§9](ROADMAP_DETAIL.md#s9) |
| 20 | **Hot-reload library** — `dlopen` + file-watch + fn-pointer rebind. Both compiler primitives already ship | — | [§8](ROADMAP_DETAIL.md#s8) |
| 21 | **Safe `std::gpu` binding wrapper** — wgpu handles→RAII `type resource`s (as `std::net` wraps sockets) + a typed acquire result naming `Occluded`; the seam's size accessor and discarded event queue. Scope: exactly the handles the seam already touches, nothing above them | M | [§8](ROADMAP_DETAIL.md#s8) |
| 22 | **`std::input`** — the seam pumps the window event queue and throws it away, on both targets: no keyboard, mouse, wheel, pointer-lock, resize, focus or gamepad. Peer of `std::gpu`, and the reason the first engine on kama derives its own window | M | [§8](ROADMAP_DETAIL.md#s8) |

## LATER — tooling & ecosystem

Most of this gates on the repo going public.

| # | item | detail |
|---|---|---|
| 23 | **Registry — hosted deployment (M3.3)** + mandatory verification and the trust model | [§10](ROADMAP_DETAIL.md#s10) |
| 24 | **Editor/registry registrations** — Zed extension registry, nvim-treesitter, linguist, Helix upstreaming, Marketplace publish | [§10](ROADMAP_DETAIL.md#s10) |
| 25 | **tree-sitter accepts 78 of the 84 reserved words as a binding name** — `Thing else = …` renders as a valid declaration in every editor on this grammar, and the compiler then rejects it. The two reserve differently by construction: `kama.l` consults one table at every identifier, tree-sitter extracts keywords CONTEXTUALLY and a binding site expects `$.identifier`. ⚠️ `check-treesitter.sh` cannot see this class, and the one fixture that looks like it covers it passes on its USE site, not its declaration | [§10](ROADMAP_DETAIL.md#s10) |
| 26 | **LSP residuals** — one build configuration per server process; the prelude-analysis floor per keystroke | [§10](ROADMAP_DETAIL.md#s10) |
| 27 | **`kama fmt`** — a native formatter. Substrate settled: use the compiler's own front end, **not** tree-sitter | [§10](ROADMAP_DETAIL.md#s10) |
| 28 | **Debugger value formatting** — render `string`/`Optional`/collections as kama values, not their emitted-C form | [§10](ROADMAP_DETAIL.md#s10) |
| 29 | **`kama query` residuals** — no `callers-of`/`implementors-of`, no stdin/unsaved-buffer mode | [§10](ROADMAP_DETAIL.md#s10) |

## FUTURE — the big arcs, in this order

The 2.0 work. Nothing here starts before the NOW list is done.

| # | item | detail |
|---|---|---|
| 30 | **Editor tooling** — the front end as a reusable query API; everything later rides on it | [§10](ROADMAP_DETAIL.md#s10) |
| 31 | **Scripting / multimodal — the flagship 2.0.** First step: refactor the C emitter behind an abstract backend interface | [§7](ROADMAP_DETAIL.md#s7) |
| 32 | **Self-hosting — the capstone, LOWEST priority.** A maturity milestone, not an enabler | [§7](ROADMAP_DETAIL.md#s7) |
