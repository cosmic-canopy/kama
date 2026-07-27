# Host-build parity + build-layout cleanup — kickoff / handoff

**Status: READY TO BUILD (fully diagnosed 2026-07-27; not started).** Two related build-infra tasks that
surfaced while getting the VS Code plugin running natively on macOS. Both are Linux-container-invisible —
they only bite a host (mac) build. Do Task 1 first (small, unblocks host concurrency testing); Task 2
(bigger, higher blast radius) makes host/container switching painless.

## Task 1 — macOS concurrency build failures (34 fixtures) — SMALL, well-scoped

**Symptom:** on a host (mac) `make`, 34 fixtures fail to *build* (`atomic_*`, `channel_*`, `isolate_*`,
`parfor_*`, `scope_*`, `shared_immutable_*`, `module_static_per_isolate`, `proc_detach_dtor`,
`trap/panic_handler`) with:
```
<unistd.h>:508: error: cannot apply asm label to function after its first use   // 'write'
```
The container (Linux) build passes all 793 — this is macOS-only.

**Root cause (confirmed):** `kama_runtime.h` implements a raw-stderr floor (write with no `<stdio.h>`),
forward-declaring `write` in its `#else` (non-`_WIN32`) branch at **4 sites — L342, L849, L876, L934** —
as `extern long write(int, const void*, size_t);` with **no asm label**. macOS `<unistd.h>` declares
`write` with an `__asm("_write")` alias (`__DARWIN_ALIAS_C(write)`). Concurrency programs include
`kama_isolate.h`, which includes `kama_runtime.h` (L14 — uses plain `write`) **then** `<unistd.h>` (L55).
clang then can't apply the asm label to an already-used `write`. Non-concurrency programs never pull in
`<unistd.h>`, so their plain forward-decl links fine to the aliased symbol — that's why *only* the
concurrency set fails. (Note: the Windows `_write` trick does NOT translate — macOS `write` mangles to
object symbol `_write`; declaring `_write` in C would mangle to `__write` and fail to link. macOS must
declare `write`.)

**Fix (clean, DRY):** add ONE `static inline` helper near the top of `kama_runtime.h` with the platform
spelling in a single place, then replace the 4 duplicated inline branches with calls to it:
```c
static inline long kama_raw_write(int fd, const void* p, unsigned long n) {
#if defined(_WIN32)
    extern int _write(int, const void*, unsigned int);        return _write(fd, p, (unsigned int)n);
#elif defined(__APPLE__)
    extern long write(int, const void*, unsigned long) __asm("_write");  // match unistd.h's label so a
    return write(fd, p, n);                                              // later <unistd.h> is consistent
#else
    extern long write(int, const void*, unsigned long);       return write(fd, p, n);
#endif
}
```
Then each site becomes `kama_raw_write(2, buf, p);`. The `__asm("_write")` on the `__APPLE__` decl is the
crux — it makes our declaration carry the SAME label `<unistd.h>` applies, so the label is not being
"applied after first use" (it's already there, consistently). (`size_t` == `unsigned long` on the LP64
targets here; use `size_t` if you prefer and `#include`-free — it's already in scope via the runtime.)

**Validation:** host `make` (brew bison auto-picked) → `sh run_tests.sh` on mac should reach **793** (was
759); container `tools/cdev test` → **793** (no regression); the wasm leg (`KAMA_WASM=1`, emscripten spells
it `write` — the `#else` path, unaffected, but re-run to be safe); freestanding/embedded still compile
(`--no-heap`, `--target embedded`). ⚠️ a host `make` clobbers the container `./kama` — rebuild whichever
you need after (this friction is exactly what Task 2 removes).

## Task 2 — platform-scoped build output dirs — BIGGER, high blast radius

**Pain:** the Makefile writes objects to `build/` and the binary to repo-root `./kama`, so host and
container builds **share one output path** and clobber each other → a forced `make clean` on every switch
between the VS Code extension (needs native `./kama`) and `tools/cdev` (Linux `./kama`).

**Desired:** platform-scoped output — e.g. `build/<os>-<arch>/` holding objects + the binary — so host and
container builds coexist and the extension always finds the native one. No more ping-pong.

**Touch points (verify each):**
- **Makefile** — parameterize `BUILD` by `$(uname -s)-$(uname -m)` (or a passed triple); the generated
  parser/lexer, `embed_prelude` output, and objects already live under `build/` so they move with it.
- **`tools/cdev`** — its `make` target + the `/work` mount; the container's platform dir differs from the
  host's, which is the whole point (they stop colliding).
- **`./kama` location** — the extension's `findKama()` (`editor/vscode/extension.js`) and CI/`release.yml`
  both expect a binary at a known path. Decide: keep top-level `./kama` as a **symlink to the active
  platform's binary**, or teach `findKama` + CI the platform path. A stable symlink is least disruptive.
- **CI** — `.github/workflows/ci.yml` (macos-14 + windows + linux legs) and `release.yml` run `make` then
  reference `./kama`; keep them working under the new layout.
- **`.gitignore`** — `build/` is already ignored; confirm the new subdirs are covered.

⚠️ Do this **one platform at a time**, verifying container + host + CI green after each — a wrong turn here
breaks every build path at once. Non-blocking for the language itself (real target = container/Linux).

## Handoff notes
- The `dev-infra` memory carries the short version of both (the CI-vs-cdev coverage story + the fix
  pointers). This doc is the execution detail.
- Authoritative test run is **`tools/cdev test`** (container, 793) — host `run_tests.sh` reports ~759 until
  Task 1 lands.
