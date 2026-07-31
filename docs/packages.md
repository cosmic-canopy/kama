# Packages & projects — a quickstart

Single-file programs need no ceremony — `kama build hello.kama -o hello && ./hello`. Once a
program spans several files or pulls in a dependency, a **project** gives you a manifest, a
reproducible lockfile, and a one-step `kama run`.

A project is just a directory with a `kama.json` manifest. Nothing is global: dependencies
resolve *into the project* (`.kama/`), the lockfile pins them, and builds are a pure read of
that resolved view — they never reach the network.

## A minimal project

```
myapp/
  kama.json
  src/app.kama
```

```json
// kama.json
{
  "name": "myapp",
  "version": "0.1.0",
  "main": "src/app.kama"
}
```

```kama
// src/app.kama
fn int32 main() {
    return 0;
}
```

Build and run it in one step — `kama run` uses the manifest's `"main"` when you don't pass a
file:

```sh
cd myapp
kama run                 # builds src/app.kama and runs it; the program's exit code is forwarded
kama run src/app.kama    # explicit file — same thing
```

`kama run` is a thin wrapper over `kama build`: it compiles a native executable to a temp
location, runs it, forwards the exit code, and cleans up. It's **native-only** (wasm needs a
browser/node, a bare-metal target emits a freestanding object) — for those, use `kama build --target …`;
see [targets.md](targets.md).

## Telling the tooling what your project contains — `sources` and `packages`

Both keys are **optional**, and both exist to replace an inference with a declaration. Editor tooling
(the language server) has to know which files make up your project before it can safely do a
project-wide operation like renaming a symbol across files. Without a declaration it has to infer —
"every `.kama` under here" — and an inference can be wrong, so it is **capped at 500 files**; past that,
cross-file rename refuses rather than answer from a set it doesn't trust. Declaring removes the guess,
and with it the cap.

**`sources`** — which files are this package's, relative to the manifest. Each entry is a directory
(searched recursively) or a single `.kama` file:

```json
{
  "name": "myapp",
  "version": "0.1.0",
  "main": "src/app.kama",
  "sources": ["src"]
}
```

Now `examples/`, `tests/` and scratch files beside them are not part of the package, so a rename can
never reach into them and a same-named type over there can never be confused with yours.

`sources` is also how **importers** find your files. Without it, a package is importable only if its
`.kama` files sit directly in its root directory — so a library laid out with `src/` needs this key to be
importable at all.

**`projects`** — the sub-projects this manifest composes, each a directory with its own `kama.json`. This
is how you declare a monorepo. A trailing `/*` expands to every immediate subdirectory that has a
manifest. (It is *not* called `packages`: `kama.lock` already uses that key for resolved dependencies, and
the distinction is the useful one — **packages are what you consume, projects are what you compose**. For
the same reason, name the directory something like `libs/` rather than `packages/`.)

```json
// the workspace root
{
  "name": "acme",
  "version": "0.1.0",
  "projects": ["libs/*", "tools/codegen"]
}
```

```
acme/
  kama.json                <- projects: ["libs/*", "tools/codegen"]
  libs/
    core/kama.json         <- sources: ["."]
    ui/kama.json           <- sources: ["."]
  tools/codegen/kama.json
  scratch/notes.kama       <- claimed by nobody: never indexed
```

`projects` is **recursive** — a sub-project may declare sub-projects of its own, so monorepos nest to any
depth — and cycles are broken automatically, so a manifest naming a directory that names it back is
harmless. A manifest with `projects` but no `sources` is a pure aggregator: it contributes no files
itself, only its sub-projects'. Editing a file in `libs/core` then makes the whole workspace the
rename scope, so renaming a type there correctly updates `libs/ui`.

### Sub-projects are self-contained — declare what you import

A sub-project should be **extractable**: liftable out of the monorepo to stand alone. That requires it to
declare every dependency it *imports*, not merely to be built alongside one that does. So a member
declares its siblings the same way it declares anything else — as a path dependency:

```json
// libs/net/kama.json — net imports config, so net declares config
{
  "name": "net",
  "version": "0.1.0",
  "sources": ["."],
  "dependencies": { "config": { "path": "../config" } }
}
```

Path dependencies are otherwise top-level-only, because a *fetched* package cannot reference a local path
reproducibly. Between two members of one declared `projects` tree that objection does not apply — the
workspace carries them both — so a path dependency there is permitted, and one pointing outside the
workspace is still refused.

Each package's imports are checked against **its own** manifest. If `libs/net` imports `config` while only
`apps/server` declares it, the build **fails** and names the line to add:

```
kama: error: libs/net/net.kama imports module 'config', but its own package
  (libs/net/kama.json) does not declare it — only apps/server/kama.json does, so
  this package will not build on its own.
kama: note: add to libs/net/kama.json: "dependencies": { "config": { "path": "../config" } }
```

An import that *nothing* declares is also an error, as it always was. A single-package project never meets
this rule at all: an import resolved through your own directory needs no declaration.

Your **editor** is not held to it. `kama lsp` (and the `kama query` CLI that mirrors it) reports the same
thing but keeps analyzing — refusing would strip cross-module hover and go-to-definition over a manifest
problem, when the code itself is fine and resolves.

The property is mechanically checkable, and worth wiring into CI: install and build each member from its
own directory, with no ancestor manifest in play.

```sh
for m in libs/config libs/net apps/server; do
    (cd "$m" && kama pkg install && kama check *.kama) || exit 1
done
```

Without either key nothing breaks — the tooling infers the file set as before. These keys buy precision
and remove the cap. If you have a large tree that genuinely is one program and you'd rather not declare
it, `KAMA_LSP_MAX_FILES` raises the inference cap (`0` = no limit).

## Adding a dependency

Dependencies come from a local path, a git repo, or a tarball URL. Add one with `kama pkg
add` (which edits `kama.json` and installs), or write it into the manifest by hand:

```sh
kama pkg add geo --git https://example.com/geo.git --rev v1.0.0
kama pkg add mathx --url https://example.com/mathx-1.2.0.tar.gz
kama pkg add utils --path ../utils        # a sibling checkout
```

```json
// kama.json
{
  "name": "myapp",
  "version": "0.1.0",
  "main": "src/app.kama",
  "dependencies": {
    "geo":   { "git": "https://example.com/geo.git", "rev": "v1.0.0" },
    "mathx": { "url": "https://example.com/mathx-1.2.0.tar.gz" },
    "utils": { "path": "../utils" }
  }
}
```

Import a dependency by its package name; the build only lets you import what the manifest
declares (an undeclared import is a hard error — no phantom dependencies):

```kama
import geo::{area};
```

Install resolves the whole dependency graph (transitively) and writes the lockfile:

```sh
kama pkg install         # materializes .kama/deps + kama.lock
kama run                 # build + run against the resolved view
```

Other manifest surgery: `kama pkg remove <name>` drops a dependency; `kama pkg update
[<pkg>]` re-resolves pins (e.g. advances a branch) and rewrites the lock without touching
the manifest.

### Version ranges (git tags)

A git dependency can pin a **version range** instead of an exact `rev`. Give it a `version`
and no `rev`, and install resolves the **highest tag** that satisfies the range (tags are read
as SemVer, an optional leading `v` stripped):

```json
{
  "dependencies": {
    "geo": { "git": "https://example.com/geo.git", "version": "^1.2.0" }
  }
}
```

Supported ranges: exact `1.2.3`; caret `^1.2.3` (compatible-with — up to the next major, or the
next minor/patch for `^0.x`); tilde `~1.2.3` (up to the next minor); the comparators `>=` `>`
`<=` `<`; and `*` (any). Only `MAJOR.MINOR.PATCH` tags are candidates — pre-release/build-metadata
tags (e.g. `v1.3.0-rc1`) are ignored.

The lock pins the **concrete** version and commit the range resolved to, so builds stay
reproducible and offline — re-installing an unchanged project reuses the locked version without
contacting the remote. `kama pkg update` re-resolves and can advance to a newer satisfying tag. A
git dependency takes **either** `rev` (exact) **or** `version` (a range), never both. When two
packages request the same dependency with different ranges, the resolver intersects them and picks
the one highest version satisfying both; if no version satisfies all requestors, it's a hard error
naming both ranges.

## Registry dependencies

A **registry dependency** names just a `version` range — no `git`/`url`/`path`. It resolves through a
**registry**: a package index that lists published versions and their tarballs. Give it an explicit
`registry` base, or configure one (see below):

```json
{
  "dependencies": {
    "geo": { "version": "^1.2.0", "registry": "file:///srv/kama-registry" }
  }
}
```

Install fetches the registry's index for `geo`, runs the same range engine as git-tag ranges over the
listed versions, picks the **highest satisfying** one, and fetches its tarball into the content-addressed
store — so a registry dep behaves exactly like a url dep once resolved, and the lock pins the concrete
version + integrity. `kama pkg add geo --version ^1.2.0 --registry <base>` writes one for you.

A registry base is transport-agnostic — `file://` (self-host / air-gap / offline testing) or `https://`.
The registry is a plain static file tree, so any file host serves it.

### Publishing

`kama publish` packages the current project and records it in a registry:

```sh
kama publish --registry file:///srv/kama-registry
```

It tarballs the sources (excluding `.git/`, `.kama/`, `build/`, and `kama.lock`), hashes them, and adds a
version entry to `<registry>/<name>/index.json` alongside the tarball. **Published versions are
immutable** — re-publishing an existing version is refused; bump the `version` in `kama.json` instead.

### Scopes and the `registries` config

A package name may be **scoped** as `@scope/name`. A scoped dependency **imports under its bare last
segment** — `@acme/geo` is `import geo::{…}` in your code — the scope only selects which registry serves
it. Bind scopes (and the default) with a top-level `registries` object:

```json
{
  "registries": {
    "default": "https://packages.example.com",
    "@acme":   "file:///srv/acme-registry"
  },
  "dependencies": {
    "@acme/geo": { "version": "^1.0.0" },
    "mathx":     { "version": "^2.0.0" }
  }
}
```

- A **scoped** name (`@acme/geo`) resolves through its bound registry; an **unscoped** name (`mathx`)
  resolves through `default`.
- A value may be an **ordered array** of bases to **layer** sources — lookup walks them in priority order
  and the first base that has a satisfying version wins (a private registry shadows a public one).
- `"default": false` **drops** the default entirely — a fully-private / air-gapped setup where an unscoped
  name with no other source is simply unresolvable.
- Two scopes cannot expose the **same** bare name in one project (both would import as `name`) — that's a
  hard error; alias one by renaming.

Because the lock pins **content identity (the integrity), not the URL**, a scope can be **re-pointed** to a
mirror by editing `registries`: a mirror serving the same bytes re-resolves identically. A mirror serving
**different** bytes under the same `name@version` is rejected (the dependency-confusion guard).

### Signing (optional)

`kama publish --key <ssh-key>` signs the tarball with an SSH key (via `ssh-keygen -Y`, the same SSHSIG
mechanism `git commit -S` uses) and records the signature + signer public key in the index. On the
consuming side:

```sh
kama pkg install            # warn-only: a bad signature warns, the install proceeds
kama pkg install --verify   # enforced: a signature must be present and verify, else it fails
```

Verification needs `ssh-keygen` on `PATH`; where it's absent, signing and verification skip gracefully.

## Dev-dependencies

Dependencies needed only for development (test kits, fixtures, tooling) go under
`dev-dependencies`. They are **off the import path for normal builds** and only resolve/link
under `--dev`:

```json
{
  "dev-dependencies": {
    "testkit": { "git": "https://example.com/testkit.git", "rev": "v1.0.0" }
  }
}
```

```sh
kama pkg add --dev testkit --git https://example.com/testkit.git --rev v1.0.0
kama pkg install         # also materializes .kama/dev-deps
kama run --dev           # dev-dependencies on the import path
kama build src/app.kama --dev
```

A production build that tries to import a dev-dependency fails to resolve it — the boundary
is enforced, not advisory. Dev-dependencies are also strictly non-transitive: a package you
depend on never drags *its* dev-dependencies into your build.

## Reproducibility — the lockfile and the store

- **`kama.lock`** records the exact resolved graph: each package's source, its pinned
  identity (git commit sha / tarball integrity hash), and its direct dependencies. It's
  deterministic — re-installing an unchanged project produces a byte-identical lock. Commit
  it.
- Fetched packages live in a **content-addressed store** (`~/.kama/store`, overridable with
  `$KAMA_STORE`), keyed by the hash of their unpacked contents and shared across projects.
  `.kama/deps/<name>` links into the store.
- Integrity is verified on fetch: git deps pin the commit and the tree hash; url and registry
  deps verify the tarball's sha256 (trust-on-first-use when no `integrity` is given, hard-fail on
  mismatch thereafter). A registry dep records the base it resolved from, but the lock pins the
  **integrity**, not the URL — so a scope can be re-pointed to a mirror without invalidating it.
- **Builds never fetch.** They read the already-materialized `.kama/deps` view. A build that
  finds declared dependencies but no resolved view tells you to run `kama pkg install` — it
  never silently reaches the network. Once a project is installed, everything works offline.

## Local overrides — `kama.local.json`

A `kama.local.json` sitting next to `kama.json` is a **per-machine override that deep-merges over
the manifest**. It's for settings that are yours, not the project's — so it is **gitignored by
convention** (the build never records it in `kama.lock`, and it can't perturb a reproducible or CI
build; that's the whole point).

Deep-merge means it layers field-by-field, not whole-file: a `log.tags` entry it adds sits
*alongside* the project's tags rather than replacing them, a scalar it sets wins, and a `flags`
entry it declares *extends* the valid flag universe.

```jsonc
// kama.local.json — bend the build to this machine without touching the committed manifest
{
  "log":        { "level": "debug", "tags": { "audio": "trace" } },  // build-time (compiler)
  "flags":      { "MY_EXPERIMENT": { "default": true } },            // build-time (compiler)
  "select":     { "TARGET": { "RPI": { "triple": "aarch64-linux-gnu",  // build configuration groups
                                       "cc": "aarch64-linux-gnu-gcc" } },
                  "BUILD_TYPE": { "FAST": { "inherits": "RELEASE" } } },
  "overrides":  { "geometry": { "path": "../geometry" } },           // install-time (kama pkg install)
  "registries": { "default": ["file:///srv/mirror"] },              // install-time (kama pkg install)
  "toolchain":  "1.3.0"                                              // selector (which compiler runs)
}
```

Precedence, high to low: **`--log`/`KAMA_LOG` (runtime) > `kama.local.json` > `kama.json`**.

**Build-time fields** (`log`, `flags`, `select`) are read by the compiler and merge field-by-field, as
above. **`select`** declares this project's build-configuration groups — extra targets (each with an
optional cross toolchain), extra build types, and any single-select axis of your own. It is described
in full in [SPEC.md](SPEC.md#conditional-compilation--compileforflag-), with the practical
toolchain setup in [targets.md](targets.md); the short version is that a
group takes exactly one value, that value's name becomes a `@compileFor` flag, and `--select
GROUP=VALUE` picks it. A local manifest may add targets and change defaults, so a developer can point
a cross target at their own sysroot without editing the committed file.

**Install-time fields** are read by `kama pkg install`:

- **`overrides`** — redirect a dependency to a **local directory** for local development (Cargo
  `[patch]` / Go `replace`). The dep stays declared in `kama.json`; the override just relinks the
  materialized view (`.kama/deps/<name>`) at your local copy, so your build compiles the local code.
  It is **never written to `kama.lock`** — the lock stays canonical, so CI (which has no
  `kama.local.json`) reproduces the published resolution exactly. The overridden package must still be
  a declared, resolvable dependency and a drop-in for it (its own *new* dependencies aren't re-followed).
- **`registries`** — a local registry config that layers over `kama.json`'s (same shape: a `default`
  chain and per-`@scope` chains). Point a scope at a private mirror, or supply a `default` a checkout
  doesn't commit. Lock-safe for free: the lock pins content integrity, not the registry URI.

The **`toolchain`** field is read by the PATH selector and overrides the committed pin locally (see
below).

## Toolchain versions

kama is its own version manager — there's no `nvm`/`pyenv`/`rustup` to wrap around it. Each
installed compiler version lives in its own directory under `~/.kama/versions/<v>/`, and the
`kama` on your `PATH` is a thin **selector**: every command resolves *which* version to run for
the current directory and hands off to it. There is no `activate` step.

Resolution order, highest priority first:

1. **Local override** — a `"toolchain"` field in `kama.local.json` beside the project's `kama.json`.
   Gitignored and dev-local, for testing this checkout under a different version without touching the
   committed pin.
2. **Project pin** — a `"toolchain"` field in the project's `kama.json` (found by walking up from
   the current directory). This makes the toolchain version a reproducible build input, alongside
   the source, `kama.json`, and `kama.lock`.
3. **`KAMA_VERSION`** environment variable — a one-off override for the current command, without
   editing any file (handy for a project that has no pin, or to test a build under another version).
4. **Global default** — recorded in `~/.kama/default`, used when nothing else applies.

```sh
kama toolchain list              # installed versions, the default (*), and what this dir resolves to
kama toolchain install 1.3.0     # add a version alongside (doesn't change the default)
kama toolchain default 1.3.0     # set the global default
kama toolchain pin 1.3.0         # pin THIS project — writes "toolchain": "1.3.0" into kama.json
kama toolchain uninstall 1.2.0   # remove a version (refuses to remove the current default)

kama update                      # install the latest and make it the default
kama update --version 1.3.0      # install a specific version and make it the default
```

A pin or selection to a version you don't have installed fails with a clear message telling you to
run `kama toolchain install <v>` — it never silently falls back to another version.

## Command reference

| Command | What it does |
|---|---|
| `kama run [<file>] [-- <args>]` | Build the entry (explicit file, else manifest `"main"`) and run it; native-only. |
| `kama build <file>… [--dev]` | Build a native/wasm/embedded artifact. |
| `kama pkg install [<dir>] [--verify]` | Resolve `kama.json` (dev-)dependencies into `.kama/{deps,dev-deps}` + `kama.lock`; `--verify` requires + checks registry signatures. |
| `kama pkg add [--dev] <name> (--git U [--rev R \| --version V] \| --url U [--integrity H] \| --path P \| --version V [--registry BASE])` | Add a dependency and install (bare `--version` = a registry dep). |
| `kama pkg remove <name>` | Drop a dependency and install. |
| `kama pkg update [<pkg>]` | Re-resolve pins and rewrite the lock. |
| `kama publish [<dir>] --registry <base> [--key <ssh-key>]` | Tarball the project + record (and optionally sign) it in the registry index. |
| `kama toolchain list` | Installed versions, the global default, and what the current dir resolves to. |
| `kama toolchain install <v>` | Install version `<v>` into `~/.kama/versions/<v>` (alongside; keeps the default). |
| `kama toolchain uninstall <v>` | Remove an installed version (refuses the current default). |
| `kama toolchain default <v>` | Set the global default version. |
| `kama toolchain pin <v>` | Pin this project's toolchain in `kama.json`. |
| `kama update [--version <v>]` | Install the latest (or `<v>`) and make it the default. |
