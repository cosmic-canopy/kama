# Floor reference — the always-in-scope surface

The **floor** is everything usable with **no `import`** and available even under **`--no-std`**: the core
contracts and types the language itself leans on — the ones the syntax produces or lowers into — and the
construction helpers. It has no module to browse, so this page *is* where it is written down.

Beside it, and documented here because it ships the same way, is module **`core`**: the runtime capabilities
a program cannot reimplement, because they bind runtime globals set before `main` — console output, the
command line, the environment and the fatal-handler hook. `core` is embedded in the compiler, so it survives
`--no-std` too, but it is an ordinary module: a file names each capability it uses in its import list
(`import { core::println };`), because a file uses only what it declares or imports (KR-87).

**There is no `global::`** (retired with KR-87). It named the floor so a local declaration could not hide
it; nothing may shadow a name in scope now, the intrinsics below are keywords, and the capabilities are
`core`'s. `global` stays a reserved project name. The floor is discovered here and by completion on a bare
name; `core`'s surface completes inside the import list (`import { core::| }`).

Everything here is declared in [`prelude/global.kama`](../prelude/global.kama) and
[`prelude/builtin.kama`](../prelude/builtin.kama) over [`kama_runtime.h`](../include/kama_runtime.h), except the
handful the compiler lowers itself because they need the call site — `panic`, `assert`, `debugAssert` (the
source text and `file:line`), `sizeof`, `alignof`, `bitcast`, `addr`, `drop`. Those eight are **reserved words**
(SPEC *kama's keywords*): each is legal only in call position, so no local, parameter, field or function can take
the name, and each is written bare. The rule for which words are reserved is
the greppability one in SPEC. The **grammar is authoritative** ([grammar.bnf](grammar.bnf)); this is
a semantics index. See also [SPEC.md](SPEC.md) for the language.

## What is floor, and what is an `import`

One rule decides:

> **Contracts, syntax and intrinsics live in the prelude — always on, and present under `--no-std`.
> Backends and concrete implementations are opt-in `std::*` modules, so you pay for what you use.**

Every feature that *looks* like a prelude candidate already has its load-bearing half here; only the
optional backend is an import:

| Feature | Always-on half (prelude / `kama_runtime.h`) | Opt-in half |
| --- | --- | --- |
| Formatting | the `Formattable` contract + `Formatter` (string interpolation lowers into these) + the number→string runtime | `std::fmt` — helpers and the `html`/`sql`/`stripIndent` tags |
| Serialization | the `Serializable`/`Deserializable`/`Serializer`/`Deserializer` contracts, `SerError`/`DeError`, + `@generate` synthesis | `std::serialization::text::json` and `std::serialization::binary::kbin` — the wire backends |
| Memory | `Owned`/`Shared`/`Weak` + `HeapOwner`/`Deref`/`Copyable`, which drive `new`/`give`/`copy` | *(none — entirely floor)* |
| Concurrency | the `spawn`/`scope`/`parallel_for` syntax, the sendability gate, the `Atomic` borrow exemption | `std::concurrent` — `Isolate`/`Channel`/`Atomic` over the C seams |

So string interpolation works under `--no-std` while `std::fmt` stays optional, and the same shape holds for
serialization. The reason the split is drawn at cost: a module's link and runtime cost is triggered only when
its seam header is actually externed — `kama_isolate.h`/`kama_channel.h` pull in `-lpthread` natively, `<math.h>` pulls `-lm`, `kama_gpu.h` pulls the GPU stack, Windows
sockets pull `-lws2_32` and Windows entropy (`kama_random.h`) pulls `-lbcrypt`. One is keyed tighter still,
because its cost is a hosting model rather than a library: a wasm build goes threaded (`-pthread
-sPROXY_TO_PTHREAD`, a SharedArrayBuffer and the COOP/COEP headers that requires) only when the program
**creates a thread** — a `spawn`, `isolate` or `parallel_for` — never because it imported `Atomic`. Folding any of those into
the always-on floor would tax every program, including
`--no-std` and bare-metal builds.

Two tiers of "built-in" follow from that:

- **Prelude** — embedded in the compiler binary, always in scope, present under `--no-std`:
  `prelude/global.kama` (`Optional`/`Result`/`Unit`/`string` and the core contracts) plus the
  `Owned`/`Shared`/`Weak` triad, which needs no import although it lives in module `std::memory`.
- **`core`** — embedded too, so present under `--no-std`, but imported by name like any module:
  `lib/core/src/core.kama`, the runtime capabilities.
- **On-disk `std::*`** — an explicit `import`, absent under `--no-std`: everything else.

`--no-std` is an **install flavour**, not a compiler flag: `install.sh --no-std` (or `KAMA_NO_STD=1`) installs
the compiler without the bundled `lib/std`, which is what a bare-metal toolchain ships. What stays is
this page.

Among those, the compiler knows about some more than others. It lowers syntax directly into the prelude
globals and the memory triad; it knows `std::concurrent` and `std::serialization` by name (the channel and
atomic templates, the sendability gate, the `@generate` targets) though both are ordinary kama; and it knows
nothing at all about `collections`, `fmt`, `io`, `net`, `fs`, `time`, `math`, `num` and `app`, which are
plain libraries.

## Diagnostics — fatal checks (halt the program)

For *bugs and broken invariants* — "this can't continue." Recoverable errors stay on `Result<T, E>`; never
`panic` them. All write a message + `file:line` to stderr and `abort()` (the clean-trap discipline — no
`<stdio.h>`, no UB); on `--target embedded` they route through the weak `kama_panic_handler`.

| Signature | Notes |
|---|---|
| `panic(msg: string)` | Unconditional abort with a message. |
| `assert(cond: bool, msg: string)` | Abort iff `cond` is false. **`msg:` is mandatory** (empty allowed); the condition's source text is **auto-appended** — `assert(cond: x > 0, msg: "")` → `assertion failed: x > 0 (f.kama:12)`, and a non-empty `msg` appends after an em dash. **Always-on** (production invariants). |
| `debugAssert(cond: bool, msg: string)` | Identical to `assert`, but **stripped under `--release`** (`NDEBUG` / `debug_assert!` — for expensive dev-only checks). |
| `setPanicHandler(handler: PanicHandler)` | *Module `core`.* Install a custom fatal handler (`fnptr void PanicHandler()`) for cleanup/exhibition — a shipped game/GUI shows a dialog / flushes a save instead of a bare stderr abort. **Contract:** set **once** at startup before spawning isolates; **re-entrancy-guarded** (a panic while handling one hard-aborts); the runtime **always terminates** after it (not a resume point). Covers every hosted fatal path (panic/assert/bounds). On embedded, provide a strong `kama_panic_handler` symbol instead (this is a no-op there). |

*(`warn` is **not** here — a warning must never abort. That is a log level; see [`std::log`](SPEC.md#logging-stdlog-).)*

## Console I/O — the print family (module `core`)

`import { core::print, core::println, core::eprint, core::eprintln };` — each one a file uses.

Diagnostics that **keep running**: write text to the standard streams. String-only — formatting rides
interpolation (`println(s: "x = ${x}")`). Unbuffered line writes. On `--target embedded` there is no fd, so both
streams route to the overridable weak `kama_log_sink` (default no-op; a firmware author pipes it to UART/RTT).

| Signature | Stream | Notes |
|---|---|---|
| `print(s: string)` | stdout (fd 1) | The program's data/results (a CLI's real output). No trailing newline. |
| `println(s: string)` | stdout | `print` + `'\n'`. The 90% case. |
| `eprint(s: string)` | stderr (fd 2) | Prompts / progress / diagnostics. `e` = "to stderr" (Rust convention), **not** "error" (that's a log level). |
| `eprintln(s: string)` | stderr | `eprint` + `'\n'`. |

A literal or interpolation temp passes by value directly; a **named** `string` is passed with `copy`/`give`
like anywhere else in the value model.

**Not a duplicate of `std::io::stdout()`.** The two coexist on purpose. The print family is always
available, survives `--no-std`, and writes immediately — it is for diagnostics that must
work when nothing else does. `std::io`'s `stdin()`/`stdout()`/`stderr()` are ordinary `Reader`/`Writer`
*values*, so they **compose**: `pump(from: file, to: out)`, `BufWriter.make(inner: out)`,
`serializeJsonStream(v: cfg, to: out)`, a function that takes a `Writer` and does not care whether it is a socket, a
file or the terminal. Use print to *say* something; use the handles to *plumb* something.

## Command-line arguments + environment (module `core`)

`import { core::args, core::env, core::envOr, … };` — `Args` is importable too, for a local of that type.

Bound to runtime globals stashed before `main` (so a `--no-std` program cannot reimplement them). Full prose
in [SPEC.md](SPEC.md) "Command-line arguments + environment". On `--target embedded` these are no-op stubs
(`args()` empty, the rest `None`).

| Signature | Notes |
|---|---|
| `args() -> Args` | The user arguments, **excluding** `argv[0]`. `Args` is one handle: `foreach` (`Iterator<string>`) + `count() -> int32` + `get(at: int32) -> Optional<string>`. |
| `programInvocation() -> Optional<string>` | `argv[0]` **verbatim** (the exact launch string). `None` on firmware. |
| `programName() -> Optional<string>` | **basename** of `argv[0]` (usage text / multi-call dispatch). Spoofable. |
| `programPath() -> Optional<string>` | The **OS-resolved** absolute executable path (not `argv[0]`). `None` on wasm/embedded/unsupported hosts. |
| `env(name: string) -> Optional<string>` | Environment lookup; `None` if unset. |
| `envOr(name: string, dflt: string) -> string` | `env` with a default. |

## Core contracts & construction (floor tier)

Available everywhere without import (the tier of `Optional`/`Result`); see [SPEC.md](SPEC.md) and
[TYPE_MODEL.md](TYPE_MODEL.md) for full semantics.

- **Sum types:** `Optional<T>` (`Some`/`None`), `Result<T, E>` (`Ok`/`Err`).
- **Core contracts:** `Deref<T>`/`DerefMut<T>`, `HeapOwner<T>`, `Movable`, `Copyable<T is This>`,
  `Hashable`, `Equatable<T is This>`, `Comparable<T is This>` (+ `Ordering`), `Error`,
  `Iterator<T>`/`IteratorMut<T>`, `Iterable<T>`/`IterableMut<T>`, `Viewable<V>`/`ViewableMut<V>`,
  `Sendable` (may cross an isolate), `Immutable`, `Allocator`, `GlobalHeap` (the contract a
  `@globalAllocator` implements; `GlobalAllocator` is the default `value` behind it),
  `Serializable`/`Deserializable`/`Serializer`/`Deserializer` (+ `SerError`, `DeError`, `FieldKey`).
- **String iteration:** `Chars` (from `.chars()`) and `Split` (from `.split(separator:)`), both views.
- **Raw memory and the compile-time-sized types:** `UnsafePtr<T>`, `UnsafeConstPtr<T>` (and `ptrOrNull`),
  `InlineArray<T>#(N)`, `Simd<T>#(N)`, and `BindableFunctionPtr<Sig>` behind `fnptr`.
- **Text rendering (`std::fmt` core):** the `Formattable` contract + `Formatter` sink, and `"${x}"` interpolation
  — the machinery `"${…}"` interpolation lowers onto.
- **Construction / memory builtins** (see [SPEC.md](SPEC.md) "Writing a collection *in* kama"): `sizeof(T)`,
  `alignof(T)`, `bitcast<T>(x)`, `drop(ptr:)`, `addr(of:)`, and `unwrapPtr(Optional<UnsafePtr>)` (infallible-alloc
  adapter). The smart-pointer triad (`Owned`/`Shared`/`Weak`, in module `std::memory` but always in scope)
  is a built-in module, not bare floor.
