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
./run_tests.sh              # ~1819 s; the cost is per-fixture C compilation, not the tests
./dev fixture <name>...     # the inner loop
```

If the suite looks slow, it is not hung. It is ~1819 s here against ~75 s in the Linux container, and
what it is spending that on is process startup plus the C compile — **not** a network or socket
timeout, whatever an older note may have said. `net_addr_ctor` opens no socket at all and cost the
same 40 s as `net_refused`.

⚠️ Both halves of that comparison are of their own date, and only the first is current. This page read
`~906 s` from 2026-08-11 until 2026-09-06, when the same box measured `~1819 s` — a corpus that grew
from 972 assertions to 1583 over the same weeks, not a regression. Per-assertion the box got slightly
*faster*. Re-measure before reading any trend into these, and re-measure the container leg too: `~75 s`
is the older figure and has not been taken against today's corpus.

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

⚠️ **Measured 2026-09-06, and it is worse than one translation layer: this box is also a VM.**

```
PROCESSOR_IDENTIFIER = ARMv8 (64-bit) Family 8 Model 0 Revision 0, QEMU
Win32_Processor Name = virt-10.0                    (12 cores)
/ucrt64/bin/clang    = PE32+ … x86-64               Target: x86_64-w64-windows-gnu
```

So the `~1819 s` was taken through **two** layers — QEMU-virtualized ARM64, then x86_64 emulation on
top — across ~1525 `kama build` invocations per leg. **Do not read that number as a property of
Windows.** If you are optimizing here, this box is its own control and that is fine; but a lever whose
justification is only these numbers belongs on this page as local configuration, not baked into
`run_tests.sh` as a repo default.

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

So `out/$(uname -s)-$(uname -m)/` used to be the **same directory** for both, and there was no safe
way to keep both builds:

- **No `PLATFORM=`** → the second build silently overwrote the first *in place*. The harness then
  tested whichever compiler built last while reporting the platform of the other. This is the
  stale-binary trap that `./dev` exists to make unrepresentable, and here it was representable.
- **`PLATFORM=<name>`** → the build landed in `out/<name>/`, and the harness **never saw it**, because
  `tools/kama-bin.sh` resolved `$KAMA` from `uname` rather than from `PLATFORM`. The other
  environment's binary was still sitting at that path, so the `-x` test passed and the fallback was
  never reached. You built ARM64, and `./run_tests.sh` tested the stale x86_64 one without a word.

**FIXED.** Both the `Makefile`'s `PLATFORM ?=` and `tools/kama-bin.sh` now derive the name from
[`tools/platform.sh`](../../tools/platform.sh), which keys off **`$MSYSTEM`** on msys2 — the variable
that actually selects the toolchain, and one msys2 does pass to child processes. The two environments
now land in `out/UCRT64-x86_64/` and `out/CLANGARM64-x86_64/` and cannot collide.

⚠️ The derivation lives in **one** script on purpose. It was spelled twice — identically — which was
safe only for as long as both stayed the same one-liner; the moment one gained the `$MSYSTEM` arm and
the other did not, `make` would build into a directory the harness never looks in, which is the same
trap wearing different clothes.

Two residuals worth knowing:

- **The root `./kama` is still "whichever built last"** — `ln -s` degrades to a **copy** on msys2, so
  it is a snapshot, not a pointer. `kama-bin.sh` falls back to it when this platform has no build yet
  (that fallback is load-bearing for an installed tree, which has no `out/` at all), so switching to a
  not-yet-built environment gets you the other one's binary. Build first, or `export KAMA=<abs path>`,
  which `kama-bin.sh` honors and `tools/run-checks.sh` passes down to the guards.
- **`Makefile`'s `-static` rule still keys off `uname -s` matching `MINGW*`,** and should. That asks a
  different question — "are we on msys2 at all" — which is answered the same way in every environment.

`UCRT64` remains the one that matches CI, which is why everything on this page uses it.

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
- **cmd.exe's command line stops at 8,191 characters**, and every C compile and link reaches it (`system()`,
  and the `-j` pool's `cmd /c`). `CreateProcessW` takes 32,767 and clang takes `@file` of any length, so the
  limit is the shell's. A package with enough `csources` directories (`@kama/sodium`: 120 sources in 78, one
  `-I` each) answered *"The command line is too long."* and could not build here at all — filed by the first
  consumer as KB-27. Since `0.9.393` a command that would not fit moves the lists the DRIVER generated
  (includes, inputs, objects) into response files beside the objects; a command that fits is byte-for-byte
  what it was. `tools/check-long-command.sh` holds it down.
- **A text-mode `std::ofstream` writes `\r\n`.** kama's own `std::fs` opens everything binary, but the driver
  wrote `kama.lock`, the emitted C and the generated header in text mode, so a lock written here differed byte
  for byte from the same lock written anywhere else (KB-28, `0.9.392`). Every stream the driver opens is
  binary now; `tools/check-lf-output.sh` holds it down. ⚠️ And msys2's `grep` strips a CR at end of line
  before matching, so `grep -c $'\r'` counts ZERO on a CRLF file — count the bytes with `tr -cd '\r' | wc -c`.
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
- **`getaddrinfo` is a Winsock call and needs `WSAStartup`**, exactly like `socket`. Every other entry
  point in `kama_os.h` reaches it through a socket that already called `kama_net_init()`; the resolver
  does not, so it needs its own call. Without one, a resolve performed *before* the program's first
  `bind`/`connect` fails with `WSANOTINITIALISED` while the same call after one succeeds — and it fails as
  `HostUnreachable`, which reads like a network answer rather than a missing init. `tests/net_resolve`
  scored 111 of 127 on exactly that: the flag lost was the `resolve("localhost")` that ran before its
  listener bound.
- **`<iphlpapi.h>` defines `interface`, `hyper` and 19 more lowercase macros**, despite `WIN32_LEAN_AND_MEAN`:
  its chain reaches `<rpc.h>`/`<rpcndr.h>` (`#define interface struct`). A kama name spelled that way then breaks
  the C, and at `0.9.376` `UdpSocket.joinMulticastV4(interface:)` failed to compile EVERY program importing
  `std::net`. So `kama_os.h` declares `if_nametoindex` itself (`0.9.377`). Before adding a Windows SDK header to
  the seam, diff its `clang -dM -E` lowercase macros against the current set. Even without it, `<windef.h>` leaves
  `near`, `far`, `pascal` and `cdecl` defined, which is KR-67.
- **IPv6 on Windows, measured 2026-09-17 (`0.9.377`, `net_ipv6` 127, `net_udp_multicast` 31):**
  * `IPV6_V6ONLY` defaults ON. With the seam's `setsockopt(…, 0)` deleted, both dual-stack cases fail (127 → 109),
    so that line is load-bearing here, where Linux and macOS would pass without it.
  * `if_nametoindex` takes the NDIS name: `loopback_0` → 1, `ethernet_32769` → 4. The friendly
    `Loopback Pseudo-Interface 1` and a POSIX `lo` are `NotFound`.
  * V6 multicast DOES deliver on loopback, from a socket bound to `::` with the loopback index, as on macOS
    and unlike Linux's `lo`.
- **Binding a wildcard (`::`, `""`, `0.0.0.0`) raises a Windows Defender Firewall prompt** for every new
  executable, and each fixture is a fresh `.exe` in a fresh temp dir, so `net_ipv6` and `net_udp_multicast`
  prompt on every run. The dialog does not block the program, and Cancel is harmless: it refuses INBOUND
  traffic from the network, while the fixtures talk over loopback. To silence it on a dev VM, run in an admin
  shell: `Set-NetFirewallProfile -Profile Domain,Public,Private -NotifyOnListen False`. ⚠️ `False` is a
  `GpoBoolean` (`True`/`False`/`NotConfigured`), NOT PowerShell's `$false`, which fails to cast. Check with
  `Get-NetFirewallProfile -All | Select-Object Name, NotifyOnListen`.
- **`abort()` exits 127; it is not a death by signal.** So the POSIX `>= 128` trap predicate matches
  nothing here, and 127 is a value a program can also *return* — the runtime's own message on stderr is
  the discriminator. `run_tests.sh` (trap fixtures) and `tools/check-release-arith.sh` each carry this
  branch. A bare `__builtin_trap` *does* surface as `128 + SIGILL`, which is why the two are not one rule.
- **A binary that has just aborted stays locked**, so the next link over the same path dies with
  "cannot open output file: Permission denied" — the running-executable rule, extended by however long
  the OS or a crash reporter holds the image. Give each build its own output path rather than retrying.
- **There is no `<dlfcn.h>` and no `-ldl`.** mingw-w64 ships neither, so a POSIX `dlopen` host does not
  fail at load time — it fails to COMPILE, and a guard around it reports the feature as broken when what
  broke is the harness. `LoadLibraryA`/`GetProcAddress`/`FreeLibrary` are the same three operations
  (`tests/support/expose_shared_check.sh`).
- **A directory junction stores an ABSOLUTE path**, by definition of its reparse point. `kama pkg install`
  materializes `.kama/deps/<name>` with `mklink /J` because the relative alternative — a directory
  *symlink* — needs `SeCreateSymbolicLinkPrivilege` (Developer Mode or elevation), which an ordinary
  install cannot depend on. So a resolved dependency tree is **not relocatable here**, and
  `tools/check-packages.sh`'s KB-6 case asserts only what survives: the link is made, and the tree builds
  where it was resolved.
- **`system()` is cmd.exe, and cmd's `echo` does not strip quotes** the way `/bin/sh` does. That matters
  to any guard using `--cc "echo <compiler>"` to read a command line back: kama quotes every input path,
  so the identical, correct command reads `…app.c ` on POSIX and `…app.c" ` here. Match on a
  quote-stripped copy, or the ordering assertion fails on the platform whose quotes survived.
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
- **There is no AddressSanitizer, and no sanitizer runtime of any kind.** mingw-w64's `compiler-rt`
  package ships `builtins`, `profile`, and the three `fuzzer` archives — measured with `pacman -Fl`,
  asan files: **0** on all three of ucrt64 / mingw64 / clangarm64. It is not a package you forgot: the
  clang package does not even depend on `compiler-rt`, and adding it to the CI install list changes
  nothing. ⚠️ **clang accepts `-fsanitize=address` anyway and fails at the LINK**, so a whole project
  compiles before the first sign of trouble, and a compile-only probe reports the sanitizer as
  *available*. Anything probing for it must link:

  ```
  ld: cannot find .../libclang_rt.asan_dynamic.dll.a: No such file or directory
  ```

  This is why `tools/check-compiler-asan.sh` skips here rather than failing. The guard asserts memory
  safety in portable C++ that no Windows machine is needed to check, and the Linux and container legs
  assert it — but it went in without a probe, and the Windows leg was red for it.
- **git does not create real symlinks** without `core.symlinks` (needs Developer Mode or elevation);
  it writes a text file containing the target path instead. Do not commit symlinks.
- **A `.exe` suffix is load-bearing** in any path comparison against a running binary — and in any
  filename predicate a test writes. `find … -name app` matches a filename, not a stem, so a guard
  asserting that a build produced `app` reports "the output went somewhere else" when it went exactly
  where it should. `case "$(uname -s)" in MINGW*) EXE=".exe"` is the idiom (`tools/check-toolchain.sh`).
- ⚠️ **`opendir` on a `\\?\` path past `MAX_PATH` lists the WRONG DIRECTORY — silently.** mingw-w64's
  dirent returns a valid `DIR*` and then enumerates the **current working directory**; measured twice on
  a 357-character tree, which listed this repo's scratch files instead of the one file actually there.
  Unprefixed at that length it fails honestly, so adding the prefix turns a reportable error into a wrong
  answer. `FindFirstFileA`/`FindNextFileA` on the same tree is correct (3 entries). Nothing under
  `include/` or `src/` uses dirent on Windows any more — `std::fs` uses the `W` pair, and the compiler's
  `listDir` goes through `src/kama.winpath.cpp` — and this entry is why a future one must not either.
- **A UTF-8 `activeCodePage` manifest makes the NARROW API UTF-8, process-wide.** Windows 10 1903+.
  Measured: the same program given a `日本語` argument sees `63 63 63` (`???`) without it and the exact
  nine UTF-8 bytes with it — and `argv`, `fopen`, `_mkdir`, `stat`, `system()` and the `A` family all
  follow, with no call-site changes. It does **not** lift `MAX_PATH`: that is the `\\?\` prefix's job,
  and the prefix works with the narrow CRT too (`_mkdir` to 356, `fopen` to 365, with
  `LongPathsEnabled = 0`). `kama.exe` carries one (`src/kama.manifest`); `std::fs` is wide and needs none.
- ⚠️ **cmd.exe parses a batch FILE in the CONSOLE code page, not the process code page.** Measured: a
  UTF-8 `.bat` holding `type "…\日本語\x.txt"` prints the file under `chcp 65001` and says "The system
  cannot find the path specified" under `chcp 437` — which is what a console-less process (kama under
  msys2 bash, the LSP under an editor) gets by default. The identical text on cmd's COMMAND LINE works
  under both, because a command line arrives as UTF-16. So a manifest cannot rescue a generated `.bat`,
  and the `-j` pool hands its lines to `CreateProcessW` instead of writing them to disk.
- ⚠️ **GNU `ld` and `ar` are narrow programs: a non-ASCII path in their argv is `???` and "Invalid
  argument", input or output.** Measured on the UCRT64 toolchain (binutils 2.47) against a `日本語`
  directory: `clang -c` into it, out of it and with `-I` on it all work — clang is LLVM and reads its
  UTF-16 command line — while `ld` cannot find an object in it, cannot open an output in it, and `ar`
  fails both ways. The same tree through its **8.3 alias** (`6A7A~1`) links fine, which is what the
  compiler now hands them (`toolPath`). 8dot3 name creation is on by default on the system volume
  (`fsutil 8dot3name query C:`), where a user profile — the common non-ASCII directory — lives; a data
  volume may have it off, and there the linker fails as it always did.
- ⚠️ **cmd.exe's redirections stop at MAX_PATH.** `>"<300-char path>"` on a `cmd /c` line says "The system
  cannot find the path specified" even though the directory exists and the CRT with a `\\?\` prefix can
  write there. The 8.3 alias answers this one too: every component shrinks to eight characters.
- ⚠️ **A non-ASCII `%TEMP%` breaks clang's own one-step link, with no non-ASCII path on the command
  line.** `clang hello.c -o hello.exe` — every path ASCII — compiles to a temporary object under `%TEMP%`
  and hands THAT to `ld`, which cannot read it (`ld: cannot find …\???\hello-dbca23.o`). `-save-temps=obj`
  works, `-fuse-ld=lld` would, and a user whose Windows account name is non-ASCII has exactly this `%TEMP%`.
  It is the toolchain's, not kama's: kama's own intermediates go beside the output, which is why its
  per-TU `-j` path is unaffected; the single-invocation path (one TU, or `-j 1` with several) is not.

## Where the remaining work is

**The suite is green here: 1583 passed, 0 failed** (`./run_tests.sh`, ~1819 s — 1409 s of fixtures after
410 s of guards; measured 2026-09-06 at 0.9.209), and the `windows-test` CI leg is no longer
`continue-on-error`. Windows is a supported platform, not a
best-effort one, and a program kama builds here is distributable as it stands.

⚠️ **Building `kama.exe` needs `windres`** (binutils) since `0.9.219`, for the manifest. The msys2 clang
package depends on it transitively (clang → gcc → binutils), so the install line at the top of this page
is still complete; a toolchain assembled some other way fails the build loudly at the `.res.o` step.

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

- **The filesystem and process seam is UTF-16 at the edge — SHIPPED `0.9.217`.** Every path, command
  line, environment block and cwd that `include/kama_os.h` hands to Windows is converted UTF-8 → UTF-16
  immediately before a `W` call (`kama__wide` / `kama__wpath`), and a name coming back (`readDir`, the
  inherited environment) is converted the other way. No `A` function and no narrow CRT path call remains
  in that header. That is the [utf8everywhere.org](https://utf8everywhere.org/) prescription, which is
  kama's stance (`lib/std/path/path.kama:6-7`), and what Rust, Go, Zig, .NET and libuv all ship. Past 248
  characters a path is normalized by `GetFullPathNameW` and given the `\\?\` prefix, so the `MAX_PATH`
  ceiling is gone without asking the user for a registry setting. `tools/check-long-path.sh` and
  `tools/check-path-unicode.sh` hold both halves down; they were landed RED first (exit 2 and exit 12),
  which is the observation that they guard anything.

  ⚠️ One thing this did NOT close, and it is a **non-goal** rather than pending work:
  **`CreateProcessW`'s cwd and executable stay ≤ 260** — a Win32 limit, not a seam limit (below), so
  nothing kama does can lift it. `std::process` says what answers the need in `Command.cwd`'s own doc
  comment: hand a child absolute paths rather than a deep `cwd()`.

- **The compiler itself is UTF-8 and long-path clean — SHIPPED `0.9.219`.** The other side of the same
  boundary: `kama.exe`'s own argv, the manifests and sources it reads, the directories it lists for
  module discovery, the output directory it creates, the C it writes and the compiler command lines it
  runs. Held by `tools/check-compiler-path.sh`, a kama-spawns-kama guard (the shell cannot pass a
  non-ASCII argument — below) that builds and RUNS a two-module project under `日本語-Привет`, under a
  330-character directory, under both, and with a non-ASCII output name — and, since the junction fix
  below, installs and builds through a **path dependency** at a 327-character project, with a short-path
  control beside it. Landed RED first: exit 14,
  `kama: …\???-??????\kama.json does not exist`. Then it went RED four more times before it was green,
  each at the next ceiling — the record below is in that order. **None of it is "go wide"**:
  `<windows.h>` cannot enter the driver's TU (its token enum collides), and measured, it never needed to.

  * **Encoding is the manifest** (`src/kama.manifest`, embedded by `windres` via `src/kama.rc` in the
    Makefile's MINGW branch): the process ANSI code page is UTF-8, so the narrow CRT and every `A` call
    see kama's bytes with zero call-site changes. Windows 10 1903+; older keeps the old behaviour.
  * **Length is the `\\?\` prefix, applied at the OS edge only** — `osp()` in `kama.driver.cpp` wraps
    the argument of every `fopen`/`ifstream`/`stat`/`_mkdir`/`remove`/`rename`, and nothing stored or
    compared ever carries it. Same 248 threshold and `GetFullPathNameW` normalization as the runtime's
    `kama__wpath`, in `src/kama.winpath.cpp`, the one TU under `src/` that includes `<windows.h>`.
  * **Directory listing is `FindFirstFileW`**, because `opendir` on such a path lists the WRONG directory
    (below). `listDir` replaced all seven `opendir` loops.
  * **The `-j` pool no longer writes `.bat` files.** cmd.exe parses a batch FILE in the console code page
    (below), so the manifest could not have saved it; the line goes to `CreateProcessW` verbatim.
  * **The linker and the archiver get 8.3 aliases.** GNU `ld` and `ar` ANSI-deserializeJsonBuffer their argv and cannot
    open a non-ASCII path in either direction, and cmd.exe's `>out 2>err` redirections stop at MAX_PATH
    (both below); clang's own compile step has neither limit. `toolPath()` spells every object, the `-o`
    and the two redirections by the 8.3 alias of the longest existing prefix when the path is non-ASCII
    or 248+ characters, and byte-for-byte otherwise. A non-ASCII OUTPUT NAME has no alias yet (the file
    does not exist), so it is linked under an ASCII stand-in in the same directory and renamed into place.
  * **Two spellings of the verbatim prefix are stripped** — `\\?\`, which `GetFinalPathNameByHandle`
    always answers in (`absolutePath`), and `//?/`, which is what **msys2 hands a native child for a long
    POSIX argument** and what `cygpath -m` prints for one (measured: `kama build /tmp/<319 chars>/kama.json`
    arrived as `//?/C:/msys64/tmp/…`; `cliPath()` strips it at the operand). The kernel recognizes neither
    with forward slashes inside, and a lexical join collapses the second to `/?/`, which names nothing.
  * ⚠️ **The DEPENDENCY LINK is made with `FSCTL_SET_REPARSE_POINT`, not `cmd /c mklink /J`** — fixed
    `0.9.303`, and the one place the reasoning above was applied and turned out not to hold. `kama pkg
    install` materializes `.kama/deps/<name>`, a junction on Windows, and this call had been reasoned safe
    on the grounds that `osp()` wraps it "like everything else". It does not: **`osp()` applies at the
    Win32 file-call edge and can do nothing for a command line handed to a shell**, and cmd.exe is
    MAX_PATH-bound however the path is spelled. A path dependency under a 265-character project failed
    with `The system cannot find the path specified.` / `kama install: cannot link dependency`, while the
    byte-identical project at a short path linked fine. `kama_win_make_junction` (`kama.winpath.cpp`) now
    does it with `CreateDirectoryW` + `DeviceIoControl`, taking the verbatim spelling and spawning no
    process; replacement is `RemoveDirectoryW`, which on a junction removes the LINK and never the
    target's contents, and on a real non-empty directory fails, which is the outcome wanted.

    ⚠️ **Two alternatives rejected, so neither is re-proposed.** A directory **symlink** needs
    `SeCreateSymbolicLinkPrivilege` (Developer Mode or elevation), which an ordinary `pkg install` cannot
    require — that is why a junction was chosen originally, and "just use a symlink" is not the answer.
    The **8.3 alias** (`kama_win_shortpath`, the trick `toolPath()` uses for `ld`/`ar`) is wrong *here*
    specifically: a junction STORES its target, so the reparse point would permanently hold
    `C:\MSYS64~1\…` and every later "which package owns this path?" comparison would see the alias — and
    8dot3 creation can be disabled per volume, where that helper returns its input unchanged and the fix
    would silently not apply. `DeviceIoControl` has neither problem.

  What is deliberately NOT covered, each a settled verdict rather than pending work: starting an
  executable past MAX_PATH (`CreateProcessW`, below — the guard runs its deep cases from a second build
  into a short directory); a volume with 8dot3 names disabled, where the linker fails exactly as it did
  before; and a non-ASCII `%TEMP%`, which breaks clang's own single-invocation link with no kama path
  involved at all (below). Two more are **genuinely optional** rather than non-goals, and each is a small
  known edit if it is ever reported: `selfExePath` and `relativizeToCwd` keep 260-byte buffers
  (`_get_pgmptr` / `getcwd` into `PATH_MAX`) — a process cannot *have* a cwd past 260 without the registry
  opt-in kama does not ask for, and the second is cosmetic (`kama: built .`); the first would be one
  `GetModuleFileNameW` sizing loop in `src/kama.winpath.cpp`. And `longPathAware` in the manifest is a
  **non-goal**: it does nothing unless the machine's `LongPathsEnabled` registry flag is set, which the
  `\\?\` prefix makes unnecessary, and a behaviour that switches on a setting nobody is asked to change is
  exactly the implicit path [GOALS.md](../GOALS.md) rejects.

  **`args()`, `env()` and `programPath()` followed in `0.9.218`** (`include/kama_runtime.h`): the CRT's
  `main` argv, `getenv` and `_get_pgmptr` are the ANSI re-encodings of the process's UTF-16 command line,
  environment and image path, so the runtime re-reads the command line through `GetCommandLineW` and splits
  it itself (`kama__cmdline_split`), and the other two go through `GetEnvironmentVariableW` / `GetModuleFileNameW`
  (**since `0.9.385` the split runs on the FIRST `args()`/`programName()`, not at startup, and not through
  `CommandLineToArgvW`** — that one allocated before `main`, through the funnel AND through `LocalAlloc`, so a
  declared `@globalAllocator` lost two slots to it and a `--no-heap` program allocated at startup; KR-73.
  `tools/check-winargv.sh` diffs the splitter against `CommandLineToArgvW` on 31 cases, Windows only.)
  (⚠️ not `_wget_pgmptr`: it is declared in `<stdlib.h>` and absent from mingw-w64's UCRT import
  library, so it fails at LINK — after the whole program compiled). ⚠️ No
  bash guard can witness this half: msys2 converts a native child's arguments through the ANSI code page
  before kama sees them (the two path guards keep their names out of argv for that reason). It was
  verified by hand from PowerShell — a native launch passes UTF-16 argv — with a program that prints the
  bytes of `args().get(at: 0)`, `env(name: …)` and `programPath()` given `日本語`, `Привет😀` and a
  directory named `日本語`.

  ⚠️ **`<wchar.h>` is not includable from `kama_os.h`.** It pulls in `<stdio.h>`'s `stdout` macro, which
  breaks every emitted function with a parameter of that name (`std::process::Output.make` has one). `wcslen`
  is reachable through `<string.h>` on UCRT, and that is all the seam needs.

  The measurements the design rests on, probed here 2026-09-06 (msys2 UCRT64, clang 22.1.8,
  `LongPathsEnabled = 0`), with a negative control on every case — every unprefixed `_w*` call past 260
  failed, as it must for the rest to mean anything:

  * `CreateFileW` / `CreateDirectoryW` / `DeleteFileW` / `GetFileAttributesW` / `FindFirstFileW` all
    work past `MAX_PATH` with a `\\?\` prefix **while the registry flag is 0** — so the prefix is
    load-bearing and the registry setting is not something a user has to be told to change.
  * `GetFullPathNameW` does `/`→`\`, collapses `.`/`..` and doubled separators, tolerates a trailing
    `*`, passes an existing `\\?\` through untouched, leaves `\\host\share\x` unprefixed, and fails on
    `""`.
  * ⚠️ **Win32 already collapses `..` lexically**, even across a junction: with `j -> outer\real`,
    `CreateFileW("j\..\sib")` finds the sibling next to `j`, not next to `real`. So normalizing with
    `GetFullPathNameW` before prefixing makes existing behaviour visible rather than changing it —
    which is the claim the whole design rests on.
  * ⚠️ **The `_w*` CRT family *does* honour `\\?\` here** (`_wopen`, `_wstat64`, `_wmkdir` probed;
    `_wunlink`, `_wrmdir` and `MoveFileExW` proven by the long-path guard's rename and teardown steps).
    The shipped seam keeps the CRT for the file calls for exactly that reason: the CRT sets `errno`
    itself, so `kama_last_error()` stays the one error channel with no `GetLastError` mapping. Only what
    has no CRT spelling (`Find*W`, `GetFileAttributesW`, `MoveFileExW`, `CreateProcessW`) is raw Win32.
  * ⚠️ **`\\?\NUL` resolves.** The common claim that verbatim prefixing kills reserved device names is
    **false** here — it reaches the NT object-manager entry. Do not use it as an argument.
  * ⚠️ **`CreateProcessW`'s `lpCurrentDirectory` fails past 260 either way** (`GLE=267`,
    `ERROR_DIRECTORY_INVALID`), prefixed *and* unprefixed. Going wide buys `std::process` full UTF-8
    correctness for the program, its arguments, its environment and its cwd; it does **not** buy
    long-path support for the cwd or the executable. That is a Win32 limit, not a seam limit.

## The suite's wall clock, and where the time actually goes

The suite is **~1356 s** here (2026-09-12, `0.9.309`) against ~75 s in the Linux container — read the
vintage note under [Running things](#running-things) before comparing those two. It was ~1819 s on
2026-09-06; the difference is a guard-side campaign that is now finished, and what remains is inherent.

**The phase breakdown is the thing to read before optimizing anything**, because it has repeatedly been
guessed wrong:

| phase | wall | |
| --- | --- | --- |
| single-file fixtures | 437 s | one `kama` + one `clang` each |
| xfail fixtures | 338 s | same |
| `check-*.sh` guards | 322 s | 67 guards in parallel |
| analysis agreement | 51 s | |
| multi-file fixtures | 16 s | |

So **the fixture phase is ~62% and the guards ~24%.** `kama build -j` parallelizes, but `run_tests.sh`
deliberately pins `KAMA_BUILD_JOBS=1` and fans out per fixture instead, which is correct under a
saturating fan-out. **Any future suite-speed work has to target the fixture phase** — the guard side is
spent (below). A Defender exclusion on the runner's temp dir remains the cheapest untried lever, and it
is machine configuration rather than anything the repo can ship.

### The guard-spawn campaign — finished, with numbers worth keeping

msys2 has no real `fork` and emulates it, which makes a process spawn pathologically expensive here and
made three guards spend most of their time on string handling a shell builtin does for free. Measured
idle on this box, and these ratios are **structural — portable Windows facts, unlike the seconds**:

| operation (×100) | cost | per call |
| --- | --- | --- |
| `printf \| wc -c \| tr` | 15468 ms | 154.7 ms |
| `${#var}` | 46 ms | 0.46 ms — **~336×** |
| `$(dirname …)` | 4964 ms | 49.6 ms |
| `${p%/*}` | 48 ms | 0.48 ms — **~103×** |

`check-doc-claims` (495.8 s → out of the slowest-8), `check-query` (147 s → 89 s alone; 239 s → ~167 s
in phase) and `check-lsp` (46 s → 12 s alone; 161 s → out of the slowest-8) were rewritten fork-free,
each proven byte-identical on the happy *and* failure paths. ⚠️ **A guard costs far more inside the
parallel phase than alone** (`check-lsp` 46 s → 161.5 s), because process creation is what contends —
so removing spawns helps more than an isolated measurement predicts.

⚠️ **Three substitutions that look obvious and are wrong** — each cost real time, and each is now
commented at its call site:
- **`${#var}` is not a drop-in for `printf | wc -c`.** `Content-Length` is a BYTE count and `${#}` is
  locale-aware in bash; 6 of `check-lsp`'s 116 frames carry non-ASCII, where it reports short and
  desyncs the stream. Toggle `LC_ALL=C` around the expansion (bash re-runs `setlocale` on an `LC_*`
  assignment).
- **`${p%/*}` is not a drop-in for `dirname`.** A slash-free path comes back unchanged, so a
  walk-to-root loop never terminates; `/a` yields `""` rather than `/`.
- **`case "$out" in *"$want"*)` must keep the variable QUOTED**, or `[`, `*` and `?` become globs. In
  `check-query`'s `reject` that would report a prelude leak as clean.

### ⚠️ This box cannot resolve a small wall-clock effect

Measured 2026-09-12 with `./dev check`, 68 guards, 0 failed every run: 408 s before the sweep, 346 s
after — and a **repeat of the identical post-change configuration gave 268 s**. Run-to-run spread (78 s)
exceeds the effect being measured (62 s). A tell in the same data: `check-packages` "improved" ~20 s
across those runs *without being touched*.

So **per-guard numbers reproduce and phase totals do not** — `check-query` read 168.2 s and 166.7 s on
the two post-change runs, 1% apart. Quote per-guard figures; treat any phase-wall claim as needing
interleaved A/B/A/B over several runs, or a quieter machine. This is the same caveat as the QEMU/emulation
note above, sharpened into a number.

**What is deliberately NOT being pursued**, so it is not re-proposed as unexamined: `check-packages`
(~175 s) and `check-manifest` (~120 s) are now the top guards and are **not** spawn-bound — 103 real
`kama` invocations and 67 full build+link+run respectively, at ~2.8-2.9 s each, with six text-processing
spawns in 1,185 lines and ~67 in 789. Their only lever is *fewer compiler invocations per guard*
(sharing one published registry, `kama check` where no artifact is asserted), and the arithmetic caps
that at ~147 s summed ≈ 20-30 s of suite wall — about **2%**. Genuinely optional until the guard phase
is the bound again, which it is not.
