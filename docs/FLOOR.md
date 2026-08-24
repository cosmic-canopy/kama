# Floor reference — the always-in-scope surface

The **floor** is everything usable with **no `import`** and available even under **`--no-std`**: the core
contracts/types the language itself leans on, the construction helpers, plus the diagnostics, console-I/O and
args/env capabilities that a program cannot reimplement (they bind runtime globals set before `main`). It has
no module to browse, so this page *is* where it is written down — grouped by concern, kept current as the
floor grows. Bare is the everyday style (importing to call `assert`/`println` would be terrible).

**`global::` names the floor explicitly** (shipped with the language server, LSP M4.8). `global::assert` is
the *same symbol* as bare `assert` — the qualifier resolves from the root, ignoring the file's own scope, its
imports and its aliases, so it still reaches the floor where a local declaration shadows the spelling.
Precedent: C#'s `global::`. `global` is therefore a **reserved project name**, and that is the whole of its
specialness — it is not a third kind of scope, just a name nobody else may claim. It names ONLY the floor: a
longer `global::a::b::X` used to name a module absolutely and was deleted with the `namespace` declaration
(design/module-system.md §2f.29), because `global` being a project name would make it mean module `a/b` OF a
project called `global`. The job it did — reach a name past an `import … as` alias shadowing it — is done at
the source now: such an alias is refused where it is written. Typing `global::` in an editor lists this
whole surface — which is why the qualifier waited for a language server: without completion it would have
been a spelling with no discovery payoff.

Everything here lives in [`prelude/global.kama`](../prelude/global.kama) over
[`kama_runtime.h`](../include/kama_runtime.h). The **grammar is authoritative** ([grammar.bnf](grammar.bnf)); this is
a semantics index. See also [SPEC.md](SPEC.md) for the language.

## What is floor, and what is an `import`

One rule decides:

> **Contracts, syntax and intrinsics live in the prelude — always on, and present under `--no-std`.
> Backends and concrete implementations are opt-in `std::*` modules, so you pay for what you use.**

Every feature that *looks* like a prelude candidate already has its load-bearing half here; only the
optional backend is an import:

| Feature | Always-on half (prelude / `kama_runtime.h`) | Opt-in half |
| --- | --- | --- |
| Formatting | the `Format` contract + `Formatter` (string interpolation lowers into these) + the number→string runtime | `std::fmt` — helpers and the `html`/`sql`/`stripIndent` tags |
| Serialization | the `Serialize`/`Deserialize`/`Serializer`/`Deserializer` contracts + `@generate` synthesis | `std::serialization::{binary,json}` — the byte backends |
| Memory | `Owned`/`Shared`/`Weak` + `HeapOwner`/`Deref`/`Copyable`, which drive `new`/`give`/`copy` | *(none — entirely floor)* |
| Concurrency | the `spawn`/`scope`/`parallel_for` syntax, the sendability gate, the `Atomic` borrow exemption | `std::concurrent` — `Isolate`/`Channel`/`Atomic` over the C seams |

So string interpolation works under `--no-std` while `std::fmt` stays optional, and the same shape holds for
serialization. The reason the split is drawn at cost: a module's link and runtime cost is triggered only when
its seam header is actually externed — `kama_isolate.h`/`kama_channel.h` pull in `-lpthread` natively and
`-pthread -sPROXY_TO_PTHREAD` on wasm, `<math.h>` pulls `-lm`, `kama_gpu.h` pulls the GPU stack, Windows
sockets pull `-lws2_32`. Folding any of those into the always-on floor would tax every program, including
`--no-std` and bare-metal builds.

Two tiers of "built-in" follow from that:

- **Prelude** — embedded in the compiler binary, always in scope, present under `--no-std`:
  `prelude/global.kama` (`Optional`/`Result`/`Unit`/`string` and the core contracts) plus the
  `Owned`/`Shared`/`Weak` triad. `import std::memory` is a no-op, satisfied by the prelude.
- **On-disk `std::*`** — an explicit `import`, absent under `--no-std`: everything else.

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
| `setPanicHandler(handler: PanicHandler)` | Install a custom fatal handler (`fnptr void PanicHandler()`) for cleanup/exhibition — a shipped game/GUI shows a dialog / flushes a save instead of a bare stderr abort. **Contract:** set **once** at startup before spawning isolates; **re-entrancy-guarded** (a panic while handling one hard-aborts); the runtime **always terminates** after it (not a resume point). Covers every hosted fatal path (panic/assert/bounds). On embedded, provide a strong `kama_panic_handler` symbol instead (this is a no-op there). |

*(`warn` is **not** here — a warning must never abort. That is a log level; see [`std::log`](SPEC.md#logging-stdlog-).)*

## Console I/O — the print family

Diagnostics that **keep running**: write text to the standard streams. String-only — formatting rides
interpolation (`println("x = ${x}")`). Unbuffered line writes. On `--target embedded` there is no fd, so both
streams route to the overridable weak `kama_log_sink` (default no-op; a firmware author pipes it to UART/RTT).

| Signature | Stream | Notes |
|---|---|---|
| `print(s: string)` | stdout (fd 1) | The program's data/results (a CLI's real output). No trailing newline. |
| `println(s: string)` | stdout | `print` + `'\n'`. The 90% case. |
| `eprint(s: string)` | stderr (fd 2) | Prompts / progress / diagnostics. `e` = "to stderr" (Rust convention), **not** "error" (that's a log level). |
| `eprintln(s: string)` | stderr | `eprint` + `'\n'`. |

A literal or interpolation temp passes by value directly; a **named** `string` is passed with `copy`/`give`
like anywhere else in the value model.

## Command-line arguments + environment

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
- **Core contracts:** `Deref<T>`, `HeapOwner<T>`, `Movable`, `Copyable`, `Hashable`, `Equatable`,
  `Comparable` (+ `Ordering`), `Error`, `Iterator<T>`/`IteratorMut<T>`/`Iterable<T>`, `Viewable<V>`,
  `Allocator`
  (+ `GlobalAllocator`), `Serialize`/`Deserialize`/`Serializer`/`Deserializer`.
- **Text rendering (`std::fmt` core):** the `Format` contract + `Formatter` sink, and `"${x}"` interpolation
  — the machinery `"${…}"` interpolation lowers onto.
- **Construction / memory builtins** (see [SPEC.md](SPEC.md) "Writing a collection *in* kama"): `sizeof(T)`,
  `alignof(T)`, `bitcast<T>(x)`, `drop(value:)`, `addr(of:)`, and `unwrapPtr(Optional<UnsafePtr>)` (infallible-alloc
  adapter). The smart-pointer triad (`Owned`/`Shared`/`Weak`, in module `std::memory` but always in scope)
  is a built-in module, not bare floor.
