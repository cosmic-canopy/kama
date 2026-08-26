# Packages & projects — a quickstart

Single-file programs need no ceremony — `kama build hello.kama -o hello && ./hello`. Once a
program spans several files or pulls in a dependency, a **project** gives you a manifest, a
reproducible lockfile, and a one-step `kama run`.

A project is just a directory with a `kama.json` manifest. Nothing is global: dependencies
resolve *into the project* (`.kama/`), the lockfile pins them, and builds are a pure read of
that resolved view — they never reach the network.

## A minimal project

`kama seed` writes one, so the manifest below is something to read rather than something to type:

```sh
kama seed myapp            # prompts, with a sensible default on every answer
kama seed myapp --yes      # take every default; also what a script or CI gets
```

It asks for a name, a version, and a **kind** — `executable`, `library`, or `monorepo` — then writes the
manifest, a starter source file, a `.gitignore`, and a README stub. Prompting happens only when it has a
terminal to prompt on; a pipe, a script or a CI runner behaves as `--yes`, so `kama seed` never hangs a
build. Every answer also has a flag (`--name`, `--version`, `--kind`, `--members`), and `--agents` adds
the [AI-agent guidance](agents.md). It refuses to touch a directory that already has a `kama.json`, and
if any other file it would write already exists it writes **nothing** rather than half a project.

What `--kind executable` produces:

```
myapp/
  kama.json
  src/app.kama
  .gitignore
  README.md
```

```json
// kama.json
{
  "name": "myapp",
  "version": "0.1.0",
  "kind": "executable",
  "entry": "src/app.kama",
  "modules": {
    ".": { "visibility": "internal" }
  }
}
```

```kama
// src/app.kama
fn int32 main() {
    return 0;
}
```

**`entry`, not `main`** — the key names the file holding the project's `main`, which is
Cargo's `[[bin]] path`. npm's `main` means the opposite thing: the entry point *importers* get. kama
expresses that with `source` and `export` instead, so the two never share a name. Spelling it `main`
is an error that says so.

There is no `source` key here because it defaults to `"src"`, which is where the seed put the files.

Build and run it in one step. **Name the project** — the operand is what picks what gets built:

```sh
cd myapp
kama run kama.json       # builds the project and runs it; the program's exit code is forwarded
```

`kama run` is a thin wrapper over `kama build`: it compiles a native executable to a temp
location, runs it, forwards the exit code, and cleans up. It's **native-only** (wasm needs a
browser/node, a bare-metal target emits a freestanding object) — for those, use `kama build --target …`;
see [targets.md](targets.md).

### Naming what to build — the operand is the mode

There are three ways to name a compilation, and **the operand's basename picks between them**:

| you write | you get |
|---|---|
| `kama build a.kama src/b.kama` | **loose** — just those files. No manifest is read at all. |
| `kama build kama.json` · `kama build libs/core/kama.json` | **project** — its `source` files, its dependencies, its flag universe, its `out/` root |
| `kama build kama_workspace.json` | **workspace** — every member, each built on its own |

Operands are either N `.kama` files **or** exactly one manifest, so "these files, and also that
project" is not so much rejected as unspellable. No `./` is needed anywhere: naming the file is what
matters, not qualifying it.

The rule that matters most is the first one. A loose build applies **no** manifest — not the one in the
directory above it, not the one in your shell's working directory. Before this, `kama build src/app.kama`
inside a project walked up, found the manifest and silently applied it, so there was no way to say "just
these files" and no way to tell which of the three you were getting. Commands that act *on* a project —
`pkg install`, `publish`, `toolchain pin`, `agents install` — name one for the same reason: the current
directory should not decide which manifest gets rewritten.

The one exception is `kama query`, and it is a different shape of command rather than a carve-out. A
query is *target-addressed*: it asks one question about one file, and the manifest says how far to look.

```sh
kama query kama.json src/app.kama --refs 12:11             # every use in this project
kama query kama_workspace.json libs/core/src/a.kama --refs 12:11   # ...and across every member
```

`kama lsp` is the other place discovery survives, and inherently: an editor hands the server a buffer
and a folder, never a command line.

### The two kinds, and the workspace above them

A project **states its kind** in the manifest — it is a library or an executable, and nothing infers
that from the other keys. A monorepo root is **not a project at all**: it aggregates members and has no
sources, module tree or artifact of its own, so it has no `kama.json`, and its own file is a different
one.

| `--kind` | writes | on disk |
|---|---|---|
| `executable` | `kama.json` with `kind` + `entry` | `src/app.kama` with `fn int32 main()` |
| `library` | `kama.json` with `kind` + a `modules` map | `src/<name>.kama` with an `export { … };` |
| `monorepo` | `kama_workspace.json` | one seeded library per `--members` name |

A monorepo takes the member names from you rather than inventing a directory convention:

```sh
kama seed acme --kind monorepo --members engine,server
```
```json
// acme/kama_workspace.json — the members, and nothing else
{
  "projects": {
    "engine": { "optional": false },
    "server": { "optional": false }
  }
}
```

Note what is *not* there: **no name and no version**. A **project** is the smallest sharable unit, so it
has both; a workspace is only a collection organizing a workflow, with nothing to name or to version. So
`--name` and `--version` are refused with `--kind monorepo` rather than quietly dropped.

Each member is seeded as a library, because that is what most members are; promoting one to an
executable is adding `entry` and a `main`. A member that imports a sibling still declares it as a path
dependency — see [Members are self-contained](#members-are-self-contained--declare-what-you-import).

A **project's name has to be a legal kama identifier**, of either kind, because the name is the
project's root module. `kama seed --kind library --name my-lib` is refused and says to use
`my_lib`: `import my-lib::{ … }` does not parse, so that package could never be imported by anyone —
and an executable is not the softer case it looks like, since its own symbols are qualified by the
same name. A monorepo directory may be called anything: it has no name in any file.

## Build output — `out`

Everything a build generates goes under one root, so ignoring it is one line rather than a hunt:

```
myapp/out/aarch64-macos-none/debug/app
myapp/out/aarch64-macos-none/release/app
myapp/out/wasm32-emscripten-none/debug/app.html
```

The root defaults to `out` and the `"out"` key moves it. It is scoped by **target triple** and by
**build type** because those vary independently, and a collision between them is silent — you would get
yesterday's binary and no diagnostic. `-o` still overrides everything.

A loose `.kama` file with **no manifest** is unchanged: `kama build hello.kama` still writes `./hello`
beside it. `out/` is a project's concept, and one file is not a project.

## Telling the tooling what your project contains — `source`

This key exists to replace an inference with a declaration. Editor tooling (the language server) has to
know which files make up your project before it can safely do a project-wide operation like renaming a
symbol across files. Without a manifest it has to infer — "every `.kama` under here" — and an inference
can be wrong, so it is **capped at 500 files**; past that, cross-file rename refuses rather than answer
from a set it doesn't trust. A manifest removes the guess, and with it the cap.

**`source`** — the one directory holding this project's `.kama` files, relative to the manifest,
searched recursively. It defaults to `"src"`, so most projects never write it:

```json
{
  "name": "myapp",
  "version": "0.1.0",
  "kind": "executable",
  "entry": "src/app.kama",
  "modules": { ".": { "visibility": "internal" } }
}
```

Now `examples/`, `tests/` and scratch files beside them are not part of the project, so a rename can
never reach into them and a same-named type over there can never be confused with yours.

`source` is also what **importers** resolve through: a package's files are found under its source root
and nowhere else. Files sitting beside `src/` rather than inside it belong to no project — which is the
point, and the reason a package whose sources are in the wrong place fails to import rather than
quietly working.

It is **one** directory, not a list. Two roots would let `src/shapes/` and `gen/shapes/` silently be one
module with nothing in the model able to say which of them a name came from. It must also name a real
**subdirectory**: `"."` is refused, because it would put the manifest itself, `.kama/deps`, `out/` and
any vendored dependency *inside* the source root — and a project may not contain another project's
`kama.json` under its `source`. Keeping the source root one level down makes all of those structurally
outside it, with no exceptions to remember.

## Naming what you publish — `modules`

**Required, and non-empty.** A module is a **folder**, and `modules` is what gives a folder a name: a
nested map mirroring the tree under `source`, where a module's name is the chain of keys read down to it,
rooted at the project's `name`. Nothing in a source file says which module it is in — the file's location
does, and this map is where that location becomes an API.

```json
"modules": {
  ".":             { "visibility": "internal" },   // src/*.kama          -> `myapp`
  "collections":   { "visibility": "public",       // src/collections/   -> `myapp::collections`
                     "modules": {
                       "detail": { "visibility": ["collections"] }   // -> `myapp::collections::detail`
                     } },
  "oddly-named":   { "visibility": "public", "name": "tidy" }        // -> `myapp::tidy`
}
```

- **`"."` is the project root** — the files directly under `source`. A project whose sources are all at
  the root still writes it, and that is the smallest legal map.
- **A folder with no entry is not a module**, and its files are not homeless either: they belong to the
  nearest listed folder above them. So nothing joins your API by accident, and adding a subdirectory is
  not automatically a published name.
- **The map is NESTED, never flat with `a/b` keys.** In a flat map, composition would be *inferred* from
  which other entries happen to exist — adding an unrelated `"collections"` entry would silently rename
  `collections/detail`'s public API. Nesting writes composition down instead.
- **`name` overrides one segment**, which is the escape for a folder whose name is not a legal kama
  identifier (`my-lib`) or simply is not the API word you want. A `::`-joined `name` is an error: that
  would smuggle hierarchy past the nesting.
- **`visibility` is required on every node**, including one whose folder holds no `.kama` files yet —
  making it conditional on file presence would mean *adding a source file invalidates the manifest*. The
  four forms are a list of modules, `"children"`, `"internal"`, or `"public"`.

`kama seed` writes the root entry for you, so most projects start from a working example rather than a
blank key.

### Where the FFI goes — a `native` module, by convention

`extern` is the one declaration that is not a module symbol. It keeps its literal C spelling, so it is
never `export`ed and never `import`ed: **a file that names an extern declares it**, and every declaration
of one C symbol in the program must agree ([SPEC](SPEC.md#ffi--calling-c-)). Repeating
`extern fn UnsafePtr malloc(usize n);` in each file that calls `malloc` is correct and idiomatic — a
declaration is not a definition, and it is exactly what including a C header does.

When you would rather not repeat it, **wrap the extern in an ordinary `fn` and export that.** The wrapper
is a normal module symbol, so `export`, `import` and `visibility` all work on it the usual way — and it is
free: `--release` folds the program into a single translation unit, so a pass-through wrapper compiles to
the same instructions as calling the extern directly.

The convention is to put those together in **one `native` module per project**, at the source root:

```
src/
  native/
    libc.kama      ← the externs, plus the wrappers that publish them
    curl.kama
  net/client.kama  ← import { myproj::native::alloc };
```
```json
"modules": { ".":      { "visibility": "public" },
             "native": { "visibility": "internal" },
             "net":    { "visibility": "public" } }
```
```kama
// src/native/libc.kama
export { alloc, release };
extern "<stdlib.h>";
extern fn UnsafePtr malloc(usize n);      // file-private: an extern never leaves its file
extern fn void free(UnsafePtr p);
unsafe fn UnsafePtr alloc(usize n) { return malloc(n: n); }
unsafe fn void release(UnsafePtr p) { free(p: p); }
```

`native` is **not a keyword and not a reserved name** — the compiler knows nothing about it. It is an
ordinary `modules` entry with ordinary `visibility`, and the value is entirely that "what does this program
call out to?" has one directory as its answer. Nothing requires it: a single-file program with no manifest
declares its externs inline, which is what most of this repo's own fixtures do.

## Composing projects — `kama_workspace.json`

A **workspace** is the scope above a project: a monorepo root, declaring the projects it composes. It is
a **separate file**, and everything about it follows from one invariant —

> **A project never reads its workspace file for anything that affects compilation.**

which is what keeps every member *extractable*: it must build identically whether or not its siblings
are checked out. The workspace file is for tooling — the language server's rename scope, and
`kama pkg install` across the repo — and never for what the compiler emits.

```json
// acme/kama_workspace.json
{
  "projects": {
    "libs/*":        { "optional": false },   // the glob must match at least one project
    "tools/codegen": { "optional": true  }    // absent is fine, contributes nothing
  }
}
```

```
acme/
  kama_workspace.json      <- libs/* and tools/codegen
  libs/
    core/kama.json         <- source: src/ (the default)
    ui/kama.json           <- source: src/ (the default)
  tools/codegen/kama.json
  scratch/notes.kama       <- claimed by nobody: never indexed
```

A member is any directory holding its own `kama.json`. A trailing `/*` expands to every immediate
subdirectory that has one. (The key is *not* called `packages`: `kama.lock` already uses that word for
resolved dependencies, and the distinction is the useful one — **packages are what you consume, projects
are what you compose**. For the same reason, name the directory something like `libs/` rather than
`packages/`.)

Editing a file in `libs/core` makes the whole workspace the rename scope, so renaming a type there
correctly updates `libs/ui`.

**Every entry states `optional`.** There is no default and no bare `{}`, because a project is the
smallest shippable unit and a checkout is routinely *partial* — submodules, role-scoped trees, a subtree
externals are not given. Which members may be absent is the first thing a reader of this file wants to
know, so it is spelled at every entry rather than inferred from silence:

- `"optional": false` and the directory is not there → an **error naming the path**.
- `"optional": false` on a **glob** that matches nothing → an error too. That is the question `optional`
  asks of a glob: `libz/*` expanding to zero directories is the same class of typo as a missing project.
- `"optional": true` → absent is fine; it simply contributes nothing.

**Neither workspaces nor projects nest.** There is one of these files per repository, and depth is
spelled with a deeper glob (`"group/libs/*"`) rather than a second file. A `kama.json` beside a
`kama_workspace.json` is an error for the same reason: a workspace root is not a project.

The only other key is **`dependencies`** — build-time tooling, built for the *host* rather than for the
target you are cross-compiling to, which is why it lives here rather than in a project. There is no
`name`, no `version` and no `toolchain`: a workspace is not a project, and which compiler builds a member
is settled by that member's own manifest.

### The manifest is checked, not skimmed

An **unknown key is an error**. `"sourses": ["src"]` used to be accepted and silently ignored, which
made a typo indistinguishable from a key that does nothing — and the manifest is where a project's
public surface is about to be declared, so a swallowed key would mean a swallowed decision. Every
command that reads the manifest at all validates the whole file, so the typo is caught by whichever one
you happen to run.

The same applies to values with a closed set: `"kind": "libary"` is refused by name rather than being
read as "not an executable" — and to the workspace file, which has its own closed set: `"name"` there is
refused rather than ignored, because a workspace does not have one.

### Members are self-contained — declare what you import

A member should be **extractable**: liftable out of the monorepo to stand alone. That requires it to
declare every dependency it *imports*, not merely to be built alongside one that does. So a member
declares its siblings the same way it declares anything else — as a path dependency:

```json
// libs/net/kama.json — net imports config, so net declares config
{
  "name": "net",
  "version": "0.1.0",
  "kind": "library",
  "dependencies": { "config": { "path": "../config" } }
}
```

Path dependencies are otherwise top-level-only, because a *fetched* package cannot reference a local path
reproducibly. Between two members of one `kama_workspace.json` that objection does not apply — the
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
    kama pkg install "$m/kama.json" && kama check "$m/kama.json" || exit 1
done
```

Without either key nothing breaks — the tooling infers the file set as before. These keys buy precision
and remove the cap. If you have a large tree that genuinely is one program and you'd rather not declare
it, `KAMA_LSP_MAX_FILES` raises the inference cap (`0` = no limit).

## Adding a dependency

Dependencies come from a local path, a git repo, or a tarball URL. Add one with `kama pkg
add` (which edits `kama.json` and installs), or write it into the manifest by hand:

```sh
kama pkg add kama.json geo --git https://example.com/geo.git --rev v1.0.0
kama pkg add kama.json mathx --url https://example.com/mathx-1.2.0.tar.gz
kama pkg add kama.json utils --path ../utils        # a sibling checkout
```

```json
// kama.json
{
  "name": "myapp",
  "version": "0.1.0",
  "entry": "src/app.kama",
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
import { geo::area };
```

Install resolves the whole dependency graph (transitively) and writes the lockfile:

```sh
kama pkg install kama.json   # materializes .kama/deps + kama.lock
kama run kama.json           # build + run against the resolved view
```

Other manifest surgery: `kama pkg remove kama.json <name>` drops a dependency; `kama pkg update kama.json
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
contacting the remote. `kama pkg update kama.json` re-resolves and can advance to a newer satisfying tag. A
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
version + integrity. `kama pkg add kama.json geo --version ^1.2.0 --registry <base>` writes one for you.

A registry base is transport-agnostic — `file://` (self-host / air-gap / offline testing) or `https://`.

### The registry is a static file tree

There is no registry *service* to run: a base URI plus two well-known paths, so any file host serves one
(the Go `GOPROXY` / cargo sparse-index shape). A dynamic service is optional and speaks the same protocol.

- **Index** — `<base>/<name>/index.json`, the published versions of one package:

  ```json
  { "name": "geo", "versions": [
      { "version": "1.2.0",
        "integrity": "sha256-<tarball-hash>",
        "tarball": "geo/1.2.0.tar.gz",
        "dependencies": { "mathx": { "version": "^1.0.0" } } }
  ] }
  ```

- **Artifact** — whatever `tarball` points at: a gzipped tar of the package sources, the same format a
  `url` dependency takes.

`tarball` is resolved **relative to `<base>`** (or absolute), so metadata and artifacts can live on
different hosts — an index on static pages, tarballs on a release host. Each version records its own
`dependencies`, so a consumer resolves the whole transitive graph from metadata without downloading
candidate tarballs.

A published `<name>@<version>` is **write-once**: its integrity is pinned forever and `kama publish`
refuses to overwrite it. That is the lockfile-drift and dependency-confusion guarantee at the source.

### Publishing

`kama publish` packages the current project and records it in a registry:

```sh
kama publish kama.json --registry file:///srv/kama-registry
```

It tarballs the sources (excluding `.git/`, `.kama/`, `out/`, and `kama.lock`), hashes them, and adds a
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

### Integrity — what is actually guaranteed today

**Content integrity is enforced and hard-failing.** Every fetch is checked, and a mismatch stops the
install with nothing entering the store:

- a **git** dep pins the resolved `commit` and the canonical **tree hash** of its unpacked sources;
- a **url** or **registry** dep verifies the tarball's sha256 against the index's `integrity` — or records
  it trust-on-first-use when none is given, after which the lock pins it;
- the content-addressed store is **keyed by the tree hash**, so identical content from any source collapses
  to one entry and a re-install is byte-identical;
- re-resolving a locked `name@version` to *different* bytes is a hard error (the dependency-confusion
  guard), which is what makes re-pointing a scope to a mirror safe.

This is the same layer Go's `go.sum`, cargo's index hashes and npm's lockfile integrity provide, and it is
the guarantee you should rely on.

### Signing (optional, and not yet an identity check)

`kama publish kama.json --key <ssh-key>` signs the tarball with an SSH key (via `ssh-keygen -Y`, the same SSHSIG
mechanism `git commit -S` uses) and records the signature + signer public key in the index:

```sh
kama pkg install kama.json            # warn-only: a bad signature warns, the install proceeds
kama pkg install kama.json --verify   # a signature must be present and cryptographically valid, else it fails
```

**Be precise about what this proves.** Verification runs `ssh-keygen -Y check-novalidate`, so it confirms
only that *the signature is valid for these bytes under the key embedded in the signature itself*. There is
no allowed-signers set, so **nothing binds that key to a publisher** — a tampered package re-signed with an
attacker's key verifies. Two further limits: verification runs on a **cold fetch** only (a warm store hit is
not re-checked), and git dependencies carry no signature at all.

So today, signing is a mechanism in place ahead of its policy. **The integrity guarantees above are the ones
that carry weight.** A trust model — an allowed-signers set, then CI/OIDC provenance recorded in a
transparency log — is tracked in [ROADMAP_DETAIL.md](ROADMAP_DETAIL.md) §10, and verification becomes mandatory with it.

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
kama pkg add --dev kama.json testkit --git https://example.com/testkit.git --rev v1.0.0
kama pkg install kama.json   # also materializes .kama/dev-deps
kama run kama.json --dev     # dev-dependencies on the import path
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
| `kama seed [<dir>] [--kind executable\|library\|monorepo]` | Turn a directory into a project — manifest, starter source, `.gitignore`, README, optionally `AGENTS.md` — or, with `monorepo`, into a workspace of them. Interactive on a terminal; a pipe or a script behaves as `--yes`. Also `--name`, `--version` (projects only), `--members a,b` (monorepo only), `--force`. |
| `kama run <kama.json> [-- <args>]` | Build the project and run it; native-only. A workspace errors and names its members. |
| `kama build <file>…\|<kama.json>\|<kama_workspace.json> [--dev]` | Build a native/wasm/embedded artifact. The operand picks the mode; a workspace builds every member. |
| `kama pkg install <kama.json>\|<kama_workspace.json> [--verify]` | Resolve `kama.json` (dev-)dependencies into `.kama/{deps,dev-deps}` + `kama.lock`; `--verify` requires + checks registry signatures. |
| `kama pkg add [--dev] <kama.json> <name> (--git U [--rev R \| --version V] \| --url U [--integrity H] \| --path P \| --version V [--registry BASE])` | Add a dependency and install (bare `--version` = a registry dep). |
| `kama pkg remove <kama.json> <name>` | Drop a dependency and install. |
| `kama pkg update <kama.json> [<pkg>]` | Re-resolve pins and rewrite the lock. |
| `kama publish <kama.json> --registry <base> [--key <ssh-key>]` | Tarball the project + record (and optionally sign) it in the registry index. |
| `kama toolchain list` | Installed versions, the global default, and what the current dir resolves to. |
| `kama toolchain install <v>` | Install version `<v>` into `~/.kama/versions/<v>` (alongside; keeps the default). |
| `kama toolchain uninstall <v>` | Remove an installed version (refuses the current default). |
| `kama toolchain default <v>` | Set the global default version. |
| `kama toolchain pin <v> <kama.json>` | Pin that project's toolchain in its `kama.json`. |
| `kama update [--version <v>]` | Install the latest (or `<v>`) and make it the default. |
