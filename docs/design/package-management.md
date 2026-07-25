# Kama toolchain & package management — design of record (kickoff)

**Status: PREPARED, not built.** Kickoff brief for a fresh session — the campaign after conditional
compilation (`@compileFor`, ✅ shipped 2026-07-24). Goal of this doc: capture the vision, what already
exists to build on, distilled prior art, per-pillar leans, and the open questions — so the next session
can start designing, not re-deriving. Nothing here is implemented yet.

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
