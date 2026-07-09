# kama roadmap

The forward plan — near-term to long-term, read in sequence. The language's **history** lives in the git
log; what the language **is** lives in [SPEC.md](SPEC.md). This file is only *what's next*.

## The shape

- **1.0 — language complete.** Strings are **done** (Phase 3 shipped); **math** is the last core piece,
  after which the language surface is stable — you build *with* it, not *on* it.
- **1.x — systems & runtime.** Capabilities built ON the finished language: reflection + serialization,
  file I/O, networking, an embedded/MCU target. Mostly library + codegen, little new syntax.
- **2.0 — dual-mode scripting** (flagship): the *same* language usable compiled OR scripted, via a shared
  IR feeding C, direct-wasm, and a bytecode VM — the `kama` binary self-contained.
- **Concurrency — shared-nothing by construction** (1.x/2.0 direction): data-race freedom by removing
  shared mutable state, not a borrow checker. 1.0 ships a single-threaded core.
- **Engine track** (product north star): a portable lightweight **WebGPU** game engine, woven through
  1.x. Its Tier-0 math types are unblocked now.

## 1. Remaining before 1.0

1. ~~**Strings — Phase 3 (ergonomics).**~~ **Done.** `+` / `==` / `!=` operators (compiler special-cases
   string operands → `concat`/`equals`; `string` is a primitive, so not a user overload); owned
   `substring(start:, end:)` (byte-range copy); search `find` (→ `Optional<usize>`, null-safe),
   `contains`, `startsWith`, `endsWith`, `isEmpty`; transforms `trim`/`trimStart`/`trimEnd`, `replace`,
   ASCII `toLower`/`toUpper`; and a lazy `split(separator:)` → `Split` iterator (no collections import).
   All intrinsic on the primitive, native + wasm, ASan/UBSan-clean. Owned-string temporaries now RAII-drop
   correctly in operator/receiver/`if`-condition/chained-method positions, in string-intrinsic arguments,
   and as `foreach`-yielded owned values (`s.trim() == "x"`, `foreach (p in s.split(...))`). **Tracked
   limitation:** an owned-string rvalue passed by value to a *non-string-intrinsic* call (a user function,
   or an ownership-taking method) still leaks — bind it to a local; a general owned-temporary drop pass is a
   later refinement (the ownership of a by-value arg is call-specific). **Deferred to a later Unicode
   module** (tracked): Unicode-correct casing + whitespace (this phase is ASCII), `'é'` multibyte source
   char literals, string interpolation/`Display` (which also gives `string + <number>`), and an eager
   `List<string>` collect for `split`.
2. **Language completeness & memory-safety hardening (1.0 blockers).** "1.0 = language complete" can't ship
   with a **fundamental** (non-library) gap open — least of all a double-free in *safe* code. These are the
   fundamental gaps surfaced while hardening strings, promoted here from §2 (each reproduces with plain
   resources/collections, independent of the strings feature). Reserved-for-a-later-track keywords
   (`expose`, `volatile`) stay deferred — they hard-error, never miscompile, and aren't core semantics.
   - ✅ **By-value ownership of collections & `string` — DONE** (commit `f873a7c`). The `give`/`copy` +
     move-tracking machinery now covers a by-value collection/`string` in every hand-off position:
     assignment / field store (`this.a = give x`), function/constructor arguments, and returns. A by-value
     slot **OWNS** it (drops at fn-end), `ref` borrows, and read-only `kama_string__*` intrinsics keep
     borrowing (marker-free). Closes the confirmed `Box(x: x)` double-free; native + ASan/UBSan clean. The
     old "by-value collection params → post-1.0" deferral is **retired**.
   - ✅ **Owning-value collection ELEMENTS — `List<string>` DONE** (commit `55f45f1`). `string`/collections
     are now first-class `Copyable`/owning elements: `string` satisfies `Copyable`, `copy` of a
     collection/string into a variant deep-copies, a bare named payload into a variant is rejected, the raw
     `Ptr` element store honors `give`/`copy`, and `foreach` drops its owned by-value binding. `List<string>`
     now supports `add(item: give s)`, by-value `foreach`, `copy`, and collecting `split` pieces into a list
     — ASan/UBSan-clean. **Remaining (smaller, same axis):** `Array<string>` element place-store
     (`a[i] = <owned>`) and nested collections (`List<List<string>>` — recursive element copyability).
   - **`.chars()` double-evaluates its receiver** (emits it twice, for `.data`/`.len`) — a side-effecting /
     owned rvalue receiver desyncs → OOB/leak. Apply the same stable-ref gate `.split()`/`.chars()` already
     use, or a single-eval lowering. Small.
   - **General destroy-temporaries pass.** Owned rvalues the compiler doesn't specifically drop leak
     (Phase 3 covered operators / receivers / `if`-conditions / string-intrinsic args / `foreach` yields;
     residual = non-string-intrinsic by-value args, owned temps in `while`/`for` conditions). Durable fix:
     a full temporary-drop pass. **The string-`ref` slice of this is ✅ DONE** — a string rvalue (literal /
     `+` concat / call result) passed to a `ref string` param is now materialized into a scope-freed temp so
     `&temp` is legal C (was "cannot take the address of an rvalue"); the borrow is read-only and the temp
     frees at scope end (ASan/LSan clean). `readFile("foo.txt")` / `f(a + b)` work; named lvalues keep their
     direct `&`. Fixture `tests/ref_string_temp.kama`. The general (non-string, arbitrary-owned-temp) pass
     remains.
   - **Parser/typer accept a narrower grammar than expected — surprising ergonomics gaps** (each with a
     clean workaround, none miscompile; found building `std::fs`/`std::net`). Worth closing before 1.0 since
     they bite constantly:
     - ✅ **DONE** — `cast<T>(...)` now accepts a full **`expression`** operand (was `unary_expression`,
       so `cast<int32>(a + b)` was a syntax error). The `( … )` already delimits it, like a parenthesized
       primary; no new grammar conflicts. Fixture `tests/cast_expr.kama` (arithmetic/modulo/ternary).
     - **A value-producing `match` block arm (`case X: { …stmts…; tail }`) must end in a `statement_expression`
       (call / assignment / `++`/`--` / inline-ctor / nested `match`)** — a bare identifier / arithmetic /
       ternary tail is a syntax error, because the block's value is its last *statement* and Kama (like C#)
       does not treat a bare `x + 1;` as a statement. **Deferred — a genuine design fork, not a quick fix:**
       (a) a Rust-style tail expression `{ stmts; tailExpr }` (no trailing `;`) is the clean answer but is
       LALR(1)-hard (after an identifier, "start of a `;`-statement" vs "the `;`-less tail" is a
       shift/reduce conflict); (b) relaxing `expression_statement` to any `expression` is easy and
       conflict-free but silently allows no-op statements (`x + 1;`) everywhere, against the "explicit /
       catch bugs" ethos. **Workaround is clean and idiomatic** (extract the tail into a helper and call it,
       or use a non-block `case X: <expr>` arm — which already accepts any expression incl. ternaries), so
       this stays parked pending an explicit call on (a) vs (b).
     - ✅ **DONE** — inline `match (Type::staticFn(...))` now works. `exprClass` inferred return types for
       instance-method and free-function calls but not **static-method calls** (`File::open(...)` — a
       qualified identifier, not a `MemberAccessNode`), so the subject's type was unknown → "requires an
       enum subject." Added the `Class::method` resolution (mirrors `emitFnPtrBind`). Fixture
       `tests/match_static_factory.kama`. (The Ok payload being a resource was a red herring — the gap was
       purely the static call.)
   - **`contract` refining a `contract`** (multi-level contract inheritance) — parses, not lowered.
   - **Multibyte source char literal `'é'`** — lexer gap (write `'\u{E9}'` today).
   - **Verify-then-1.0-or-downgrade:** explicit type args when inference fails
     ([kama.cemit.cpp:3467] — does turbofish `f::<T>()` already cover it?); inline `new Concrete` into a
     smart-ptr-over-interface ([kama.cemit.cpp:5623] — has a bind-to-local workaround).
3. **Standard library — native I/O foundation (`std::io` / `std::fs` / `std::net`).** Landed to unlock a
   native single-binary HTTP static-file server (the first real Kama program). **Library over the existing
   FFI — no compiler features** beyond a one-line prelude addition (`enum Unit`, the empty `Result<Unit,E>`
   payload — the analogue of Rust's `Result<(),E>`, chosen so std keeps **one** error convention). Platform
   differences live in a single bundled C bindings header, **`kama_os.h`** (pay-for-what-you-use: pulled in
   only via `extern "kama_os.h";`), which keeps OS aggregates (`struct stat`, `sockaddr_in`, `dirent`, `DIR`)
   opaque behind `static inline` accessors — the standard FFI boundary (Rust `libc` / Zig `@cImport` /
   Go `syscall`), forced by "extern struct emits the literal C name, no typedef."
   - ✅ **POSIX (Linux/macOS/iOS/Android) — DONE.** `std::io` (`IoError` + `lastError` classification);
     `std::fs` (`File` RAII fd + `open`/`read`/`readAll`/`writeAll`, free `readFile`/`writeFile`/`stat`/
     `readDir`/`remove`); `std::net` (`TcpListener` bind/accept, `TcpStream` connect/read/writeAll, both
     RAII-closing). Production bar on the shipped subset: `EINTR` retry, write-to-completion, errno→`IoError`,
     no fd leak on any error path (capture error before close). 6 fixtures (fs roundtrip >64 KiB / notfound /
     readdir / raii-5000-opens; net loopback / refused) — native + ASan/UBSan clean (347/347).
   - **Windows (first-class) — WRITTEN, CI-verifying.** `kama_os.h`'s `#if defined(_WIN32)` branch is
     filled: Winsock (`WSAStartup`/`SOCKET`/`closesocket`, `WSAGetLastError`→`errno` translation so
     `kama_last_error()` is uniform) + CRT (`_open`+`_O_BINARY`/`_stat64`/`_read`/`_write`) +
     `FindFirstFileA` dir cursor. Driver links `-lws2_32` on native Windows; release payloads now ship
     `kama_os.h`; a best-effort `windows-latest` leg runs the full suite in CI (MSYS2 UCRT + clang).
     All 29 binding signatures verified identical to the POSIX branch (a mismatch = guaranteed Windows
     compile error). **Blind write — not compilable on the Linux/macOS dev boxes; first green Windows CI
     run is the real verification.** Flip the CI leg to required once reliably green.
   - ✅ **WASM — build-checked.** `std::fs` **builds and runs** under emscripten (`--target wasm --cc emcc`,
     verified `fs_roundtrip` → exit 42 under node's in-memory MEMFS — the virtual FS is fully functional).
     `std::net` **compiles** to wasm (emscripten ships the `<sys/socket.h>` shims) but does not run without a
     WebSocket proxy — wasm net stays deferred to the WebRTC/host path (2.0). Native is the first-class target.
   - **Deferred (tracked):** UDP, DNS/`getaddrinfo`, ephemeral-port `getsockname`, buffered readers, richer
     `Metadata` (mtime/perms), path helpers, `mkdir`.
   - ✅ **End-to-end proof — `examples/httpd/`.** A ~200-line static-file HTTP/1.1 server (the
     `python -m http.server` spirit) over `std::net` + `std::fs` + strings — the first real Kama program.
     Serves the actual `site/` (kama-lang.org) locally: verified over `curl` — `200` html/png/txt
     (binary-safe, correct `Content-Type`/`Content-Length`), `404`/`400`(traversal)/`405`; ASan/UBSan-clean
     under live traffic and LSan-clean on the startup path. Self-contained (installed `kama` only) so the
     folder seeds a standalone repo. `GET`-only, `Connection: close`, single-`recv` request — honestly a
     dev/preview server (keep-alive, dir listings, percent-decoding, threading deferred).
4. **Math layer.** `Vec2/3/4`, `Mat4` (`Fixed<float32,16>` or `Fixed<Vec4,4>`), `Quat` as ordinary `value`
   types — **no prerequisites** (operator overloading + `Fixed` shipped). Unblocks the engine's Tier-0 math.
   Buildable as value types now; package as a stdlib module once the packaging question (§3) is settled —
   forward-compatible either way.
5. **Docs reconcile → tag 1.0.** 1.0 is the API-stability point; naming/case conventions are fixed here
   (PascalCase types, lowerCamel methods, no `I`-prefix on contracts, lowercase `string`).

## 2. Deferred language bits (tracked)

Policy: **no known limitation stays untracked** — each is scheduled or a declared non-goal. The
**fundamental** gaps (multibyte char literals, `contract`-refining-`contract`, by-value collection
ownership + owning elements) were promoted to §1.2 — a "language complete" 1.0 closes them. What remains
here is genuinely later-track or opt-in.

- **String interpolation `"${x}"` + formatting** — needs a general to-string / `Display`-like mechanism
  (also covers number→string); sequences with reflection (its to-string substrate).
- **`export` keyword** — reserved, hard-errors today. **Split by scope:** a **minimal `export`** (C-ABI
  linkage for `--shared` reload entry points) is **pulled forward to 1.x** (§5, engine dev-loop); the
  **full `export`** (wasm module exports + the scripting host interface) stays **2.0** (§7). The keyword
  slot is reserved at 1.0 either way, so activating it in 1.x follows the same reserved-then-lit pattern
  as `volatile`.
- **`volatile` keyword** — reserved → **1.x embedded** (emit C `volatile` for ISR↔loop flags / MMIO).
  (`contract`-refining-`contract` and by-value collection ownership moved to §1.2 as 1.0 items.)
- **Minor niceties (post-1.0):** an opt-in `Equatable` derive (auto `==` for `value` types) and
  post-increment returning the old value in expression position (`i++` works as a statement today).
- **Non-goal — function / constructor overloading.** Deliberately not planned: it conflicts with "one way
  to do a thing," and **named parameters** already cover the disambiguation overloading is usually reached
  for. **Operators are the sanctioned exception** — a type may carry several `operator*` distinguished by
  operand type (`mat*vec`, `mat*mat`, `v*s`, `s*v`), matching C++/C#/Rust. Reopen only if a concrete case
  shows named params can't express it.

## 3. Open design questions (settle before the work they gate)

- **Modular / opt-in stdlib — how does "pay for what you use" work?** The **prelude mechanism**
  (`PRELUDE_SRC` — parsed kama collected before user code, the model `Optional`/`Result`/`Chars` use) is
  the seed: a stdlib = more prelude-collected kama modules in a `Std` namespace. Generic types already
  emit only when instantiated, and `--gc-sections` prunes unused functions in release. Open: whether that
  pruning suffices, or explicit per-module opt-in / dead-function elimination is warranted before a large
  stdlib. A design pass before the container/math packaging.
- **Structural → nominal contracts for bounds.** `foreach` is now nominal (an iterator must `implements
  Iterator`/`IteratorMut`, a container `Iterable`/`IterableMut`), but a generic bound `<T: Weighable>`
  still accepts a type **structurally**. Decision (user): go fully nominal — require `implements` for
  bounds too, so the keyword is load-bearing everywhere and errors pin to the declaration. A migration
  pass over the bound fixtures.
- **`Copyable` as a formal contract.** Today it's recognized nominally by name (`implements Copyable`);
  formalize as `type contract Copyable { This copy(); }` — bundle with the structural→nominal migration.
- **Math: language-level value types vs a stdlib module.** Recommendation: build as `value` types now (no
  prereqs), and *package* as a module once the modular-stdlib question above is settled.

## 4. Reflection + attributes (1.x — design brief)

Opt-in compile-time reflection driving polymorphic serialization — the final self-hosting-stdlib step. The
only new *language* surface is the attribute mark; serializers land as modules.

- **Opt-in / zero-cost when unused.** *Nobody pays a byte or a cycle if they don't reflect.* A type is
  inert unless it opts in; no global registry, no per-type metadata emitted unless requested.
- **Declarative marks on types** — an attribute syntax (`[Reflect]` / `@derive(...)` — spelling TBD) opts a
  type in. This is the new language surface (grammar + AST + emit).
- **Granular — per-field opt-in** (mark which fields reflect / serialize / skip), not all-or-nothing.
- **Scenegraph-capable (composition)** — reflect object graphs, not just flat structs, which needs
  **temporary IDs** so a serialized graph round-trips shared/back references without cycles.
- **Polymorphic by serializer** — one reflection description, many back ends (text / binary / JSON / YAML;
  little-endian canonical for binary). The serializer is a module; reflection is the substrate it reads.

Mostly codegen over `ClassInfo`. **String interpolation rides on the same to-string substrate**, so it
sequences here.

## 5. 1.x — systems & runtime (post-1.0)

Capabilities built on the finished language — the substrate the engine needs (asset I/O, scene
serialization, networking).

- **Shared-lib build + minimal `export` (pulled forward from 2.0 — engine-unblocking).** `kama build
  --shared` → `.so`/`.dylib`/`.dll` and a **minimal `export`** (C-ABI linkage for entry points) — the two
  small compiler primitives under the engine's desktop **dev-loop hot-reload** (§8). Both are independent
  of the 2.0 IR refactor, so they land here to make engine iteration fast *early* rather than waiting on
  the VM. The reload loop itself is a library (`dlopen`/watch/rebind over `unsafe`/`Ptr`), not roadmap
  work. Deliberately excludes the full 2.0 `export` (wasm module exports + scripting host, §7).
- **Reflection + declarative serialization** — see the brief above; back ends follow as modules.
- **File I/O** — safe file APIs; gates serialization and engine asset loading.
- **Networking** — native UDP/TCP sockets vs browser **WebRTC DataChannels** (unreliable) /
  **WebSockets** (reliable), via FFI (the browser has no raw sockets — a real wasm nuance).
- **Embedded / MCU target** — globals/statics for ISR flags, `volatile` *emit*, ISR attributes, no-heap
  mode, avr/arm toolchains.
- **Native dispatch devirtualization** *(optimization, not a gap).* On a *monomorphic* call site clang
  does not devirtualize the emitted C vtable while rustc does — a clang-vs-rustc optimizer gap (hand-written
  C is equally behind), not a kama defect. kama can still win where it *sees* the concrete type by emitting
  a **direct call** instead of a vtable call — a laddered pass:
  - **Tier 1 — sound static devirtualization (no inlining).** Direct-call when the target is provable: a
    **concrete-value receiver**, a **`final` class/method**, or a **method with no overrides
    program-wide** (a slot→overridden map after `buildVtables()`). kama's whole-program view makes the
    last one free where C++ needs LTO + `-fwhole-program-vtables`. (`isFinalClass` / `MethodInfo::isFinal`
    / `exprClass()` already exist.) Land this first.
  - **Tier 2 — intraprocedural type-flow.** Devirtualize a base-typed local with a proven concrete
    assignment. Sound, no inlining.
  - **Tier 3 — inlining-enabled / guarded devirtualization.** A kama-level inliner (hard part: integrating
    callee scope-cleanup / drop order / move-state with `emitScopeCleanup`/`emitUnwindAll`) then re-run
    Tier 1, or guarded/speculative inline caches. A separate, larger project — pursue only if a real hot
    path (engine ECS dispatch) proves Tier 1 insufficient.

## 6. Concurrency — shared-nothing by construction (design direction)

The intended concurrency model. **1.0 ships a single-threaded core**; this is the 1.x/2.0 direction, not a
shipped feature. It earns data-race freedom the way kama earns null-safety — by making the hazard
*unrepresentable*, not by checking it. Where Rust proves exclusivity over shared memory with a borrow
checker, kama **removes the shared mutable state**.

- **Model — isolates + ownership-transferring channels.** An *isolate* is a shared-nothing unit of
  execution (≈ an OS worker natively, a Web Worker on wasm). Crossing a channel reuses the existing
  ownership model: send a `value` → **copy**; send a `resource` → **`give`** (move, zero-copy;
  use-after-send is already a compile error via move tracking); genuinely shared hot-path data → a narrow
  **`Atomic<T>` / shared-region** seam — the concurrency analog of `unsafe { }`/`Ptr` at the FFI boundary
  (opt-in, greppable, atomics-only).
- **Maps 1:1 onto wasm.** isolate → Web Worker; `give` across a channel → postMessage *transferable*
  (zero-copy, browser-enforced no-use-after-transfer); shared-region → SharedArrayBuffer + Atomics.
  Concurrency stays portable native↔browser from one source — which threaded C++/Rust do not.
- **Isolate vs job — two levels.** An *isolate* is the unit of *isolation* (few — ~one per core / Web
  Worker); a *task/job* is the unit of *work* scheduled onto isolates (many). The engine's job system is a
  library on top, not language.
- **Structured concurrency = RAII for tasks.** A concurrency scope joins its child tasks at scope exit —
  deterministic task lifetimes, no orphans. The concurrency version of the no-leak guarantee.
- **"Proceed until ready" without coloring.** The do-other-work-until-a-result-is-ready ergonomic is cheap
  tasks that block on a channel while a scheduler runs other ready work (the Go/Erlang model) — **not**
  Rust-style stackless `async/await`. Function coloring / `Pin` / self-referential state machines would be
  kama's least-kama feature, against "one way / favor simplicity."
- **Lock-free default, locks as expert opt-in.** The default path has no shared state → no locks. Atomics
  power expert lock-free structures, built once in the engine/stdlib (as Rust's std/crossbeam do over
  `unsafe`). No mandatory mutex-everywhere model.
- **Recommended language surface.** `isolate`/`task`, an ownership-transferring channel (reusing
  `give`/`copy`), `Atomic<T>`, a structured-concurrency scope, and two targeted *safe* sharing primitives
  that recover what shared-nothing otherwise costs:
  - **immutable `Shared` read-across-isolates** — immutable data is race-free even when shared (cheap
    read-only sharing of big assets);
  - **scoped disjoint-slice parallel-for** — a scope lends each task a non-overlapping mutable slice of one
    buffer and reclaims it at join; safe by disjointness (the `rayon`/`split_at_mut` pattern).
- **Deferred — general shared-memory ("hybrid").** Co-equal shared-memory threading is *not* planned; it
  reintroduces the hazard the model removes. Capability is retained (via the seam + the two primitives);
  only some ergonomics move behind the seam. Reopen only if a concrete case the seam can't express appears.
- **Positioning.** A *different, simpler, more portable* safe-concurrency model. Honest trade: Rust's
  shared-memory-with-static-exclusivity is more flexible for max-perf shared mutation; kama's
  shared-nothing is far easier to reason about and portable to wasm. Prior art: **Dart isolates** (closest),
  **Erlang/Elixir** actors, **Web Workers** + SharedArrayBuffer, **structured concurrency**
  (Swift/Kotlin/Trio); **Pony** for the type-level ceiling.

## 7. 2.0 — dual-mode: compiled + scripting/REPL (flagship)

The end goal is **one language, two modes** — the same kama syntax usable both compiled and as a scripting
language with a full REPL. The guiding constraint: **the `kama` binary is the only tool you need.**
External C toolchains stay *optional* — used for the portable-C release path, never required to write, run,
or iterate.

This rests on a **polymorphic emitter**: one front end lowered to a **shared IR**, then rendered by several
backends.

```
   Frontend  (parser → type checker → ownership/move analysis)
                          │
                          ▼
                     shared IR          (monomorphized, drops inserted,
                          │              vtables + match/operators desugared)
          ┌───────────────┼────────────────┐
          ▼               ▼                 ▼
          C              WASM            Bytecode
          │               │                 │
     TinyCC / clang    browser /          native VM
     (JIT + release)   Wasmtime         (REPL, self-contained)
```

Every backend shares the same front end, so the safety analysis (ownership, move tracking, exhaustiveness)
is proven **once**, before the IR.

- **C backend — the portability moat (kept, always).** kama → readable portable C → any C toolchain.
  `clang`/`emcc` for release; a bundled **TinyCC** for near-instant in-process JIT (`kama run foo.kama`
  and the REPL are **JIT-compiled, not tree-walked**). "Runs anywhere C runs" is the whole moat; the new
  backends are *additive*, never a replacement.
- **WASM backend — the self-contained web path.** Direct kama → wasm (no `emcc`), run in the browser or
  under Wasmtime. The web scripting/engine substrate; C→emcc remains the maximal-compatibility option.
- **Bytecode + VM backend — the self-contained native REPL.** A kama-owned VM gives a true interactive
  REPL with zero external tooling.

**The IR is the crux, and the real work.** Today there is no IR: the C emitter writes C text directly and
*bakes in* monomorphization, RAII drop insertion, vtable layout, and match/operator desugaring
(`kama.cemit.*`, ~150 methods). The refactor pulls that **semantic lowering up into the shared IR**,
leaving each backend a comparatively dumb renderer. Design constraints:

- **Keep the IR high-level and structured** (retain `if`/`while`/`for` and named locals), *not*
  SSA/basic-blocks — so the C backend can still emit the readable, `#line`-mapped C that is a headline
  feature.
- **Move the runtime into kama.** Collections/smart-pointers/`string` live as hand-tuned C in
  `kama_runtime.h` today (with a growing share already ported to kama library types); a wasm or VM
  backend can't `#include` it. Finishing the port so those flow through the shared IR and monomorphize into
  *any* backend makes multi-backend and the kama-stdlib/self-hosting goal the **same project**: do it once,
  all three backends inherit it.
- **Contain semantic drift.** A VM is a second execution semantics — the main risk. Having the C backend
  and the VM consume the *same lowered IR* reduces drift from "two languages" to "two renderers of one IR."
  Build the IR first; then a VM is a legitimate, low-drift option.

**Speed ladder** (fastest last): tree-walk < bytecode VM < TinyCC-JIT < AOT C→clang. If raw scripting speed
dominates, JIT wins; if zero-install + interactivity dominate, the VM / direct-wasm win. The "binary is the
only tool" constraint tilts the *default* iteration toward the VM + direct-wasm, with C/JIT for native speed
or C compatibility.

- **Why it beats other scripting languages:** Python/Ruby/Lua are bytecode interpreters; kama scales from a
  self-contained VM up to JIT/AOT-native — the same source, at or near native speed.
- *Licensing note:* TinyCC is LGPL; if a bundled JIT ships, confirm the linking terms against the
  MIT/permissive goal (GOALS #8). The VM / direct-wasm paths sidestep this entirely.

## 8. Engine track (product north star)

A portable lightweight **WebGPU** game engine. Tiers: **math types** (Tier 0 — unblocked) →
buffers/bindings → first triangle → scene/material. Depends on the 1.x systems (file I/O for assets,
serialization for scenes). See [ENGINE_READINESS.md](ENGINE_READINESS.md).

- **Dev-loop hot-reload — a *library* on two small compiler primitives.** Live-reload of gameplay code
  (edit → rebuild → swap without restarting) splits cleanly by layer, and *most of it is not the
  compiler's job* — which answers "language or engine feature?": mostly library, on a thin compiler base.
  - **Compiler (small — scheduled 1.x, §5):** a `kama build --shared` mode emitting a
    `.so`/`.dylib`/`.dll` (`-fPIC -shared`; on Windows the `dllexport` decoration + copy-before-load), and
    reuse of the reserved **`export`** keyword (§2) to give reload entry points **C-ABI linkage**. That is
    the *same* kama→host boundary the **wasm exports** and the **scripting host** (§7) already need — so
    hot-reload adds **no new language surface**, it consumes planned surface. One boundary, three consumers.
  - **Library:** the `dlopen`/`dlsym`/`dlclose` + file-watch + function-pointer rebind loop — pure FFI over
    `unsafe`/`Ptr`, **zero compiler changes**. This is the bulk of the feature and it lives in a module.
  - **Engine:** the *data-in-host, code-in-module* architecture (world state lives in the platform-layer
    arena, passed *into* the reloaded module) so a reload doesn't wipe the world. Prior art: Handmade Hero,
    Our Machinery, Unreal Live Coding (Live++), Godot GDExtension, Bevy `hot_lib_reloader`.
  - **Scope — desktop dev only.** dlopen is absent/forbidden on the *ship* targets: no `dlopen` in wasm
    (host re-instantiates a module instead), **banned on iOS** (no loading non-bundled native code, no
    JIT), Android/Quest only via a **pushed** `.so` (no on-device compile). Cross-platform *shipping*
    scripting is the §7 **VM**, not this. This path buys fast native iteration on Linux/Mac/Windows —
    nothing more, and that is enough to justify the two tiny primitives.

## 9. Performance

Current standing (full detail in [benchmarks/RESULTS.md](benchmarks/RESULTS.md)): kama is at **C/C++
parity** on native compute (fib/pi/collatz/fnptr/alloc **and** dynamic dispatch — all LLVM-AOT languages
compiled at `-O3`), and wins decisively on footprint (~2 MB RSS, ~66 KB binary) and the no-GC `alloc`
workload. `kama→wasm` (optimized) **beats hand-written JS on fib/pi/collatz/fnptr (up to ~4.5×)** and is
near-parity on `alloc`/`dispatch`.

- **WASM tiering.** Measure at the optimizing tier (`node --no-liftoff` — what a real long-running app
  gets); the bench forces TurboFan for the wasm track so numbers reflect steady-state, not V8's short-lived
  Liftoff baseline.
- **Bench methodology (don't re-chase).** Short workloads skew under parallel load — run with nothing else
  competing. The `/work` bind mount adds only ~0.3–1.7 ms (negligible). Keep all LLVM-AOT languages at the
  same `-O` level (`-O3`), or the optimization level, not the language, dominates a tiny kernel.
- **Bench cohort — add Zig.** The bench covers the no-GC AOT peers (C/C++/Rust/Go) but not **Zig** —
  kama's closest *language* rival (no-GC, AOT, and, as `zig cc`, already kama's bundled backend). Add a
  `zig` track: port the 4 workloads to `.zig`, add the toolchain to `bench/Dockerfile` + `build.sh` (or
  reuse the pinned zig from the release pipeline). Expect it to **cluster with C/Rust on raw compute**
  (all LLVM at `-O3`) — the signal is in the *compile-time / binary-size / RSS* columns and cohort
  completeness, not the perf ranking. Low-value on the perf axis; worth it for "kama vs its actual peers"
  being visibly complete.

## 10. Tooling / distribution (deferred)

- **VS Code Marketplace publish** — the `.vsix` is built + attached to releases; Marketplace publishing is
  deferred.
- **Brand rename → Kama** — done: the mechanical rename (binary, `.kama` file extension, internal symbols,
  docs) landed in one commit. Remaining external steps: rename the GitHub repo to `cosmic-canopy/kama` so the
  flipped URLs resolve, and stand up `kama-lang.org`.
- **FreeBSD CI** — a non-blocking `vmactions/freebsd-vm` job once Windows is proven on a tag.
- **Browser-debug ergonomics** — richer wasm source maps / a no-extension flow.
