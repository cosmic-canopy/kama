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
  std::log). `--log` overwrites the env (it is primary).

**Baked `kama.json` default — as shipped (M5.1).** The manifest gains a `log` section, a JSON object
`{ "level": <lvl>, "tags": { <tag>: <lvl> } }` (both optional; level names validated at build time). The
`ManifestReader` translates it to the canonical `KAMA_LOG` spec string (the runtime C parser stays the single
source of grammar truth) and threads it to the emitter (`setLogDefault`), which — when the program imports
std::log — compiles a `kama_log_set_default("<spec>")` call into `main` right after `kama_log_init_args`. That
helper does `setenv("KAMA_LOG", spec, /*overwrite=*/0)`, so it only fills the env when unset. Result:
`--log` (overwrite=1) > a pre-existing `KAMA_LOG` env > baked default > the `info` floor — exactly the design
precedence, and routed through the same process-global env so the multi-TU static-copy hazard is sidestepped
(no runtime default slot). Embedded is a no-op (no env).

**`kama.local.json` deep-merge — as shipped (M5.2).** A gitignored sibling of the manifest layers over it for
the fields the compiler reads directly: the `log` config (a structured `LogConfig` merge — a local `level`
wins, local tags override per-name while base tags are preserved) and `@compileFor` `flags` (union — a local
flag extends the declared universe and a local `default:true` activates it). The runtime precedence therefore
becomes `--log` > `KAMA_LOG` > `kama.local.json`-merged baked default > `info` floor. Local-only by
construction (never committed, never in the lockfile), so it can never perturb a reproducible/CI build.

**`kama.local.json` install/selector overrides — as shipped (M5.3).** The second, non-logging half of the
`kama.local.json` mechanism: the fields read by the install/selector paths rather than the compiler. Three
overrides, all dev-local and all CI-safe by construction:
- **`overrides`** — dependency *path*-overrides (Cargo `[patch]` / Go `replace`). **Patch-style / pure
  view-relink:** `kama pkg install` resolves canonically from `kama.json` and writes the normal `kama.lock`
  *unchanged*, then — as a strict post-step — relinks the materialized view (`.kama/deps/<name>`) at the
  local path so this dev's build compiles the local code. The override is **never written to the lock**, so a
  committed `kama.lock` stays canonical and CI (no `kama.local.json`) reproduces the published resolution
  exactly. That is the whole guarantee, and it is why the model is view-relink and not resolution-substitution.
  Consequences (intentional v1): the overridden dep must still be a declared, canonically-resolvable
  dependency and a drop-in for it — its *new* transitive deps aren't re-followed (replace-style, which
  resolves an unpublished dep as a path, is a clean later milestone). A `path`-less or non-dependency override
  is a hard error.
- **`registries`** — a local `RegConfig` merged over `kama.json`'s in `resolveProject` (`RegConfig::applyLocal`:
  a local `default`/`default:false` replaces the base default; a local `@scope` replaces that scope's chain).
  Lock-safe for free because the lock pins integrity, not the URI (M3.1b), so re-pointing to a same-bytes
  mirror re-resolves byte-identically.
- **`toolchain`** — highest-precedence pin in the selector's `resolvePin` (`kama.local.json` > `kama.json` pin
  > `KAMA_VERSION` > default), so a dev can test a checkout under a different local toolchain without editing
  the committed pin.

Parser: one `overrides` key added to `ManifestReader` (reusing the dependency-object parser) + a
`loadManifestLocalInstall` loader; `kama publish` also excludes `kama.local.json` from the tarball. Tested via
`tools/check-packages.sh` (dep-override relinks the view + **`kama.lock` stays byte-identical**; non-dependency
override errors; a local `registries.default` resolves a dep that `kama.json` alone can't) and
`tools/check-toolchain.sh` (a local `toolchain` beats the `kama.json` pin).

Tested via `tools/check-log.sh` (registered in `run_tests.sh`, native-only — std::log writes to stderr, which
the sanitizer harness captures, so it must not be a `tests/*.kama` fixture): level+tag filtering, `--log`
(both forms) + `KAMA_LOG` + precedence, a custom sink, and the freestanding `--target embedded` lowering.

## `std::log` v2 (the recognized-facade lowering) — as shipped (M7)

Part C realized: the five facade calls are **compiler-recognized and lowered** so the message is built only if
the record survives two guards. Because a facade call returns `void` it can only appear as an
`ExpressionStatement` — the lowering intercepts there (`emitLogFacade`, before the generic expression-statement
path in `kama.cemit.cpp`), which also gives the statement-level control ISO C11 needs (no statement-expressions)
to place the message build *inside* the guard. `logFacadeLevel` matches the resolved callee against the five
`std__log__log{Error,Warn,Info,Debug,Trace}` keys (both the bare-imported and `std::log::`-qualified spellings
resolve the same via `resolveFunc`), yielding the level ordinal 0..4. A recognized statement lowers to:

```c
{   kama_string tag = <tag>;                       // bound once (the filter needs it)
    if (kama_log_enabled(L, tag.data, tag.len)) {  // inlined here — reads the process-global config, TU-safe
        <message build hoisted HERE>               // interpolation/concat/call — skipped when filtered
        kama_log_dispatch(L, tag.data, tag.len, msg.data, msg.len);
    }
}
```

- **Compile-time strip (level).** Under `--release` a `Debug`(3)/`Trace`(4) call is dropped *physically* at emit
  time — `emitLogFacade` returns nothing, exactly like `debugAssert` (works at any `-O`, no reliance on C DCE);
  `Error`/`Warn`/`Info` are kept. The floor is a single field, `_logCompileMin` (release ⇒ 3, else 99 = no
  strip), derived in `setRelease` — so a future `kama.json log.compileMin` override is a one-line change.
- **Runtime guard (level above the floor + tag), message inside.** The inlined `kama_log_enabled` reuses the v1
  filter. The message and tag ride `hoistStringTemp` (`logSpanOf`): an owned rvalue (interpolation/concat/call)
  becomes a scope-dtor'd temp dropped — via `dropCondTemps` — *inside* the guard where it was hoisted (the
  message's `Formatter` temp included), a literal/lvalue is a borrow temp (never dropped, so a heap-owning
  variable arg can't double-free). `logEnabled` stays available but is no longer manually required.
- **The swappable sink slot is process-global (external linkage).** The inlined `kama_log_dispatch` reads the
  sink slot from *its own* TU, while `setLogSink` writes it from the std::log TU — so a per-TU `static` slot
  would read empty and always hit the console default (the multi-TU-static hazard the config side dodges via the
  process-global env). The fix is the general one, not a per-call-site workaround: `kama_log_slot` has
  **external linkage** with a single definition emitted in the entry TU (`isEntry` in `kama.cemit.cpp`), so every
  TU shares one object. This is the same treatment the **panic-handler slot** needs — see the note below.
  `@compileFor(FLAG)` remains the full physical-drop escape hatch for a whole subsystem — no new mechanism.
  `tools/check-log.sh` gains: an observable-side-effect message proving it is built only when the record passes
  (default vs `--log=debug`), and a `--release` transpile-grep proving `Debug`/`Trace` call sites are stripped
  while `Info` is kept (and all survive a non-release build).

## Process-global runtime slots — external linkage (the panic handler / sink cross-TU fix)

Found while shipping M7: the runtime is header-only (`static inline` functions + `static` globals in
`kama_runtime.h`/`kama_log.h`), and a multi-file/debug build compiles **one TU per source** (each including the
shared `.gen.h`), so every `static` runtime global is **duplicated per TU**. That is deliberate for genuinely
isolate-local state (the MCU per-isolate statics use `KAMA_ISOLATE_LOCAL`/`_Thread_local`) — but it is a bug for
a **process-global, set-once slot written in one TU and read in another**:

- **`kama_panic_hook`** (+ the `kama_in_panic_hook` re-entrancy flag) — `setPanicHandler` writes it from the
  entry TU, but a panic *originates* wherever a `static inline` fatal path (bounds/panic/assert) was inlined:
  every TU. A per-TU slot let a panic raised in **library/stdlib code** read its own empty copy and silently
  take the **default abort** instead of the user's handler. (Not caught earlier because the M2 fixture triggers
  its panic from `main`'s TU; a single-file trap fixture is one TU and *cannot* exhibit it.)
- **`kama_log_slot`** — the M7 sink case above, same root cause.
- **`kama_argc` / `kama_argv`** — the prelude floor `args()`/`programName()`/`programInvocation()`/
  `programPath()` are `static inline` reading argv, but `kama_args_init` runs only in `main` (the entry TU). A
  library-module call to `args()` read an empty argv (`args().count()` == 0 while `main` saw the real count).
  This one *contradicted its own documented intent* — the header already says "argv is process-wide, every
  isolate must see the same vector" — so external linkage realizes that intent rather than changing it. (An
  earlier decision had kept argv per-TU and instead env-bridged the one config that needed it, the std::log
  `--log` flag; that covered only that path, not the general `args()` surface.)

All three are now **external-linkage with a single definition emitted in the entry TU** (`isEntry` in
`kama.cemit.cpp`, at file scope before `main`): the panic-hook pair and `kama_argc`/`kama_argv` under
`#if !defined(KAMA_TARGET_EMBEDDED)` (the headers declare them under the same guard — embedded uses the weak
`kama_panic_handler` and has no argv), and `kama_log_sink_fn kama_log_slot` when the program imports std::log
(declared unconditionally, present on embedded too). The accessors stay `static inline` referencing the shared
externs, so read paths keep zero call overhead. Verified by `tools/check-panic-multitu.sh` (a two-file build
whose panic is raised in `Lib`'s TU while `setPanicHandler` runs in `main`'s), `tools/check-argv-env.sh` case 4
(a library-TU `args()` must match `main`'s), and check-log's cross-TU custom-sink case.

**Deliberately NOT changed (per-TU is correct):** the `KAMA_LOG` filter *cache* (`kama_log_global`/`_ntags`/…)
— each TU re-derives identical state from the process-global env, so per-TU copies never disagree — and
`kama_trace_acc`, a documented single-TU test helper.

## Residual implementation details (settle in the build session)
- Exact `LogRecord` fields if `emit` grows beyond `(level, tag, msg)` (timestamp / source loc / isolate id).
- `--log` / `KAMA_LOG` grammar edge cases (multiple tags, wildcard `*`, level names vs numbers).
- Whether the default `ConsoleLogger` colorizes on a TTY (and how it detects one without `<stdio.h>` leak).
- Multiple simultaneous sinks (console + file) in the default backend, or v1 single-sink.
