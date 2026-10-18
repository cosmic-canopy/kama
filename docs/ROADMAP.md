# kama roadmap

**The order of work, and nothing else.** One row per item: what it is, and a link to the reasoning in
[ROADMAP_DETAIL.md](ROADMAP_DETAIL.md). The language's **history** lives in the git log; what the language
**is** lives in [SPEC.md](SPEC.md).

> **Keep this file short.** No reasoning here — it goes in [ROADMAP_DETAIL.md](ROADMAP_DETAIL.md), whose
> header carries the full maintenance rule (where a shipped item's record goes, and what may stay behind).
> This file drifted to 1,279 lines once by absorbing that prose, at which point "what is next" stopped
> being answerable without reading all of it. `tools/check-roadmap.sh` holds the split down.

> ⚠️ **`KR-<n>` is a PERMANENT id, not a position.** It is assigned once, never reused, and never
> renumbered: a row keeps its id wherever it moves in the list, and a shipped row's id retires with it,
> leaving a gap. **A new row takes the id on the counter below, and bumps the counter in the same edit.**
> `tools/check-roadmap.sh` holds uniqueness and format down, and holds the counter above every id ever cited.
>
> **Why a counter, and not "one more than the highest id present".** The highest id PRESENT is not the highest
> id ever ISSUED: once a newer row ships and is deleted, its number looks free again. Two ids were nearly
> reissued that way, and on 2026-09-14 two machines each filed the same id for different rows. The counter is
> one line, so two concurrent issues edit the same line and the second rebase CONFLICTS — the collision is loud
> instead of two rows silently sharing a name.
>
> **Why, because the old scheme cost real work.** Rows used to be numbered by POSITION, so deleting a
> shipped row renumbered every row below it and silently re-pointed every `row N` written in prose
> anywhere — in this file, in the detail, in a commit message, in a note someone kept. It had already
> happened twice, and closing two rows in one sitting broke two more references. Position numbering also
> made "find the row by its TEXT, never its number" a standing instruction to every reader, which is a
> workaround for a numbering scheme rather than a property anyone wanted. **A `KR-` id is safe to cite.**

**Next id: KR-209**

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
*Generics*), and the last one after it — the floor became module `core`, and a module path is written only in
`import` (`0.9.426`–`0.9.429`, [SPEC.md](SPEC.md) *Modules*). Everything remaining on this list is additive and can land in any 1.x; the tag waits only on how much stdlib reach the maintainer wants in the first release.
Ordering the rows below is the maintainer's call.

**Size** is a batching hint, not a commitment: **S** fits beside others in one session · **M** is about a
session · **L** is several · **XL** wants its own design doc before any code. It is read off the linked
detail, so it is only as good as that reasoning: `?` marks a row the detail itself says is unprobed, and
**`—` means never scoped** — sizing work nobody has looked at would be invention, not estimation.

| id | item | size | detail |
|---|---|---|---|
| KR-200 | **An accepted stream's blocking mode depends on the OS** — from a non-blocking `TcpListener` or `UnixListener`, macOS hands back a non-blocking stream and Linux a blocking one (`accept4` does not carry `O_NONBLOCK`), so a loop written on macOS stalls on Linux at the first idle client. Nothing in kama sets the mode | S | [§1](ROADMAP_DETAIL.md#s1-accept-mode) |
| KR-199 | **A module `comptime` or `static` whose initializer names a generic instance nothing else does fails in clang** — `comptime usize A = sizeof(Pair<int64>);` (or `InlineArray<uint8>#(16)`, `Optional<int64>`, `alignof`, or a `static`) passes `kama check`, and the C names a type that is never emitted ("use of undeclared identifier"). The module sweep scans such a declaration's type, never its initializer. The same `sizeof` in a function body builds | S | [§2](ROADMAP_DETAIL.md#s2-sizeof-unemitted) |
| KR-194 | **A use-after-move is reported in code no path reaches** — `while (true) { …; if (c) { continue; } return give xs; } return give xs;` is refused at the trailing `return`, after a loop with no `break`. Without the `continue`, or without the trailing `return`, it builds | S | [§2](ROADMAP_DETAIL.md#s2-unreachable-move) |
| KR-195 | **`replace(place: o, with: Optional::None)` does not infer `T` from `place:`** — "cannot infer generic type parameter 'T'", though `place:` is a typed local or field. A typed `with` local, or the turbofish, builds | S | [§2](ROADMAP_DETAIL.md#s2-replace-infer) |
| KR-196 | **Some semantic errors still report column 0** — a generic-inference failure, an unmet bound and the "takes a raw pointer, so it must be declared `unsafe`" refusal print `file:line:0`, where most diagnostics carry the column | S | [§10](ROADMAP_DETAIL.md#s10-column-zero) |
| KR-197 | **Two debug-tier frames still hold a large value twice** — a large value returned into a local (`Big b = newBig(…)` lowers to a declaration and then an assignment: 32,824 B for a 16 KB `Big` at -O0), and a `[v; N]` field initializer, filled by value in the constructor (35,400 B for 35 KB of arrays). A frame-size flag on a debug build trips on both; release is clean | M | [§5](ROADMAP_DETAIL.md#s5-debug-frame-copies) |
| KR-198 | **Two holes in the diagnostic channel** — in a multi-module `kama build`, a refusal raised while the shared header is emitted prints no file at all (``:10:4: error: an `InlineArray<T>#(N)` element must be a `value`…``; `kama check` names it); and `unsupported(what, node)` on a synthesized node calls itself with the same arguments, so the compiler would recurse until its stack overflows | S | [§10](ROADMAP_DETAIL.md#s10-diag-no-file) |
| KR-101 | **Publish from Perforce (and other VCS)** — the publish seam's second implementation (`vcsSnapshot`/`vcsRead`, rule in packages.md § *Publishing*): `p4 have` for the versioned files, `p4 opened` + `p4 status` for local changes, the changelist as the revision, `p4 print -k` for the depot bytes. Perforce is the game-studio default, which is the engine track's audience. Unbuildable blind: needs a P4 server to test against | — | [§10](ROADMAP_DETAIL.md#s10) |
| KR-3 | **Job system / event-loop scheduler** — libraries on the shipped concurrency primitives; the pool is sized, **scheduling** is what is missing | ? | [§6](ROADMAP_DETAIL.md#s6) |
| KR-201 | **A `Poller` wakeup another isolate can trigger** — a `Sendable` waker whose `wake()` ends a `wait` elsewhere (eventfd, a pipe, a loopback pair on Windows). Today a loop learns that handed-off work is done only at its timeout | S–M | [§1](ROADMAP_DETAIL.md#s1-poller-waker) |
| KR-202 | **A DNS lookup a loop can poll, with a deadline** — `resolve` is `getaddrinfo`, which blocks for seconds and takes no timeout. On KR-201 | M | [§1](ROADMAP_DETAIL.md#s1-async-dns) |
| KR-203 | **Constant-time equality in `std::digest`** — std warns that comparing a tag with `==` leaks through timing, and offers nothing else | S | [§1](ROADMAP_DETAIL.md#s1-constant-time) |
| KR-204 | **Percent-encoding and a URL type** — std has neither, while its own WebSocket and WebTransport connect take a URL as a bare string. **Wants a verdict: std (recommended) or a package** | M | [§1](ROADMAP_DETAIL.md#s1-url) |
| KR-206 | **Listener options: the backlog, and reuse-port where the OS balances it** — the backlog is fixed at 128 and `SO_REUSEPORT` cannot be set, so several isolates cannot accept on one port | S–M | [§1](ROADMAP_DETAIL.md#s1-listen-options) |
| KR-207 | **`sendfile`** — a file response copies through user space; Linux and macOS have `sendfile`, Windows `TransmitFile` | S–M | [§1](ROADMAP_DETAIL.md#s1-sendfile) |
| KR-208 | **An in-memory connected `ReliableStream` pair** — every package that speaks a protocol writes its own test pipe (Go's `net.Pipe`) | S | [§1](ROADMAP_DETAIL.md#s1-memory-stream) |
| KR-205 | **Coverage-guided fuzzing** — no `kama fuzz` and no documented libFuzzer harness, though every parser that reads a stranger's bytes wants one. Design first | M? | [§1](ROADMAP_DETAIL.md#s1-fuzz) |
| KR-5 | **`std::io` transform adapters** — compression et al., composing with serde and net. The compiler already writes deflate/gzip and tar, in C++, for `kama publish` (`src/kama.archive.cpp`, `0.9.452`); the stdlib half — kama code a program can call — **wants a maintainer verdict: stdlib or package** | — | [§1](ROADMAP_DETAIL.md#s1) |
| KR-7 | **`std::process`** — live/streaming child-stream reads, and a non-blocking `File` (a child's pipe read without blocking) | — | [§1](ROADMAP_DETAIL.md#s1) |
| KR-19 | **MCU toolchain packaging** — board presets, vendor-HAL glue, a real-hardware flash pass; AVR later | — | [§5](ROADMAP_DETAIL.md#s5) |
| KR-21 | **Remaining language limitations** — no bound spells "an integer primitive", so `cast<T>` in a generic is checked per instantiation (the stdlib avoids it: pass by address, move `sizeof(T)` bytes); unresolved type names inside generic arguments ⚠️ (re-probed: the diagnostic is *"cannot tell which `DynamicArray` to construct — give the type arguments"* even when the turbofish IS written, so it advises the thing the author already did — still so at `0.9.317`, repro `kr21_3`); `@compileFor` gates whole declarations BY DESIGN — never a member (the maintainer, 2026-09-30: a platform's deltas live in per-platform TYPES, not in conditionals inside one) — but a `type adapter <…>` block takes no attribute at all (its grammar rule, `marked_adapter_declaration` in `kama.y`, never admitted one), so a platform-specific adapter conformance cannot be gated; `Fixed<B,const F>` implementing `Real`; ⚠️ **`@align`/`@packed` on an `enum` is NOT a silent miss — re-probed 2026-09-10, it is a clean refusal** naming the reason and the fix (`type enum E : IntType`), so what is left here is only whether a tagged enum should honor a layout attribute at all; an `expose fn` with neither `@callerThread` nor `@foreignEntry` is not a region, so a host-called body reading a mutable static is unchecked (a source break for 11 in-tree files if required) | — | [§2](ROADMAP_DETAIL.md#s2) |
| KR-107 | **An `fnptr` type is not a type argument** — `DynamicArray<Op>` (the callback-registry shape SPEC names) does not instantiate: the turbofish is refused as ambiguous, the instance has no methods, and the element store trips the fnptr-bind rule. Any generic over an `fnptr` element, `BindableFunctionPtr` included. Found probing the default rule (`0.9.480`) | — | [§2](ROADMAP_DETAIL.md#s2-fnptr-type-arg) |
| KR-192 | **`main`'s status past what the platform's exit status carries** — Linux and macOS keep its low byte, so `return 256;` exits 0 (success) and `return -962;` exits 62, with no word; Windows keeps all 32 bits. kama traps or refuses every other silent narrowing. It hid four failing fixture checks on every POSIX leg until Windows showed one (`0.9.666`). **Wants a maintainer verdict:** a debug trap on a one-byte platform (recommended), a `uint8` `main`, or the truncation documented | S | [§2](ROADMAP_DETAIL.md#s2-exit-status) |
| KR-190 | **In-place uninitialized storage** — storage with `T[N]`'s size and alignment that the compiler never constructs, copies or drops, reached as `UnsafePtr<T>` in unsafe code (Rust's `MaybeUninit`, C++'s aligned storage): the in-place half of the "unsafe core, safe API" model std's heap containers are written in (GOALS #1). Design first | M | [§5](ROADMAP_DETAIL.md#s5-bounded-vector) |
| KR-191 | **A generic type's kind following its parameters** — a library type that is a `value` when what it holds owns nothing and a `resource` otherwise, as an intrinsic or an enum already is (C++26's conditional triviality; Rust has none: `ArrayVec` is never `Copy`). Design first | M | [§5](ROADMAP_DETAIL.md#s5-bounded-vector) |
| KR-187 | **A bounded inline vector, `InlineDynamicArray<T>#(N)`** — after KR-190 and KR-191, kama code in std over both — capacity `N` in place, length `0..N`, no heap (C++26 `inplace_vector`, Zig's `BoundedArray`, Rust's `heapless::Vec`): the growable list that can live inside a `type value`, the natural container for `--no-heap` and the MCU track | S–M | [§5](ROADMAP_DETAIL.md#s5-bounded-vector) |
| KR-22 | **Collections knobs** — HashDoS-resistant keyed hashing; zero-size-field elision; thin smart-ptr handles | — | [§5](ROADMAP_DETAIL.md#s5) |
| KR-23 | **Performance** — bench cohort (add Zig), serialization benchmark track, devirtualization ladder, CPU-tuning knob | — | [§9](ROADMAP_DETAIL.md#s9) |
| KR-24 | **Hot-reload library** — `dlopen` + file-watch + fn-pointer rebind. Both compiler primitives already ship | — | [§8](ROADMAP_DETAIL.md#s8) |
| KR-189 | **Serialization follow-ups** — what §4 still schedules, with no row of its own until now: a tagged-enum payload holding `Shared`/`Weak`; a fat `Owned<Contract>` field; the `text::yaml` and `text::csv` back ends (csv wants its shape decided before it is built); a `Format` contract carrying a back end's entry points; and two bugs §4 names (`Weak<Contract>` does not monomorphize; a temporary passed to a contract-typed parameter takes an rvalue's address). Last measured 2026-09-09 — re-probe each before building | — | [§4](ROADMAP_DETAIL.md#s4) |
| KR-79 | **Declared reachability properties — generalize what `--no-heap` already proves** — `--no-heap`/`@noheap` is a whole-program reachability proof with a general engine (the reach walk over the call graph, the region marker, the chain-shaped diagnostic, the `@heap extern fn` escape) and exactly ONE hardcoded predicate: does this edge allocate. The same walk decides *no panic*, *no recursion* (bounded stack — what embedded and real-time users actually ask for, and a cycle check on a graph already built), *no blocking syscall* (audio/render callbacks, ISRs) and *no unsafe*. Each is a property asserted in a comment today and verified by reading, which is the class of claim this repo refuses to trust. Opened 2026-09-20 after looking at Bend 2's `LAWS.bend`: the same instinct reached from dependent types, and the point is that kama needs none of that — these are properties about REACHABILITY, not about values. Unscoped: the surface, per-function vs per-build, how a property crosses a contract slot (as `--no-heap` does since `0.9.659`, an edge to every implementer), and whether user-defined properties are a goal — all want a design pass and a written verdict before any code | — | [§5](ROADMAP_DETAIL.md#s5) |
| KR-40 | **Binding comments** — documentation syntax that cannot drift from the declaration it references. Motivating case: a contract member with no caller in kama source (the compiler emits the call), where a reader asks "why is this here?" and a comment is the only answer — and comments are not binding. Wanted at the declaration, checked against what it names, so a rename or a removal is an error rather than silent rot. Unscoped: needs a design pass on what a binding comment may assert before any syntax is proposed | — | [§1](ROADMAP_DETAIL.md#s1) |
| KR-68 | **Every user TU parses the whole `gen.h` and `kama_runtime.h`** — the shared header carries the prelude's `static inline` bodies and every generic instance (a `std::fs` program's `main.gen.h` is 9,431 lines, against `kama_runtime.h`'s 1,291 expanded), and each user TU pays both. Measure what a TU actually uses before proposing a fix. The OS seam's own depth is settled: the body split was retired as a measured loss, and `<windows.h>` is trimmed in a program whose every header is kama's own (`0.9.647`) | ? | [§9](ROADMAP_DETAIL.md#s9-tu-header-cost) |
| KR-83 | **Extended `asm` (operand constraints) and `@naked` functions** — SPEC called them "tracked follow-ons" with no row; they are the MCU seam's remaining half | — | [§5](ROADMAP_DETAIL.md#s5) |

## LATER — tooling & ecosystem

Most of this gates on the repo going public.

| id | item | detail |
|---|---|---|
| KR-27 | **Registry — hosted deployment (M3.3)** + mandatory verification and the trust model — IN PROGRESS: `registry.kama-lang.org` is live and the built-in default since `0.9.457`, and `@kama/sodium@0.5.0` is its first package (2026-09-27). The index lives in git and deploys to Pages; the tarballs live in a locked R2 bucket (`dl.kama-lang.org`) behind a `_redirects` rule. What remains is the trust model — an allowed-signers set, then CI/OIDC provenance — and a resolution-time compiler check | [§10](ROADMAP_DETAIL.md#s10) |
| KR-135 | **Signed releases** — `release.yml` signs nothing: the Windows `kama.exe` has no Authenticode signature (SmartScreen warns on download, Smart App Control refuses to start it) and the macOS binary has no Developer ID signature and is not notarized (Gatekeeper refuses a quarantined copy). Signing kama cannot help what a build PRODUCES — SAC blocks every new unsigned executable, whatever compiled it | [§10](ROADMAP_DETAIL.md#s10-release-signing) |
| KR-28 | **Editor/registry registrations** — Zed extension registry, nvim-treesitter, linguist, Helix upstreaming, Marketplace publish | [§10](ROADMAP_DETAIL.md#s10) |
| KR-29 | **tree-sitter accepts 78 of the 84 reserved words as a binding name** — `Thing else = …` renders as a valid declaration in every editor on this grammar, and the compiler then rejects it. The two reserve differently by construction: `kama.l` consults one table at every identifier, tree-sitter extracts keywords CONTEXTUALLY and a binding site expects `$.identifier`. ⚠️ `check-treesitter.sh` cannot see this class, and the one fixture that looks like it covers it passes on its USE site, not its declaration | [§10](ROADMAP_DETAIL.md#s10) |
| KR-30 | **LSP residuals** — one build configuration per server process; the prelude-analysis floor per keystroke; ⚠️ **a receiver typed by a generic instance over an UNBOUND parameter resolves to nothing in completion** — `const ref Node<K>` inside another generic offers no members at all, PUBLIC ones included, while the same receiver spelled `Node<int32>` offers every one (measured `0.9.300`, writing the `friend`-across-generics fixtures; it is receiver resolution, not visibility); a type argument's span runs into a NAMED size (`InlineArray<int32>#(LIMIT)`: hover on `LI` answers `int32`) | [§10](ROADMAP_DETAIL.md#s10) |
| KR-31 | **`kama fmt`** — a native formatter. Substrate settled: use the compiler's own front end, **not** tree-sitter | [§10](ROADMAP_DETAIL.md#s10) |
| KR-33 | **`kama query` residuals** — no `callers-of`, no stdin/unsaved-buffer mode; a `fnptr` signature type has no definition site | [§10](ROADMAP_DETAIL.md#s10) |
| KR-84 | **`kama describe --json`** — the language surface as data, the other half of `kama query --json` (GOALS §7) | [§10](ROADMAP_DETAIL.md#s10) |
| KR-148 | **kama's own AI-agent tooling** — brief written 2026-10-15, nine decisions owed: recommended a `kama` Claude Code plugin (`kama lsp` pushes diagnostics after every edit; the skill's text printed by the installed compiler), symbol-named `kama query` modes, call hierarchy, a guard that runs the guidance, the same tools in this repo; MCP proposed a non-goal. L once decided | [§10](ROADMAP_DETAIL.md#s10-agent-tooling) |
| KR-173 | **`@kama/http` — a first-party HTTP package for both targets** — `fetch` and `EventSource` in the browser, HTTP/1.1 client and server natively on std's TCP, native WebSocket (on std's SHA-1 + base64), TLS through `@kama/tls`. HTTP stays out of std on every target (decided 2026-10-14); this is where a web-first kama gets it | [§1](ROADMAP_DETAIL.md#s1-http-package) |
| KR-25 | **`@kama/gpu` — the safe WebGPU binding, a first-party package** (decided 2026-10-16: not std — no systems language ships it there, and webgpu.h still breaks between releases). wgpu handles as RAII `type resource`s, `Copyable(bare: give)` = `AddRef`, a typed acquire naming `Occluded`; growing toward the whole of webgpu.h. Its own repository | [§8](ROADMAP_DETAIL.md#s8-gpu-wrapper) |
| KR-26 | **`@kama/window` — a window and its events, a first-party package** (decided 2026-10-16): `Window` (several, headless + `post`), `Event` in one coalescing queue with `Dropped(n)`, physical `Key` + `Text`, explicit `pump()`, gamepad. No consumer waits on it; the downstream engines' own platform layers are its reference consumers. Its own repository | [§8](ROADMAP_DETAIL.md#s8-input) |

## FUTURE — the big arcs, in this order

The 2.0 work. Nothing here starts before the NOW list is done.

| id | item | detail |
|---|---|---|
| KR-34 | **Editor tooling** — the front end as a reusable query API; everything later rides on it | [§10](ROADMAP_DETAIL.md#s10) |
| KR-35 | **Scripting / multimodal — the flagship 2.0.** First step: refactor the C emitter behind an abstract backend interface | [§7](ROADMAP_DETAIL.md#s7) |
| KR-36 | **Self-hosting — the capstone, LOWEST priority.** A maturity milestone, not an enabler | [§7](ROADMAP_DETAIL.md#s7) |
