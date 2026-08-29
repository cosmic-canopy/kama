# Developing kama on Windows

Windows is the platform this repo is least often built on, and that shows up as a specific failure
mode: things break in ways that stay green in CI, because everything that tested Windows ran inside
the shell that built it. The whole of the 2026-08-11 triage was that shape — see the commits from
that day for the detail.

This page is what a contributor (or a fresh AI session) needs to pick up Windows work without
rediscovering the environment. The *plan* lives in [ROADMAP.md](../ROADMAP.md); this is the how.

## The environment

The CI leg is msys2 / UCRT64 with mingw-w64 clang, and a local checkout should match it.

```powershell
winget install --id MSYS2.MSYS2 -e
```

Then, from `C:\msys64\usr\bin\bash.exe -l`:

```sh
pacman -Syuu --noconfirm                       # twice; the first pass updates the runtime
pacman -S --needed --noconfirm make bison flex diffutils git \
    mingw-w64-ucrt-x86_64-clang
```

`diffutils` and `git` are **not** optional. `tools/run-checks.sh` refuses to start without them,
because a guard that cannot find `diff` does not report a missing tool — it reports the comparison as
failed, and a clean tree gets blamed for it.

Everything must run with `MSYSTEM=UCRT64` set, or `uname -s` reports `MSYS_NT` and the build lands in
a different `out/` directory than the one the tests look in. Driving that shell from *outside* msys2
— another terminal, an editor task, an agent — needs two things that pull against each other:

```sh
MSYSTEM=UCRT64 /c/msys64/usr/bin/bash.exe -lc 'pushd /c/path/to/kama >/dev/null; ./run_tests.sh'
```

- **`-l` is required.** Without a login shell `/ucrt64/bin` is not on `PATH`, so there is no `clang`
  and no `bison` — `-c` alone gets you a shell that cannot build.
- **`-l` also `cd`s to `$HOME`,** and `run_tests.sh` resolves `tools/kama-bin.sh` relatively, so it
  dies on a path that is not there. Hence the explicit `pushd`. ⚠️ `CHERE_INVOKING=1` is **not** the
  answer, whatever it does elsewhere: this msys2's `/etc/profile` never mentions it, and msys2's
  environment filter does not pass it to the child anyway (`MSYSTEM` it does pass). It was in this
  page until someone ran it.
- A `tools/check-*.sh` invoked by **absolute path** needs neither — each resolves `ROOT` from `$0`.

⚠️ **`uname -s` carries the OS build number** — `MINGW64_NT-10.0-26200-ARM64` — so `out/<platform>/`
is not a name anything outside that shell can reconstruct. The VS Code extension globs `out/*/` and
takes the newest rather than trying. UCRT64 and CLANGARM64 report the *same* `uname` and so share one
`out/` directory — `PLATFORM=` separates the build but **not** the harness, which is its own trap:
see [Two msys2 environments](#two-msys2-environments-and-why-the-build-system-cannot-tell-them-apart).

## Running things

`./dev` works as documented. The container legs (`./dev test san|wasm`) need Docker or podman and are
not usually available on a Windows box, so `./dev test` and `./dev check` are the local gate.

```sh
make -j4                    # ~100 s here
./run_tests.sh              # ~906 s; the cost is per-fixture C compilation, not the tests
./dev fixture <name>...     # the inner loop
```

If the suite looks slow, it is not hung. It is ~906 s here against ~75 s in the Linux container, and
what it is spending that on is process startup plus the C compile — **not** a network or socket
timeout, whatever an older note may have said. `net_addr_ctor` opens no socket at all and cost the
same 40 s as `net_refused`.

⚠️ **Do not compare against a CI number to size this up.** A `~1837 s` figure for the CI runner was
briefly recorded here as if it were the native-x86_64 control. It is not usable as one: it was taken
on a leg that was `continue-on-error` and RED, and several of this suite's failure modes are ~40 s
timeouts, so dozens of failures inflate the wall clock for reasons unrelated to the platform.

**The likelier explanation for this box specifically is emulation, and it is worth checking before
blaming Windows.** `uname -s` reporting `…-ARM64` while `uname -m` reports `x86_64` means the kernel
is ARM64 and the msys2 environment is not: every process — `clang`, `kama.exe`, and each fixture
binary — is x86_64 running under Windows' x64 translation layer. `file "$(which clang)"` says which
you have. See
[Two msys2 environments](#two-msys2-environments-and-why-the-build-system-cannot-tell-them-apart).

## Two msys2 environments, and why the build system cannot tell them apart

On an ARM64 Windows host both of these can be installed at once, and they select **different
compilers for the same source tree**:

| `MSYSTEM` | `which clang` | that clang is | matches CI? |
|---|---|---|---|
| `UCRT64` | `/ucrt64/bin/clang` | x86-64 — **emulated** on an ARM64 host | yes |
| `CLANGARM64` | `/clangarm64/bin/clang` | ARM64 — native | no |

⚠️ **They are indistinguishable to the build system.** Measured, both from this box:

```
UCRT64      uname -s=MINGW64_NT-10.0-26200-ARM64  uname -m=x86_64
CLANGARM64  uname -s=MINGW64_NT-10.0-26200-ARM64  uname -m=x86_64
```

Identical on both keys — `uname -m` says `x86_64` in *both*, because `bash.exe` is itself the
emulated x86_64 binary reporting on itself, not on the compiler it is about to invoke. msys2 ships no
native ARM64 runtime, so `bash` and `make` stay emulated whichever environment you pick.

So `out/$(uname -s)-$(uname -m)/` is the **same directory** for both, and there is no safe way today
to keep both builds:

- **No `PLATFORM=`** → the second build silently overwrites the first *in place*. The harness then
  tests whichever compiler built last while reporting the platform of the other. This is the
  stale-binary trap that `./dev` exists to make unrepresentable, and here it is representable.
- **`PLATFORM=<name>`** → the build lands in `out/<name>/`, and the harness **never sees it**.
  `tools/kama-bin.sh` resolves `$KAMA` from `uname`, not from `PLATFORM`:

  ```sh
  KAMA="$ROOT/out/$(uname -s)-$(uname -m)/kama"
  [ -x "$KAMA" ] || KAMA="$ROOT/kama"
  ```

  The other environment's binary is still sitting at that path, so the `-x` test passes and the
  fallback is never reached. You build ARM64, and `./run_tests.sh` tests the stale x86_64 one without
  a word. (The root `./kama` is no help either: `ln -s` degrades to a **copy** on msys2, so it is a
  snapshot of whichever build ran last, not a pointer.)

  **Until the fix below lands, `export KAMA=<abs path>` is the only way to be sure which compiler the
  harness runs** — `kama-bin.sh` honors an externally set one, and `tools/run-checks.sh` passes it
  down to the guards.

**Until that is fixed, pick one environment per checkout and stay in it.** `UCRT64` is the one that
matches CI, which is why everything on this page uses it. Keying the platform off `$MSYSTEM` — in
both `Makefile`'s `PLATFORM ?=` and `tools/kama-bin.sh` — is the fix, since `$MSYSTEM` is what
actually selects the toolchain and msys2 does pass it to child processes.

*(Whether the native ARM64 toolchain is meaningfully faster for the suite is untested — do not assume
it from the emulation fact alone. `bash`, `make`, and the per-fixture process churn stay emulated
either way, and this suite's cost is dominated by process startup.)*

## The language server, while you are changing the compiler

A running executable is **locked** on Windows, so while `kama lsp` is up, `make` cannot relink
`out/<platform>/kama.exe` and the build fails. macOS has no equivalent problem, which is why the
workflow differs here. Point the editor at a snapshot instead:

```sh
mkdir -p out/lsp && cp out/<platform>/kama.exe out/lsp/ && cp -r include out/lsp/
```

…and set `"kama.path"` in VS Code's **user** settings to `out/lsp/kama.exe`. `out/` is gitignored, so
nothing enters the worktree. Refresh the snapshot when you want the editor to see new behavior, then
run *kama: Restart Server*. (The `include/` copy is what lets the snapshot resolve `kama_runtime.h` —
`resolveRuntimeDir` probes `<exeDir>/include`.)

## Things that are true on Windows and nowhere else

Worth knowing before debugging, because each of these produced a confident wrong answer once:

- **`long` is 32-bit** (LLP64). `strtol` silently saturates above `INT32_MAX`; use `strtoll`.
- **cmd.exe is the shell `system()` uses.** It has no `/dev/null` (use `NUL`), no `command -v` (use
  `where`), it cannot start a `#!/bin/sh` script, it reads a leading `/` as a switch, and it strips
  the outer quotes off a line that begins with one.
- **`%RANDOM%` is seeded from the system clock**, so processes started in the same tick draw identical
  sequences. It is not a unique-name source for concurrent children.
- **Concurrent appends to one file are not atomic**, unlike POSIX `O_APPEND` under `PIPE_BUF`.
- **`_isatty` means "is a character device"**, and `NUL` is one. Use `GetConsoleMode` for a terminal.
- **The CRT opens stdin/stdout/stderr in TEXT mode**, which rewrites `\n` ⇄ `\r\n`. kama sets binary
  mode in `main()` and in `kama_args_init`; anything new that writes bytes must not undo that.
- **msys2 rewrites POSIX paths in ARGUMENTS** when spawning a native child, but **not in environment
  variables**, and **not inside a payload it cannot see into** — a `file://` URI in a JSON-RPC frame is
  just a string. `kama_native_path` in `tools/kama-bin.sh` covers the second case; `furi` in
  `tools/check-lsp.sh` covers the third. A `/c/Users/…` that reaches native kama unrewritten resolves
  against the CURRENT DRIVE as `C:\c\Users\…`, which exists nowhere, and the failure is silent: the
  server finds no manifest and answers under defaults rather than reporting a path it cannot open.
- **`_fullpath` does not resolve reparse points**, where POSIX `realpath` resolves symlinks. Directory
  junctions (`mklink /J`, how `kama install` materializes `.kama/deps/<name>`) therefore stayed
  unresolved, and every "which package owns this file?" test answered differently than on macOS.
  `absolutePath` opens a handle and asks `GetFinalPathNameByHandle`, which is the only API that knows.
- **`-lfoo` prefers the DLL import library over the static archive.** mingw-w64 installs both
  `libfoo.dll.a` and `libfoo.a` for most of its packages, and the linker takes the import library — so
  a bare `-lpthread` bound `libwinpthread-1.dll` out of `/ucrt64/bin`, which exists on no other
  machine. The binary then dies at process start with `STATUS_DLL_NOT_FOUND` (`0xC0000135`), before
  `main`, so no build-time check and no `try` catches it. `-Wl,-Bstatic -lfoo -Wl,-Bdynamic` pins one
  library without the blast radius of a blanket `-static` (which would also re-bind `-lglfw3` and
  break `--webgpu`). This is what `kama build` now does for its own runtime; `docs/targets.md`
  § *Runtime linkage* is the user-facing half.
- **git does not create real symlinks** without `core.symlinks` (needs Developer Mode or elevation);
  it writes a text file containing the target path instead. Do not commit symlinks.
- **A `.exe` suffix is load-bearing** in any path comparison against a running binary — and in any
  filename predicate a test writes. `find … -name app` matches a filename, not a stem, so a guard
  asserting that a build produced `app` reports "the output went somewhere else" when it went exactly
  where it should. `case "$(uname -s)" in MINGW*) EXE=".exe"` is the idiom (`tools/check-toolchain.sh`).

## Where the remaining work is

**The suite is green here: 972 passed, 0 failed** (`./run_tests.sh`, ~906 s), the guards pass, and the
`windows-test` CI leg is no longer `continue-on-error`. Windows is a supported platform, not a
best-effort one, and a program kama builds here is distributable as it stands.

**What shape a Windows *application* is, as opposed to a Windows console tool**, used to be two open
entries here. One has shipped:

- **Subsystem — SHIPPED.** A GUI program asks for it; console stays the default. See
  [targets.md § Subsystem](../targets.md#subsystem) for the full surface.

  ```sh
  kama build game.kama -o game.exe --target WINDOWS                      # PE32+ executable (console)
  kama build game.kama -o game.exe --target WINDOWS --subsystem windows  # PE32+ executable (GUI)
  ```

  ⚠️ **It needed no Windows machine to build or to guard** — `file` reads the PE subsystem field
  directly, so `tools/check-target.sh` builds both ways and asserts the word in parentheses. Worth
  knowing before scheduling anything else here around hardware. Three things make that work, and each
  cost a wrong turn:

  * **`zig cc` is the cross toolchain.** The driver resolves it for any non-host target
    (`resolveCCompiler`, `kama.driver.cpp`), and it brings its own `lld`, which is what accepts
    `--subsystem`. Apple's `ld` does not — a hand-rolled `clang --target=aarch64-windows-gnu` fails with
    `unknown options: -Bdynamic`, which looks like a flag problem and is a *linker* problem.
  * ⚠️ **`--cc <override>` suppresses the target triple.** The driver assumes an explicitly named compiler
    knows its own target, so an override must supply `-target <triple>` itself.
  * ⚠️ **`kama: built <path>` does not prove a file exists.** The driver reports success on the C
    compiler's exit status without stat-ing its own output, so a `--cc` that silently produces nothing
    still prints "built". Any guard that goes through `--cc` must assert on `file` output, never on the
    build message.

  ⚠️ **The one part a cross-build cannot check — VERIFY THIS ON REAL HARDWARE.** A GUI-subsystem process
  gets no console, so `kama_args_init` (`include/kama_runtime.h`) calls
  `AttachConsole(ATTACH_PARENT_PROCESS)` and rebinds the standard streams so `print` still reaches a
  terminal that launched it. Building proves it compiles and links; only a Windows host proves it
  *works*. Two specific things to confirm, because both were reasoned rather than measured:

  * **It is the FILE DESCRIPTORS that must be rebound, not `stdout`/`stderr`.** `print` goes through
    `kama_raw_write` → `_write(fd, …)` and never touches a `FILE*`, so the obvious
    `freopen("CONOUT$", "w", stdout)` fixes a stream kama does not use and leaves `print` silent. The
    code opens `CONOUT$`, wraps the HANDLE with `_open_osfhandle`, and `_dup2`s it over fd 1 and 2 —
    plus `SetStdHandle`, since the CRT fd and the Win32 std handle are independent namespaces.
    (The ROADMAP prose specified `freopen`; it was wrong, for this reason.)
  * **Double-clicked from Explorer there is no parent console**, `AttachConsole` fails, the rebind is
    skipped, and output is discarded — which is the correct behaviour, but confirm it does not hang or
    crash.

- **Long paths** — the temp-path builder assumes `MAX_PATH`-class lengths. Surfaces only on a deep
  working directory. Still open, and parked in [ROADMAP.md](../ROADMAP.md); not a regression.

The **wall clock** is the other thing to know: the suite is ~906 s here against ~75 s in the Linux
container. The cost is per-fixture C compilation plus Windows process startup — `kama build -j`
parallelizes, but `run_tests.sh` pins `KAMA_BUILD_JOBS=1` and fans out per fixture instead. A Defender
exclusion on the runner's temp dir is the cheapest untried lever.
