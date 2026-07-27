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

---

## M2 — Windows parity (KICKOFF — not yet built)

**Goal:** the SAME `lib/std/process/process.kama` surface works on Windows, by implementing the process
functions in `kama_os.h`'s `#if defined(_WIN32)` branch with EXACTLY the POSIX signatures (the header's
invariant: kama modules are byte-identical across platforms, only the header differs). **Verification is
Windows CI only** — the `windows-test` job in `.github/workflows/ci.yml` (windows-latest, mingw-w64-ucrt +
clang, `continue-on-error: true`) runs the full suite over the Windows `kama_os.h` branch; there is no local
Windows box (user's UTM attempt failed). So M2 is written blind and proven in CI.

### Central design decisions to settle first (with leans)

- **D1 — handle field `int32` → `isize` (RECOMMENDED, and it touches the kama surface, so decide first).**
  POSIX waits on a `pid` (int); Windows waits on a process `HANDLE` (void*, wider than int) — the PID does
  NOT work with `WaitForSingleObject`/`TerminateProcess`. Store the wait handle as `isize` (pid on POSIX,
  `(isize)hProcess` on Windows), like `TcpStream`'s socket handle. Change: `Process` field `int32 pid` →
  `isize handle`; seam `kama_proc_spawn(…, Ptr<isize> outHandle, Ptr<int32> outPid)`,
  `kama_waitpid(isize handle, …)` → rename `kama_proc_wait`, `kama_kill(isize handle, …)`. Keep a separate
  `int32 pid` field so `Process.id()` stays the real OS pid on both (POSIX: pid==handle; Windows:
  `PROCESS_INFORMATION.dwProcessId`). This is a small, mechanical POSIX-branch refactor done up front so M2
  isn't a surface change.

- **D2 — reuse CRT fds for child stdio (RECOMMENDED).** Keep `File{int32 fd}` on Windows via the CRT fd
  layer the Windows fs branch already uses (`_open`/`_read`/`_write`/`_close`). `kama_pipe` on Windows =
  `_pipe(fds, 65536, _O_BINARY)` (returns int fds). For `CreateProcess`, convert the child-side fd →
  `HANDLE` with `_get_osfhandle(fd)`, make it inheritable (`SetHandleInformation(h, HANDLE_FLAG_INHERIT)` or
  `DuplicateHandle`), put it in `STARTUPINFO.hStdInput/Output/Error`, `bInheritHandles=TRUE`. The FD_CLOEXEC
  trick has a Windows analog: mark ONLY the child's ends inheritable (the parent's kept ends stay
  non-inheritable) — the reverse-default of POSIX but the same intent. Keeps the kama surface (int32 fd)
  identical, so `stdin()/stdout()/stderr()`/`File`/`closeStdin` need zero change.

- **D3 — argv → command line quoting.** `CreateProcess` takes ONE command-line string, not argv[]. Implement
  the canonical MSVCRT/`CommandLineToArgvW` quoting in a C helper `kama_win_cmdline(char** argv) -> char*`
  (quote args with space/tab/quote/empty; escape embedded `"` and runs of `\` before a `"`). Reference:
  Daniel Colascione, "Everyone quotes command line arguments the wrong way." This is the classic Windows
  footgun — get it byte-exact and fixture it.

- **D4 — environment block.** Windows env is a single double-NUL buffer `KEY=VALUE\0KEY=VALUE\0\0`, not
  `char*[]`. The kama `buildEnvp` already assembles `KEY=VALUE` strings; add a Windows `kama_win_envblock`
  that merges inherited env (`GetEnvironmentStringsW`/`_environ`) + overrides (replace-by-key, same semantics
  as `kama_envp_build`) into that buffer. `NULL` = inherit.

- **D5 — `run()` concurrent dual-drain WITHOUT select-on-pipes (THE BIG ONE — biggest risk).** M1's `run()`
  reuses `std::net::Poller`, but on Windows `select()` works on SOCKETS ONLY — it can't poll pipe fds/handles.
  So the Poller path can't drain child pipes on Windows. Options: (a) **thread-per-stream** — a reader
  drains stderr while the main thread drains stdout, then join (simplest correct; matches Rust/Python on
  Windows); (b) overlapped/async `ReadFile`; (c) `PeekNamedPipe` poll loop (busy). **Lean:** do the
  concurrent capture in a **C seam helper** so `run()`'s kama stays platform-neutral — e.g.
  `kama_capture2(int outFd, int errFd, <two growable byte sinks>)` that POSIX implements with `poll` and
  Windows with two threads (or overlapped I/O). The sink-marshalling is the wrinkle (kama `DynamicArray`
  can't cross FFI directly) — either (i) the helper fills two `malloc`'d buffers + returns lengths and kama
  copies them in, or (ii) keep the kama-level drain but branch it with **`@compileFor(WINDOWS)`** (the
  shipped conditional-compilation primitive): POSIX arm = Poller, Windows arm = an `isolate` reading stderr +
  a channel handing the bytes back. **Settle D5 before writing code** — it decides whether `run()` grows a C
  helper or a `@compileFor` split.

- **D6 — reap/signal semantics (straightforward).** No signals on Windows → `ExitStatus.signal` is always 0,
  `success()` = `code == 0`. `~Process()` non-blocking = `WaitForSingleObject(h, 0)` then `CloseHandle(h)`
  (detach; Windows auto-reaps when the last handle closes — no zombies). `wait()` =
  `WaitForSingleObject(h, INFINITE)` + `GetExitCodeProcess`. `tryWait()` = `WaitForSingleObject(h, 0)` →
  `WAIT_TIMEOUT` = still running. `kill()`/`terminate()` = `TerminateProcess(h, 1)` (Windows has no
  SIGKILL/SIGTERM distinction; both map to TerminateProcess — document that `signal(sig)` is a POSIX-only
  fidelity and on Windows any signal terminates).

### Seam mapping (POSIX `kama_*` → Windows)

| kama seam fn | POSIX (shipped) | Windows (M2) |
|---|---|---|
| `kama_pipe` | `pipe()` + FD_CLOEXEC | `_pipe(…, _O_BINARY)`; mark child end inheritable only |
| `kama_proc_spawn` | `fork`+`chdir`+`dup2`+`execvp` | `kama_win_cmdline` + `kama_win_envblock` + `CreateProcessA` (STARTUPINFO w/ redirected `_get_osfhandle` handles, `bInheritHandles`, `lpCurrentDirectory`) |
| wait (`kama_proc_wait`) | `waitpid` | `WaitForSingleObject` + `GetExitCodeProcess` |
| `kama_kill` | `kill(pid,sig)` | `TerminateProcess(h,1)` |
| `kama_proc_exited/exit_code/signaled/term_signal` | `WIF*` macros | exit code from `GetExitCodeProcess`; signaled always 0 |
| argv/envp | `kama_argv_*` + `kama_envp_build` | `kama_win_cmdline` + `kama_win_envblock` (the argv-vector helpers are platform-agnostic C — hoist them above the `#if` split or duplicate) |

### Fixtures / CI (the OTHER blocker)

The M1 `proc_*` fixtures invoke **POSIX-only** programs (`sh`, `sleep`, `cat`, `echo`, `printenv`, `pwd`,
`true`, `false`, `/bin/sh`, `/usr/bin/printenv`) — **none exist on Windows**, so they'd fail the `windows-test`
leg on a fresh checkout even before the seam is written. Two tasks:
1. **A bundled, cross-platform test helper** (RECOMMENDED) — a tiny program the fixtures drive instead of
   system utilities, with deterministic subcommands (`echo <text>`, `exit <n>`, `cat` stdin→stdout,
   `sleep <ms>`, `printenv <k>`, `pwd`). Build it once (a small `.c` or a `.kama` compiled to a native binary
   in the test harness); every `proc_*` fixture then runs identically on POSIX and Windows. This is the clean
   fix and removes the current dependency on `/bin/sh` et al.
2. Until then, `run_tests.sh` should **skip `proc_*` on the Windows leg** (mirror the wasm skip: a
   `uses_proc` + Windows-target guard) so the best-effort Windows job isn't newly red from M1. (Optional tidy
   that could land NOW, independent of M2 — flag to the user.)

### Risks (ranked)

1. **D5 pipe-drain without select** — the only genuinely new mechanism; everything else is a direct API
   swap. Prototype the thread/overlapped drain first.
2. **D3 argv quoting** — subtle, security-relevant (injection), must be byte-exact; heavily fixture it.
3. **No local test box** — blind development, CI round-trips are slow; the cross-platform helper (Fixtures §1)
   makes the CI signal trustworthy.
4. **Handle vs fd/pid impedance (D1/D2)** — mechanical but touches the surface; do the `isize` refactor up
   front on the POSIX branch so it's proven green before the Windows branch exists.

### Suggested M2 order

D1 (isize handle refactor, POSIX, stays green) → cross-platform test helper + re-point fixtures (POSIX stays
green) → Windows `kama_pipe`/`kama_proc_spawn`/wait/kill + `kama_win_cmdline`/`kama_win_envblock` → D5 Windows
`run()` drain → iterate on the `windows-test` CI leg until the `proc_*` fixtures pass there.

---

## As shipped — M2 (Windows parity), 2026-07-26

Followed the suggested order; `process.kama` is byte-identical across platforms (only `kama_os.h` differs).
Landed green on native (791), ASan+UBSan (764), and wasm (738, `proc_*` skipped). Windows is verified by the
`windows-test` CI leg (mingw-w64-ucrt + clang).

**D1 (handle refactor, POSIX up front, stayed green):** `Process` now holds `isize handle` (POSIX: pid;
Windows: `(isize)hProcess`) + a separate `int32 pid` for `id()`. Seam: `kama_waitpid` → `kama_proc_wait(isize
handle, …)`, `kama_proc_spawn(…, Ptr<isize> outHandle, Ptr<int32> outPid)`, `kama_kill(isize handle, …)`.

**D5 (run() drain) — decided: single C seam helper on BOTH platforms (kickoff option 1).** The in-kama
`std::net::Poller` drain in `run()` was replaced by one platform-neutral `kama_capture2(outFd, errFd, …)`
call; the C seam implements it with `poll` on POSIX and two reader threads on Windows. Rationale over a
`@compileFor` split: adopts the reference (Rust std) design on both platforms, collapses `run()` to a single
path exercised identically everywhere (no divergent, CI-only-tested Windows arm), and upholds the seam
invariant. `kama_capture2` returns two heap buffers the kama side copies into `DynamicArray<uint8>` then
frees with `kama_free` (`bytesFromRaw`). `run()` no longer imports `std::net`/`FixedArray`.

**Windows `kama_os.h` branch** (`#if defined(_WIN32)`, EXACT POSIX signatures): `kama_pipe` = `_pipe(…,
_O_BINARY | _O_NOINHERIT)` (both ends non-inheritable — the FD_CLOEXEC analog); `kama_proc_spawn` =
`CreateProcessA` with `STARTF_USESTDHANDLES` (redirected ends → `_get_osfhandle` + `SetHandleInformation`
inheritable just for the call; all-inherit case uses default inheritance, no `STARTF_USESTDHANDLES`) +
`kama__win_cmdline` (MSVCRT/`CommandLineToArgvW` quoting, unit-tested byte-exact) + `kama__win_envblock`
(double-NUL block built from the char*[] `kama_envp_build` returns); `kama_proc_wait` =
`WaitForSingleObject` (poll vs INFINITE by `flags & WNOHANG`) + `GetExitCodeProcess`, closing the HANDLE on
reap; `kama_kill` = `TerminateProcess(h, 1)`; status accessors: `exited`→1, `exit_code`→st, `signaled`→0,
`term_signal`→0 (keeps kama's `statusOf` unchanged); `kama_capture2` = two `_read` threads.

**Cross-platform seam additions (both branches), to keep `process.kama` identical + correct:**
- `kama_proc_detach(handle)` — the dtor's non-blocking release. POSIX: `waitpid(WNOHANG)` (reap-or-detach);
  Windows: `WaitForSingleObject(h,0)` + `CloseHandle` (detach — no handle leak on drop-while-running).
- `kama_open_null_read/write()` — the platform null device (`/dev/null` vs `NUL`), replacing the hardcoded
  `/dev/null` string in `start()`'s `Stdio::Null` arms.
- `kama_sleep_ms(ms)` — POSIX `poll(NULL,0,ms)` / Windows `Sleep`; used by the test helper (a general
  convenience). (`kama_getcwd` from the kickoff was NOT needed — see the test helper below.)

**Test helper + fixtures.** M1's `proc_*` drove POSIX-only utilities (sh/echo/cat/printenv/pwd/sleep/…). They
now drive one bundled cross-platform child, `tests/support/procutil.kama` (a kama program — dogfoods the
floor + std::fs — built once by `run_tests.sh`, located via `KAMA_PROCUTIL`), with subcommands `echo`/`echo2`
/`exit`/`cat`/`sleep`/`printenv`/`spew`/`hasfile`. `proc_cwd` proves `cwd()` via `hasfile` (relative open),
so no `getcwd` was needed. New `proc_quote` exercises the argv-quoting round-trip. Two documented platform
divergences are asserted as the portable contract, not the OS specific: `proc_notfound` (POSIX Ok(127) vs
Windows Err — both accepted) and `proc_kill` (`!success()` — POSIX signal-9 vs Windows TerminateProcess
code 1). The Windows `proc_*` skip guard in `run_tests.sh` was removed.
