# Diagnostics & logging — design of record

**Status: DESIGN OF RECORD (settled; not yet built).** Scheduled as [ROADMAP.md](../ROADMAP.md) §1 near-term
**#3 — before `std::process`** ([std-process.md](std-process.md), now #4): a subprocess API and a CLI both
want console I/O, and the assert/handler work is adjacent. All spike questions are resolved; residual
implementation details are listed at the end.

## The gap

Kama can *build* text (`std::fmt`: `toString(x)`, `"${x}"` interpolation) but cannot *print* it. There is no
`print`/`println`/`eprintln` and no stdout/stderr handle in the floor or `std::io`. Only fatal traps
(`panic`/`assert`/bounds) reach stderr, via the runtime's raw `write(2, …)`. Output today means dropping to
FFI (`extern fn puts`) — `examples/httpd` does exactly this.

## Taxonomy (two orthogonal concerns — never conflate)

- **Fatal checks — halt the program.** `panic(msg:)` / `assert(cond:)` / `debugAssert(cond:)` / runtime
  bounds/overflow/OOM traps. For *bugs and broken invariants*: "this can't continue." Recoverable errors stay
  on `Result<T,E>` — never `panic`.
- **Diagnostics — keep running.** Logging at levels `error`/`warn`/`info`/`debug`/`trace`. **`warn` is a log
  level, not an assert** — it must never abort.

## Part A — assert / panic (small; ships with Part B)

Today: `assert(cond: c)` → `c ? (void)0 : kama_panic("assertion failed")`; `panic(msg: s)` →
`kama_panic(s)`. `panic` is an unconditional assert-with-message; `assert` carries no context. Changes:

1. **Auto-stringify the condition** as the default message — `assert(cond: x > 0)` fails with
   `assertion failed: x > 0` (the emitter has the AST; emit the condition's source text as a literal).
2. **Optional `msg:`** — `assert(cond: c, msg: "…")` **appends** to the auto-stringified condition:
   `assertion failed: x > 0 — <msg>`. (Builtins → the emitter branches on arg count.)
3. **`file:line`** in every `panic`/`assert` message (the emitter knows the source location).

### Two independent knobs (the game-engine lesson)

"Engines don't ship asserts" bundles two separate concerns — keep them separate:

- **Strip knob (cost)** → **`debugAssert(cond:[, msg:])`**: identical to `assert`, **stripped under
  `--release`** (Rust `debug_assert!` / C `NDEBUG`), for expensive dev-only checks. `assert` stays
  **always-on** (production invariants). Two names (not one `assert` + a flag) — greppable, self-documenting,
  and the shared `assert` stem links them for discovery.
- **Handler knob (exhibition)** → **a customizable, hosted panic/assert handler.** A raw stderr `abort()` is
  useless/hostile to an end user with no terminal (a shipped game/GUI). The fix is *how it exhibits*, not
  removing the check. Generalize the embedded weak `kama_panic_handler` to hosted via a kama-level
  **`setPanicHandler(fn)`** (backed by the same runtime slot; the weak C symbol stays the `--no-std`/embedded
  fallback). Default handler = message to stderr + `abort()`.

  **Handler contract (the footgun guardrails — bake these in):**
  - **Re-entrancy guard** — a panic while already handling one skips the handler and hard-aborts (no infinite
    recursion).
  - **Always terminates** — the handler is for cleanup/reporting only; the runtime **still terminates** after
    it (belt-and-suspenders `for(;;)`/abort). It is *not* a resume point — recovery is `Result`, not panic.
  - **Non-zero exit** — a handler must not `exit(0)` (would hide the crash from CI/parents).
  - **Set-once discipline** — register at startup before spawning isolates (same rule as the argv stash).
  - **Degraded-state caution** (documented) — a handler runs amid possible OOM/corruption; keep it defensive.

  This lets a critical `assert` stay compiled-in for release **and** exit gracefully — the two knobs are
  orthogonal.

An **`ensure`-style** always-on *non-fatal* check (Unreal `ensure()`: report + continue) is **not** a fatal
primitive — it's a logging pattern (`if (!cond) logError(...)`), so it lives in `std::log`.

## Part B — Tier 1: basic print (the floor)

Floor free functions (like `toString`, no keywords), so they survive `--no-std` and pair with `std::fmt`:

```
fn void print(string s);      fn void eprint(string s);      // no trailing newline
fn void println(string s);    fn void eprintln(string s);    // append '\n'
```
- **Keep all four.** `println` is the 90% case; `print` (no newline) is needed for prompts/progress/streaming.
- **String-only** — formatting rides interpolation (`println("x = ${x}")`); no generic `println<T>` overload.
- **`print`/`println` → stdout** = the program's *data/results* (a CLI's real output). **`eprint`/`eprintln`
  → stderr** = the *stderr stream* (prompts, progress, diagnostics). Named `eprint` (Rust convention, "print
  to stderr") **not** `printError` — stderr is not "error" (that's a log level), and the `e*` names cluster
  under the `print` stem.
- **Runtime seam:** floor helpers using block-scope `extern … write(1/2,…)` — the `kama_panic` discipline
  (no `<stdio.h>` leak, `--no-std`-clean). Unbuffered line writes in v1.
- **Embedded (`--target embedded`):** route to a **weak `kama_log_sink(bytes,n)` hook that defaults to a
  no-op** — zero cost on-chip by default; a firmware author overrides it once to pipe `print` out a UART/RTT.
  Mirrors `kama_panic_handler`.

## Part C — Tier 2: `std::log` (the configurable logger)

Opt-in stdlib module (`import std::log` → discoverable; the module *is* the grouping, so it's NOT scattered
as bare floor globals). Two axes — **level** and **tag** — either can suppress a call.

### Flexibility vs performance are different knobs
- **Flexibility** = a **`Logger` contract** (pluggable backend, in the library). NOT a prelude primitive —
  that would bake one policy into the language.
- **Performance** (zero-cost when off) = the **compiler being aware of the log call sites**. This is a
  logging-specialized **AST lowering**, *not* a preprocessor (see below).

### The pieces (facade + contract + swappable backend — the Rust `log`/`tracing` shape)
```
enum LogLevel { Error, Warn, Info, Debug, Trace }        // ordered

type contract Logger {                                    // the pluggable backend — opinions live here
    fn bool enabled(LogLevel level, string tag);          //   the cheap filter (bitset/map)
    fn void emit(LogLevel level, string tag, string msg); //   the actual sink + format
}
fn void setLogger(Logger l);                             // install your own (set-once, like setPanicHandler)

// facade — the ergonomic entry points everyone calls (compiler-recognized; see lowering)
logError(tag: "audio", "…"); logWarn(…); logInfo(…); logDebug(…); logTrace(…);   // tag optional
fn bool logEnabled(LogLevel level, string tag);          // manual hot-path guard (v1, before the lowering)
```
- **Default backend** (`ConsoleLogger` in `std::log`): stderr sink, level+tag filter from baked `kama.json`
  defaults + `--log`/env override, simple formatting. Auto-installed unless the user `setLogger`s.
- **Opinionated dev:** `type resource MyLogger implements Logger { … }` (custom sink/format/route to engine
  console/telemetry/file) + `setLogger(MyLogger.make())`. The facade calls are unchanged — behavior is theirs.

### The two axes and how each reaches zero cost

The facade lowers a call to a comptime-guarded, runtime-guarded, message-inside form:
```
if (KAMA_LOG_COMPILE_MIN <= DEBUG) {          // LEVEL, compile-time: a baked const → DCE'd in --release
    if (logger.enabled(DEBUG, "audio")) {     // LEVEL(above floor) + TAG, runtime: one cheap check
        logger.emit(DEBUG, "audio", "pos ${expensive()}")   // built ONLY if it survives both guards
    }
}
```
- **Level — compile-time *and* runtime.** The outer guard is a **baked compile-time constant**
  (`KAMA_LOG_COMPILE_MIN`, from `--release`/`kama.json`); when a level is below the floor it's `if (false)` →
  the optimizer **dead-code-eliminates** the whole block (strip only happens in `--release`, which *is*
  optimized, so DCE always applies; the front end may also pre-prune for certainty). Above the floor, the
  level is *also* checked at runtime (reconfigurable). So: **strip below the floor (zero cost) + adjust above
  it.**
- **Tag — runtime.** A cheap per-tag check in the same inner guard, reconfigurable via `--log`/env. Because
  the message-build sits **inside** that guard, **either axis suppressing skips the expensive
  interpolation/format/output.**
- **Explicit compile-time drop — `@compileFor`.** To make a whole tag/subsystem *physically absent* from a
  build regardless of runtime config, gate it with the existing decl-level **`@compileFor(AUDIO_LOG)`** — no
  new mechanism, greppable, drops any logic (not just logs).

### Not a preprocessor
The recognized-facade lowering is the "logging-specialized compile pass" — done at the **AST** layer, not on
text. It's typed, scoped, hygienic, reuses the one grammar, and puts message-build inside the guard — exactly
like the existing `"${x}"`/`assert`/`panic`/`print` lowerings, and exactly how Rust's `log!`/`tracing` work
(hygienic macro expansion, *not* `cpp`). A textual logging preprocessor, made correct (interpolation, tagged
strings, named args, nesting), would have to re-parse the language — i.e. collapse into this AST pass. So: **no
preprocessor, no `#ifdef`.**

## Part D — configuration layering (incl. the general `kama.local` override)

Split by *when* config is read:

- **Build-time = `kama.json` (+ `kama.local.json`).** `kama.json` gains a **`log` section** (default enabled
  tags + min level + the compile strip floor) which the compiler **bakes into the binary** (a shipped binary
  has no `kama.json` beside it). A **gitignored `kama.local.json`** sibling **deep-merges over `kama.json`** as
  a **general** local override — any field: log defaults, `@compileFor` flags, **dependency *path* overrides
  for local dev** (Cargo `[patch]`/go `replace`), registries, toolchain pin. Local-only by construction: a
  path/override bypasses the lockfile hash, so it **never** affects reproducible/CI builds (that's the point).
- **Runtime override (no rebuild) = `--log` flag (primary) + `KAMA_LOG` env (secondary).** Both ride the
  shipped argv/env floor. `kama.json` is the authoritative *default*; the runtime knob is an explicit
  *override layer* for the cases the baked config structurally can't serve — field/QA debugging of a shipped
  binary, one-off repro, CI. Grammar (both): a global level + `tag=level` overrides, e.g.
  `--log warn,audio=debug,net=trace` / `KAMA_LOG=warn,audio=debug`.
- **Precedence (high→low):** programmatic (`setLogger`/API) > `--log`/`KAMA_LOG` > `kama.local.json` (dev) >
  `kama.json` default.

This runtime-reconfigurable level+tag on a *shipped* binary is the QA/live-debugging capability most
compile-time logging setups discard.

## Part E — naming & discoverability

Two scoping tiers, discovered differently:
- **Floor (bare, no import)** — `assert`/`debugAssert`/`panic`, `print`/`println`/`eprint`/`eprintln`.
  Universal + used everywhere (importing to call `assert` would be terrible) + survive `--no-std`. No
  browsable namespace → discovery via **docs + shared naming stems** (`assert`→`debugAssert`; the `print`
  family).
- **Module (`import std::log`)** — the logger. The module is the discovery unit (`import std::log::{…}` shows
  the whole family); scattering `logInfo`/`logWarn` as bare floor globals would make them undiscoverable.

Two supports for the flat floor:
1. **Dedicated, linked "Floor reference" doc page** (its own page, linked from SPEC.md) listing everything
   always-in-scope, grouped by concern (core types · construction · diagnostics · console I/O · args/env) —
   the floor's documentary namespace, kept current as it grows. *(Do this.)*
2. **`global::` root qualifier — documented concept only for now.** Reserve the semantics (`global::assert` ≡
   bare `assert`, same symbol; the resolver already has the empty-namespace + alias machinery) so they can't
   drift, but **build the resolver alias only when an LSP exists** to give it a completion payoff (the VS Code
   extension is highlighting + source-level debugging only today — ROADMAP §10 "Language server (LSP)").
   Precedent: C# `global::`, Rust's nameable `std::prelude`. Bare stays THE everyday style.

## Phasing

1. **Part A + Part B** — assert/panic polish (auto-stringify, `msg:`, `file:line`, `debugAssert`,
   `setPanicHandler` + handler contract) + floor `print`/`println`/`eprint`/`eprintln`. Small, high-value,
   unblocks `std::process`. Plus the **"Floor reference" doc page**.
2. **`kama.local.json` general override** (Part D build-time half) — a manifest-system feature, useful on its
   own (dep path-overrides), independent of logging.
3. **`std::log` v1 (pure library)** — `Logger` contract + default `ConsoleLogger` + runtime level/tag
   filtering + `setLogger` + `--log`/`KAMA_LOG`/`kama.json` config + manual `logEnabled` hot-path guard. **No
   compiler changes.**
4. **`std::log` v2 (the compiler hook)** — the recognized-facade AST lowering: baked comptime min-level → DCE
   strip (C2) + auto `enabled(level, tag)` guard with message-build inside (C1). Pure optimization; the API
   is unchanged from v1.

## Reused seams (little is from scratch)
- `std::fmt` `Formatter`/`toString`/interpolation (floor) — message building; the lowering mirrors its shape.
- `std::io::Writer` (`write(View<uint8>)`/`flush`) + `writeAll` — logger sinks.
- `std::fs::File` (wraps `int32 fd`, `kama_write`) — stdout/stderr sinks via `File.stdout()`/`stderr()`.
- `kama.json` `ManifestReader` + `--config` + `@compileFor`/`--define`/`--release` — config + compile drop.
- `kama_panic`'s block-scope-`write` / weak-hook discipline — Tier-1 runtime + embedded + the hosted handler.
- argv/env floor (shipped) — `--log` flag + `KAMA_LOG` env parsing.

## Resolved decisions
- **A1** msg appends to the auto-stringified condition. **A2** two names (`assert` always-on / `debugAssert`
  `--release`-stripped). **A3** include `file:line`. **A4** kama-level `setPanicHandler` (hosted), weak symbol
  the embedded/`--no-std` fallback, with the handler contract above.
- **B1** keep `print`+`println` (+`eprint`/`eprintln`), string-only. **B2** embedded print → weak
  `kama_log_sink`, default no-op.
- **C1/C2** facade + `Logger` contract + swappable backend; recognized-facade AST lowering (baked comptime
  min-level → DCE strip + runtime `enabled(level,tag)` guard, message built inside); tags runtime-by-default,
  `@compileFor` for full compile-time drop; **no preprocessor**; phased v1 library / v2 lowering. Strip
  `trace`+`debug` under `--release`, keep `info`/`warn`/`error` (standard).
- **D1** `kama.local.json` whole-file deep-merge, general (incl. dep path-overrides), local-only. **D2**
  `kama.json` baked defaults + runtime `--log` (primary) / `KAMA_LOG` env (secondary).
- **E1** dedicated linked "Floor reference" page. **E2** `global::` documented-only until an LSP exists.

## `std::log` v1 — as shipped

Built as a pure library (`lib/std/log/log.kama`) over one runtime seam (`kama_log.h`), with two deviations
from Part C above, both forced by the value/resource model and both capability-preserving:

- **Sink fnptr, not a `Logger` contract instance.** A stored `Logger` would be a resource module-static
  (rejected — no static-teardown seam) or a `Ptr` to an interface (not dispatchable). So the pluggable backend
  is a **swappable sink fnptr** held in a runtime slot (`fnptr void LogSink(int32 level, string tag, string
  msg)` + `setLogSink`), exactly the `setPanicHandler` shape: the facade calls the extern `kama_log_dispatch`,
  which invokes the slot or a built-in C console default. The runtime **filter** (`kama_log_enabled`) is a
  separate concern — filter decides, sink outputs (the Rust `log`/`tracing` split). *(The sink's `level` is a
  plain `int32` ordinal, not `LogLevel`: a fnptr typedef referencing an enum forward-references the
  later-emitted enum in the shared header — a latent emitter-ordering gap, sidestepped rather than fixed in a
  v1 library milestone.)*
- **Config source is the process-global env.** Strings cross the FFI as borrowed `Ptr<int8>`+`usize` spans
  (the `print` discipline — never by value, so no ownership transfer). The filter reads `KAMA_LOG` via
  `getenv` (process-global → every module-scoped static parses the same value). Because argv is itself a
  module-scoped static (per the concurrency model, not visible outside `main`'s TU), the **`--log` flag is
  bridged into `KAMA_LOG` once in `main`** (`kama_log_init_args`, emitted only when a program imports
  std::log). `--log` overwrites the env (it is primary). The `kama.json` baked default is deferred to the
  `kama.local.json` config-layering milestone.

Tested via `tools/check-log.sh` (registered in `run_tests.sh`, native-only — std::log writes to stderr, which
the sanitizer harness captures, so it must not be a `tests/*.kama` fixture): level+tag filtering, `--log`
(both forms) + `KAMA_LOG` + precedence, a custom sink, and the freestanding `--target embedded` lowering.

## Residual implementation details (settle in the build session)
- Exact `LogRecord` fields if `emit` grows beyond `(level, tag, msg)` (timestamp / source loc / isolate id).
- `--log` / `KAMA_LOG` grammar edge cases (multiple tags, wildcard `*`, level names vs numbers).
- Whether the default `ConsoleLogger` colorizes on a TTY (and how it detects one without `<stdio.h>` leak).
- Multiple simultaneous sinks (console + file) in the default backend, or v1 single-sink.
