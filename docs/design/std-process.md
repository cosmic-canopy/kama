# `std::process` — subprocess execution (design of record / kickoff)

**Status: KICKOFF — not yet built.** Scheduled as [ROADMAP.md](../ROADMAP.md) §1 near-term order #3 (after the
argv/env prelude floor, #2). This is **pure library work over the shipped FFI + concurrency seams — no
compiler/language change.** Running other binaries is genuinely useful on Linux/servers/tooling; today it is
only reachable by hand-declaring `extern fn system(...)` etc., so this wraps it in a safe, RAII, cross-platform
module the way `std::fs`/`std::net` wrap libc.

## Why it's library, not language

- **Spawn mechanism = OS FFI.** `posix_spawn`/`fork`+`execvp`/`pipe`/`waitpid` on POSIX; `CreateProcess`/pipes/
  `WaitForSingleObject` on Windows — declared behind a bundled `kama_os.h`-style seam exactly like
  `kama_open_read`/`kama_read` in `std::fs` (see `lib/std/fs/*.kama` + `kama_os.h`). No new syntax.
- **Lifetime = RAII + `scope`.** A child process is an owned resource: a `type resource Process` whose dtor
  reaps the child (no zombies) and closes its pipe fds. The shipped **structured-concurrency `scope`** model is
  the natural fit — a child spawned in a scope is waited on at scope exit (the process analog of join-before-
  drop), so orphans are unrepresentable.
- **Async child I/O = the shipped `Poller`.** Reading a child's stdout without blocking the caller rides
  `std::net::Poller` (readiness multiplexing over the pipe fds). v1 ships blocking; the Poller path is the
  post-v1 async extension — the seam already exists, so no rework.

## Proposed surface (settle in the build session)

A `Command` builder → `spawn` → an owned `Process`, plus a one-shot `run` convenience. Explicit, no hidden
shell.

```
// std::process
type value Stdio { … }          // Inherit | Piped | Null   (per stream)
type resource Process { … }     // owns the child: pid/handle + optional piped stdin/stdout/stderr (File-like)
type value ExitStatus { … }     // code + (POSIX) signal; .success()

// builder — args as a VECTOR, never a shell string (injection-safe by construction)
Command c = Command::create(program: "git");
c.arg(a: "status"); c.arg(a: "--porcelain");     // or args(list:)
c.env(key: "K", value: "V"); c.cwd(path: "…");   // inherit-by-default
c.stdout(cfg: Stdio::piped());
Result<Process, IoError> p = c.spawn();

// on the Process
Result<ExitStatus, IoError> st = proc.wait();     // blocking reap
// piped streams are std::fs File-shaped (reuse the fd/File wrapper): proc.stdout().readAll() etc.

// one-shot convenience (Rust `Command::output` analog): spawn + capture stdout/stderr + wait
Result<Output, IoError> out = run(command: c);     // Output { status, stdout: DynamicArray<uint8>, stderr: … }
```

## Decisions to make in the build session

- **Reap policy on drop.** Blocking `wait()` in a dtor is a surprise. Lean: dtor does a **non-blocking reap /
  detach** (POSIX: `waitpid(WNOHANG)` then leave to init; or double-fork-style detach), and *explicit* `wait()`
  is how you get the exit code. Document that dropping without `wait()` detaches, doesn't block.
- **stdin/stdout/stderr handles = reuse `std::fs::File`?** Prefer reusing the existing fd/`File` wrapper so
  child pipes compose with the rest of `std::io` (readers/writers, the transform adapters in ROADMAP §1). May
  need `File` to accept a bare fd (relates to the "can you own stdin?" fd-niche question, ROADMAP §2).
- **Shell vs exec.** Default is **exec with an argv vector** (no shell, injection-safe). A `system()`-style
  `sh -c "…"` convenience is a *separate, clearly-named* opt-in (`Command::shell(line:)`), not the default.
- **Env inheritance.** Inherit the parent env by default; `env()`/`envClear()` modify. Passing the parent's
  *own* argv/env through cleanly wants the **argv/env prelude floor** (#2) — hence the ordering.
- **Windows parity.** `CreateProcess` arg-quoting rules differ from POSIX argv; the seam normalizes so the kama
  surface is identical. Pipe creation + inheritance flags differ — hide both behind `kama_os.h`.

## Acceptance

Spawn a child, capture its stdout + exit code, on **POSIX + Windows**; RAII-clean (no zombies / leaked fds/
handles — ASan-clean); a fixture that runs a real child (e.g. `echo`/a bundled helper) and asserts captured
output + status; the `run(...)` one-shot; injection-safe argv default. Async (Poller-driven) child I/O is a
labelled post-v1 follow-on.

## Relation to the concurrency campaign

The concurrency work did **not** unlock subprocesses (FFI always could) — they are orthogonal primitives
(in-process isolates vs. separate-address-space children). But it **built the seams that make this clean**: the
OS-threading runtime, the `scope` structured-lifetime model (wait-on-exit), and the `Poller` readiness
substrate (async child I/O). `std::process` is the first library to compose all three.
