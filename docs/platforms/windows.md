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
a different `out/` directory than the one the tests look in:

```sh
MSYSTEM=UCRT64 CHERE_INVOKING=1 /c/msys64/usr/bin/bash.exe -lc '. ./your-script.sh'
```

⚠️ **`uname -s` carries the OS build number** — `MINGW64_NT-10.0-26200-ARM64` — so `out/<platform>/`
is not a name anything outside that shell can reconstruct. The VS Code extension globs `out/*/` and
takes the newest rather than trying. Note also that UCRT64 and CLANGARM64 report the *same* `uname`,
so they share one `out/` directory and will clobber each other; pass `PLATFORM=` to separate them.

## Running things

`./dev` works as documented. The container legs (`./dev test san|wasm`) need Docker or podman and are
not usually available on a Windows box, so `./dev test` and `./dev check` are the local gate.

```sh
make -j4                    # ~100 s here
./run_tests.sh              # ~850 s; the cost is per-fixture C compilation, not the tests
./dev fixture <name>...     # the inner loop
```

If the suite looks slow, it is not hung. On a Windows runner the same suite takes ~1837 s against
~75 s in the Linux container, and the gap is process startup plus the C compile — **not** a network
or socket timeout, whatever an older note may have said. `net_addr_ctor` opens no socket at all and
cost the same 40 s as `net_refused`.

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
  variables**. `kama_native_path` in `tools/kama-bin.sh` exists for the second case.
- **git does not create real symlinks** without `core.symlinks` (needs Developer Mode or elevation);
  it writes a text file containing the target path instead. Do not commit symlinks.
- **A `.exe` suffix is load-bearing** in any path comparison against a running binary.

## Where the remaining work is

[ROADMAP.md](../ROADMAP.md) — §3 for the two guards still failing (`check-lsp`'s `@compileFor` section
and `check-packages`' free-rider check), and the known-issues list for the three items about what
shape a Windows *application* is: console subsystem, runtime linking, and which DLLs must ship.
