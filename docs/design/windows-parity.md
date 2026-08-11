# Windows parity — close-out campaign (live tracker)

*In-flight campaign doc. **Delete this file when the last item lands**, once ROADMAP + SPEC +
`platforms/windows.md` carry the record. Its deletion is the signal that Windows is done and the
branch can be pushed.*

## Cold start — read this first

**Windows is green.** `./run_tests.sh` on real hardware (msys2/UCRT64, ARM64 host, x86_64 toolchain):
**972 passed, 0 failed**, 906 s wall. It was 970/2 at the start of this campaign and 923/48 before
the triage that preceded it.

**One item is left: #4, static runtime linking.** It is a gap, not a regression — nothing is broken,
a threaded program simply is not distributable off this machine yet. The tree is pushable as it
stands; #4 is the last thing that needs *this* machine.

The invocation that works, from outside msys2 (this cost a session once — see
[../platforms/windows.md](../platforms/windows.md), which now explains why both halves are needed):

```sh
MSYSTEM=UCRT64 /c/msys64/usr/bin/bash.exe -lc 'pushd /c/Users/matt/Documents/kama >/dev/null; ./run_tests.sh'
```

A `tools/check-*.sh` run by absolute path needs no `pushd` — it resolves `ROOT` from `$0`. `./dev`
works as documented; container legs (`./dev test san|wasm`) need Docker and are not available here.

**Read [../platforms/windows.md](../platforms/windows.md) before touching anything.** Its "things
that are true on Windows and nowhere else" list is now nine entries, three of them added by this
campaign, and every one of them was a wrong diagnosis first.

| # | Item | Status |
|---|------|--------|
| 1 | `check-lsp` — the `@compileFor` manifest-flag assertions | **done** — `f47d5a2` |
| 2 | `check-packages` — the free-rider check fires on Windows | **done** — `96f688b`, `ce61944` |
| 3 | Drop `continue-on-error` from the `windows-test` job | **done** — `0daa00e`, plus one GitHub setting |
| 4 | Static runtime linking for USER programs | ◄ **the only thing left** |

The literal initializer/comparison asymmetry was fifth in the original order and **has left this
campaign**. It is platform-independent, it was only ever here because this is where it was found, and
a Windows test cycle is ~906 s against ~75 s in the container. It lives in
[../ROADMAP.md](../ROADMAP.md)'s known-issues list, with a warning that its recorded cause does not
survive arithmetic. Do it on the Mac.

**The house rule earned its keep three times today.** Item 1 was recorded as 6 assertions and was 12.
Item 2's hypothesis was right but hid a second failure behind it. The invocation this page opens with
was wrong in `platforms/windows.md` until someone ran it. **Items 4 and 5 below are read, not run** —
item 5 in particular records a cause that does not survive first contact with the existing fixture.

**Item 4 is read, not run.** Its source citations below were verified by reading; the DLL dependency
it describes has not been reproduced on this hardware. Reproducing it is step one — see its section.

---

## 1. `check-lsp` — 12 assertions in the `@compileFor` manifest-flag section

**Symptom.** The server does not pick up the project manifest's flags, so the editor and the CLI
disagree about which declarations a build keeps: `lsp: [always onlyWithoutA] cli: [always onlyWithA]`.
The other 32 assertions that were failing in this guard are fixed; this is the residue.

**Cause, read from the source — a guard bug, not a server bug.** `tools/check-lsp.sh:137` defines
`furi()` for exactly this hazard: under msys2 it spells a path `file:///C:/…` through `cygpath -m`,
because a raw `/c/Users/…` handed to a native `kama` resolves against the *current drive* as
`C:\c\Users\…`. Every URI in the guard that names a real file on disk goes through it — `IURI`,
`WWURI`, `OURI`, `DURI`, `CIURI`, `FRURI`.

The M6 A1 build-configuration sessions do not:

- `tools/check-lsp.sh:821,823,824` — `cfgsession()` interpolates `file://$cfgroot` / `file://$cfgfile` raw
- `tools/check-lsp.sh:908,910,911` — the `cfgG` reconfigure session
- `tools/check-lsp.sh:930` — the `cfgH` dynamic-watcher session

`uriToPath` (`src/kama.lsp.cpp:208`) strips the leading slash only when a drive letter follows, so
the server keeps `/c/Users/…` and never finds the project's `kama.json` — it falls back to default
flags, which is the reported symptom exactly. The `didOpen` text is inlined, so the buffer still
parses; that is why only the manifest-flag assertions fail and the rest of the section passes.

**Confirmed by measurement, then fixed — `f47d5a2`.** One probe session sent both spellings to the
same server. Only the `cygpath -m` one logged
`config: C:/…/tests/query/cfg/kama.json | target … | strict | flags: … FEATURE_A …` and returned
`onlyWithA`; the raw one logged `(no kama.json — permissive defaults)` and returned `onlyWithoutA`.

Two corrections to what the ROADMAP recorded:

- It was **12 assertions, not 6** — the A1 section plus one in M6 C1 (`kama/buildConfig`, a project's
  own select group).
- Two assertion **labels** carried unescaped backticks inside double quotes, so the shell ran
  `Color::` and `collections` as commands on *every* platform and pasted the empty result into the
  message you would read when that assertion broke. Fixed in the same commit.

---

## 2. `check-packages` — a free-rider check fires on Windows that should not

**Symptom.** Case 35: `kama run` exits 1 where the guard expects 2, and the output carries the
"does not declare it" diagnostic that a *fetched* package must never provoke — its sources live in
the content-addressed store, where its manifest is not the user's to edit and editing it would break
the tree hash that names its store entry.

**The junction hypothesis, now corroborated by the source but NOT yet by a run.** The chain:

- `src/kama.driver.cpp:2869-2881` — `linkDir()` materializes the dependency view with `mklink /J`
  (a **junction**) on Windows, `symlink()` on POSIX.
- `src/kama.driver.cpp:136-155` — `absolutePath()` is `realpath()` on POSIX, which **resolves**
  links, and `_fullpath()` on Windows, which does **not** resolve junctions.
- `src/kama.driver.cpp:820-841` — `owningPackageDir()` walks up from `absolutePath(fromDir)`.
- `src/kama.driver.cpp:976-979` — the free-ride check clears `owner` when it is under `storeRoot`
  (`:873`); that clearing is the whole of a fetched package's exemption.

So on POSIX a fetched dep's file resolves into `$KAMA_STORE/<name>-<hash>/`, `owner` is cleared, and
`kama run` reaches the program (exit 2). On Windows `owner` stays `<app>/.kama/deps/<name>`, which is
not under `storeRoot`, the strict check fires, and the run dies at exit 1.

**Confirmed by measurement — both halves, separately.**

1. The guard's own failure output names `owner` outright:
   `…/app/.kama/deps/geodep/kama.json does not declare it` — so the walk stopped at the *view* entry,
   not the store.
2. A standalone C probe against a hand-built junction:
   `_fullpath` → `C:\…\app\.kama\deps\geodep`;
   `GetFinalPathNameByHandleA` → `\\?\C:\…\store\geodep-deadbeef`.

**Fixed.** `absolutePath()`'s Windows branch now opens a handle (`FILE_FLAG_BACKUP_SEMANTICS`, access
0 — no lock, no read rights needed) and asks `GetFinalPathNameByHandle`, stripping the `\\?\` /
`\\?\UNC\` prefix; a path that cannot be opened, such as a `-o` output that does not exist yet, falls
through to `_fullpath` exactly as POSIX falls back when `realpath` fails. **Windows-only** — the
change is inside `#ifdef _WIN32`, so it is not the all-three-platforms change the ROADMAP feared.
It also strengthens the `collectProjectDirs` cycle break, which keyed on an unresolved spelling.

**A second, pre-existing failure came out from behind it.** Case 35 `exit 1`s, so cases 36–38 had
never run on Windows at all. Case 38 asserts `find "$out/app/out" -name app -type f` — and Windows
builds `app.exe`. `find -name` matches a filename, not a stem, so a build that landed exactly where
it should reported "did not use the project's out/ root". Fixed with the `EXE=".exe"` idiom
`tools/check-toolchain.sh` already uses. **This was exposed, not introduced.**

Two un-normalized `readlink` sites survive in the guard (`tools/check-packages.sh:490`, `:676`).
They pass today; harden them only if a later session has budget.

---

## 3. Drop `continue-on-error` — done

`continue-on-error: true` is gone from the `windows-test` job in `.github/workflows/ci.yml`, so a
Windows-only regression now fails the run. `release.yml`'s Windows job is deliberately untouched —
a release must not be blocked by it, which is the macos-13 lesson its own comment records.

> ### ⚠️ One thing is left, and it is not in this repo
>
> Failing the *run* is not the same as blocking a *merge*. "Required check" is a branch-protection
> setting on GitHub, so the workflow file cannot assert it. Add **`build + test (windows-x64)`** to
> the required-checks list for `dev` and `main`. That string is the job's `name:`, which **changed**
> with this commit — it used to carry a `, best-effort` suffix.

---

## 4. Static runtime linking for USER programs

### Step 0 — reproduce it. This has been READ, not RUN.

Nothing below has been observed on this hardware. Before designing anything, build a threaded fixture
and look at what it actually links:

```sh
MSYSTEM=UCRT64 /c/msys64/usr/bin/bash.exe -lc 'pushd /c/Users/matt/Documents/kama >/dev/null;
  ./kama build tests/<a fixture using isolate/parfor/channel>.kama -o /tmp/t.exe &&
  objdump -p /tmp/t.exe | grep "DLL Name"'
```

Two things to establish, because they change the size of the job:

1. **Does `libwinpthread-1.dll` actually appear?** The host toolchain here is
   `mingw-w64-ucrt-x86_64-clang`. If it links winpthread statically already, the reproduction needs a
   different trigger (or the gap is narrower than recorded).
2. **What else is in the list besides system DLLs?** `libc++`/`libunwind`/`libgcc_s_seh-1` would each
   widen the fix. The compiler's own `Makefile` needed plain `-static` to sweep all of them.

The decisive test is not `objdump` but **running the `.exe` in a plain `cmd.exe` with msys2 off
`PATH`** — that is where the original `STATUS_DLL_NOT_FOUND` lived, and it is the only check that
proves the artifact is distributable.

### The gap, as recorded

`src/kama.driver.cpp:7270-7278` emits `-lpthread` for every non-wasm target; on mingw-w64 that
resolves to `libwinpthread-1.dll` out of the msys2 tree. Any program using `isolate` / `parfor` /
`channel` should therefore die with `STATUS_DLL_NOT_FOUND` on a machine without msys2. This is the
largest remaining gap for anyone actually shipping kama on Windows — a game engine is not
distributable until it is closed.

`Makefile:148-159` already solved precisely this for the compiler's own binary (`LDFLAGS += -static`)
and its comment is the best statement of the problem in the repo. There is **no `-static` anywhere in
the driver** today.

⚠️ **Line numbers in this section were re-derived after `ce61944`**, which added ~39 lines near the
top of `kama.driver.cpp`. Anything quoted from an older note will be off by that much.

### The default

**Decided with the user (2026-08-11): static by default, with a real opt-out.** kama is a language,
and a language does not get to YAGNI its way out of a user who legitimately wants the DLL. The
default goes in the link tail beside the existing `-lws2_32` (`kama.driver.cpp:7293`), gated on
`g_target.isWindows()` — the *target*, so a cross-compile gets it too, and only Windows, because
`-static` on Linux would statically link glibc, which is not the intent. The rule is "link non-system
runtime statically, system components dynamically", a no-op where libc *is* the system.

### The opt-out spelling — this is the design work

`--shared` is taken; it selects a shared-library *output*, an orthogonal axis. **Survey prior art
before choosing.** The close analogues, and they disagree with each other, which is the point:

| | spelling | what it says about the model |
|---|---|---|
| Rust | `-C target-feature=+crt-static` / `-crt-static` | linkage is a property of the **target**, toggled per-target |
| Zig | `-static` / `-dynamic` | linkage is a **build mode**, one axis, both directions named |
| CMake | `MSVC_RUNTIME_LIBRARY`, `BUILD_SHARED_LIBS` | linkage is a **per-artifact property** |
| Go | `CGO_ENABLED=0`, `-linkmode` | linkage falls out of a **toolchain** choice |

Two real seams exist in the driver:

1. **A `TargetSpec` field** — the starting recommendation. `TargetSpec` (`kama.driver.cpp:1317`)
   already carries `cc`, `ar`, `sysroot`, `cflags`, `ldflags`: toolchain properties, which is what
   runtime linkage is, and it matches Rust's model. Needs the field, a `kama.json` target key parsed
   beside `cflags` (`:1999`) and merged (`:1509`), a CLI flag, and a `docs/targets.md` entry next to
   the `cflags` guidance (`docs/targets.md:232`).
2. **A built-in select group**, following `OUTPUT` (`seedBuiltinSelectGroups`, `:1454`; `--shared` is
   sugar for `--select OUTPUT=SHARED`, resolved at `:6409`). ⚠️ **Known trap, verified:** the flag
   namespace is *flat* — a group's values land in `g_activeFlags` as bare strings (`:2608`, `:2627`,
   `:2686`), so a `RUNTIME` group valued `STATIC`/`DYNAMIC` **collides with `OUTPUT=STATIC`**. It
   would need non-colliding value names, and it would expose linkage to `@compileFor`, where source
   has no business branching on it.

Either way `g_target.ldflags` is already appended last (`:7295`) and stays the escape hatch of last
resort. Note it cannot cleanly *undo* a `-static` that the driver already emitted — which is part of
why the opt-out needs to be a real switch rather than "put it in ldflags".

### Verify

- `objdump -p out.exe | grep 'DLL Name'` shows only system DLLs.
- **Run the `.exe` in a plain `cmd.exe` with msys2 off `PATH`.** This is the one that matters; the
  original failure was `STATUS_DLL_NOT_FOUND` at process start, which no build-time check catches.
- A fixture for the opt-out, and one for the default.
- Nothing regressed on macOS/Linux — the gate is `g_target.isWindows()`, so it should be inert there,
  but `./dev test` on the Mac is what proves it.

### When it lands

Delete the `docs/ROADMAP.md` known-issues entry "A threaded kama program is not standalone on
Windows". The **console-subsystem** entry beside it (`-mwindows`, every emitted binary is CONSOLE
subsystem) is a sibling and **stays** — it is the same question, "what shape is a Windows
application", but it is not this item and has its own unresolved design call.

Then this campaign is over: close out below.

---

## Close-out

0. **Not in this repo:** required status checks are a GitHub branch-protection / ruleset setting,
   server-side — no file here can assert them. If `dev`/`main` use required checks, the entry needs
   to be **`build + test (windows-x64)`**; the job's `name:` lost its `, best-effort` suffix in
   `0daa00e`. If they do not use required checks, item 3 is already complete as it stands: the run
   goes red, which is the whole of what the workflow file can do.
1. `./run_tests.sh` on Windows (`./dev matrix` also wants the container legs, which need Docker and
   are not available on this box — `./dev test` + `./dev check` is the local gate here).
2. Delete this file.
3. Shrink `docs/ROADMAP.md` §3's Windows entry to whatever genuinely remains — the wall-clock note
   and the long-path / console-subsystem known issues, at minimum.
4. Re-check the closing pointer in `docs/platforms/windows.md`. It no longer names the two fixed
   guards, but it still names runtime linking; that line goes when item 4 does.
5. `./dev test` on the Mac before switching back, because item 4 touches the shared link tail.

**The tree is already pushable** — 972/0, clean, nothing half-applied. Deleting this file is the
signal that Windows is *finished*, not the signal that it is safe to push.
