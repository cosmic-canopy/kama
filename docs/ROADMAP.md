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

**Next id: KR-76**

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

| id | item | size | detail |
|---|---|---|---|
| KR-74 | **`--no-heap` derives its facts for kama's own runtime instead of trusting `@heap` marks** — the reach walk reads only the C the compiler writes, so at the runtime's boundary it stops deriving and trusts 55 hand-placed `@heap` marks: one bit, no guard, missed three times in a week (`kama_path_meta`, `kama_resolve_host`, startup argv), and unable to say whether the C reaches the funnel (legal under a declared pool) or a foreign heap (never legal). Feed the shipped headers to the same scanner: funnel leaf → edge into the declared pool, foreign leaf (seven names: `getaddrinfo`, `GetEnvironmentStringsW`, `opendir`, `pthread_create`, `CreateThread`, `CreateProcessW`, and the funnel's own `malloc`) → a fact, always. The marks on `kama_*` externs go; `@heap` stays for a USER extern into C kama cannot see. Then `fmt`, strings, serde and `args()` are legal under a pool, and `resolve` is refused naming `getaddrinfo`. Brief: [allocation.md §6](design/allocation.md). **Mac** — changes the verdict on every target | M | [§5](ROADMAP_DETAIL.md#s5) |
| KR-66 | **A program cannot reach its `@globalAllocator` instance** — the pool is one C global only the funnel calls, so kama code cannot read its live count or high-water mark, and a module `static` cannot hold them either (per-isolate). Scheduled 2026-09-17; recommended shape: a prelude intrinsic `globalHeap<Pool>()` returning `ref Pool`, the compiler checking `Pool` is the declared type | S? | [§5](ROADMAP_DETAIL.md#s5) |
| KR-58 | **The Windows OS seam allocates a wide path on every file-system call** — `kama__wpath` puts every UTF-8 path on the heap as UTF-16, so ten `std::fs` externs (`kama_open_*`, `kama_unlink`, `kama_mkdir`/`rmdir`, `kama_is_symlink`, `kama_exists`, `kama_rename`, `kama_path_meta`) allocate on Windows, where POSIX allocates nothing, and a `--no-heap` program cannot touch a path. ⚠️ **`kama_path_meta` carries no `@heap` mark**, so `--no-heap` accepts `std::fs::stat` and it allocates on Windows (probed `0.9.369`). Convert into a caller-owned stack buffer sized to the NT limit and drop the marks; stack reserve, probe cost and one tier vs two are measured and decided first. The seven one-platform externs were judged and stay heap. **Windows box**, next there | M | [§5](ROADMAP_DETAIL.md#s5) |
| KR-2 | **No incremental build** — every build recompiles everything. ⚠️ Largely already answered and never rowed: `zig cc` has a content-addressed per-TU object cache (measured 4.13 s cold, **0.11 s after editing one file**), so a bundled install is incremental today and kama's own object cache is mostly moot. What is left is the SLIM install, which uses clang and has no cache | — | [§9](ROADMAP_DETAIL.md#s9) |
| KR-3 | **Job system / event-loop scheduler** — libraries on the shipped concurrency primitives; the pool is sized, **scheduling** is what is missing | ? | [§6](ROADMAP_DETAIL.md#s6) |
| KR-5 | **`std::io` transform adapters** — compression et al., composing with serde and net | — | [§1](ROADMAP_DETAIL.md#s1) |
| KR-7 | **`std::process`** — live/streaming child-stream reads | — | [§1](ROADMAP_DETAIL.md#s1) |
| KR-8 | **Restricted-private contract members** — *to consider*, not scheduled: a contract may declare a member non-public, with a `friend` grant naming who may reach it, and an implementer then matches that visibility instead of being forced `public`. It is no longer a serde blocker — that justification was written before the container seam was measured, and the seam turned out to need no contract at all. What stands on its own is expressiveness: binding types together with a guarantee that is not part of the public surface. ⚠️ **`private` on a contract member is ACCEPTED AND INERT today** (measured `0.9.269`): it parses and the marker is silently dropped, so with a `public` implementer a call through a contract-typed value reaches it. That is the sibling `0.9.266` left behind when it refused `friend` on a contract on the premise that a contract *cannot* have a private member. Deliberately **not fixed** — it is inert, it blocks nothing, and whether it is refused or made real is this row's decision. Both spellings already parse, so the work is emitter-only | M | [§4](ROADMAP_DETAIL.md#s4) |
| KR-19 | **MCU toolchain packaging** — board presets, vendor-HAL glue, a real-hardware flash pass; AVR later | — | [§5](ROADMAP_DETAIL.md#s5) |
| KR-21 | **Remaining language limitations** — no bound spells "an integer primitive", so `cast<T>` in a generic is checked per instantiation (the stdlib avoids it: pass by address, move `sizeof(T)` bytes); unresolved type names inside generic arguments ⚠️ (re-probed: the diagnostic is *"cannot tell which `DynamicArray` to construct — give the type arguments"* even when the turbofish IS written, so it advises the thing the author already did — still so at `0.9.317`, repro `kr21_3`); `@compileFor` is whole-declaration only, so it cannot gate a single method or one `implements` block (a gated member is refused cleanly, a same-named pair included); contract-refinement thunks; `Fixed<B,const F>` implementing `Real`; ⚠️ **`@align`/`@packed` on an `enum` is NOT a silent miss — re-probed 2026-09-10, it is a clean refusal** naming the reason and the fix (`type enum E : IntType`), so what is left here is only whether a tagged enum should honor a layout attribute at all; an `expose fn` with neither `@callerThread` nor `@foreignEntry` is not a region, so a host-called body reading a mutable static is unchecked (a source break for 11 in-tree files if required) | — | [§2](ROADMAP_DETAIL.md#s2) |
| KR-22 | **Collections knobs** — HashDoS-resistant keyed hashing; zero-size-field elision; thin smart-ptr handles | — | [§5](ROADMAP_DETAIL.md#s5) |
| KR-23 | **Performance** — bench cohort (add Zig), serialization benchmark track, devirtualization ladder, CPU-tuning knob | — | [§9](ROADMAP_DETAIL.md#s9) |
| KR-24 | **Hot-reload library** — `dlopen` + file-watch + fn-pointer rebind. Both compiler primitives already ship | — | [§8](ROADMAP_DETAIL.md#s8) |
| KR-25 | **Safe `std::gpu` binding wrapper** — wgpu handles→RAII `type resource`s (as `std::net` wraps sockets) + a typed acquire result naming `Occluded`; the seam's size accessor and discarded event queue. Scope: exactly the handles the seam already touches, nothing above them | M | [§8](ROADMAP_DETAIL.md#s8) |
| KR-26 | **`std::input`** — the seam pumps the window event queue and throws it away, on both targets: no keyboard, mouse, wheel, pointer-lock, resize, focus or gamepad. Peer of `std::gpu`, and the reason the first engine on kama derives its own window | M | [§8](ROADMAP_DETAIL.md#s8) |
| KR-39 | **`@noheap` cannot cross a contract slot even when the callee is knowable** — UNBLOCKED — allocator-aware errors shipped `0.9.395` (SPEC *Global allocator*; `tests/global_allocator_serde.kama`) — and its original premise is retired. Its first concrete consumer is measured: a serde error path under `--no-heap` with a declared pool is refused for the DISPATCH, not the heap — dropping `Owned<Error>` goes through a contract-dispatched destructor (`tests/global_allocator_serde.kama` builds and runs, and the same file under the flag does not). The row read "`serializeJsonBuffer` CONSTRUCTS its backend, so the compiler could prove the callee": true at the entry point, and useless where the proof is needed — the slot calls are in the synthesized `X__serialize(X*, Serializer* w)`, written once for every backend, so devirtualizing in emission reaches none of them (measured planning this row, `0.9.352`). It also unblocks nobody yet: no serde path is heap-free even with a proven callee (every backend ctor allocates, readers take an owned buffer, no fixed-buffer writer exists, every `Err` boxes). What answers serde is the DECLARED global allocator (`@globalAllocator`, `0.9.377`): every allocation funnels into a pool whose body `--no-heap` checks, so a pool over program-owned storage is provably not the system heap. What is left is user contracts: resolving a slot through the vtables the program actually builds | M? | [§4](ROADMAP_DETAIL.md#s4) |
| KR-40 | **Binding comments** — documentation syntax that cannot drift from the declaration it references. Motivating case: a contract member with no caller in kama source (the compiler emits the call), where a reader asks "why is this here?" and a comment is the only answer — and comments are not binding. Wanted at the declaration, checked against what it names, so a rename or a removal is an error rather than silent rot. Unscoped: needs a design pass on what a binding comment may assert before any syntax is proposed | — | [§1](ROADMAP_DETAIL.md#s1) |
| KR-57 | **A binding may take the name of a function in scope** — SPEC says kama has no shadowing, but `fn int32 use(int32 helper) { return helper + helper(); }` builds and runs; with a bare PRELUDE function (`args`, `print`) the emitted C local shadows the C function and clang refuses what kama accepted. Refuse the binding like every other shadowing, not rename it in C | S? | [§2](ROADMAP_DETAIL.md#s2) |
| KR-62 | **A generic instance named only inside `sizeof`/`alignof` is never instantiated** — `usize s = sizeof(DynamicArray<int64>);` (or `Simd<float32>#(4)`) with no other use of the type reaches clang as an undeclared identifier; kama accepted it. Register the instance when the operand is resolved, as a local declaration does | S? | [§2](ROADMAP_DETAIL.md#s2) |
| KR-67 | **A kama name that any C header `#define`s breaks the C** — fields, parameters, locals, payload fields, contract slots and fn-pointer locals reach C unprefixed, so a macro rewrites them: `float32 near` fails in clang on Windows once `std::fs` is imported (`<windef.h>`), `_LP64`/`errno`/`st_mtime`/`s6_addr` do on macOS and Linux, and a vendor header can do it anywhere; 21,748 macros are in scope of one Windows TU and 4,708 of one macOS TU. **RULED 2026-09-17** (brief: [design/c-names.md](design/c-names.md)): every name kama OWNS reaches C as `k_<name>`, the declared C surface (`extern`, `expose`) keeps its spelling, and the mangling stays reversible for KR-32. **Built on the Mac/Linux — its gate is `./dev matrix`; the Windows box is the witness** | L | [§2](ROADMAP_DETAIL.md#s2) |
| KR-32 | **The debugger shows emitted-C names and values** — stepping and breakpoints are already kama-source-level, but a `string` inspects as `kama_string {data,len,cap}`, an `Optional` as its tagged union, a type as `_Ffile__V`, a frame as `_Ffile__V__make`, and an enum value as `_Ffile__Mode_CopyFile`. Three parts: LLDB summaries for the values; synthetic children so a struct's fields read as kama names; and a name layer in the VS Code extension between it and CodeLLDB for LOCALS, the CALL STACK and watch expressions, which no formatter can reach. The first two are independent; the third needs KR-67's name map. Moved out of LATER 2026-09-17: KR-67 prefixes locals too, so this stops being polish | L? | [§10](ROADMAP_DETAIL.md#s10) |
| KR-68 | **Every module's C preprocesses the whole OS header set** — one generated `<name>.gen.h` carries every `extern` header, so importing `std::fs` anywhere puts `<windows.h>` (21,748 macros) in front of EVERY module's C. Preprocessing `kama_os.h` measured ~263 ms against ~121 ms for `kama_runtime.h` alone on the Windows VM (ratio only — that box is QEMU + x64 emulation). Moving the seam behind plain prototypes, with bodies in one TU, would cut per-TU preprocessing on every platform (GOALS #2) and shrink the macro surface; it costs inlining of the thin syscall wrappers, so it is measured on a NATIVE box first. Not a fix for KR-67 — a user's own `extern` header bleeds either way | M? | [§9](ROADMAP_DETAIL.md#s9) |
| KR-69 | **A misspelled triple-component flag in a gate is silently inactive** — `@compileFor(OS_WINODWS)` (and a `csources` entry's `compileFor`) builds and passes `kama check`: any `OS_`/`ARCH_`/`ABI_` name validates because triples are open, and the covering check skips a gate no known target activates. Wants a ruling on a closed component table | ? | [§2](ROADMAP_DETAIL.md#s2) |
| KR-70 | **A dependency cannot ship a prebuilt archive** — a relative `-L` in a dependency is refused (correctly) and `link` names no directory, so a package wrapping a cmake-built library leaves its consumer to link it. Rule first whether kama carries per-target binaries at all | ? | [§2](ROADMAP_DETAIL.md#s2) |
| KR-72 | **`--cc gcc` cannot build ANY kama program** — the driver always passes clang's `-Wno-error=incompatible-function-pointer-types` (`src/kama.driver.cpp`, the FFI callback demotion), and gcc has no such warning name, so `cc1.exe`/`cc1plus.exe` refuse it: a hello-world with `--cc gcc` exits 1 (measured `0.9.383`, msys2 gcc 16.2). Not Windows-specific — any gcc driver on any host. kama derives `gcc`→`g++` and documents gcc as a `cc`, so this is a supported path that never worked. Gate the flag on the driver family (the `deriveCxxDriver` name test is the seam) or probe once; a regression fixture needs a gcc, so it skips where there is none | S | [§2](ROADMAP_DETAIL.md#s2) |

## LATER — tooling & ecosystem

Most of this gates on the repo going public.

| id | item | detail |
|---|---|---|
| KR-27 | **Registry — hosted deployment (M3.3)** + mandatory verification and the trust model | [§10](ROADMAP_DETAIL.md#s10) |
| KR-28 | **Editor/registry registrations** — Zed extension registry, nvim-treesitter, linguist, Helix upstreaming, Marketplace publish | [§10](ROADMAP_DETAIL.md#s10) |
| KR-29 | **tree-sitter accepts 78 of the 84 reserved words as a binding name** — `Thing else = …` renders as a valid declaration in every editor on this grammar, and the compiler then rejects it. The two reserve differently by construction: `kama.l` consults one table at every identifier, tree-sitter extracts keywords CONTEXTUALLY and a binding site expects `$.identifier`. ⚠️ `check-treesitter.sh` cannot see this class, and the one fixture that looks like it covers it passes on its USE site, not its declaration | [§10](ROADMAP_DETAIL.md#s10) |
| KR-30 | **LSP residuals** — one build configuration per server process; the prelude-analysis floor per keystroke; ⚠️ **a receiver typed by a generic instance over an UNBOUND parameter resolves to nothing in completion** — `const ref Node<K>` inside another generic offers no members at all, PUBLIC ones included, while the same receiver spelled `Node<int32>` offers every one (measured `0.9.300`, writing the `friend`-across-generics fixtures; it is receiver resolution, not visibility) | [§10](ROADMAP_DETAIL.md#s10) |
| KR-31 | **`kama fmt`** — a native formatter. Substrate settled: use the compiler's own front end, **not** tree-sitter | [§10](ROADMAP_DETAIL.md#s10) |
| KR-33 | **`kama query` residuals** — no `callers-of`/`implementors-of`, no stdin/unsaved-buffer mode | [§10](ROADMAP_DETAIL.md#s10) |

## FUTURE — the big arcs, in this order

The 2.0 work. Nothing here starts before the NOW list is done.

| id | item | detail |
|---|---|---|
| KR-34 | **Editor tooling** — the front end as a reusable query API; everything later rides on it | [§10](ROADMAP_DETAIL.md#s10) |
| KR-35 | **Scripting / multimodal — the flagship 2.0.** First step: refactor the C emitter behind an abstract backend interface | [§7](ROADMAP_DETAIL.md#s7) |
| KR-36 | **Self-hosting — the capstone, LOWEST priority.** A maturity milestone, not an enabler | [§7](ROADMAP_DETAIL.md#s7) |
