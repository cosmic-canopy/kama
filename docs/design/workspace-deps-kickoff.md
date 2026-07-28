# Workspace-internal dependencies — making a sub-project extractable (cold-start brief)

**Status: NOT STARTED. This is the ACTIVE next campaign** (user, 2026-07-28), ahead of resuming the LSP
campaign at M4. Line numbers verified against **`6c97e31`**.

**The goal, in one sentence:** a sub-project inside a monorepo must be **extractable** — liftable out of
the tree to stand alone — which requires it to declare every dependency it imports, and requires that
declaration to be *expressible* and *checked*. Today it is neither.

Read first: [packages.md](../packages.md) (user-facing manifest surface, incl. the new `sources` /
`projects` keys), [package-management.md](package-management.md) (the campaign of record for the resolver,
store, lockfile and registries).

---

## The demonstration — five files, reproducible in a minute

```
acme/kama.json          projects: ["libs/*", "apps/server"]
acme/libs/config/       declares Config              sources: ["."]
acme/libs/net/          imports config::{Config}     <- declares NO dependency
acme/apps/server/       dependencies: { config: {path}, net: {path} }
```

- `cd acme/apps/server && kama pkg install && kama run` → **works, exit 8**
- `kama check acme/libs/net/net.kama` → **`cannot resolve module 'config'`**

`libs/net` free-rides on the app's dependency view. It builds where it sits and nowhere else, and nothing
warns. Lift it out and it is broken. That is the whole milestone.

---

## Why it happens: four interlocking defects

### 1. Path dependencies are top-level only — the correct declaration is *rejected* ← root cause

[kama.driver.cpp:2156](../../kama.driver.cpp) refuses a path dep from any requestor but `<root manifest>`:

> `path dependency '%s' (required by %s) is only allowed at the top level — a fetched package cannot
> reference a local path reproducibly`

Sound for a package **fetched** from git or a registry. Wrong for a sibling inside the same workspace,
where the path is exactly as reproducible as the workspace itself. So `libs/net` *cannot* declare `config`,
and free-riding becomes the only option.

**Fix (cargo's):** permit a path dep when requestor and target are both inside the same declared `projects`
tree. That tree is already computed and tested — `collectPackageTree`
([kama.driver.cpp:3059](../../kama.driver.cpp)) walks `projects` recursively with cycle protection, and
`lspFindProject` proves it against `tests/query/mono`. Keep the existing refusal for every other case.

⚠️ **A path is currently resolved against the ROOT project, not the requestor.**
[kama.driver.cpp:1747](../../kama.driver.cpp) and [:2311](../../kama.driver.cpp) both do
`absolutePath(base + "/" + spec.path)` where `base` is the root project dir. Once a non-root requestor may
declare a path dep, that is wrong — `../config` means *relative to the declaring manifest*. `struct Req`
([kama.driver.cpp:2020](../../kama.driver.cpp)) carries `requestor` as a display **name**; it will need the
requestor's **directory** alongside it.

### 2. `sameSpec` compares raw path strings

[kama.driver.cpp:518](../../kama.driver.cpp):

```cpp
static bool sameSpec(const DepSpec& a, const DepSpec& b)
{ return a.path == b.path && a.git == b.git && a.url == b.url && a.rev == b.rev && a.integrity == b.integrity; }
```

`apps/server` says `config` is at `../../libs/config`; `libs/net` says `../config`. **The same directory**,
and the conflict check at [kama.driver.cpp:2148](../../kama.driver.cpp) reports *"require different
sources"*. Paths are relative to **different manifests**, so each must be resolved against its own
requestor's directory before comparison — which is the same `requestorDir` that (1) needs.

Note the defect class: **never compare paths as strings.** The LSP retired this twice in one session
(`lspRealPath`, then `ownsFile`). Fixing (1) makes sibling path deps normal, so this goes from occasional
to constant.

### 3. `resolveModuleFiles` ignores a package's `sources`

[kama.driver.cpp:197](../../kama.driver.cpp) resolves module `foo` by trying `<root>/foo.kama`, then
`listKamaFiles(<root>/foo/)` ([:128](../../kama.driver.cpp)) — **files directly inside that directory
only, non-recursive**. A dependency laid out with `src/` (exactly what [packages.md](../packages.md)
teaches for applications) therefore **cannot be imported at all**: `.kama/deps/geo/` holds only
`kama.json`, and the sources are one level down.

That is why the worked example uses `"sources": ["."]` for its libraries — a workaround, not a design. A
package's `sources` is precisely the declaration of where its files are, so the resolver should consult it.
This is also what makes `sources` earn its keep beyond the LSP.

⚠️ **Hot path.** This runs for every import of every build. Read each dependency root's manifest **once**
and cache it. `loadManifestSources` ([kama.driver.cpp:1034](../../kama.driver.cpp)) re-reads the file on
every call — fine for the LSP's per-gesture use, not for this.

### 4. Nothing enforces per-package declaration

The undeclared-import check runs against the manifest driving the **build** — `projectDepsView`
([kama.driver.cpp:218](../../kama.driver.cpp)) resolves `.kama/deps` from `inputs[0]`'s directory or the
CWD, and every file in the compilation then shares that one view
([`loadProgramUnits`, :229](../../kama.driver.cpp)). Once (1)–(3) make the correct declaration possible,
add the check that makes it required: **a file's imports must be satisfied by its own package's manifest**,
not merely by whoever is compiling it.

**⚠️ The design decision to settle before writing code.** Surveyed at `6c97e31`, **every multi-package
fixture in this repo already free-rides** — `tests/query/mono/libs/app` imports `gearcore`, the nested
`group/libs/plugin` imports `gearcore`, and `tests/query/ws` imports `widget`, none of them declaring a
dependency. Only `tests/pkg_path_dep.d` declares what it imports. So this is not a hypothetical breaking
change: a hard error breaks the LSP fixtures on day one.

That survey also exposes the real question underneath, which is *not* merely warn-vs-error:

- **(a) A sibling import inside one declared `projects` tree needs no dependency** — the workspace *is* the
  relationship. Simplest to use, and matches what everyone already wrote by hand. **But it forfeits the
  milestone**: a package that imports a sibling it never declared is still not extractable.
- **(b) Intra-workspace imports must be declared as path deps** (cargo's rule). Verbose — every sibling
  edge becomes a manifest entry — but it is the only option that makes "lift it out and it still builds"
  true, which is the entire point.

**Lean (b)**, since extractability is the goal the user set, with the transition made survivable by
warning rather than erroring at first: warn when the importing file's own package didn't declare the module
but an ancestor did; keep today's hard error when nothing declared it anywhere; and have the warning name
the exact `kama.json` line to add. Promote to an error at a major version. Under (b) the repo's own
fixtures should gain their path deps as part of step 4 — which doubles as the first real test of
workspace-internal path deps from step 3.

Settle this with the user before writing step 4 — it changes the acceptance test.

---

## Staging

Each step ends `tools/cdev make && tools/cdev test` green, plus `sh tools/check-packages.sh`.

1. **(2) canonicalize path specs** before comparison — smallest, independently correct, unblocks testing
   the rest. Requires threading `requestorDir` into `Req`, which (1) also needs.
2. **(3) `sources`-aware module resolution + a manifest cache.** Fixture: a dependency with a `src/`
   layout, which cannot be imported today.
3. **(1) workspace-internal path deps**, gated on requestor and target sharing one declared `projects`
   tree. Fixture: the `libs/net` → `libs/config` sibling dependency above.
4. **(4) per-package import checking**, after settling warn-vs-error.

## Acceptance

**Every sub-project builds on its own, from its own directory, with no ancestor manifest in play.** That is
what "extractable" means and it is mechanically checkable — a loop over the `projects` tree running
`kama check` (or `kama build`) in each member directory. Put it in `tools/check-packages.sh`.

## Test infrastructure that already exists

- **`tools/check-packages.sh`** (683 lines) — the package-manager harness, and the right home for this
  work. Network-free by construction: a `file://` git repo and a local `file://` tarball stand in for real
  remotes, `KAMA_STORE` points at a throwaway dir, and it SKIPs gracefully when `git`/`curl`/`sha256sum`
  are absent. Fixtures are built in `$tmp` with heredocs — follow that idiom; do not commit `.kama/deps`,
  which is install output.
- **`tests/pkg_path_dep.d/`** — the only committed path-dep fixture. ⚠️ Its `geo` package keeps
  `geo.kama` at the package **root**, which is exactly the layout defect (3) forces; a `src/` variant is
  the new fixture to add.
- **`tests/query/mono/`** — the nested-workspace fixture from the LSP campaign (root composes `libs/*` +
  `group`, `group` composes `libs/*` of its own). Reusable shape for the extractability loop, though it
  has no dependencies today.
- **`tools/check-query.sh` / `tools/check-lsp.sh`** — both now build a real installed path dependency in
  `$tmp` via `kama pkg install`. If dependency *resolution* changes, these exercise it from the LSP side
  too; keep them green.

## Seam map

| what | where |
|---|---|
| `resolveModuleFiles` | [kama.driver.cpp:197](../../kama.driver.cpp) |
| `listKamaFiles` (non-recursive) | [kama.driver.cpp:128](../../kama.driver.cpp) |
| `projectDepsView` | [kama.driver.cpp:218](../../kama.driver.cpp) |
| `loadProgramUnits` | [kama.driver.cpp:229](../../kama.driver.cpp) |
| `struct DepSpec` | [kama.driver.cpp:435](../../kama.driver.cpp) |
| `sameSpec` | [kama.driver.cpp:518](../../kama.driver.cpp) |
| `loadManifestSources` / `loadManifestProjects` | [kama.driver.cpp:1034](../../kama.driver.cpp) / [:1052](../../kama.driver.cpp) |
| path dep materialization (`base + "/" + spec.path`) | [kama.driver.cpp:1747](../../kama.driver.cpp), [:2311](../../kama.driver.cpp) |
| `resolveProject` (the BFS resolver) | ~[kama.driver.cpp:1985](../../kama.driver.cpp) |
| `struct Req` (needs `requestorDir`) | [kama.driver.cpp:2020](../../kama.driver.cpp) |
| conflict check | [kama.driver.cpp:2148](../../kama.driver.cpp) |
| top-level-only path refusal | [kama.driver.cpp:2156](../../kama.driver.cpp) |
| `collectPackageTree` (the `projects` walk to reuse) | [kama.driver.cpp:3059](../../kama.driver.cpp) |

## Gotchas

- **The resolver RESTARTS.** `resolveProject` re-runs resolution with tightened constraints pre-seeded
  (`RESTART`, `MAX_ATTEMPTS = 256`) when a later requestor narrows a range. Anything stateful you add must
  be either idempotent or reset per attempt — `seeded` / `tagCache` / `regCache` show both patterns.
- **`kama.lock` must stay byte-identical on re-install** (an existing assertion). A new field or a changed
  path spelling in the lock will trip it — decide deliberately whether a canonicalized path is what gets
  written, and note that changing it invalidates existing locks.
- **`kama.json` is read by a hand-rolled `ManifestReader`**, tolerant of unknown keys by design. The one
  strict reader is `stringArray` (used by `sources` / `projects`) because those decide what tooling may
  rewrite. Keep new keys consistent with that split.
- **Naming**: `packages` is taken by `kama.lock` for resolved dependencies. The vocabulary is *packages are
  what you consume, projects are what you compose* — do not reintroduce the collision.
- The whole toolchain is containerized: `tools/cdev make`, `tools/cdev test`,
  `tools/cdev exec env KAMA_SAN=1 ./run_tests.sh`.

## Not in scope

Version reconciliation between workspace members and published versions (`kama publish` substituting a
registry version for a path dep) — the follow-on once path deps can exist inside a workspace at all.
Also out: multi-root editor workspaces, `workspace/didChangeWorkspaceFolders`, and anything LSP —
the LSP campaign resumes at **M4 completion + signature help**
([lsp-m4-kickoff.md](lsp-m4-kickoff.md)) once this lands.
