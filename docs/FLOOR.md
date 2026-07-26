# Floor reference — the always-in-scope surface

The **floor** is everything usable with **no `import`** and available even under **`--no-std`**: the core
contracts/types the language itself leans on, the construction helpers, plus the diagnostics, console-I/O and
args/env capabilities that a program cannot reimplement (they bind runtime globals set before `main`). It has
no browsable namespace, so this page *is* its documentary namespace — grouped by concern, kept current as the
floor grows. Bare is the everyday style (importing to call `assert`/`println` would be terrible); a future
`global::` root qualifier (documented-only until an LSP exists) will name it explicitly.

Everything here lives in [`prelude/global.kama`](../prelude/global.kama) over
[`kama_runtime.h`](../kama_runtime.h). The **grammar is authoritative** ([grammar.bnf](grammar.bnf)); this is
a semantics index. See also [SPEC.md](SPEC.md) for the language, [stdlib-layering](design/stdlib-layering.md)
for what is floor vs `import std::…`.

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

*(`warn` is **not** here — a warning must never abort. That is a log level; see [`std::log`](design/logging.md).)*

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
  `Comparable` (+ `Ordering`), `Error`, `Iterator<T>`/`IteratorMut<T>`/`Iterable<T>`, `Allocator`
  (+ `GlobalAllocator`), `Serialize`/`Deserialize`/`Serializer`/`Deserializer`.
- **Text rendering (`std::fmt` core):** the `Format` contract + `Formatter` sink, and `toString<T: Format>()`
  — the machinery `"${…}"` interpolation lowers onto.
- **Construction / memory builtins** (see [SPEC.md](SPEC.md) "Writing a collection *in* kama"): `sizeof(T)`,
  `alignof(T)`, `bitcast<T>(x)`, `drop(value:)`, `addr(of:)`, and `unwrapPtr(Optional<Ptr>)` (infallible-alloc
  adapter). The smart-pointer triad (`Owned`/`Shared`/`Weak`, namespaced `std::memory` but always in scope)
  is a built-in module, not bare floor.
