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

**Next id: KR-129**

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
| KR-113 | **A `return` in a destructor skips the fields' destructors** — `~Outer() { if (this.done) { return; } … }` never drops `Outer`'s fields on the early path: whatever they own leaks, silently | S | [§2](ROADMAP_DETAIL.md#s2-dtor-return) |
| KR-115 | **Bounded quantification is enforced only for a generic nobody instantiates** — `fn int32 feed<T>(T w) { return w.get(); }` is refused unused and accepted once `main` calls it, though SPEC makes it an error at the declaration | M | [§2](ROADMAP_DETAIL.md#s2-bounds-on-instances) |
| KR-116 | **Four constructs pass `kama check` and fail in the C compiler** — an early `return;` in a constructor; an `Owned<T>` passed to a `const ref T`; `copy` of an `InlineArray<string>` element; a function's name used as an operand (`a * pi`) | S | [§2](ROADMAP_DETAIL.md#s2-check-passes-c-fails) |
| KR-117 | **Under gcc, a function that ends in an exhaustive `match` does not build** — gcc cannot see that every arm returns, and `-Werror=return-type` refuses it; clang builds it. gcc is a documented `cc` | S | [§2](ROADMAP_DETAIL.md#s2-gcc-exhaustive-match) |
| KR-112 | **A `scope`'s spawned child and its parent may both use a place the child borrows** — `spawn worker(c: ref c)` and the scope body both write `c`: a data race in safe kama, accepted. A child's `ref` borrow freezes the place in the parent until the scope joins, as a `borrow` window freezes its host | M | [§2](ROADMAP_DETAIL.md#s2-scope-child-borrows) |
| KR-118 | **A literal is not typed by its destination in three positions** — a generic call infers `T` from a literal operand before the literal takes the other operand's type (`sqrt(x: 1.0 + a)` with `float32 a`); a literal `match` arm in a typed position; `bitcast`'s literal operand | M | [§2](ROADMAP_DETAIL.md#s2-literal-destinations) |
| KR-119 | **Diagnostics and docs that say nothing or misstate the rule** — a `match` statement missing its `;`; a duplicated import entry; `localAddr()`/`peerAddr()` not `const fn`; the shipped agent guide's reserved words; a `ref` field in TYPE_MODEL.md; a dangling registry link blamed on the manifest | S | [§2](ROADMAP_DETAIL.md#s2-diagnostics-batch) |
| KR-120 | **In-place construction** — `T x = T.make(…)`, `new T.make(…)` and a constructor's field initializers build a temporary and copy it, so a large `type value` costs its size two or three times in a frame. Every construction site fills the storage it names, as `0.9.534` did for `[v; N]` | L | [§5](ROADMAP_DETAIL.md#s5-in-place-construction) |
| KR-121 | **Source locations in a program are package-relative** — a panic site embeds the build machine's absolute path in every binary and prints it; a prelude panic names the user's file at the prelude's line | S | [§10](ROADMAP_DETAIL.md#s10-package-relative-locations) |
| KR-122 | **Moving a value out of a field — `replace` and `swap`** — `give this.f` is refused, and nothing else empties a field, so a resource held in a field can never be handed out. Safe library functions over an unsafe core, as Rust's `mem::replace`/`swap` | S | [§2](ROADMAP_DETAIL.md#s2-replace-swap) |
| KR-123 | **`borrow` names any place for a block** — there is no `ref` local, and `borrow` opens only an argument-less view mint, so an accessor with arguments is repeated at every use. `borrow <place> as L { … }` with the host frozen, as a view window freezes it | M | [§2](ROADMAP_DETAIL.md#s2-borrow-any-place) |
| KR-124 | **A borrow window that runs code when it closes** — a transaction wants a scope that rolls back unless committed; no stored borrows and no closures leave no way to say it. A `type view` may declare a destructor its window runs on every path out | M | [§2](ROADMAP_DETAIL.md#s2-view-destructor) |
| KR-125 | **Deserialize into an existing value** — a nested object present in the data is rebuilt from its own type's defaults, so a partial tuning file resets fields it never named. A patch read: only the fields present are written, recursively | M | [§4](ROADMAP_DETAIL.md#s4-deserialize-into) |
| KR-126 | **Strict decoding: unknown fields refused at the read site** — a misspelt key in a config file does nothing and says nothing. The read decides, not the type, and the error names the key (with KR-109) | S | [§4](ROADMAP_DETAIL.md#s4-strict-decoding) |
| KR-127 | **A byte view of a plain value** — mirroring a plain `type value` as bytes needs `addr(of:)`, a cast and a redeclared `memcpy`. A compiler-verified `Bytewise` contract (numbers only, no padding) and safe `bytesOf`/`bytesOfMut` | M | [§5](ROADMAP_DETAIL.md#s5-bytewise) |
| KR-128 | **An optimized package in a debug build** — DSP runs ~95× slower in debug, and a dependency's `cflags` reach the whole program. The root manifest names packages built at the release optimization level in a debug build, with kama's checks kept | M | [§5](ROADMAP_DETAIL.md#s5-debug-optimize) |
| KR-101 | **Publish from Perforce (and other VCS)** — the publish seam's second implementation (`vcsSnapshot`/`vcsRead`, rule in packages.md § *Publishing*): `p4 have` for the versioned files, `p4 opened` + `p4 status` for local changes, the changelist as the revision, `p4 print -k` for the depot bytes. Perforce is the game-studio default, which is the engine track's audience. Unbuildable blind: needs a P4 server to test against | — | [§10](ROADMAP_DETAIL.md#s10) |
| KR-3 | **Job system / event-loop scheduler** — libraries on the shipped concurrency primitives; the pool is sized, **scheduling** is what is missing | ? | [§6](ROADMAP_DETAIL.md#s6) |
| KR-5 | **`std::io` transform adapters** — compression et al., composing with serde and net. The compiler already writes deflate/gzip and tar, in C++, for `kama publish` (`src/kama.archive.cpp`, `0.9.452`); the stdlib half — kama code a program can call — **wants a maintainer verdict: stdlib or package** | — | [§1](ROADMAP_DETAIL.md#s1) |
| KR-7 | **`std::process`** — live/streaming child-stream reads | — | [§1](ROADMAP_DETAIL.md#s1) |
| KR-8 | **Restricted-private contract members** — *to consider*, not scheduled: a contract may declare a member non-public, with a `friend` grant naming who may reach it, and an implementer then matches that visibility instead of being forced `public`. It is no longer a serde blocker — that justification was written before the container seam was measured, and the seam turned out to need no contract at all. What stands on its own is expressiveness: binding types together with a guarantee that is not part of the public surface. ⚠️ **`private` on a contract member is ACCEPTED AND INERT today** (measured `0.9.269`): it parses and the marker is silently dropped, so with a `public` implementer a call through a contract-typed value reaches it. That is the sibling `0.9.266` left behind when it refused `friend` on a contract on the premise that a contract *cannot* have a private member. Deliberately **not fixed** — it is inert, it blocks nothing, and whether it is refused or made real is this row's decision. Both spellings already parse, so the work is emitter-only | M | [§4](ROADMAP_DETAIL.md#s4) |
| KR-19 | **MCU toolchain packaging** — board presets, vendor-HAL glue, a real-hardware flash pass; AVR later | — | [§5](ROADMAP_DETAIL.md#s5) |
| KR-21 | **Remaining language limitations** — no bound spells "an integer primitive", so `cast<T>` in a generic is checked per instantiation (the stdlib avoids it: pass by address, move `sizeof(T)` bytes); unresolved type names inside generic arguments ⚠️ (re-probed: the diagnostic is *"cannot tell which `DynamicArray` to construct — give the type arguments"* even when the turbofish IS written, so it advises the thing the author already did — still so at `0.9.317`, repro `kr21_3`); `@compileFor` gates whole declarations BY DESIGN — never a member (the maintainer, 2026-09-30: a platform's deltas live in per-platform TYPES, not in conditionals inside one) — but a `type adapter <…>` block takes no attribute at all (its grammar rule, `marked_adapter_declaration` in `kama.y`, never admitted one), so a platform-specific adapter conformance cannot be gated; contract-refinement thunks; `Fixed<B,const F>` implementing `Real`; ⚠️ **`@align`/`@packed` on an `enum` is NOT a silent miss — re-probed 2026-09-10, it is a clean refusal** naming the reason and the fix (`type enum E : IntType`), so what is left here is only whether a tagged enum should honor a layout attribute at all; an `expose fn` with neither `@callerThread` nor `@foreignEntry` is not a region, so a host-called body reading a mutable static is unchecked (a source break for 11 in-tree files if required) | — | [§2](ROADMAP_DETAIL.md#s2) |
| KR-107 | **An `fnptr` type is not a type argument** — `DynamicArray<Op>` (the callback-registry shape SPEC names) does not instantiate: the turbofish is refused as ambiguous, the instance has no methods, and the element store trips the fnptr-bind rule. Any generic over an `fnptr` element, `BindableFunctionPtr` included. Found probing the default rule (`0.9.480`) | — | [§2](ROADMAP_DETAIL.md#s2-fnptr-type-arg) |
| KR-108 | **`DatagramSocket` generic over its address** — so `UnixDatagram` implements it beside `UdpSocket`: a syslog client sends to `/dev/log` or UDP 514 through one code path. A source break for `<S: DatagramSocket>` bounds; the maintainer rules on it | S | [§1](ROADMAP_DETAIL.md#s1-datagram-address) |
| KR-109 | **`DeError` names the field it reports** — since `0.9.516` a derived `deserialize` returns `Err(MissingField)` for a field the data left out, and the message is "missing field", not "missing field `port`"; `TypeMismatch` likewise names nothing. A payload changes a prelude enum that every backend latches as its sticky error code (`DeError` would become destructible), so the shape wants a maintainer verdict before any code | M | [§4](ROADMAP_DETAIL.md#s4-deerror-names) |
| KR-110 | **An untyped JSON value — `text::json`'s document tree** — a JSON document is read only through a typed `@generate(Deserializable)` schema: unknown fields are skipped, and every member a schema does not own is declared `Optional` or `@field(default)`. A document whose KEYS are data (glTF's `extensions`, a response keyed by ids), or whose shape a program only partly knows, has no reading at all. A tagged enum (`Null`/`Bool`/`Number`/`String`/`Array`/`Object`), a parse and a write, and whether a typed field may hold one as an opaque subtree. Number fidelity past 2^53 is the open design question | M | [§4](ROADMAP_DETAIL.md#s4-json-value) |
| KR-111 | **A project or dependency named `std` or `core` installs and cannot be imported** — `import { core::v }` resolves to the floor module and `std::…` to the stdlib, so either package is unreachable, and a project file `src/core.kama` collides with the prelude's own (`two files of module core`). `kama seed` refuses both names; the manifest reader cannot yet, because `lib/kama.json` is itself named `std`. Refuse both as a project `name` and as a dependency key | S | [§10](ROADMAP_DETAIL.md#s10-std-core-names) |
| KR-22 | **Collections knobs** — HashDoS-resistant keyed hashing; zero-size-field elision; thin smart-ptr handles | — | [§5](ROADMAP_DETAIL.md#s5) |
| KR-23 | **Performance** — bench cohort (add Zig), serialization benchmark track, devirtualization ladder, CPU-tuning knob | — | [§9](ROADMAP_DETAIL.md#s9) |
| KR-24 | **Hot-reload library** — `dlopen` + file-watch + fn-pointer rebind. Both compiler primitives already ship | — | [§8](ROADMAP_DETAIL.md#s8) |
| KR-25 | **Safe `std::gpu` binding wrapper** — wgpu handles→RAII `type resource`s (as `std::net` wraps sockets) + a typed acquire result naming `Occluded`; the seam's size accessor and discarded event queue. Scope: exactly the handles the seam already touches, nothing above them | M | [§8](ROADMAP_DETAIL.md#s8) |
| KR-26 | **`std::input`** — the seam pumps the window event queue and throws it away, on both targets: no keyboard, mouse, wheel, pointer-lock, resize, focus or gamepad. Peer of `std::gpu`, and the reason an engine on kama derives its own window | M | [§8](ROADMAP_DETAIL.md#s8) |
| KR-39 | **`@noheap` cannot cross a contract slot even when the callee is knowable** — UNBLOCKED — allocator-aware errors shipped `0.9.395` (SPEC *Global allocator*; `tests/global_allocator_serde.kama`) — and its original premise is retired. Its first concrete consumer is measured: a serde error path under `--no-heap` with a declared pool is refused for the DISPATCH, not the heap — dropping `Owned<Error>` goes through a contract-dispatched destructor (`tests/global_allocator_serde.kama` builds and runs, and the same file under the flag does not). The row read "`serializeJsonBuffer` CONSTRUCTS its backend, so the compiler could prove the callee": true at the entry point, and useless where the proof is needed — the slot calls are in the synthesized `X__serialize(X*, Serializer* w)`, written once for every backend, so devirtualizing in emission reaches none of them (measured planning this row, `0.9.352`). It also unblocks nobody yet: no serde path is heap-free even with a proven callee (every backend ctor allocates, readers take an owned buffer, no fixed-buffer writer exists, every `Err` boxes). What answers serde is the DECLARED global allocator (`@globalAllocator`, `0.9.377`): every allocation funnels into a pool whose body `--no-heap` checks, so a pool over program-owned storage is provably not the system heap. What is left is user contracts: resolving a slot through the vtables the program actually builds | M? | [§4](ROADMAP_DETAIL.md#s4) |
| KR-79 | **Declared reachability properties — generalize what `--no-heap` already proves** — `--no-heap`/`@noheap` is a whole-program reachability proof with a general engine (the reach walk over the call graph, the region marker, the chain-shaped diagnostic, the `@heap extern fn` escape) and exactly ONE hardcoded predicate: does this edge allocate. The same walk decides *no panic*, *no recursion* (bounded stack — what embedded and real-time users actually ask for, and a cycle check on a graph already built), *no blocking syscall* (audio/render callbacks, ISRs) and *no unsafe*. Each is a property asserted in a comment today and verified by reading, which is the class of claim this repo refuses to trust. Opened 2026-09-20 after looking at Bend 2's `LAWS.bend`: the same instinct reached from dependent types, and the point is that kama needs none of that — these are properties about REACHABILITY, not about values. Unscoped: the surface, per-function vs per-build, how a property crosses a contract slot (inherits KR-39 verbatim), and whether user-defined properties are a goal — all want a design pass and a written verdict before any code | — | [§5](ROADMAP_DETAIL.md#s5) |
| KR-40 | **Binding comments** — documentation syntax that cannot drift from the declaration it references. Motivating case: a contract member with no caller in kama source (the compiler emits the call), where a reader asks "why is this here?" and a comment is the only answer — and comments are not binding. Wanted at the declaration, checked against what it names, so a rename or a removal is an error rather than silent rot. Unscoped: needs a design pass on what a binding comment may assert before any syntax is proposed | — | [§1](ROADMAP_DETAIL.md#s1) |
| KR-68 | **The OS seam needs 25 system headers because it ships bodies** — ⚠️ **re-scoped 2026-09-18 after its fan-out half was split out and shipped (`0.9.408`); measure AFTER that, which may retire this one.** ✅ **MEASURED ON WINDOWS 2026-09-20, and the fan-out half is KEPT** — the revert condition is not met, by a wide margin. The same 7-file/17-TU program, same method (interleaved A/B, min of 40), on the Windows VM: an innocent TU preprocesses in **132 ms with the fix against 237 ms without — 104.5 ms saved per TU, and 1.79x**. Deterministic and noise-free beside it: **7,029 preprocessed lines / 855 macros with, 65,652 / 21,749 without**, where macOS differed by 164 lines. So the union was charging every TU the whole of `<windows.h>`, which is the cost both halves were filed for, and macOS's 0.32 ms was the platform where it does not bite rather than the verdict. ⚠️ The ABSOLUTE numbers are QEMU-inflated (this box emulates x64) — the ratio and the line counts are the finding. What is left here is DEPTH, not fan-out: `kama_os.h` pulls `<windows.h>`/`<winsock2.h>`/`<dirent.h>`/… only because its `static inline` bodies need them, and plain prototypes over plain C types would need none. Re-measured natively (macOS): `kama_os.h` 27.9 ms / 4,269 macros vs `kama_runtime.h` 22.3 ms / 1,112 — so **5.6 ms per TU here against the Windows VM's 2x and 21,748 macros: mostly a WINDOWS win**. Costs inlining of the thin syscall wrappers (the performance invariant applies), so the tradeoff is measured on a native box before any code. ⚠️ **The `--no-heap` verdict READS those bodies** (`0.9.401`) — the TU they move to joins that scan in the same commit, at no cost (measured below noise); `tools/check-header-scan.sh` is the tripwire. ⚠️ Separately measured and unrowed: `kama_runtime.h` is the DOMINANT per-TU cost on macOS (~18 of 22.5 ms), bigger than the OS seam | M? | [§9](ROADMAP_DETAIL.md#s9) |
| KR-83 | **Extended `asm` (operand constraints) and `@naked` functions** — SPEC called them "tracked follow-ons" with no row; they are the MCU seam's remaining half | — | [§5](ROADMAP_DETAIL.md#s5) |
| KR-94 | **A struct's `sizeof` and any `alignof` as a compile-time VALUE** — refused in every `comptime` since `0.9.444`, because the target's C compiler decides layout and a kama answer that disagreed would be a silent miscompile. Unlock: a per-target layout model in the compiler, re-checked by the C compiler — every baked size emits a `_Static_assert`, so a wrong model fails the build, never the program. (`comptime assert(cond: sizeof(T) == 20, …)` already works: C checks it) | M? | [§5](ROADMAP_DETAIL.md#s5) |
| KR-96 | **Operators on an enum: support them, or declare a non-goal** — an operator in an enum body was silently dropped until `0.9.443` and is now refused (`tests/xfail/enum_operator.kama`); `\|` over enum values stays refused on purpose. Wants a maintainer verdict before any code | — | [§2](ROADMAP_DETAIL.md#s2) |

## LATER — tooling & ecosystem

Most of this gates on the repo going public.

| id | item | detail |
|---|---|---|
| KR-27 | **Registry — hosted deployment (M3.3)** + mandatory verification and the trust model — IN PROGRESS: `registry.kama-lang.org` is live and the built-in default since `0.9.457`, and `@kama/sodium@0.5.0` is its first package (2026-09-27). The index lives in git and deploys to Pages; the tarballs live in a locked R2 bucket (`dl.kama-lang.org`) behind a `_redirects` rule. What remains is the trust model — an allowed-signers set, then CI/OIDC provenance — and a resolution-time compiler check | [§10](ROADMAP_DETAIL.md#s10) |
| KR-28 | **Editor/registry registrations** — Zed extension registry, nvim-treesitter, linguist, Helix upstreaming, Marketplace publish | [§10](ROADMAP_DETAIL.md#s10) |
| KR-29 | **tree-sitter accepts 78 of the 84 reserved words as a binding name** — `Thing else = …` renders as a valid declaration in every editor on this grammar, and the compiler then rejects it. The two reserve differently by construction: `kama.l` consults one table at every identifier, tree-sitter extracts keywords CONTEXTUALLY and a binding site expects `$.identifier`. ⚠️ `check-treesitter.sh` cannot see this class, and the one fixture that looks like it covers it passes on its USE site, not its declaration | [§10](ROADMAP_DETAIL.md#s10) |
| KR-30 | **LSP residuals** — one build configuration per server process; the prelude-analysis floor per keystroke; ⚠️ **a receiver typed by a generic instance over an UNBOUND parameter resolves to nothing in completion** — `const ref Node<K>` inside another generic offers no members at all, PUBLIC ones included, while the same receiver spelled `Node<int32>` offers every one (measured `0.9.300`, writing the `friend`-across-generics fixtures; it is receiver resolution, not visibility); a type argument's span runs into a NAMED size (`InlineArray<int32>#(LIMIT)`: hover on `LI` answers `int32`) | [§10](ROADMAP_DETAIL.md#s10) |
| KR-31 | **`kama fmt`** — a native formatter. Substrate settled: use the compiler's own front end, **not** tree-sitter | [§10](ROADMAP_DETAIL.md#s10) |
| KR-33 | **`kama query` residuals** — no `callers-of`/`implementors-of`, no stdin/unsaved-buffer mode; a `fnptr` signature type has no definition site | [§10](ROADMAP_DETAIL.md#s10) |
| KR-84 | **`kama describe --json`** — the language surface as data, the other half of `kama query --json` (GOALS §7) | [§10](ROADMAP_DETAIL.md#s10) |

## FUTURE — the big arcs, in this order

The 2.0 work. Nothing here starts before the NOW list is done.

| id | item | detail |
|---|---|---|
| KR-34 | **Editor tooling** — the front end as a reusable query API; everything later rides on it | [§10](ROADMAP_DETAIL.md#s10) |
| KR-35 | **Scripting / multimodal — the flagship 2.0.** First step: refactor the C emitter behind an abstract backend interface | [§7](ROADMAP_DETAIL.md#s7) |
| KR-36 | **Self-hosting — the capstone, LOWEST priority.** A maturity milestone, not an enabler | [§7](ROADMAP_DETAIL.md#s7) |
