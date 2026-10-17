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

**Next id: KR-189**

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
| KR-183 | **A generic contract written bare as a value type reaches the C compiler** — `fn take(Src s)` passes `kama check`, then "unknown type name" in C; blocks KR-108 | S | [§2](ROADMAP_DETAIL.md#s2-bare-generic-contract) |
| KR-184 | **An enum's copyability depends on the order of its payloads** — `Arr(DynamicArray<J>)` before a non-Copyable payload fails, swapped builds; blocks KR-110 | S | [§2](ROADMAP_DETAIL.md#s2-enum-copy-order) |
| KR-185 | **The JSON reader takes invalid numbers, aborts on overflow, and overflows the stack on deep input** — `[1.5]` reads 1 and `[007]` 7; an int64 overflow traps the process; 10⁶ `[` is a SIGSEGV | S | [§4](ROADMAP_DETAIL.md#s4-json-numbers) |
| KR-178 | **`UnixStream.pair()`** — a connected pair of local sockets (`socketpair`), one end handed to a child as a stream (`Stdio::Handle`, `0.9.624`): the verdict that held it optional until `std::process` could hand a descriptor to a child has had its condition met. Windows has no `socketpair`; its twin is a listener on a private path and one connect | M | [§1](ROADMAP_DETAIL.md#s1-unix-pair) |
| KR-131 | **A by-value `match` or `foreach` binding copies its value even when nothing writes it** — `case Ok(value: b):` over a `Result<Big, E>` copies the 16 KB `Big` into `b` (and `foreach (Big b in xs)` each element), though the subject is already borrowed. A binding its body never writes can be read in place | M | [§5](ROADMAP_DETAIL.md#s5-match-binding-copy) |
| KR-123 | **`borrow` names any place for a block** — there is no `ref` local, and `borrow` opens only an argument-less view mint, so an accessor with arguments is repeated at every use. `borrow <place> as L { … }` with the host frozen, as a view window freezes it | M | [§2](ROADMAP_DETAIL.md#s2-borrow-any-place) |
| KR-124 | **A borrow window that runs code when it closes** — a transaction wants a scope that rolls back unless committed; no stored borrows and no closures leave no way to say it. A `type view` may declare a destructor its window runs on every path out | M | [§2](ROADMAP_DETAIL.md#s2-view-destructor) |
| KR-101 | **Publish from Perforce (and other VCS)** — the publish seam's second implementation (`vcsSnapshot`/`vcsRead`, rule in packages.md § *Publishing*): `p4 have` for the versioned files, `p4 opened` + `p4 status` for local changes, the changelist as the revision, `p4 print -k` for the depot bytes. Perforce is the game-studio default, which is the engine track's audience. Unbuildable blind: needs a P4 server to test against | — | [§10](ROADMAP_DETAIL.md#s10) |
| KR-3 | **Job system / event-loop scheduler** — libraries on the shipped concurrency primitives; the pool is sized, **scheduling** is what is missing | ? | [§6](ROADMAP_DETAIL.md#s6) |
| KR-5 | **`std::io` transform adapters** — compression et al., composing with serde and net. The compiler already writes deflate/gzip and tar, in C++, for `kama publish` (`src/kama.archive.cpp`, `0.9.452`); the stdlib half — kama code a program can call — **wants a maintainer verdict: stdlib or package** | — | [§1](ROADMAP_DETAIL.md#s1) |
| KR-7 | **`std::process`** — live/streaming child-stream reads, and a non-blocking `File` (a child's pipe read without blocking) | — | [§1](ROADMAP_DETAIL.md#s1) |
| KR-8 | **`private`/`protected`/`public` on a contract member are accepted and inert** — `private` parses and is silently dropped, so a call through a contract-typed value reaches the member (`KEYWORDS.md` even claims `protected` there is an error). Decided 2026-10-16: refuse all three words, and declare restricted-private contract members a NON-GOAL — a key-type parameter whose ctor only the intended caller may call answers the need (measured) | S | [§4](ROADMAP_DETAIL.md#s4) |
| KR-19 | **MCU toolchain packaging** — board presets, vendor-HAL glue, a real-hardware flash pass; AVR later | — | [§5](ROADMAP_DETAIL.md#s5) |
| KR-21 | **Remaining language limitations** — no bound spells "an integer primitive", so `cast<T>` in a generic is checked per instantiation (the stdlib avoids it: pass by address, move `sizeof(T)` bytes); unresolved type names inside generic arguments ⚠️ (re-probed: the diagnostic is *"cannot tell which `DynamicArray` to construct — give the type arguments"* even when the turbofish IS written, so it advises the thing the author already did — still so at `0.9.317`, repro `kr21_3`); `@compileFor` gates whole declarations BY DESIGN — never a member (the maintainer, 2026-09-30: a platform's deltas live in per-platform TYPES, not in conditionals inside one) — but a `type adapter <…>` block takes no attribute at all (its grammar rule, `marked_adapter_declaration` in `kama.y`, never admitted one), so a platform-specific adapter conformance cannot be gated; `Fixed<B,const F>` implementing `Real`; ⚠️ **`@align`/`@packed` on an `enum` is NOT a silent miss — re-probed 2026-09-10, it is a clean refusal** naming the reason and the fix (`type enum E : IntType`), so what is left here is only whether a tagged enum should honor a layout attribute at all; an `expose fn` with neither `@callerThread` nor `@foreignEntry` is not a region, so a host-called body reading a mutable static is unchecked (a source break for 11 in-tree files if required) | — | [§2](ROADMAP_DETAIL.md#s2) |
| KR-107 | **An `fnptr` type is not a type argument** — `DynamicArray<Op>` (the callback-registry shape SPEC names) does not instantiate: the turbofish is refused as ambiguous, the instance has no methods, and the element store trips the fnptr-bind rule. Any generic over an `fnptr` element, `BindableFunctionPtr` included. Found probing the default rule (`0.9.480`) | — | [§2](ROADMAP_DETAIL.md#s2-fnptr-type-arg) |
| KR-108 | **`DatagramSocket` generic over its address** — so `UnixDatagram` implements it beside `UdpSocket`: a syslog client sends to `/dev/log` or UDP 514 through one code path. Decided 2026-10-16: `DatagramSocket<A>`, taking the break (contract values and `implements` spell `<SocketAddr>`); after KR-183 | S | [§1](ROADMAP_DETAIL.md#s1-datagram-address) |
| KR-188 | **The GPU seam leaves std** — `lib/std/gpu`'s C seam and the driver's GLFW special case serve only `examples/webgpu`; GPU and window are packages now (KR-25/26). The example becomes an ordinary project (`"webgpu": true`, its own `csources` and `link`); `--webgpu` stays as the SDK link; a guard checks the example and smoke on native and wasm | S | [§8](ROADMAP_DETAIL.md#s8-seam-out) |
| KR-186 | **`OrderedMap<K, V>` and `OrderedSet<K>`** — the insertion-ordered map and set (Java's `LinkedHashMap`, Python's `dict`, .NET 9's `OrderedDictionary`), beside `Map` (hashed) and `SortedMap` (by key): O(1) lookup, iteration in insertion order, `moveToEnd`. KR-110's JSON `Object` is one | M | [§5](ROADMAP_DETAIL.md#s5-ordered-map) |
| KR-110 | **An untyped JSON value — `text::json`'s document tree** — a JSON document is read only through a typed `@generate(Deserializable)` schema: unknown fields are skipped, and every member a schema does not own is declared `Optional` or `@field(default)`. A document whose KEYS are data (glTF's `extensions`, a response keyed by ids), or whose shape a program only partly knows, has no reading at all. A tagged enum (`Null`/`Bool`/`Number`/`String`/`Array`/`Object`), a parse and a write, and whether a typed field may hold one as an opaque subtree. Decided 2026-10-16: exact-text numbers, `Object` an `OrderedMap` (KR-186), and `JsonValue` serde-integrated; after KR-184/185/186 | M | [§4](ROADMAP_DETAIL.md#s4-json-value) |
| KR-187 | **A bounded inline vector** — capacity `N` in place, length `0..N`, no heap (C++26 `inplace_vector`, Zig's `BoundedArray`, Rust's `heapless::Vec`): the growable list that can live inside a `type value`, the natural container for `--no-heap` and the MCU track | S | [§5](ROADMAP_DETAIL.md#s5-bounded-vector) |
| KR-111 | **A project or dependency named `std` or `core` installs and cannot be imported** — `import { core::v }` resolves to the floor module and `std::…` to the stdlib, so either package is unreachable, and a project file `src/core.kama` collides with the prelude's own (`two files of module core`). `kama seed` refuses both names; the manifest reader cannot yet, because `lib/kama.json` is itself named `std`. Refuse both as a project `name` and as a dependency key | S | [§10](ROADMAP_DETAIL.md#s10-std-core-names) |
| KR-22 | **Collections knobs** — HashDoS-resistant keyed hashing; zero-size-field elision; thin smart-ptr handles | — | [§5](ROADMAP_DETAIL.md#s5) |
| KR-23 | **Performance** — bench cohort (add Zig), serialization benchmark track, devirtualization ladder, CPU-tuning knob | — | [§9](ROADMAP_DETAIL.md#s9) |
| KR-24 | **Hot-reload library** — `dlopen` + file-watch + fn-pointer rebind. Both compiler primitives already ship | — | [§8](ROADMAP_DETAIL.md#s8) |
| KR-39 | **`@noheap` cannot cross a contract slot even when the callee is knowable** — UNBLOCKED — allocator-aware errors shipped `0.9.395` (SPEC *Global allocator*; `tests/global_allocator_serde.kama`) — and its original premise is retired. Its first concrete consumer is measured: a serde error path under `--no-heap` with a declared pool is refused for the DISPATCH, not the heap — dropping `Owned<Error>` goes through a contract-dispatched destructor (`tests/global_allocator_serde.kama` builds and runs, and the same file under the flag does not). The row read "`serializeJsonBuffer` CONSTRUCTS its backend, so the compiler could prove the callee": true at the entry point, and useless where the proof is needed — the slot calls are in the synthesized `X__serialize(X*, Serializer* w)`, written once for every backend, so devirtualizing in emission reaches none of them (measured planning this row, `0.9.352`). It also unblocks nobody yet: no serde path is heap-free even with a proven callee (every backend ctor allocates, readers take an owned buffer, no fixed-buffer writer exists, every `Err` boxes). What answers serde is the DECLARED global allocator (`@globalAllocator`, `0.9.377`): every allocation funnels into a pool whose body `--no-heap` checks, so a pool over program-owned storage is provably not the system heap. What is left is user contracts: resolving a slot through the vtables the program actually builds | M | [§4](ROADMAP_DETAIL.md#s5-noheap-contract-slot) |
| KR-79 | **Declared reachability properties — generalize what `--no-heap` already proves** — `--no-heap`/`@noheap` is a whole-program reachability proof with a general engine (the reach walk over the call graph, the region marker, the chain-shaped diagnostic, the `@heap extern fn` escape) and exactly ONE hardcoded predicate: does this edge allocate. The same walk decides *no panic*, *no recursion* (bounded stack — what embedded and real-time users actually ask for, and a cycle check on a graph already built), *no blocking syscall* (audio/render callbacks, ISRs) and *no unsafe*. Each is a property asserted in a comment today and verified by reading, which is the class of claim this repo refuses to trust. Opened 2026-09-20 after looking at Bend 2's `LAWS.bend`: the same instinct reached from dependent types, and the point is that kama needs none of that — these are properties about REACHABILITY, not about values. Unscoped: the surface, per-function vs per-build, how a property crosses a contract slot (inherits KR-39 verbatim), and whether user-defined properties are a goal — all want a design pass and a written verdict before any code | — | [§5](ROADMAP_DETAIL.md#s5) |
| KR-40 | **Binding comments** — documentation syntax that cannot drift from the declaration it references. Motivating case: a contract member with no caller in kama source (the compiler emits the call), where a reader asks "why is this here?" and a comment is the only answer — and comments are not binding. Wanted at the declaration, checked against what it names, so a rename or a removal is an error rather than silent rot. Unscoped: needs a design pass on what a binding comment may assert before any syntax is proposed | — | [§1](ROADMAP_DETAIL.md#s1) |
| KR-68 | **The OS seam's header depth** — `kama_os.h` ships ~158 `static inline` bodies and so pulls 25 system headers (all of `<windows.h>` on Windows). Measured 2026-10-15: on macOS only 4 stdlib TUs include it, the per-unit cache skips them after the first build, and one TU compiling all the bodies costs 105.7 ms against 33.2 — so splitting the bodies out is a net LOSS. Decided 2026-10-16: retire the split; keep Windows `NOGDI`/`NOMINMAX`-style trims before `<windows.h>` (Windows CI owes the measurement); the real per-TU cost is `main.gen.h` | S | [§9](ROADMAP_DETAIL.md#s9-os-seam-bodies) |
| KR-83 | **Extended `asm` (operand constraints) and `@naked` functions** — SPEC called them "tracked follow-ons" with no row; they are the MCU seam's remaining half | — | [§5](ROADMAP_DETAIL.md#s5) |
| KR-94 | **A struct's `sizeof` and any `alignof` as a compile-time VALUE** — refused in every `comptime` since `0.9.444`, because the target's C compiler decides layout and a kama answer that disagreed would be a silent miscompile. Unlock: a per-target layout model in the compiler, re-checked by the C compiler — every baked size emits a `_Static_assert`, so a wrong model fails the build, never the program. (`comptime assert(cond: sizeof(T) == 20, …)` already works: C checks it) | M | [§5](ROADMAP_DETAIL.md#s5-sizeof-comptime) |
| KR-96 | **Operators on an enum are a NON-GOAL; enums gain `Comparable`** — decided 2026-10-16. Write the non-goal beside the refusal (`tests/xfail/enum_operator.kama`; flags are a `type value`, as `std::fs::Permissions` is), and make SPEC's promise reachable: `Comparable` admits an enum, `<` reaches `compareTo`, and `@generate(Comparable)` orders by declaration | S | [§2](ROADMAP_DETAIL.md#s2) |
| KR-176 | **A std enum that grows after 1.0 breaks every `match` with no `_` arm** — `IoErrorKind` has gained variants six times, and `ProcessGroup` already differs per OS. Decided 2026-10-16: `@nonExhaustive` on a `type enum`; outside its PACKAGE a `match` must carry `case _:`; a user enum may carry it (Rust's `#[non_exhaustive]`) | S | [§2](ROADMAP_DETAIL.md#s2-growable-enums) |
| KR-179 | **A panic handler learns nothing about the panic** — `core::PanicHandler` takes no arguments, and a bounds, `cast`, overflow or divide trap names no file. Decided 2026-10-16: the hook receives `const ref PanicInfo` (message, file, line; nothing allocates), every trap carries a location in every build, and a caller-location attribute makes a std `operator[]` report its caller's line | L | [§2](ROADMAP_DETAIL.md#s2-panic-info) |

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
