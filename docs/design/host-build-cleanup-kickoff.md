# Host-build parity + build-layout cleanup — as shipped

**Status: BOTH TASKS SHIPPED (2026-07-27).** Two related build-infra tasks that surfaced while
getting the VS Code plugin running natively on macOS. Both were Linux-container-invisible — they
only bit a host (mac) build. This doc is the design of record; it records what landed, not a plan.

Result: the host suite went from **760 passed / 33 failed → 790 passed / 3 failed**, the container
suite stayed at **793 / 0**, and host and container builds now coexist with no `make clean` between
them.

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

### Three host-only failures remain (pre-existing, NOT caused by either task)

Verified present at baseline before any change. None reproduce in the container:
- **`check-embedded`** — the `@section` fixture uses ELF section names; a mac host clang targets
  mach-o, which requires `segment,section`. Genuinely needs an ELF-capable cross compiler (the
  container has one), so this check can't pass on a bare mac host.
- **`check-packages`** (re-point-to-same-bytes-mirror case) — bsdtar and GNU tar don't produce
  byte-identical archives, so the two "same bytes" mirrors hash differently on macOS.
- **`proc_detach_dtor`** — exits 120 (its `Command.start()` errors) on macOS under the 3000x
  spawn loop; a std::process portability issue, unrelated to the write floor.

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
- The authoritative test run is still **`tools/cdev test`** (container, 793/793). The host
  `run_tests.sh` now reaches 790 with the three host-only failures listed above.
- The `dev-infra` memory carries the short version of both.
