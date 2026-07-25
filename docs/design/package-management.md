# Kama toolchain & package management — design of record (kickoff)

**Status: M2 (per-project packages / pillar 4) ✅ COMPLETE 2026-07-24 (M2.0–M2.3). NEXT = M1 (toolchain
version management, pillars 1–3) — kickoff PREPARED (see the "M1 — implementation kickoff" section below).**
Kickoff brief for the campaign after conditional compilation (`@compileFor`, ✅ shipped 2026-07-24). The
user chose to build **M2 first**, then **M1**; the executable plan for each milestone is the design of
record for its build. This doc keeps the vision, seam map, prior art, and open questions. North star
(user): **kama is its own opt-in version manager** — never wrap nvm/pyenv/rustup around it.

**Implementation progress:**
- **M2.0 — path deps + lockfile + import wiring ✅ shipped.** `kama.json` `dependencies` (path/git/url
  schema, `path` fetched) → `ManifestReader::depsObject()`; deterministic `kama.lock` writer
  (`writeLockFile`); `kama install [<dir>]` materializes `<project>/.kama/deps/` (directory symlinks,
  `linkDir`) + writes the lock; the build appends the resolved view to the import roots
  (`projectDepsView` → `loadProgramUnits`), so declared deps resolve and undeclared imports fail
  (phantom-dep guarantee). Build guards a missing view with "run `kama install`". Fixtures:
  `tests/pkg_path_dep.d` (resolves via the view only), `tests/xfail/pkg_undeclared_import`. Native +
  wasm green. (The lock *reader* / `parseLock` is deferred to M2.2 where the resolver makes honoring a
  pinned lock meaningfully differ from re-resolving — path deps re-resolve identically, so the
  deterministic writer alone holds the reproducibility spine.)
- **M2.1 — content-addressed store + integrity + git/url fetch ✅ shipped.** `kama install` now fetches
  `git` (shallow `--branch <rev>` clone → `rev-parse HEAD` for the commit) and `url` (curl tarball,
  `tar --strip-components=1`) deps into a shared content-addressed store, then links the per-project view
  into it. Store id = sha256 of the canonical unpacked **tree** (`treeHashOf`: sorted `relpath\0`+bytes,
  built in C++, hashed once) → `store/<name>-<hash>/`; staging is atomic (`rename()` only after the hash
  is known). Integrity: git lock entry records `commit` + `integrity=sha256-<treehash>`; url records the
  **tarball** sha256 (verified against the manifest's `integrity`, hard-fail on mismatch, else
  trust-on-first-use). **sha256 = shell out** (`sha256sum`→`shasum -a 256`→`certutil`) via the new
  `runCmdCapture` (popen) — no vendored crypto/HTTP, same subprocess model as git/curl/tar. Store root =
  `~/.kama/store` with a `KAMA_STORE` override (NOT `KAMA_HOME`, which selects the read-only stdlib root).
  New seams in `kama.driver.cpp`: `runCmdCapture`/`rmRfCmd`/`firstSha256Hex`/`sha256Of`/`collectFilesRel`/
  `treeHashOf`/`storeDir`/`fetchToStore`; `cmdInstall` git/url branch rewired (writer/`LockEntry`/`DepSpec`
  unchanged). Guard: `tools/check-packages.sh` (network-free `file://` git repo + local tarball — store+
  view+lock+run, byte-identical re-install, url TOFU, tamper→hard-fail), registered on the plain native
  leg of `run_tests.sh`. Native 755 / wasm 725 green; install path ASan/UBSan-clean. **Known limit:** git
  `rev` supports tag/branch only (a raw commit sha can't ride `--depth 1 --branch`) — full sha pinning is
  a candidate for M2.2's resolver.
- **M2.2 — resolver + `parseLock` + `kama pkg` tree + dev-dependencies ✅ shipped.** Lock **reader**
  (`parseLockFile`/`LockReader`, mirrors the writer exactly) → lock-honoring install (cargo model): a git/url
  dep whose manifest spec is unchanged reuses the pin — a **warm store links with ZERO fetch (offline)**, a
  **cold store re-fetches by the recorded `commit`/`integrity`** and asserts the tree hash still matches
  (hard-fail on drift). Git **raw-commit-sha pinning** (`isSha1Hex` → `git init`+`fetch --depth 1 origin
  <sha>`+`checkout FETCH_HEAD`) — also the mechanism that reproduces a branch-pinned dep by its `commit`.
  **Transitive BFS resolver**: reads each fetched package's own `kama.json` and closes the graph; conflict
  policy (no SemVer yet) = one spec per name, identical dedups, divergent **hard-errors** naming both
  requestors; `LockEntry.dependencies[]` now records each package's direct deps (deterministic sorted).
  **`dev-dependencies`** with a strict **`--dev` build boundary**: dev-deps link into a separate
  `.kama/dev-deps/` view that is on the import path ONLY under `kama build --dev` (decoupled from
  `--release`), so production code physically can't import a test-only dep (the phantom-dep guarantee makes
  it a resolve error at any opt level); dev is strictly **non-transitive** (a fetched package's dev-deps are
  never pulled; prod-reachable names win over dev). A `treeHash` lock field closes the url offline gap (url's
  `integrity` is the tarball, its store key is the tree; git omits it since integrity == tree). New CLI
  namespace **`kama pkg install|add|remove|update`** (opt-in; the former `kama install` moved under it, no
  alias; resolves the collision with toolchain `kama update`): `add [--dev] <name> --git/--url/--path` +
  `remove` mutate `kama.json` via a **byte-preserving textual splice** (name/version/flags/unknown keys kept
  verbatim; only the target section re-emitted); `update [<pkg>]` re-resolves pins (advance a branch pin)
  without touching the manifest. Guard `tools/check-packages.sh` extended (cases 5–10: transitive + dev-dep
  non-propagation, sha pin, offline/cold-store honor, dev `--dev` boundary, add/remove round-trip, conflict
  hard-fail). Native 755 / ASan 737 / wasm 725 green; install/resolver path ASan+UBSan-clean.
- **M2.3 — `kama run` + manifest `main` field + user docs ✅ shipped.** `kama run [<file>] [-- <args>]`
  builds the entry `.kama` to a temp native binary, execs it, forwards the exit code, and removes the temp —
  a thin wrapper that **reuses the whole build branch** (`if (subcommand == "build" || runMode)`) rather than
  duplicating it. Entry resolution: explicit `<file>` wins, else the manifest **`main`** field (captured by a
  new `ManifestReader::mainOut` / `loadManifestMain`; discovered via `--config` else `kama.json` in CWD),
  else a clear error. **Native-only** (wasm/embedded rejected pointing at `kama build`). `-- <args>` are
  accepted + forwarded at the OS process boundary but **inert today** — kama's `main` takes no args (argv
  marshaling is deferred; see ROADMAP §2 "command-line args / env in the PRELUDE FLOOR", the eventual
  consumer). Docs: user-facing **`docs/packages.md`** quickstart + a README pointer. Guard
  `tools/check-packages.sh` extended (cases 11–14: `main`-field run + explicit run forwarding a non-zero exit,
  the `--dev` boundary composed with run, native-only rejection, no-input/no-`main` errors). Native 755 /
  wasm 725 green; run/driver path ASan+UBSan-clean. **Closes pillar 4 (per-project packages).**

## Scope — four pillars (user, 2026-07-24)

Kama should answer the ecosystem question so users don't roll their own. Opt-in, as always.

1. **Toolchain version management** — like `kvm`/`nvm`/`rustup`: manage multiple kama compiler versions
   on one machine.
2. **Self-update / download specific versions** — update kama, or fetch a specific compiler version
   (and, later, scripting runtimes when kama goes multi-modal).
3. **Env management** — work with multiple kama versions on one machine cleanly (like python venvs, but
   cleaner). Less critical for the native compiler; **more important for the future scripting runtime**.
4. **Per-project package management** — like node/npm, but with the footguns fixed and simplified.

## What already exists — build on it, don't duplicate

- **Install prefix `~/.kama`** (`KAMA_HOME`), via `install.sh` / `install.ps1` (`curl … | sh`). Today it
  installs **one** version *in place* (re-running updates it). Slim vs `-bundled` (zig cc) flavor by
  C-compiler detection. `KAMA_VERSION=vX.Y.Z` selects a release; assets are GitHub release tarballs.
- **`kama update [--version vX.Y.Z]`** (`cmdUpdate`, kama.driver.cpp:593) — self-update by re-running the
  canonical installer. This is the seed of pillar 2.
- **`kama.json` manifest** (shipped with conditional compilation) — currently `name` / `version` /
  `flags`; `name`/`version` are **parsed-and-ignored, reserved for exactly this campaign**. This is the
  seed of pillar 4's manifest. Its C++ reader (`ManifestReader`, kama.driver.cpp) already tolerates
  unknown keys, so adding `toolchain` / `dependencies` sections is additive.
- **`docs/PUBLISHING.md`** — the existing release/distribution process (GitHub releases, the site,
  VS Code marketplace). A registry (M3) extends this.

## North stars (inherit GOALS + the reproducibility ethos)

- **One way to do a thing; explicit over implicit; simplicity** (GOALS).
- **Reproducible by construction** — the discipline const-eval and `@compileFor` already hold: a build
  output is a **pure function of (source + `kama.json` + lockfile + toolchain version)**. Nothing decided
  by ambient shell/env. This is the antidote to most npm/pip footguns.
- **Opt-in** — a bare single-file `kama build foo.kama` needs none of this. Manifest/lock/store engage
  only for a real project.

## Prior art distilled — what to borrow, what to avoid

| Tool | Borrow | Avoid |
|---|---|---|
| **rustup** | THE model for pillars 1–3: a versioned toolchain store, a thin **shim** on `PATH`, per-project pin (`rust-toolchain.toml`), `default`/override. | — |
| **cargo** | THE model for pillar 4: manifest + **lockfile** + content-addressed registry cache + SemVer; **no install scripts** by default; reproducible. | Occasional deep-rebuild pain (fine at our scale). |
| **pnpm** | **Content-addressed global store** + linking (one copy per version, shared across projects) — kills node_modules duplication. | — |
| **nvm** | The idea (multiple versions). | Shell-function/`source`-into-shell mechanism — fragile, not "clean." Prefer a compiled shim. |
| **npm/node** | Familiar UX (`add`/`install`/`run`). | **The footguns** (see below). |
| **deno** | Central cache + **integrity hashes** + `deno.json`; run without an activation step. | URL-only imports (we want named deps + a manifest). |
| **python venv / pip** — user's "cleaner than this" bar | uv's speed + lockfile + **no-activation** run model. | `activate`/`deactivate` shell state; global site-packages leakage; non-reproducible pip installs. |

### The npm/node footguns to FIX (the user's explicit ask)

- **`node_modules` bloat / duplication / phantom deps** → a **content-addressed global store** linked
  into a per-project resolved view (pnpm model); a **flat, single-version-per-package** graph where
  possible (no deep diamond duplication). **Only declared deps are importable** — no phantom deps.
- **Arbitrary `postinstall` code execution** (the biggest supply-chain footgun) → **no install scripts.**
  Packages are kama source (compiled by the consumer) or declared prebuilt artifacts; any native build
  step is an explicit, sandboxed, *declared* directive — never arbitrary code on install.
- **Lockfile drift / floating `^` ranges** → **lock by default**; the build reads exact versions +
  **integrity hashes** from `kama.lock`, not the manifest's ranges. Ranges are resolved *once* and pinned.
- **Global vs local confusion** → explicit and separate: project deps live in the project's resolved
  view; installed *tools* are a separate concern.
- **Dependency confusion / typosquatting** (registry era) → namespaced packages + integrity hashes +
  (later) signing.

## Per-pillar design leans (to confirm/refine in the kickoff session)

### Pillars 1–3 — toolchain & env management (rustup-style, one clean tool)

- **Versioned store**: `~/.kama/versions/<version>/{bin,lib}`; today's single in-place `~/.kama` becomes
  one entry (keep back-compat / a migration). Modality-aware from day one — key entries by *(kind,
  version)* so future scripting runtimes live in the same store (pillar 3's real payoff is here).
- **Shim/selector**: the `kama` on `PATH` is a thin launcher that resolves *which* version to run per
  directory — **no activate step** (this is "cleaner than python venvs"):
  project pin → `KAMA_VERSION` override → global default.
- **Commands**: `kama toolchain list | install <v> | uninstall <v> | default <v>`; `kama update` (exists)
  self-updates the launcher/default; `kama --version` (exists).
- **Open**: launcher as a separate tiny binary vs the kama binary self-detecting and re-exec'ing; where
  the project pin lives (a `kama.json` `toolchain` field vs a `.kama-version` file); bootstrap ordering
  (installer must lay down the shim before any version).

### Pillar 4 — per-project packages (cargo + pnpm inspired)

- **Manifest**: extend `kama.json` with `dependencies` (name → SemVer req) and maybe `dev-dependencies`.
- **Lockfile `kama.lock`**: exact resolved versions + integrity hashes; committed; **the build reads the
  lock**, not the manifest ranges → reproducible.
- **Content-addressed store** `~/.kama/store/`: each `pkg@version+hash` resolved once, shared across
  projects, linked into a per-project view (no nested duplication).
- **Resolution**: prefer a **flat, single-version-per-major** graph; decide the conflict policy up front.
- **Source of packages**: **Git/URL deps + integrity hashes FIRST** (no central-registry infra needed to
  ship), a hosted **registry later** (M3, extends PUBLISHING.md). Namespacing throughout.
- **Import model**: `import pkg::mod` resolves only *declared* deps (kills phantom deps). Optional deps
  could compose with `@compileFor` gates.
- **Commands**: `kama add <pkg>` / `remove` / `install` (sync from lock) / `update <pkg>` / `run`.

## Reproducibility invariants (this campaign's #0)

- A build output is a **pure function** of *(source, `kama.json`, `kama.lock`, toolchain version)* —
  nothing ambient.
- **Integrity-hash every fetched artifact**; verify on use.
- Lockfile committed; CI builds byte-reproducible.

## Staged plan (each its own milestone)

- **M1 — toolchain store + shim + `kama toolchain` + project pin** (pillars 1–3). Native first.
- **M2 — manifest deps + `kama.lock` + content-addressed store + resolver** (pillar 4); Git/URL deps +
  integrity; `kama add/install/run`. **No registry yet.**
- **M3 — hosted registry + `kama publish` + namespacing/signing.** Extends PUBLISHING.md.
- **M4 — multi-modal**: scripting-runtime versions in the store; per-modality env resolution (pillar 3's
  full form).

## Open questions for the kickoff session

1. **Project pin & manifest layout** — everything in `kama.json` (`toolchain` + `dependencies` +
   `flags`) vs split (`.kama-version` + `kama.lock` alongside `kama.json`).
2. **Launcher architecture** — separate shim binary vs kama self-re-exec.
3. **Registry now vs Git/URL-first** (lean: Git/URL-first).
4. **Resolution policy** — flat single-version vs allow multiple versions of a package.
5. **Store layout & linking** — hardlink / symlink / copy; Windows constraints.
6. **Package form** — source-only (consumer compiles) vs prebuilt artifacts; how native build steps are
   declared without reintroducing `postinstall` arbitrary-code execution.
7. **Self-hosting tie-in** — once the compiler is self-hosted, the manifest/lock JSON could be read by
   the kama-level `std::serialization::json` library instead of the driver's C++ reader (today it must be
   C++; see the conditional-compilation as-shipped note). Worth noting for the long arc.

## Cross-references

- `kama.json` + its `ManifestReader` (conditional compilation, shipped) — the manifest seed.
- `install.sh` / `install.ps1`, `kama update` / `cmdUpdate` — the install/update seed.
- `docs/PUBLISHING.md` — release/distribution (registry extends it).
- `GOALS.md` — one way / explicit / simplicity, and the reproducibility ethos const-eval and
  `@compileFor` established.

---

## M2.1 — implementation kickoff (content-addressed store + integrity + git/url fetch)

**Status: PREPARED, not built.** Next slice after M2.0 (✅ shipped). Start building from here — the
decisions below are settled (approved plan); the seams are verified in code. Everything stays a pure
function of *(source, `kama.json`, `kama.lock`, toolchain)*; no arbitrary code ever runs on install.

### Goal

Make `kama install` fetch **git** and **url** dependencies (M2.0 only did `path`) into a shared
**content-addressed store** at `~/.kama/store/`, verify **sha256 integrity**, and record the pinned
identity + integrity in `kama.lock`. The per-project view (`.kama/deps/<name>` symlinks, M2.0) then
points into the store instead of at a local path. Build/import wiring is unchanged (already reads the
view). Prove it with a network-free guard script.

### Where M2.0 left the seams (fresh anchors, `kama.driver.cpp`)

- `struct DepSpec` (377) — already carries `path/git/url/rev/integrity/version`. No change needed.
- `struct LockEntry` (553) — already has `source/path/git/url/rev/commit/integrity/dependencies`.
- `writeLockFile` (571) + `jsonEscape` — already emit every LockEntry field. **No writer change** — just
  populate `git/url/rev/commit/integrity` in the entries (M2.0 only set `path`).
- `makeDirs` (597), `linkDir` (617) — reuse as-is (linkDir already does symlink→junction→(copy TODO)).
- `cmdInstall` (779) — the git/url branch is currently the stub at ~819 (`"uses a git/url source …"`).
  **Replace that stub** with fetch→hash→store→link. The `path` branch (816) stays.
- Dispatch `if (subcommand == "install")` (867) — unchanged.

### What to add (all shell-outs via existing `runCmd`, 605)

1. **`sha256Of(path)`** — no C/C++ hasher exists in-repo, so shell out: `sha256sum` (Linux/container)
   → `shasum -a 256` (macOS) → `certutil -hashfile <f> SHA256` (Windows). Parse the first hex field.
   Return `"sha256-<hex>"`.
2. **`treeHashOf(dir)`** — the store identity = sha256 of the **canonical unpacked source tree** (NOT
   the raw tarball/clone: git adds `.git/`, tarballs vary in wrapper/compression). List all files
   sorted, hash `path\0` + file-bytes in sorted order → one digest. Simplest impl: shell out
   `find <dir> -type f | sort | while read f; do printf '%s\0' "${f#dir/}"; cat "$f"; done | sha256sum`
   (or build the concatenation in C++ and hash once). Keep it dead simple; cache = the store dir name.
3. **`storeDir()`** → `$KAMA_HOME/store` (else `<exe>/../store`, mirroring `resolveStdlibDir` at 128).
4. **Fetch, in the cmdInstall git/url branch:**
   - **git:** `git clone --depth 1 --branch <rev> <url> <staging>`; `git -C <staging> rev-parse HEAD` →
     `commit`; `rm -rf <staging>/.git`; `treeHashOf(staging)` → hash; `rename()` staging →
     `store/<name>-<hash>/` (dedup: skip fetch if it already exists). Lock entry:
     `source="git", git, rev, commit, integrity=sha256-<hash>`.
   - **url:** `curl -fsSL <url> -o <staging>.tgz`; if manifest `integrity` given, verify `sha256Of` ==
     it → **hard error + non-zero exit on mismatch, nothing enters the store**; else trust-on-first-use
     and write the computed hash to the lock. `tar -xzf` into `<staging>/`, strip one wrapper dir;
     `treeHashOf`; `rename()` into store. Lock entry: `source="url", url, integrity`.
   - **Staging:** `store/.tmp-<pid>/`, atomic `rename()` into the content-addressed path only after the
     hash is known — a killed fetch never leaves a half-populated `<name>-<hash>`.
   - **View:** `linkDir(store/<name>-<hash>, .kama/deps/<name>)` (git/url) — same call as path deps.
5. **`.gitignore`** — add `.kama/` guidance (or leave to M2.3 docs).

### Decisions (settled — don't re-litigate)

- Store hash = canonical unpacked **tree**, not the artifact. Linking = symlink (junction/copy on
  Windows). sha256 = shell out (no vendored crypto). Integrity mismatch = hard fail. Dedup by
  `<name>-<hash>`. No SemVer ranges yet (git pins `rev`, url pins `url`+integrity). No postinstall.

### Testing — `tools/check-packages.sh` (model on `tools/check-compilefor.sh`, 92 lines)

The `.d/` harness only runs `kama build`, never `kama install`, so install/store/integrity go in a
guard script run once on the plain native leg (see `run_tests.sh` 127-154; register a
`[ -f tools/check-packages.sh ] && sh tools/check-packages.sh` block next to the others). It must:
1. Make a throwaway project in a tmp dir with a **`file://` git repo** and/or a **local tarball** dep
   (create both under the tmp dir — **no live network**, mirroring how net tests gate on
   `KAMA_WASM`/`KAMA_BROWSER`). `git init` a tiny package, commit, tag `v1.0.0`, use `file://$PWD/...`.
2. `kama install` → assert `kama.lock` has the `commit`/`integrity`; assert `.kama/deps/<name>` links
   into `~/.kama/store/<name>-<hash>`; assert the built program runs.
3. Re-`install` offline (store populated) → **byte-identical `kama.lock`** (reproducibility) + exit 0.
4. Tamper test: corrupt the tarball or pass a wrong `integrity` → install **fails** non-zero.
5. Skip gracefully if `git`/`sha256sum` absent (they ship on the base image; be defensive). Set
   `KAMA_HOME` to a tmp store so the check never pollutes the real `~/.kama`.

### Then

M2.2 = resolver (transitive BFS, `parseLock` reader, lock-honoring, git sha pinning) + `dev-dependencies`
with a strict `--dev` boundary + the `kama pkg` command tree (`install`/`add`/`remove`/`update`) — **✅
shipped 2026-07-24** (see the M2.2 as-shipped bullet in the implementation-progress list at the top).
M2.3 = `kama run` + `main` field + docs. See the staged plan above.

---

## M2.3 — implementation kickoff (`kama run` + manifest entry field + user docs)

**Status: ✅ SHIPPED 2026-07-24** (see the M2.2/M2.3 as-shipped bullets in the implementation-progress list
at the top). The final per-project-packages slice (pillar 4), closing the campaign's first pillar. The
leans below were all confirmed as written, with one refinement: the eventual argv/env consumer belongs in
the **prelude/runtime floor, not an opt-out `std::env`** (a non-reimplementable, runtime-owned capability —
ROADMAP §2). The `-- <args>` passthrough was kept (accepted + forwarded, inert today).

### Goal

`kama run [<file>] [-- <args>]` — build the project's entry `.kama` and **execute it in one step**,
forwarding the program's exit code. Add a `main` entry field to `kama.json` so `kama run` with no file
works from a project dir. Ship a user-facing quickstart tying the whole `kama pkg` flow together. Nothing
about the reproducibility model changes — `run` is `build` + `exec`.

### Where the seams are (`kama.driver.cpp`)

- **`main` dispatch** — add `if (subcommand == "run")` next to `build`/`transpile`/`pkg`. **Reuse the SAME
  option parser + build pipeline** (`loadProgramUnits` → transpile → `cc` → native binary). `run` = the
  native build branch, then an exec of the produced binary.
- The **build branch already produces a native executable** at `output` (a default/temp name when unset).
  `run` builds to a temp path, execs it, forwards the exit code, unlinks. Do **not** `execv`-replace the
  process (so it can clean up the temp and return the child's code — `fork`+`exec`+`waitpid`, or `system()`
  with `WEXITSTATUS`, mirroring the existing `runCmd`).
- **`ManifestReader`** (today `name`/`version` are `skipValue`'d, [~505]) — capture a `main` string (the
  entry `.kama`, relative to the manifest). Add a tiny `loadManifestMain(path, out, err)` (or extend the
  reader with a `std::string* mainOut`), same idiom as the `dev-dependencies` add in M2.2.
- **Manifest/entry discovery** — `projectDepsView`/the build already find the project dir from the first
  input; for `kama run` with no `<file>`, discover `kama.json` in CWD → read `main` → that path is the input.

### What to add

1. `kama run [<file>] [--release|--debug] [--dev] [--define NAME]... [--config PATH] [-- <program args>]`.
   **Native-only**: reject `--target wasm|embedded` with a clear message (wasm needs node/a browser;
   embedded emits a freestanding object) — point at `kama build`.
2. **Entry resolution**: explicit `<file>` wins; else the manifest `main` field; else error
   (`"no input file and kama.json has no \"main\""`).
3. **Build → exec → forward exit → clean up.** Build into a temp binary (as the `.d` test harness names
   outputs), run it, propagate `WEXITSTATUS`, remove the temp.
4. **`main` manifest field** — a string, entry `.kama` relative to `kama.json`; reserved-and-ignored today,
   now read by `run`.

### Decisions (leans — confirm, don't re-derive)

- **Field name = `main`** (npm-familiar; kama's entry fn is already `fn int32 main()`). Alt considered:
  `entry`/`bin`. Lean `main`.
- **Arg passthrough is forward-looking only.** kama's `main` takes **no args** and argv marshaling is
  explicitly **not wired yet** (see the synthesized entry point in `kama.cemit.cpp` ~10570: hosted `main`
  does `(void)argc; (void)argv;`). So `-- <args>` forwarded to the child process is **inert until argv
  lands** (a separate language feature). Lean: **accept + forward `-- <args>` anyway** (zero cost, ready for
  when argv is wired) — or drop the `--` syntax until then. Confirm.
- **Always build fresh** for M2.3 (simple, reproducible); up-to-date-skip caching is a later optimization.
- **Temp binary, removed after exec** (no new persistent artifact surface) — vs a kept `.kama/bin/<name>`.
  Lean temp.
- **`run` only** — do NOT also make `build`/`transpile` fall back to `main` when given no input (keep those
  explicit); revisit if users ask.
- **Docs**: a new `docs/packages.md` (user-facing quickstart) + a README pointer. Confirm the location.

### Testing — extend `tools/check-packages.sh` (network-free, native-only)

- Project with a `main` field + a `file://` git dep: `kama run` (no file) builds + runs + **forwards a known
  non-zero exit code**; and the explicit `kama run <file>` form.
- `kama run --dev` resolves a dev-dep-importing entry; `kama run` (no `--dev`) **fails to resolve** it
  (the `--dev` boundary composes with `run`).
- `kama run --target wasm` → clean **native-only** error.
- No input + no `main` → clear error.

### Then

M2.3 closes **pillar 4** (per-project packages). The remaining, currently-unscheduled milestones:
**M1** — toolchain store + PATH shim + `kama toolchain` + project pin (pillars 1–3); **M3** — hosted
registry + `kama publish` + namespacing/signing + SemVer range resolution (extends `docs/PUBLISHING.md`;
the M2.2 resolver's one-spec-per-name check is where range-intersection slots in); **M4** — multi-modal
(scripting-runtime versions in the same store). See the staged plan above.

---

## M1 — implementation kickoff (toolchain store + selector + `kama toolchain` + project pin)

**Status: PREPARED, not built.** The next milestone (pillars 1–3, chosen after pillar 4 shipped). The
guiding north star, from the user: **kama is its own version manager — opt-in.** You should never have to
wrap `nvm`/`pyenv`/`rustup` *around* kama; kama itself offers multi-version management, and a bare
single-file `kama build foo.kama` needs none of it. Everything below is a settled lean to confirm at the
top of the build session; the seams are verified in code.

### The key enabling property (already true in code — this is why M1 is small)

The versioned store mostly **"just works" with near-zero resolver surgery**, because every support-dir
resolver is already **exe-relative**, and the package store is already **version-independent**:

- `resolveRuntimeDir` / `resolveStdlibDir` / `resolveCCompiler` ([kama.driver.cpp:101/134/155]) resolve
  `../include`, `../lib/kama`, `../libexec/zig` **relative to the running binary**. So a binary living at
  `~/.kama/versions/<v>/bin/kama` finds *that version's* runtime, stdlib, and bundled zig with no change.
- `storeDir()` ([kama.driver.cpp:1054]) is `$HOME/.kama/store` — **already shared across versions**,
  independent of the exe. Packages resolve once, for all toolchains. No change.
- `KAMA_VERSION` is **compile-stamped** from the `VERSION` file (`-DKAMA_VERSION` in the Makefile), so each
  binary **knows its own identity** — the guard that stops a selector re-exec loop.

So M1 is: a **versioned store layout**, a **selector** that execs the right version per directory, the
**`kama toolchain`** command surface, a **project pin**, and **installer changes** — *not* a compiler
rewrite. ⚠️ The one real snag is the `KAMA_HOME`-first override inside the two resolvers (see Decisions).

### Goal

`kama toolchain install <v> | uninstall <v> | list | default <v>` manages multiple compiler versions in
`~/.kama/versions/<v>/`. The `kama` on `PATH` becomes a thin **selector**: it resolves *which* version to
run per directory — **project pin → `KAMA_VERSION` env → global default** — and execs it, with **no
activate step** (the "cleaner than python venvs" bar). A project pins its toolchain in `kama.json`, making
the toolchain version one of the reproducible build inputs *(source, kama.json, kama.lock, toolchain)*.

### Where the seams are

- **Install layout, `install.sh` / `install.ps1`.** Today a *flat* prefix: `tar --strip-components=1` into
  `$PREFIX` → `~/.kama/{bin/kama, lib/kama, include, libexec/zig, store}`. M1 changes the target to
  `~/.kama/versions/<version>/…` and lays down the selector at `~/.kama/bin/kama` (the only thing on PATH).
  Windows already moves a running `.exe` aside to self-overwrite ([install.ps1:36]) — **the versioned store
  sidesteps this entirely** (a new version is a new dir; the running binary is never overwritten).
- **`cmdUpdate`** ([kama.driver.cpp:919]) — self-update = re-run the canonical installer via curl/irm,
  honoring `KAMA_VERSION`. M1 keeps `kama update` (updates the *default* / selector) and adds `kama
  toolchain install <v>` as the per-version fetch (same installer, `KAMA_VERSION=<v>`, into the store).
- **The resolvers** ([kama.driver.cpp:101/134]) — exe-relative already (good); the `if (getenv("KAMA_HOME"))
  return …` first branch is the snag (see Decisions).
- **Dispatch** — insert `if (subcommand == "toolchain")` between the `update` branch ([kama.driver.cpp:1534])
  and the `pkg` branch ([kama.driver.cpp:1544]); mirror the `pkg` two-level verb dispatch + a `toolchainUsage()`.
- **Manifest** — `ManifestReader` ([kama.driver.cpp:401]) tolerates unknown keys; add a `toolchain` string
  capture (same idiom as the M2.3 `mainOut` field) for the project pin.
- **Version identity** — `KAMA_VERSION` macro + `kama --version` ([kama.driver.cpp:1526]); the selector
  compares the resolved pin against a candidate binary's `--version` (or its store dir name) to pick/guard.

### What to add

1. **Versioned store** `~/.kama/versions/<v>/{bin,include,lib,libexec}` + a **migration** of today's flat
   `~/.kama` install into `versions/<current>/` on first M1-aware run (keep back-compat).
2. **Selector** on `PATH` at `~/.kama/bin/kama`: read the pin (project → `KAMA_VERSION` → default), resolve
   the store dir, and exec `versions/<v>/bin/kama` with the original argv. Guard against re-exec loops via
   the compile-stamped `KAMA_VERSION`. Fast path: unpinned + already-default = no re-exec.
3. **`kama toolchain`**: `list` (installed versions + which is default + what the CWD resolves to),
   `install <v>` (installer with `KAMA_VERSION=<v>` into the store; skip if present), `uninstall <v>`,
   `default <v>` (record the global default).
4. **Project pin** — read a `toolchain` field from `kama.json`; `kama toolchain pin <v>` (or reuse
   `default --project`?) writes it via the same byte-preserving splice the M2.2 `kama pkg add` uses.
5. **Global default record** — a tiny `~/.kama/default` (or `~/.kama/settings.json`) the selector reads.
6. **Installer changes** — install into the versioned path, drop the selector on PATH, set the default.

### Decisions (leans — confirm at the top of the session, don't re-derive)

- **Store layout** = `~/.kama/versions/<v>/{bin,include,lib,libexec}`; the shared **package** store stays
  at `~/.kama/store` (unchanged); a `~/.kama/bin/` holds the selector. **Modality-aware key** for M4:
  `versions/<kind>-<v>/` (kind=`compiler` today) so scripting runtimes share the store later.
- **Selector = self-re-exec (lean), not a separate shim binary.** Fits the "kama is its own manager"
  north star (one artifact). `~/.kama/bin/kama` is a copy of (or symlink to) a real version whose `main`,
  before dispatch, resolves the pin and `execv`s the correct `versions/<v>/bin/kama` if it isn't itself;
  the compile-stamped `KAMA_VERSION` is the loop guard. **Alternative:** a tiny dedicated `kama-shim` (the
  rustup model — smaller, never needs the compiler). ⚠️ **This is the #1 confirm** (design-doc open Q2).
- **Pin lives in `kama.json`** as `"toolchain": "<v>"` — one manifest, one source of truth, and it makes
  the toolchain a first-class reproducible input (vs a separate `.kama-version` file, rustup-style). ⚠️
  Confirm (design-doc open Q1). The selector must read just that one key cheaply *before* running a compiler.
- **`KAMA_HOME` reconciliation (must settle — it's the one real code snag).** The resolvers return
  `$KAMA_HOME` / `$KAMA_HOME/lib` *first*, which would defeat per-version exe-relative resolution if a user
  has `KAMA_HOME` exported. Lean: **demote `KAMA_HOME` to an explicit dev/override escape hatch only** (or
  drop the first branch and rely on exe-relative + a separate `KAMA_VERSIONS_DIR`/store root). The installer
  never exports `KAMA_HOME` today (it only reads it as the install prefix), so real installs are unaffected —
  but this must be explicit so a set `KAMA_HOME` doesn't silently pin every version to one lib tree.
- **Resolution order** = **project pin (`kama.json` `toolchain`) → `KAMA_VERSION` env → global default**;
  **no activate step**, ever.
- **Native first.** Windows: the versioned store means installs never overwrite a running exe (a real win);
  the selector still needs a Windows exec path (`_execv` / spawn+wait). Symlink vs copy for `~/.kama/bin/kama`
  on Windows (junctions don't apply to files) — lean **copy**.
- **Reuse the installer for per-version fetch** (`KAMA_VERSION=<v>` into the store) rather than a new
  download path — one canonical fetch/verify code path, slim-vs-bundled flavor auto-detected as today.

### Open questions (settle at the top of the session)

1. **Selector architecture** — self-re-exec (lean) vs separate `kama-shim` binary. (Doc open Q2.)
2. **Pin location** — `kama.json` `toolchain` field (lean) vs a `.kama-version` file. (Doc open Q1.)
3. **`KAMA_HOME` semantics** — demote to dev-override vs repurpose as the store root vs drop.
4. **Global default record** — a bare `~/.kama/default` file vs a small `~/.kama/settings.json`.
5. **`kama update` vs `kama toolchain`** — does `update` become "install-latest + set-default", or stay the
   self-update-in-place seed and defer entirely to `toolchain`? Avoid two ways to do one thing (GOALS).
6. **Migration** — auto-migrate the existing flat `~/.kama` into `versions/<current>/` on first run, or
   require a re-install? (Lean: auto-migrate, it's a one-time move.)

### Testing — `tools/check-toolchain.sh` (network-free, native-only; model on `check-packages.sh`)

The live installer hits GitHub, so the guard must **not** fetch. Point `HOME` (or a `KAMA_VERSIONS_DIR`, if
introduced) at a tmp dir, hand-populate a fake versioned store with two stub `kama` binaries that just
print their own version, and prove the *selector + resolution*, not the download:

- Two fake versions installed; `kama toolchain list` shows both + the default.
- `default <v>` switches which version an unpinned dir resolves to.
- A project with `"toolchain": "<vA>"` in `kama.json` resolves to `<vA>` even when the default is `<vB>`.
- `KAMA_VERSION=<vB>` overrides the default but **not** a project pin (precedence order).
- A pin/selection to a missing version → a clear error (naming the version + `kama toolchain install`).
- Skip gracefully where a needed tool is absent; never touch the real `~/.kama`.

### Then

M1 closes pillars 1–3 — kama is a self-contained, opt-in toolchain + package manager. Remaining: **M3**
(hosted registry + `kama publish` + namespacing/signing + SemVer range resolution — the M2.2 resolver's
one-spec-per-name check is the range-intersection seam; extends `docs/PUBLISHING.md`) and **M4**
(multi-modal — scripting-runtime versions in the same store, pillar 3's full form via the modality-aware
store key seeded here).
