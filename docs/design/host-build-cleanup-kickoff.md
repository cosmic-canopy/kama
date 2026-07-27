# Host-build parity + build-layout cleanup — as shipped

**Status: BOTH TASKS SHIPPED (2026-07-27).** Two related build-infra tasks that surfaced while
getting the VS Code plugin running natively on macOS. Both were Linux-container-invisible — they
only bit a host (mac) build. This doc is the design of record; it records what landed, not a plan.

Result: the host suite went from **760 passed / 33 failed → 793 passed / 0 failed** — full parity
with the container, which also stayed at **793 / 0** — and host and container builds now coexist with
no `make clean` between them.

## Task 1 — macOS concurrency build failures — SHIPPED

**Symptom:** on a host (mac) `make`, 30 fixtures failed to *build* (`atomic_*`, `channel_*`,
`isolate_*`, `parfor_*`, `scope_*`, `shared_immutable_*`, `module_static_per_isolate`,
`trap/panic_handler`, plus `check-panic-multitu`) with:
```
<unistd.h>:508: error: cannot apply asm label to function after its first use   // 'write'
```

**Root cause:** `kama_runtime.h` implements a raw-stderr floor (write with no `<stdio.h>`),
forward-declaring `write` at 4 sites as `extern long write(int, const void*, size_t);` with **no asm
label**. macOS `<unistd.h>` declares `write` with an `__asm("_write")` alias
(`__DARWIN_ALIAS_C(write)`). Concurrency programs include `kama_isolate.h`, which includes
`kama_runtime.h` (uses plain `write`) **then** `<unistd.h>` (for `sysconf` in
`kama_parfor_workers`). clang then can't apply the asm label to an already-used `write`.
Non-concurrency programs never pull in `<unistd.h>`, so their plain forward-decl links fine to the
aliased symbol — that's why *only* the concurrency set failed.

**Fix (`3ead87f`):** one `static inline kama_raw_write(int fd, const void*, size_t)` near the top of
`kama_runtime.h` carries the platform spelling in a single place; the 4 duplicated `#if _WIN32 /
#else` branches collapse into calls to it (`kama_bounds_fail`, `kama_panic`, `kama_fail_emit`,
`kama_print_write`). The crux is the `__APPLE__` branch:

```c
extern long write(int, const void*, size_t) __asm("_write");
```

Our declaration now carries the SAME label `<unistd.h>` applies, so a later include is a consistent
redeclaration rather than a label applied after first use. (The Windows `_write` trick does NOT
translate: macOS `write` mangles to object symbol `_write`, so declaring `_write` in C would mangle
to `__write` and fail to link. macOS must declare `write`.) The helper is guarded out under
`KAMA_TARGET_EMBEDDED`, where every caller already routes to the weak `kama_panic_handler` /
`kama_log_sink`.

`kama_os.h` still hand-declares `read`/`write`/`close` and still avoids `<unistd.h>` — its comment
now records that `write` is safe by label and that `read` is the one still kept off the collision
course.

## Task 3 — the last three host failures — SHIPPED (`46bd876`, `52ac8c6`)

After task 1, three failures were left. All three were verified pre-existing (baselined by stashing
the task-1 fix), and none reproduced in the container — but only ONE turned out to be a genuine
platform limitation. The other two were real defects that Linux happened to hide.

- **`check-embedded`** — the only true host limitation. `--target embedded` is triple-AGNOSTIC by
  design: it hands the freestanding flags to whatever `--cc` emits, which on a mac defaults to
  mach-o, and mach-o rejects the ELF section names the `@section` fixture uses. Fixed in the guard,
  not the compiler: pin a bare-metal ELF triple (`clang --target=armv7m-none-eabi`) on Darwin, which
  is what a real firmware build does anyway. Compile-only, so no sysroot or cross libc is needed,
  and `nm` reads the resulting ARM ELF object fine (2b's libc-symbol check still applies).
- **`check-packages`** (re-point-to-same-bytes mirror) — a **registry-correctness bug**, not a test
  artifact: `kama publish` was not byte-reproducible, so publishing the same sources to two mirrors
  produced two different integrity hashes and a consumer re-pointing at a mirror tripped the
  dependency-confusion guard. Two causes, neither fixable with portable tar flags (GNU's
  `--mtime`/`--sort` don't exist on bsdtar), so the inputs are normalized instead: every staged entry
  is clamped to a fixed timestamp (the staging wrapper dir is created fresh each publish, so its
  mtime was landing in the archive), and compression goes through `gzip -n` (libarchive's `tar -cz`
  stamps the current time into the gzip header; GNU tar was reproducible here only by accident,
  because it pipes to gzip via stdin, which stores 0).
- **`proc_detach_dtor`** — a **real `std::process` resource leak**, described in full in
  `docs/design/std-process.md`. `~Process()` assumed the OS reparents a dropped-but-live child to
  init; POSIX does that only when the *parent* exits, so every un-reaped child stayed a zombie
  holding a process-table slot. macOS's `kern.maxprocperuid` (2666) made it reachable — the fixture
  died at iteration ~2384 with zombies climbing monotonically — while Linux's higher cap hid it.
  `kama_proc_detach` now parks a still-running pid and sweeps the park with `WNOHANG` on later
  drops; still non-blocking, and it can never steal a status from a `Process` the user can `wait()`
  on. Zombies now stay at 0–2 across the whole fixture.

## Task 2 — platform-scoped build output dirs — SHIPPED

**Was:** the Makefile wrote objects to `build/` and the binary to repo-root `./kama`, so host and
container builds shared one output path and clobbered each other → a forced `make clean` on every
switch between the VS Code extension (needs native `./kama`) and `tools/cdev` (Linux `./kama`).

**Now:** `BUILD = build/$(PLATFORM)` where `PLATFORM ?= $(shell uname -s)-$(shell uname -m)`. The
objects, generated parser/lexer, embedded prelude AND the binary all live there, so
`build/Darwin-arm64/` and `build/Linux-aarch64/` coexist. Switching platforms is a symlink refresh,
not a rebuild.

**What landed:**
- **Makefile** — `$(BUILD)/kama` is the real link target; root `./kama` is a **`.PHONY` symlink
  target** refreshed on every build. PHONY is load-bearing: make stats *through* the symlink, so
  after the other platform built last it would see a newer file and skip the relink, leaving
  `./kama` pointing at a foreign binary. `clean` removes this platform's dir only — nuking `build/`
  wholesale would defeat the coexistence (a container `make clean` would wipe the host build).
- **`tools/kama-bin.sh`** (new) — the shared resolver, sourced with `$ROOT` set: honors an external
  `$KAMA`, else prefers `build/<os>-<arch>/kama`, else falls back to the root `./kama` symlink (an
  installed tree has no `build/` at all). `run_tests.sh`, `tools/lspref.sh` and all 13
  `tools/check-*.sh` guards source it instead of hardcoding `$ROOT/kama` — that is what lets a host
  run and a `tools/cdev` run interleave with no rebuild in between.
- **`kama.driver.cpp`** — `resolveRuntimeDir` / `resolveStdlibDir` gained a dev-tree probe
  (`<exeDir>/../..` and `<exeDir>/../../lib`). Without it the binary at `build/<plat>/kama` found
  neither `kama_runtime.h` nor `lib/std` and silently degraded to cwd-relative — which passed from
  the repo root but broke `kama run` inside a project dir (caught by `check-packages` case 11).
- **`editor/vscode/extension.js`** — `findKama()` probes `build/<plat>/kama` before the root
  symlink, mapping `process.platform`/`process.arch` to the `uname` spelling. A `tools/cdev make`
  can no longer hand the extension a Linux binary it can't launch.
- **CI / release** — unchanged by design. Every leg runs `make` then uses `./kama`, and
  `cp kama payload/bin/kama` follows the symlink. (`-o <name>` is used verbatim by clang, so no
  `.exe` surprise on the msys2 leg; `ln -s` there degrades to a copy, which also works.)
- **`.gitignore`** — already covered: `build/` catches the new subdirs, `/kama` the symlink.

## Handoff notes
- **A host `sh run_tests.sh` on macOS now reaches 793/793, same as `tools/cdev test`.** The container
  remains the authoritative run (it is the real target and covers the MCU/QEMU legs a bare mac skips),
  but the mac is no longer a second-class test host.
- Lesson worth keeping: two of the three "macOS-only" failures were **portability bugs Linux was
  hiding**, not platform quirks. A second OS is a bug detector — the reproducible-publish and
  zombie-reaper fixes both matter on Linux too.
- The `dev-infra` memory carries the short version.
