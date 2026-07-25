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
browser/node, embedded emits a freestanding object) — for those, use `kama build --target …`.

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
- Integrity is verified on fetch: git deps pin the commit and the tree hash; url deps verify
  the tarball's sha256 (trust-on-first-use when no `integrity` is given, hard-fail on
  mismatch thereafter).
- **Builds never fetch.** They read the already-materialized `.kama/deps` view. A build that
  finds declared dependencies but no resolved view tells you to run `kama pkg install` — it
  never silently reaches the network. Once a project is installed, everything works offline.

## Toolchain versions

kama is its own version manager — there's no `nvm`/`pyenv`/`rustup` to wrap around it. Each
installed compiler version lives in its own directory under `~/.kama/versions/<v>/`, and the
`kama` on your `PATH` is a thin **selector**: every command resolves *which* version to run for
the current directory and hands off to it. There is no `activate` step.

Resolution order, highest priority first:

1. **Project pin** — a `"toolchain"` field in the project's `kama.json` (found by walking up from
   the current directory). This makes the toolchain version a reproducible build input, alongside
   the source, `kama.json`, and `kama.lock`.
2. **`KAMA_VERSION`** environment variable — a one-off override for the current command, without
   editing any file (handy for a project that has no pin, or to test a build under another version).
3. **Global default** — recorded in `~/.kama/default`, used when nothing else applies.

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
| `kama pkg install [<dir>]` | Resolve `kama.json` (dev-)dependencies into `.kama/{deps,dev-deps}` + `kama.lock`. |
| `kama pkg add [--dev] <name> (--git U [--rev R] \| --url U [--integrity H] \| --path P)` | Add a dependency and install. |
| `kama pkg remove <name>` | Drop a dependency and install. |
| `kama pkg update [<pkg>]` | Re-resolve pins and rewrite the lock. |
| `kama toolchain list` | Installed versions, the global default, and what the current dir resolves to. |
| `kama toolchain install <v>` | Install version `<v>` into `~/.kama/versions/<v>` (alongside; keeps the default). |
| `kama toolchain uninstall <v>` | Remove an installed version (refuses the current default). |
| `kama toolchain default <v>` | Set the global default version. |
| `kama toolchain pin <v>` | Pin this project's toolchain in `kama.json`. |
| `kama update [--version <v>]` | Install the latest (or `<v>`) and make it the default. |
