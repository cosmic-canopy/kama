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
- **M3.0 — SemVer parsing + version-range resolution (git-tag deps) ✅ shipped 2026-07-25.** The first
  M3 slice, and the spine every later slice reuses — no registry or hosting needed. A git dep opts into
  ranges with `git` + `version` + **no** `rev` (`isRangeDep`): install runs `git ls-remote --tags`,
  parses tags as `MAJOR.MINOR.PATCH` (leading `v` stripped; pre-release/build-metadata tags skipped, not
  mis-ordered; annotated-tag `^{}` derefs stripped+deduped), and pins the **highest tag satisfying** the
  range. Operators: exact, caret `^` (incl. the correct `^0.x` narrowing), tilde `~`, comparators
  `>=`/`>`/`<=`/`<`, wildcard `*`. All inline in `kama.driver.cpp` (`SemVer`/`VersionReq` normalized to
  one lower/upper bound → `satisfies` is a single interval test; `parseSemVer`/`parseVersionReq`/
  `intersect`/`gitVersionTags`/`selectHighestTag`, ~130 lines, no new file). **Conflict policy**: two
  requestors of one name **intersect** their ranges and take the one highest version satisfying both;
  disjoint → hard error naming both ranges (the existing `sameSpec` exact-source check stays for
  non-range deps; mixing an exact pin and a range for one name is a hard error). The resolver stays a
  flat single-version-per-name BFS but gained a **restart-with-seeded-constraint fixpoint**: because it
  resolves each node on first sight, a later tighter requestor that would pick a *lower* version restarts
  resolution with the tightened range pre-seeded (bounded — a name's chosen version only ever decreases).
  **Lock**: a new optional `LockEntry.version` records the resolved concrete version (`rev` = selected
  tag, `commit` = sha); emitted only when non-empty so existing entries stay byte-identical. **Offline &
  reproducible**: a range dep whose locked `version` still satisfies the manifest range is reused verbatim
  — **no `ls-remote`, no clone** (warm store links offline; cold store re-fetches by the pinned commit);
  `kama pkg update` re-enumerates and can advance. `rev`+`version` together is rejected at manifest parse
  (explicit over implicit). Guard `tools/check-packages.sh` extended (cases 15–18: highest-satisfying
  across ^/~/comparator/*, intersection, a transitive-range **downgrade** exercising the restart path,
  disjoint→hard-fail naming both ranges, offline byte-identical re-install). **Deferred within M3**:
  hosted registry, `kama publish`, namespacing `@scope/name`, signing/provenance, pre-release ordering
  (SemVer §11), compound/hyphen/`||` ranges, multiple versions of one package, url-dep ranges.
- **M3.1a — registry protocol + `kama publish` + registry deps ✅ shipped 2026-07-25.** A **registry
  dependency reduces to a url dependency once resolved**, so it is mostly reuse. The registry is a static
  file tree (Go `GOPROXY` / cargo sparse-index shape): `<base>/<name>/index.json` lists versions
  (`{version, integrity, tarball, dependencies}`), and `tarball` is a URI relative to the base (or
  absolute). A **registry dep** = a bare `version` range with no `git`/`url`/`path` (`isRegistryDep`,
  the sibling gate to `isRangeDep`); an optional per-dep `registry` pins the base. A small hand-parser
  (`IndexReader`, mirroring `LockReader`) reads the index; the resolver's version machinery is unified
  behind `selectVersion` (git ranges enumerate tags, registry deps enumerate the index) so intersection,
  the restart fixpoint, and lock-honoring offline reuse all carry over. A resolved registry dep pins to
  its tarball URI + index integrity and flows through the **existing** `fetchToStore` url path verbatim;
  the lock records `source:"registry"` + base + version + integrity. `kama publish [<dir>] --registry
  <dir-or-file-uri>` tarballs the sources (excluding `.git`/`.kama`/`build`/`kama.lock` into a
  single-component wrapper), hashes them, and splices a **write-once** version entry into the index
  (immutable — refuses to overwrite). `kama pkg add <name> --version <range> [--registry <base>]` writes a
  registry dep. **Network-free** against a `file://` registry; remote-transport publish is M3.3. Guard
  cases 19–21 (publish + immutability, resolve/build/run picking the highest version, transitive from the
  registry, offline byte-identical). Native 756 green; driver ASan/UBSan-clean.
- **M3.1b — scopes + `registries` config + dependency-confusion guard ✅ shipped 2026-07-25.** Scoped
  names `@acme/foo` **import under their bare last segment** (`foo`, the Go/Cargo model) — scope is
  registry-routing metadata only, so the import syntax and view mechanism are unchanged; two scopes
  exposing the same bare name collide on one view link → an explicit hard error (`importNameOf`; the
  store labels a scoped dir with `/`→`_`). A new top-level **`registries`** object (`{ "default":
  <base|[bases]|false>, "@scope": <base|[bases]> }`) configures sources: an explicit per-dep `registry`
  wins, else the scope chain, else the default chain, else the built-in default (a compile constant, not
  wired live until M3.3). Ordered arrays **layer** sources in priority order (the first base with a
  satisfying version wins — a private registry shadows a public one); `default: false` **opts out** of the
  built-in (air-gapped). The **confusion guard** rides the "lock pins content identity, not the URI"
  guarantee: re-pointing a scope to a mirror serving the **same** bytes re-resolves with an unchanged
  integrity; a mirror serving **different** bytes under the same name@version is a hard error (registry
  offline reuse requires the locked base to still be an active candidate, forcing the cross-check on a
  re-point). Guard cases 22–24 (scoped routing + opt-out, re-point same/different bytes, collision).
  Native 756 green; ASan/UBSan-clean.
- **M3.2a — sign-on-publish / verify-on-install ✅ shipped 2026-07-25.** First signing bits via
  **`ssh-keygen -Y` (SSHSIG)** — the mechanism `git commit -S` uses under `gpg.format=ssh`: ed25519,
  namespaced (`kama-registry`), cross-platform, detect-and-skip like git/curl/sha256. `kama publish --key
  <ssh-key>` signs the tarball and records the SSHSIG blob + signer public key in the index entry.
  Verification runs in `fetchToStore` while the tarball still exists (`ssh-keygen -Y check-novalidate`);
  it is **warn-only by default** (a present-but-invalid signature only warns — mechanism before policy)
  and **enforced under `kama pkg install --verify`** (a present signature must verify; a missing one is a
  hard error). The signature + key ride from the index into the resolved spec; offline reuse trusts the
  already-verified store. A configured trust set (TOFU / allowed-signers) is a later M3.2 slice. Guard
  case 25 (signed publish records signature+key; `--verify` passes a signed package, fails a tampered one,
  and warns without it; skips if ssh-keygen is absent). Native 756 green; ASan/UBSan-clean. **Leaves M3.3**
  (deploy the host + wire the live base URI — ops) and the **rest of M3.2** (mandatory verification + the
  trust model) as the only package-management work gated on hosted services.

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
- **M3 — hosted registry + `kama publish` + namespacing/signing + SemVer ranges.** Extends PUBLISHING.md.
  Sliced (like M2) because it mixes self-contained compiler work with real hosted-service/ops work:
  - **M3.0 — SemVer parsing + version-range resolution (git-tag deps). ✅ shipped 2026-07-25** (see the
    as-shipped bullet above). No registry/hosting; the range engine every later slice reuses.
  - **M3.1 — registry *protocol* + `kama publish` (static-first, URI-abstracted). ✅ SHIPPED 2026-07-25
    (M3.1a + M3.1b; see the as-shipped entries above).** The registry is a
    handful of well-known URIs behind a **base URI** (the Go `GOPROXY` / cargo sparse-index model): an
    index (`name → [{version, tarball-uri, integrity, dependencies}]`) + integrity-hashed tarballs. A
    `registry` dep in `kama.json` resolves by fetching the index, running the M3.0 range engine over the
    listed versions, and fetching the chosen tarball into the *existing* content-addressed store. `kama
    publish` packages the project + emits the index entry + tarball. **A static file host AND a dynamic
    service satisfy the identical protocol** — build the static emitter first; self-hosting either kind is
    just a base URI. Fully testable against a `file://` static registry (no live service). Namespacing
    `@scope/name` is a naming convention in the index (this slice or its own sub-slice).
    - **Registry sources — default + layered per-project overrides (user, 2026-07-25).** kama ships a
      **default primary** registry base URI (the official host). A project may declare `registries` in
      `kama.json` as an ordered list to **layer** additional sources; lookup walks them in **priority
      order** (first match wins), and a project can **opt out of the default** (omit/exclude it) for a
      fully-private or air-gapped setup. A base URI is transport-agnostic — `file://` (self-host /
      air-gap / the network-free M3.1 tests), `https://`, etc. — the client only knows the base URI, so
      the same layering works whether a source is a static host or a dynamic service. Mirrors cargo's
      `[registries]` + source-replacement and pip's `--index-url`/`--extra-index-url`, with the default
      being removable. ⚠️ **Dependency-confusion note:** priority-order-first-match means a source
      earlier in the list can shadow a name in a later one; the robust guard is per-scope pinning
      (`@scope/name` → a specific registry, npm-style) so a private scope can't be shadowed by the
      public default. Fold the scope→registry binding into the namespacing sub-slice.
    - **Scope→registry binding is re-pointable, not baked into identity (user, 2026-07-25).** The
      per-scope registry pin is plain `kama.json` config — editable by hand or a `kama pkg`-style
      byte-preserving splice command — so a scope can be **moved to a different registry later** without
      re-resolving. This works because the **lock pins content identity (the integrity hash), not the
      registry URI** — a registry is only *where to fetch the same bytes*. Re-pointing a scope to a
      mirror re-fetches identical content (verified by the existing integrity check) and lands in the
      **same content-addressed store entry** (keyed by tree hash, already location-independent), so the
      lock stays valid and byte-identical. This is cargo source-replacement / Go GOPROXY-mirror. Two
      workflows it enables: (a) **pull-then-self-host** — resolve latest from the default registry, mirror
      those exact packages (name + version + integrity) into an internal static registry, repoint the
      scope, builds verify the same hashes; (b) **isolated / air-gapped dev** — point scopes (or drop the
      default) at a `file://` mirror so `install` never touches remotes (composes with the existing
      "builds never fetch" guarantee — only `install` fetches, and it can be pointed fully offline).
  - **M3.2 — signing / provenance.** **M3.2a ✅ SHIPPED 2026-07-25** (sign-on-publish / verify-on-install
    via `ssh-keygen -Y` SSHSIG, warn-only + `--verify` enforcement; see the as-shipped entry above). The
    **rest of M3.2** picks the trust model — two credible ones: Go's checksum-transparency log vs npm/PyPI's
    sigstore/OIDC provenance attestations — and makes verification mandatory. Shell-out signing (like the
    existing sha256 shell-out); publish signs, install verifies.
  - **M3.3 — hosted deployment (ops; gated on the site being live + repo public).** Stand up the real
    registry host (Cloudflare Pages static index + GitHub Releases/R2 tarballs), wire the default base URI,
    extend PUBLISHING.md. Pure ops — no compiler change; may be preceded by finishing the website. A
    dynamic (Workers/KV/R2 or Node) service is an *optional* drop-in speaking the M3.1 protocol.
- **M4 — multi-modal**: scripting-runtime versions in the store; per-modality env resolution (pillar 3's
  full form). **Deferred until the kama scripting runtime (ROADMAP 2.0 dual-mode scripting) actually
  exists** — until there's a second modality to install, M4 has nothing to version. The store key is
  already modality-aware-seeded (`versions/<kind>-<v>/`, `kind=compiler` today) so no rework is needed
  when a scripting runtime lands.

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

**Status: ✅ SHIPPED.** Pillars 1–3 (after pillar 4). The guiding north star, from the user: **kama is its
own version manager — opt-in.** You never wrap `nvm`/`pyenv`/`rustup` *around* kama; kama itself offers
multi-version management, and a bare single-file `kama build foo.kama` needs none of it. The as-shipped
summary — including how the six open questions resolved — is in **"As shipped"** at the end of this section;
the narrative below is the original kickoff brief. User docs: [../packages.md](../packages.md) §"Toolchain versions".

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

### As shipped (the six open questions, resolved)

All native (Linux/macOS branches compiled; Windows branches present, native-first per the plan). Landed in
`kama.driver.cpp`, `install.sh`, `install.ps1`, `tools/check-toolchain.sh` (wired into `run_tests.sh`).

1. **Selector = self-re-exec** (not a separate shim). But the loop-guard is *not* the version stamp — it's
   **"am I the selector binary?"**: `maybeReExec` compares `selfExePath()` (`/proc/self/exe` on Linux,
   `_NSGetExecutablePath` on macOS, `_get_pgmptr` on Windows — argv[0] is unreliable via PATH) against
   `~/.kama/bin/kama`. Only the selector re-execs; a per-version binary — or a dev/repo `./kama` — runs in
   place. This is both the loop-stopper (the re-exec target is a different path) and what stops a dev build
   from silently handing off to an installed toolchain. The selector is *only* ever a hand-off (it has no
   sibling `include`/`lib`), so it always execs the versioned location. ⚠️ **The re-exec must set
   `argv[0] = versionBin(v)`** — the versioned compiler resolves its runtime/stdlib exe-relative from
   `argv[0]`, so passing the selector's argv[0] made it look in `~/.kama/include` (nonexistent) and every
   build failed. Caught only by a *real*-compiler end-to-end (stubs can't surface it).
2. **Pin = `kama.json` `"toolchain"`** — read via `loadManifestToolchain` (mirrors the `main` idiom),
   written by `kama toolchain pin` via a byte-preserving top-level-string splice (`manifestSetTopString`,
   reusing the pkg-add helpers). Resolution walks up from the cwd to find the manifest.
3. **`KAMA_HOME` resolver override — DROPPED** (not demoted). Nothing used it (the store isolates via
   `KAMA_STORE`; dev/container builds are exe-relative), and it was a hazard (a stray export would pin every
   version to one lib tree). `KAMA_HOME` survives *only* as the installer's install-prefix knob.
4. **Global default = a bare one-line `~/.kama/default`** (not a settings.json).
5. **`kama update` = install-latest + set-default.** `runInstaller(version, makeDefault)` is the one fetch
   path; `update` passes `makeDefault=true`, `toolchain install` passes `false`. The installer refreshes the
   selector + default on `KAMA_SET_DEFAULT=1` **or** the first-ever install (so there's always a default).
6. **Migration — none.** No kama in the wild yet, so the installer just lays down the versioned layout fresh.
   `toolchain default` does *not* refresh the selector (the always-hand-off design makes matching pointless);
   only the installer / `kama update` refresh it.

`KAMA_VERSION` keeps three coherent faces: the compile-stamped macro (identity / `--version`), the installer
env ("install this release"), and the selector's middle tier ("run this, just this once").

### Then

M1 closes pillars 1–3 — kama is a self-contained, opt-in toolchain + package manager. Remaining: **M3**
(hosted registry + `kama publish` + namespacing/signing + SemVer range resolution — the M2.2 resolver's
one-spec-per-name check is the range-intersection seam; extends `docs/PUBLISHING.md`) and **M4**
(multi-modal — scripting-runtime versions in the same store, pillar 3's full form via the modality-aware
store key seeded here).

---

## M3.1 + M3.2 — implementation kickoff (registry protocol + `kama publish` + first signing bits)

**Status: ✅ SHIPPED 2026-07-25 (M3.1a `2d18348`, M3.1b `c8c9444`, M3.2a `144d422`).** The as-shipped
summaries are in the milestone list near the top of this doc; this section is retained as the design of
record (the leans below were confirmed as built, with only minor deviations noted inline). M3.0 (SemVer
ranges, `19ebadf`) is the range engine this reused. Everything here was built + verified **network-free
against a `file://` registry** — **nothing was gated on the website.** Only M3.3 (deploying the real host +
wiring the default base URI to a live URL) waits on the site / the repo being public; that's pure ops, no
compiler change.

**As-built deviations from the leans below:** transitive resolution reads the fetched tarball's own
manifest (the url-dep path) rather than the index `dependencies` — kama's greedy highest-version BFS never
downloads a non-chosen candidate, so the index-metadata optimization wasn't needed for resolution; the
index still *records* `dependencies` for protocol conformance. Registry-package addressing at a base uses
the **full** (scoped) name (`<base>/@acme/foo/index.json`), while the **import name + view link + store
label** use the bare last segment. Signing settled on `ssh-keygen -Y check-novalidate` for the crypto
check (a configured trust set is deferred to the rest of M3.2).

### The core idea (why this is mostly reuse)

A **registry dependency reduces to a url dependency once resolved.** Resolution = fetch the package's
index → run the **M3.0 range engine** over the listed versions → the chosen version's index entry yields a
**tarball URI + integrity**, which feeds the *existing* url path of `fetchToStore` (`kama.driver.cpp:1287`,
curl + `tar --strip-components=1` + `treeHashOf` + integrity verify + content-addressed store) **verbatim**.
So the new surface is small: registry-base resolution (config), index fetch+parse, a `registry` source kind
on `DepSpec`/`LockEntry`, and `kama publish` (which is the inverse — tarball a project + write an index
entry). The transitive BFS + range intersection + restart fixpoint + lock-honoring offline reuse all carry
over unchanged.

### Staging (small, committable checkpoints — mirror the M2/M3.0 rhythm)

- **M3.1a — protocol + unscoped registry deps + resolution + `kama publish` (to a `file://`/dir registry).**
  The spine. No scopes yet, no signing yet.
- **M3.1b — namespacing `@scope/name` + `registries` config (default + per-scope binding + priority-order
  layering + opt-out-of-default) + the dependency-confusion guard + re-pointable scopes.** The settled
  decisions live in the M3.1 registry-sources notes above — implement them here.
- **M3.2a — first signing bits: sign-on-publish + verify-on-install (local keys).** The rest of M3.2
  (trust model: transparency-log vs sigstore/OIDC provenance) is a later slice.

### The registry protocol (settle the schema first)

A base URI + a handful of well-known, **static-serveable** paths (Go `GOPROXY` / cargo sparse-index model;
a dumb file tree or a dynamic service satisfy the identical shape; `file://` works for tests + air-gap):

- **Index (metadata):** `<base>/<name>/index.json` — the published versions of one package:
  ```json
  { "name": "geo", "versions": [
      { "version": "1.2.0", "integrity": "sha256-<tarball-hash>",
        "tarball": "geo/1.2.0.tar.gz", "dependencies": { "mathx": { "version": "^1.0.0" } } }
  ] }
  ```
  `tarball` is a URI **relative to `<base>`** (or absolute), so metadata and artifacts can live on
  different hosts (e.g. index on Pages, tarballs on GitHub Releases/R2 — matches the existing infra).
  `dependencies` are recorded in the index so a consumer resolves the transitive graph from metadata
  **without downloading every candidate** (same discipline as the lock's serialized `dependencies[]`).
- **Artifact:** whatever `tarball` points at — a gzipped tar of the package sources (the `url`-dep format).

Immutability: a published `<name>@<version>` is write-once (its integrity is pinned forever). `publish`
**refuses to overwrite** an existing version — a re-publish requires a version bump. This is the
lockfile-drift / dependency-confusion guarantee at the source.

### Where the seams are (`kama.driver.cpp`, anchors fresh at `19ebadf`)

- `struct DepSpec` (388) — add a `registry` source kind. A dep with `version` + neither `git`/`url`/`path`
  → a **registry dep** (resolved via configured registries). Optional explicit `"registry": "<base>"` to
  pin a source. (`isRangeDep`, 573, is the M3.0 opt-in gate — the registry path is its sibling.)
- `bool depsObject` (619) — parse the new `registry` key; the mutual-exclusion validation lives here
  (registry dep = version, no git/url/path).
- `struct LockEntry` (748) — add `source == "registry"`: record `registry` (base), `version`, `integrity`,
  `tarball`, `dependencies[]`. Writer/reader mirror the git/url fields (byte-identical discipline).
- `fetchToStore` (1256) — **no change needed**: a resolved registry dep is handed to it as a `url` dep
  (`d.url = <resolved tarball URI>`, `d.integrity = <index integrity>`). Reuses curl/tar/hash/store as-is.
- `resolveOne` (1330) / `resolveProject` (1426) — add a `registry` branch parallel to `isRangeDep`: resolve
  the base(s) → fetch+parse index → select highest satisfying version (reuse `parseVersionReq`/`satisfies`/
  a `selectHighest`-over-index-versions analog of `selectHighestTag`, 1290-region) → set the tarball
  `url`+`integrity` → existing fetch. Intersection/restart/lock-honoring all reuse M3.0. Offline reuse: a
  registry dep whose locked `version` still satisfies the range is reused verbatim (no index fetch), exactly
  like the M3.0 git-range offline path (`resolveProject`, the `oldLock` reuse block).
- Index fetch/parse — a small new hand-parser (mirror `LockReader`, 802) or reuse `ManifestReader`'s idioms;
  fetch via `runCmdCapture("curl -fsSL …")` or curl-to-file (both already used).
- `kama publish` — a new subcommand next to `pkg`/`toolchain`/`run` dispatch (`subcommand == "pkg"`, 2133).
  Reuse: `treeHashOf`/`sha256Of` (1201/1165), `tar -czf` (the test harness already does this), `makeDirs`,
  the manifest reader for `name`/`version`/`dependencies`. Refuse to overwrite an existing index version.
- `registries` config — read from `kama.json` (a new top-level object; `ManifestReader`, add a capture like
  `mainOut`/`toolchainOut`). Re-pointing a scope = the byte-preserving `manifestSetTopString`/`manifestAddDep`
  splice family (1753/1929). A built-in default base URI is a compile constant (overridable) — like
  `KAMA_VERSION`; **do not** wire it to a live URL until M3.3.

### `kama publish` (M3.1a shape)

`kama publish --registry <dir-or-file-uri>` (remote-transport publish deferred to M3.3):
1. Read `kama.json` → `name`, `version`, `dependencies`.
2. `tar -czf` the project sources into a wrapper dir (exclude `.kama/`, `.git/`, build artifacts) → compute
   `sha256Of` (tarball integrity).
3. Refuse if `<registry>/<name>/index.json` already lists `version` (immutability).
4. Copy the tarball to `<registry>/<name>/<version>.tar.gz`; append the version entry (version, integrity,
   tarball, dependencies) to `<registry>/<name>/index.json` (create it if absent), written deterministically.

This makes "resolve latest from the default registry → mirror those exact packages into an internal
`file://`/dir registry → repoint the scope" a pure file operation, and the whole loop is testable offline.

### M3.2a — first signing bits (leans)

- **Mechanism = shell-out** (like `sha256Of`): sign the tarball (or its integrity string) on `publish`,
  verify on `install`. Candidate tools, in order of availability on the base image: `ssh-keygen -Y sign`/
  `-Y verify` (OpenSSH, usually present) or `minisign` (tiny, ed25519, the cleanest UX) — **detect and skip
  gracefully** in the guard (like git/curl/sha256). Settle the tool at kickoff.
- **Index carries** `signature` + a key id per version entry; `publish` signs with a publisher key;
  `install` verifies against a trusted key (TOFU first, a configured trust set later). Start **warn-only or
  opt-in** enforcement so the mechanism lands before the policy; a later M3.2 slice makes it mandatory +
  picks the trust model (transparency-log vs sigstore/OIDC provenance).

### Decisions (leans — confirm at the top of the session, don't re-derive)

- **`registry` dep = `version` with no git/url/path** (registry is the default source for a bare
  versioned name). Explicit `"registry": "<base>"` pins one. One way: a name is a git-range dep XOR a
  registry dep XOR an exact pin — mixing is a hard error (extends the M3.0 `rev`+`version` rule).
- **Index entry carries `dependencies`** so transitive resolution reads metadata, not tarballs.
- **Immutable versions**; `publish` refuses overwrite.
- **Default base URI = a compile constant**, overridable by `registries.default`; **not** wired live until
  M3.3. Layering/opt-out/scope-binding + confusion guard per the M3.1 registry-sources notes above.
- **Publish targets a `file://`/dir registry in M3.1**; remote-transport publish is M3.3.
- **Namespacing staged to M3.1b** — M3.1a ships unscoped names to keep the first slice small. The
  `@scope/name` import-path question (does `@acme/foo` import as `foo`, and how two scopes with the same
  name disambiguate) is a real cut to settle in M3.1b.

### Open questions (settle at the top of the session)

1. **Index granularity** — one `index.json` per package (sparse, lean) vs a single registry-wide index.
   Lean: per-package (static-serveable, scales, matches cargo sparse).
2. **`registries` schema** — object with `default` + `@scope` keys (values a base URI or an ordered array
   for layering; `default: false`/omit to drop it) vs a list of `{match, uri}` rules. Lean: the object form.
3. **`@scope/name` import path** (M3.1b) — bare last segment vs `scope::name`; collision disambiguation.
4. **Signing tool** — `ssh-keygen -Y` (present) vs `minisign` (cleaner). Lean: whichever is on the base
   image; detect + skip in the guard.
5. **Publish auth** for the eventual remote (M3.3) — token model; out of scope for M3.1/M3.2a.

### Testing — extend `tools/check-packages.sh` (network-free, `file://` registry)

Model on the M3.0 cases (a local `gv` repo + `file://`). Build a `file://` **static registry** as a dir
tree (`reg/<name>/index.json` + `reg/<name>/<version>.tar.gz`) — either hand-written or produced by `kama
publish` (dogfood it):
- **M3.1a:** `publish` a package to a dir registry → assert `index.json` + tarball exist, integrity recorded,
  immutability (a second `publish` of the same version fails). A consumer with a `registry` dep + a range
  resolves the highest version from the index, fetches the tarball into the store, links the view, and the
  built program runs; the lock records `source:"registry"`, version, integrity. Transitive: a published
  package depending on another registry package resolves both from metadata. Offline: re-install with the
  registry dir removed but the store warm + lock kept → byte-identical, no fetch.
- **M3.1b:** a scoped `@acme/foo` resolves via its bound registry, not the default; opt-out-of-default makes
  an unscoped name unresolvable; re-pointing a scope to a mirror serving the same integrity re-resolves
  byte-identically; a mirror serving *different* bytes under the same name+version → integrity hard-fail
  (the confusion guard).
- **M3.2a:** a signed `publish` + a verifying `install` pass; a tampered signature fails (skip gracefully if
  the signing tool is absent).

### Then

M3.1+M3.2a leave only **M3.3** (deploy the real registry host + wire the default base URI — ops, gated on
the site/public repo) and the **rest of M3.2** (mandatory verification + the chosen trust model). **M4**
(multi-modal) stays deferred until the scripting runtime exists.
