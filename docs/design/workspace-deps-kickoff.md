# Workspace-internal dependencies — making a sub-project extractable (cold-start brief)

**Status: NOT STARTED.** Written 2026-07-28 against `8bdf1f1`, from holes found while building a realistic
monorepo example for the new `projects` manifest key ([packages.md](../packages.md)).

**The goal, in one sentence:** a sub-project inside a monorepo must be **extractable** — liftable out of
the tree to stand alone — which requires it to declare every dependency it imports, and requires that
declaration to be *expressible* and *checked*. Today it is neither.

This is package-manager work, not LSP. It is what the `projects` key invites people into, so it should
land before monorepos get real use.

---

## The demonstration

```
acme/kama.json          projects: ["libs/*", "apps/server"]
acme/libs/config/       declares Config
acme/libs/net/          imports config::{Config}   <- declares NO dependency
acme/apps/server/       dependencies: { config: path, net: path }
```

`kama run` in `apps/server` → **works, exit 8**.
`kama check libs/net/net.kama` on its own → **`cannot resolve module 'config'`**.

`libs/net` free-rides on the app's dependency view. It builds where it sits and nowhere else, and nothing
warns. Lift it out and it is broken.

---

## Four defects, in dependency order

### 1. Path dependencies are top-level only — the correct declaration is *rejected*

[kama.driver.cpp:2155](../../kama.driver.cpp) refuses a path dep from any requestor but `<root manifest>`:
*"a fetched package cannot reference a local path reproducibly."* That reasoning is sound for a package
**fetched** from git/a registry, and wrong for a sibling inside the same workspace, where the path is as
reproducible as the workspace itself.

So `libs/net` **cannot** declare `config` even though it should. This is the root cause: it is what makes
free-riding the only option.

**The standard fix is cargo's:** permit a path dependency when both packages are inside the same declared
`projects` tree (which `lspFindProject`'s `collectPackageTree` already computes — the traversal exists and
is tested), and substitute a version on publish. Keep the existing refusal for everything else.

### 2. `sameSpec` compares raw path strings

```cpp
static bool sameSpec(const DepSpec& a, const DepSpec& b)
{ return a.path == b.path && a.git == b.git && ...; }
```

`apps/server` says `core` is at `../../libs/core`; `libs/net` says `../core`. **The same directory**, and
the resolver reports *"require different sources"*. Paths are relative to **different manifests**, so
comparison must resolve each against its own requestor's directory first — the requestor's base dir has to
be threaded into the check.

Note this is the same defect class the LSP just retired twice (`lspRealPath`, then `ownsFile`): **never
compare paths as strings.** Fixing (1) makes sibling path deps common, so this becomes reachable
constantly rather than occasionally.

### 3. `resolveModuleFiles` ignores a package's `sources`

[kama.driver.cpp:197](../../kama.driver.cpp) resolves module `foo` by trying `<root>/foo.kama`, then
`listKamaFiles(<root>/foo/)` — **files directly inside the directory only**. A dependency laid out with
`src/` (exactly what [packages.md](../packages.md) teaches for applications) therefore **cannot be
imported at all**: `.kama/deps/geo/` holds only `kama.json`, and the sources are one level down.

That is why the worked example in the docs uses `"sources": ["."]` for its libraries — a workaround, not a
design. A package's own `sources` is precisely the declaration of where its files are, so the resolver
should consult it. That also makes `sources` earn its keep beyond the LSP.

⚠️ This is on the hot path of every build. Read the manifest once per dependency root and cache it; do not
re-read per import. (`loadManifestSources` currently re-reads the file on every call — fine for the LSP's
per-gesture use, not for this.)

### 4. Nothing enforces per-package declaration

The undeclared-import check runs against the manifest driving the **build**. Once (1)-(3) make the correct
declaration possible, add the check that makes it required: **a file's imports must be satisfied by its
own package's manifest**, not merely by whoever is compiling it.

Design decision to settle first: **error or warning?** It is a breaking change for any existing tree that
free-rides. Lean: warn when the importing file belongs to a package that did not declare the module but
some ancestor did, error otherwise — with the message naming the exact `kama.json` line to add. Revisit
promoting it to an error at a major version.

---

## Suggested staging

Each step ends `tools/cdev make && tools/cdev test` green, plus `tools/check-packages.sh`.

1. **(2)** canonicalize path specs before comparison — smallest, independently correct, and unblocks
   testing the rest.
2. **(3)** `sources`-aware module resolution + the manifest cache. Fixture: a dependency with a `src/`
   layout, which cannot be imported today.
3. **(1)** workspace-internal path deps, gated on both packages being in one declared `projects` tree.
   Fixture: the `libs/net` → `libs/config` sibling dependency from the demonstration above.
4. **(4)** per-package import checking. Fixture: the free-riding tree, asserting the diagnostic; plus the
   extractability test that makes the whole milestone meaningful — **build each sub-project standalone**,
   which is the acceptance criterion.

## Acceptance

For the `tests/query/mono` workspace (or a package-management sibling of it): **every sub-project builds
on its own**, from its own directory, with no ancestor manifest in play. That is the property "extractable"
means, and it is mechanically checkable — a loop over the `projects` tree running `kama check` in each
member directory belongs in `tools/check-packages.sh`.

## Not in scope

Version reconciliation between workspace members and published versions (`kama publish` substituting a
registry version for a path dep) — that is the follow-on once path deps can exist inside a workspace at
all. Also out: multi-root editor workspaces, and `workspace/didChangeWorkspaceFolders`.
