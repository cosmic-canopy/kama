# Kama toolchain & package management — design of record (kickoff)

**Status: M2 (per-project packages) IN PROGRESS.** Kickoff brief for the campaign after conditional
compilation (`@compileFor`, ✅ shipped 2026-07-24). The user chose to build **M2 first**; the executable
plan for it is the design of record for the build. This doc keeps the vision, seam map, prior art, and
open questions.

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
- M2.1 (content-addressed store + integrity + git/url fetch), M2.2 (resolver + `add`/`remove`/`update`),
  M2.3 (`run` + docs) — pending.

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

M2.2 = resolver (transitive BFS, single-version-per-major, `parseLock` reader) + `add`/`remove`/
`update`. M2.3 = `kama run` + `main` field + docs. See the staged plan above.
