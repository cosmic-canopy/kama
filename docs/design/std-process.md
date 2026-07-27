# `std::process` — subprocess execution (design of record / kickoff)

**Status: KICKOFF — not yet built.** Scheduled as [ROADMAP.md](../ROADMAP.md) §1 near-term order #4 (after the
argv/env prelude floor #2 and diagnostics & logging #3 — a subprocess API wants console I/O + child stdio). This is **pure library work over the shipped FFI + concurrency seams — no
compiler/language change.** Running other binaries is genuinely useful on Linux/servers/tooling; today it is
only reachable by hand-declaring `extern fn system(...)` etc., so this wraps it in a safe, RAII, cross-platform
module the way `std::fs`/`std::net` wrap libc.

## Why it's library, not language

- **Spawn mechanism = OS FFI.** `posix_spawn`/`fork`+`execvp`/`pipe`/`waitpid` on POSIX; `CreateProcess`/pipes/
  `WaitForSingleObject` on Windows — declared behind a bundled `kama_os.h`-style seam exactly like
  `kama_open_read`/`kama_read` in `std::fs` (see `lib/std/fs/*.kama` + `kama_os.h`). No new syntax.
- **Lifetime = RAII.** A child process is an owned resource: a `type resource Process` whose dtor closes its
  pipe fds and does a *non-blocking* reap (`waitpid(WNOHANG)`: reap if already exited, else detach). A
  `Process` that falls out of a `scope` gets **ordinary RAII drop**, exactly like a `File` closing its fd —
  it does **not** ride the structured-concurrency join barrier. That barrier is hard-coded for `isolate`s
  because only an isolate can hold a live `ref`-borrow into a parent local, which the join-before-drop
  ordering protects; a child process shares no address space, so there is nothing to make race-free and
  nothing to wait on for safety. Net: dropping a `Process` never blocks and orphans are **not** structurally
  prevented — a still-running detached child outlives its handle (documented, like Rust's `Child`). Use
  explicit `wait()` when you want the exit status and a definite reap.
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
OS-threading runtime and the `Poller` readiness substrate (which `run()` reuses to drain two pipes without
deadlocking). Note the `scope` structured-lifetime model does **not** apply to a `Process` — see the
corrected "Lifetime = RAII" bullet above; a child gets plain RAII drop, not the isolate join barrier.

---

## As shipped — M1 (POSIX), 2026-07-26

Pure library over the `kama_os.h` seam; **no compiler/language change**. Landed green on native (788), ASan
+UBSan (761), and the wasm leg (737, all `proc_*` fixtures skipped — no fork/exec in the sandbox).

**Surface** (`lib/std/process/process.kama`, `namespace std::process`, auto-discovered from disk):
- `enum Stdio { Inherit, Piped, Null }` — per-stream disposition.
- `type value ExitStatus { code; signal; success() }` — POSIX exit-code XOR terminating-signal (`code=-1`
  when signalled, `signal=0` when exited).
- `type resource Command` — builder: `make(program:)`, `shell(line:)` (opt-in `sh -c`, argv vector is the
  injection-safe default), `arg`/`args`/`cwd`/`env`/`envClear`/`stdin`/`stdout`/`stderr`, then
  `start() -> Result<Process, IoError>` or `run() -> Result<Output, IoError>`.
- `type resource Process` — `id()`, `wait()` (blocking reap), `tryWait() -> Optional<ExitStatus>`
  (non-blocking), `kill()`/`terminate()`/`signal(sig)`, `stdin()`/`stdout()`/`stderr()` (borrow the piped
  `File`), `closeStdin()` (send EOF). Dtor = non-blocking reap/detach (never blocks).
- `type resource Output` — `status()`, `stdout()`, `stderr()` (captured byte buffers).

**Key decisions as built:**
- **Reap = detach + non-blocking** (settled with the user): `~Process()` does `waitpid(WNOHANG)` — reap a
  zombie if the child already exited, else detach. Never blocks (avoids the blocking-dtor-vs-undrained-pipe
  deadlock). Explicit `wait()` is the blocking reap.
- **Spawn = `fork` + `chdir` + `dup2` + `execvp`** (not `posix_spawn`): fully portable cwd, and the child
  touches only async-signal-safe calls before exec. A custom env is a **fully-built `envp` assigned to
  `environ` in the child** (built in the parent, where malloc is safe) — never `setenv` post-fork (not
  async-signal-safe), and portable across macOS's missing `execvpe`. Safe under a multithreaded (isolate)
  parent: only the forking thread survives in the child and it holds no lock.
- **Child streams reuse `std::fs::File`**: `File.make(fd)` was made public + `rawFd()` and `close()`
  accessors added (mirroring `TcpStream.make`/`rawHandle`). Feeding stdin then EOF uses `closeStdin()` =
  `File.close()` on the write end (an explicit, idempotent close that zeroes the fd so the dtor's `fd > 0`
  guard won't double-close). Two fork/exec hygiene points were needed and are worth recording: (1) the child
  inherits a *copy of every parent pipe fd* via fork — including the stdin **write** end — so **both** ends
  of each pipe are created `FD_CLOEXEC` (`kama_pipe` sets it via `fcntl`; macOS has no `pipe2`), which makes
  `exec` drop every inherited end while the dup2'd 0/1/2 survive; without it a child like `cat` never sees
  EOF. (2) An explicit `close()` — not the reassign-and-drop idiom — is what `closeStdin` uses, because a
  field reassignment's drop-of-old did not reliably close the fd here; `close()` is unambiguous and is a
  generally useful early-close on `File`.
- **`run()` drains both pipes concurrently via `std::net::Poller`** (poll works on pipe fds) — reading one
  stream to EOF then the other would deadlock when the child fills the second pipe. Guarded by
  `proc_large_both_no_deadlock` (>64 KiB to both). Async streaming of a *live* child is the labelled post-v1
  follow-on; stdin *feeding via the write end* is supported (`stdin()`+`closeStdin()`), but non-blocking
  live-stream reads ride that same post-v1 Poller extension.

**Seam additions** (`kama_os.h`, POSIX `#else` branch, all `static inline`, pruned when unused):
`kama_argv_new/set/free`, `kama_envp_build` (inherited-env merge with per-key override), `kama_pipe`,
`kama_proc_spawn`, `kama_waitpid`, `kama_proc_exited/exit_code/signaled/term_signal`, `kama_kill`,
`kama_WNOHANG/SIGKILL/SIGTERM`. Hand-declared the process syscalls (`fork`/`execvp`/`pipe`/`dup2`/`chdir`/
`_exit`/`kill`/`strdup`/`environ`) in the file's existing block-scoped-extern style (the container compiles
strict ISO C, which hides POSIX decls behind feature-test macros; matches how `read`/`write`/`close` are
already handled).

**Deferred to later milestones:** Windows (`CreateProcess`/`CreatePipe`/`WaitForSingleObject` + argv-quoting)
= M2, own session (the handle field may widen to `isize` then, like `TcpStream`). Async/Poller-driven
*live* child-stream reads = post-v1.
