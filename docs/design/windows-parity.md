# Windows parity — close-out campaign (live tracker)

*In-flight campaign doc. **Delete this file when the last item lands**, once ROADMAP + SPEC +
`platforms/windows.md` carry the record. Its deletion is the signal that Windows is done and the
branch can be pushed.*

Where things stood at the start (2026-08-11, msys2/UCRT64, real hardware): **970 passed, 2 failed**,
up from 923/48, with **no fixture failures left**. The `windows-test` leg is still
`continue-on-error`. Environment and gotchas: [../platforms/windows.md](../platforms/windows.md) —
read it first, every session.

**The order is load-bearing.** Item 3 flips CI to required; doing it before 1 and 2 means the first
*required* run fails on failures that were already known.

| # | Item | Status |
|---|------|--------|
| 1 | `check-lsp` — the `@compileFor` manifest-flag assertions | not started |
| 2 | `check-packages` — the free-rider check fires on Windows | not started |
| 3 | Drop `continue-on-error` from the `windows-test` job | blocked on 1 + 2 |
| 4 | Static runtime linking for USER programs | not started |
| 5 | The literal initializer/comparison asymmetry | not started |

**The house rule applies to every line below.** These sections quote source, and the source was read
— but only item 1's cause is pinned to a mechanism that fully explains the symptom. Items 2 and 5
say so in their own words and require a *run* before a fix.

---

## 1. `check-lsp` — 6 assertions in the `@compileFor` manifest-flag section

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

**Fix.** Route those six interpolations through `furi`. `cfgsession` reassigns the global `session`
variable that `frame()` appends to — keep that intact. No-op on POSIX, where `furi` is the identity.

**If that is not it.** The fallback hypothesis is server-side manifest discovery keying off `rootUri`
rather than the document path. Instrument `uriToPath` / `owningPackageDir` before touching the
server, and record which it was here.

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

**⚠️ Confirm before fixing.** The cheapest probe is running case 35's fixture by hand with `owner`
and `storeRoot` printed from a scratch build. The claim to falsify is specifically *"`_fullpath`
leaves the junction unresolved"*.

**Fix, narrowest first.** Preferred: give the Windows branch of `absolutePath()` the contract the
POSIX branch already documents, resolving through `GetFinalPathNameByHandleW(…, VOLUME_NAME_DOS)`
and stripping the `\\?\` prefix, keeping the existing as-given fallback for a path that does not
exist yet. That is a **Windows-only** change that makes the two branches agree — not the
all-three-platforms change the ROADMAP entry feared. Two things to watch:

- It also strengthens the `collectProjectDirs` cycle break (`:433`), which today keys on an
  unresolved spelling — two names for one directory look distinct.
- It must **not** leak into `joinPathLexical` (`:186-190`), whose comment explains why a path dep has
  to keep reaching its package *through* the view link.

Fallback if the broad change destabilizes the suite: resolve only inside `owningPackageDir`, or test
store membership against the *view* prefix instead of the store prefix.

**Verify.** The guard alone, then the full `./run_tests.sh` on Windows (~850 s) — `absolutePath` is
used everywhere, so the whole suite is the regression test — then macOS/Linux unchanged.

Two un-normalized `readlink` sites survive in the guard (`tools/check-packages.sh:490`, `:676`).
They pass today; harden them only if the session has budget.

---

## 3. Drop `continue-on-error` — only after 1 and 2 are green

Remove `continue-on-error: true` from the `windows-test` job at `.github/workflows/ci.yml:66` and
rewrite the comment above it, which currently says "flip to required once reliably green", to record
that it now is.

**Leave `.github/workflows/release.yml` alone.** Its Windows job (`:101-107`) is deliberately
best-effort and its comment records the macos-13 lesson.

---

## 4. Static runtime linking for USER programs

**The gap.** `src/kama.driver.cpp:7241-7251` emits `-lpthread` for every non-wasm target; on
mingw-w64 that is `libwinpthread-1.dll` out of the msys2 tree. Any program using `isolate` /
`parfor` / `channel` dies with `STATUS_DLL_NOT_FOUND` on a machine without msys2. This is the
largest remaining gap for anyone actually shipping kama on Windows — a game engine is not
distributable until it is closed.

`Makefile:148-159` already solved precisely this for the compiler's own binary (`LDFLAGS += -static`)
and its comment is the best statement of the problem in the repo. There is **no `-static` anywhere in
the driver** today.

**Decided with the user (2026-08-11): default static, with a real opt-out.** kama is a language, and
a language does not get to YAGNI its way out of a user who legitimately wants the DLL. The default
goes in the link tail beside the existing `-lws2_32` (`:7256`), gated on `g_target.isWindows()` —
the *target*, so a cross-compile gets it too, and only Windows, because `-static` on Linux would
statically link glibc, which is not the intent. The rule being applied is "link non-system runtime
statically, system components dynamically", a no-op where libc *is* the system.

**The opt-out spelling is the design work.** `--shared` is taken (it selects a shared-library
*output*). Survey prior art first — the close analogues are Rust's `-C target-feature=+crt-static`
(a *target* property), Zig's `-static` / `-dynamic`, and CMake's `MSVC_RUNTIME_LIBRARY`. Two real
seams exist in the driver:

1. **A `TargetSpec` field.** `TargetSpec` (`:1280-1294`) already carries `cc`, `ar`, `sysroot`,
   `cflags`, `ldflags` — toolchain properties, which is what runtime linkage is. Needs the field, a
   `kama.json` target key parsed beside `cflags` (`:1961-1962`) and merged (`:1471-1472`), a CLI
   flag, and a `docs/targets.md` entry next to the `cflags` guidance (`:232`).
2. **A built-in select group**, following `OUTPUT` (`:1424-1429`; `--shared` is sugar for
   `--select OUTPUT=SHARED`, resolved at `:6370-6377`). ⚠️ **Known trap:** the flag namespace is
   *flat* — a group's values land in `g_activeFlags` as bare strings (`:2571`, `:2590`, `:2649`), so
   a `RUNTIME` group valued `STATIC`/`DYNAMIC` **collides with `OUTPUT=STATIC`**. It would need
   non-colliding value names, and it would expose linkage to `@compileFor`, where source has no
   business branching on it.

Either way `g_target.ldflags` is already appended last (`:7257-7259`) and stays the escape hatch of
last resort.

**Verify.** Build a threaded fixture, then `objdump -p | grep 'DLL Name'` (or `dumpbin /dependents`)
and confirm only system DLLs remain. The decisive test is running the `.exe` **outside** the msys2
shell — a plain `cmd.exe` with msys2 off `PATH` — because that is where the original failure lived.
Add a fixture for the opt-out. Then delete the known-issues entry at `docs/ROADMAP.md` ("A threaded
kama program is not standalone on Windows"). The console-subsystem entry beside it is a **sibling,
not part of this**, and stays.

---

## 5. The literal initializer/comparison asymmetry

Platform-independent and unrelated to Windows; last only because nothing is blocked on it. The
known-issues entry reports `uint32 x = 2147483648;` then `x != 2147483648` comparing **unequal**.

**Reproduce before theorizing.** The recorded cause — the comparison's literal narrowing through
`Int32Node` (`src/kama.y:485-490`, where every unsuffixed literal is unconditionally an `Int32Node`,
plus the duplicate production at `:535`) — does not by itself explain the symptom:
`tests/int_literal_wide.kama` already proves `assigned != 4294967295` compares **equal** through that
same path, because C's usual arithmetic conversions turn the truncated `int32_t` back into the right
`uint32_t`. Something more specific happens near `INT32_MAX`. Write the failing case, watch it fail,
and **read the emitted C** before touching the grammar.

**Candidate fix, not a decision.** Pick the literal's node type from its *value* — the C ladder,
widening rather than truncating — rather than teaching the parser the target type. That is a language
semantics call: check it against `docs/GOALS.md` and write it into `docs/SPEC.md`.

**Verify.** Extend `tests/int_literal_wide.kama` (or add a sibling) across initializer, assignment
and comparison at `INT32_MAX`, `INT32_MAX+1` and `UINT32_MAX`, decimal and hex. If the resolution
makes some spelling *rejected*, that needs a `tests/xfail/` fixture in the same commit — a negative
claim with no fixture is not a claim.

---

## Close-out

1. `./dev matrix` on Windows, and again on macOS before switching back.
2. Delete this file.
3. Shrink `docs/ROADMAP.md` §3's Windows entry to whatever genuinely remains — the wall-clock note
   and the long-path / console-subsystem known issues, at minimum.
4. Update the closing pointer in `docs/platforms/windows.md`, which names the two failing guards and
   the three application-shape items and will be stale.
5. That deletion commit is the push signal.
