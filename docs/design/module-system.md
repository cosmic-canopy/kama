# The module system — identity, visibility, and the C symbol

*In-flight design doc. **Delete this file when the work ships**, once SPEC + ROADMAP carry the record — see
the maintenance table at the top of [ROADMAP_DETAIL.md](../ROADMAP_DETAIL.md).*

**→ START HERE.** One ROADMAP row points here. It replaces the former rows 1 and 2 (*C keyword
collisions* and *file-private C symbols are positional*), which are **not shipped** — they are folded in,
because both are downstream of the model this doc replaces.

The short version: kama today has **two ways to name a thing** — a `namespace` declaration and a
filesystem path — and nothing keeps them in agreement. Six separate claims in SPEC.md turn out to be
prose with no enforcement behind them (§1). The C symbol defects are what that ambiguity looks like once
it reaches the emitter. Fixing the naming without fixing the model would bake the ambiguity into the C
ABI, so this is one campaign: **replace the model, and the symbol rule falls out of it.**

The model in one paragraph: **five scopes — workspace → project → module → file → declaration — and only
the top two get a manifest.** A project is a `kama.json`; its `name` is its root namespace. Modules are a
**nested map** inside that manifest, mirroring the folder tree, so a module's name is the path of keys you
read down to it and nothing else can change it. Every node declares a `visibility`, which is a list of
modules or one of `children` / `internal` / `public`. A file exports with one `export` block; there is no
`to`, no `private`, and no file is addressable anywhere in the model. The C symbol is
`project · module path · symbol`, so the file contributes nothing.

---

## 1. Why — what was measured, not assumed

Every row below was **run**, not recalled. The house rule is that a doc is not evidence; that rule is what
produced this section, since all of these were documented behaviors that do not hold.

### 1a. Six unenforced claims

| # | the claim | what actually happens |
|---|---|---|
| 1 | every `*.kama` in `a/b/c/` shares `namespace a::b::c` — [SPEC:3173](../SPEC.md) | two files in one directory may declare **different** namespaces. Accepted. A sibling reference then fails downstream with `unsupported method call on unresolved receiver` — never naming the real cause. Go rejects the same shape by name: `found packages bad (a.go) and sad (b.go) in .` |
| 2 | a namespace corresponds to its path | a flat `zzz.kama` may declare `namespace deeply::nested::thing;` and it builds. Resolution by path is only the *disk-search* rule — a file already in the compilation satisfies an import from anywhere |
| 3 | *"a qualified spelling reaches no further than an `import` would"* — [SPEC:3164](../SPEC.md) | enforced in **type positions only**. `import geo;` then `geo::secretFn()` on a **non-exported** function **builds, links and runs (exit 0)**. Same for `geo::Secret.make(…)`. The per-symbol import of the same name *is* rejected, so the two disagree |
| 4 | — | `export { … }` in a file with **no** `namespace` is silently accepted and entirely inert: nothing can import such a file |
| 5 | — | `export { };` is a **syntax error** — there is no way to spell an explicitly empty public surface |
| 6 | *modules do not nest* — an early draft of §2b | **the stdlib itself nests**, in three places that fixtures import by name: `std::net::web` (2 files), `std::serialization::json`, `std::serialization::binary`. And `json::encode` vs `binary::encode` is the one real name collision in the whole library (§2b), so those two cannot be merged |

Reproductions for 1, 3 and 4 are the smallest cases that show it; keep them as `xfail` fixtures when the
rules land (§7), because a prose claim that something is rejected is unguarded without one.

**Claim 5 dissolves rather than gets fixed** — see §4/D3. It was only a wart because claim 4 made the
no-block case ambiguous. Once claim 4 closes, a file with no `export` block has one unambiguous meaning and
needs no synonym, so `export { }` stays a syntax error. Four claims gain enforcement; the fifth stops
being a defect.

Two smaller warts found alongside: **unknown `kama.json` keys are silently skipped**
([kama.driver.cpp:2314](../../src/kama.driver.cpp)), so `"sourses": ["src"]` is accepted and ignored; and
several of these diagnostics print `at :1` with an **empty filename**.

### 1b. The two C symbol defects

**Positional private scopes.** `ctx.scope = "_F" + std::to_string(fileIndex)`
([kama.cemit.cpp:272](../../src/kama.cemit.cpp)), where the index is the file's position in the
compilation:

```
kama build a.kama b.kama c.kama   ->  _F4__Widget   (files a_0.c b_1.c c_2.c)
kama build b.kama a.kama c.kama   ->  _F5__Widget   (files a_1.c b_0.c c_2.c)
```

Not ugliness — **non-reproducibility**. Two builds cannot be diffed, hand-written C beside the output
cannot depend on a symbol, and a version-controlled `--keep-c` churns for nothing. The generated `.c`
**filenames** carry the same index ([kama.driver.cpp:7186](../../src/kama.driver.cpp)). Note also that
`_F4__` is a leading `_` followed by an uppercase letter — **reserved to the implementation for any use**
by C11 §7.1.3. Today's output is already in C's reserved namespace.

⚠️ **The `_N` filename suffix is not free to delete.** It exists because two source files may share a
basename; the replacement must be **path-derived**, not bare-basename (§2e).

**C keyword collisions.** Comparing kama's own 78 reserved words ([kama.l:380](../../src/kama.l)) against
C11's 44, **27 C11 keywords are legal kama identifiers** — 17 ordinary lowercase words (`auto double
float goto inline int long register restrict short signed struct switch typedef union unsigned volatile`)
plus the 10 `_Capital` forms. Six more arrive with C23 (`nullptr constexpr typeof static_assert
thread_local alignas`). They emit raw:

```
kw.c:398: error: expected member name or ';' after declaration specifiers   //  int32_t switch;
kw.c:565: error: invalid parameter name: 'switch' is a keyword
```

*(The old roadmap entry said 25; it was never derived, and the set above is measured. `switch` and
`float` matter most — kama spells them `match` and `float32`/`float64`, so both are free names, and
`switch` is entirely plausible in the embedded code kama targets.)*

**⚠️ The exposure is six families, not the three the old roadmap entry claimed.** It said *"types,
functions and methods are already namespace-scoped and cannot collide"* — false. A **method name is the
vtable struct member** ([kama.cemit.cpp:18041](../../src/kama.cemit.cpp),
[:17941](../../src/kama.cemit.cpp)), and an enum **case name is a union member**
([:17907](../../src/kama.cemit.cpp)):

```kama
type contract Ticks for value { fn int32 switch(); }
//  -> vt.c:524:15: error: expected member name or ';' after declaration specifiers

type enum Event { Resize(int32 union), … }
//  -> vr.c:406:21: error: declaration of anonymous union must be a definition
```

The six: **struct fields · parameters · locals and bindings · vtable/contract slot names · enum and
variant case names · `@generate` bag-ctor parameters** (which derive from field names).

### 1c. Two more holes, found while designing the replacement

**A declared workspace member that is not on disk is silently skipped.** `collectProjectDirs` returns on
`!fileExists(manifest)` with no diagnostic ([kama.driver.cpp:473](../../src/kama.driver.cpp)), and
`collectPackageTree` does the same ([:5599](../../src/kama.driver.cpp)). So a **typo'd member path and a
deliberately-absent one are indistinguishable**. §2a makes absence a declared state.

**Two `main`s are rejected for a reason the diagnostic misstates.** `qualify()` short-circuits on `main`
*before* any scope prefixing — `if (name == "main") return "kama_main";` precedes the `_nsCtx.scope` check
([kama.cemit.cpp:309](../../src/kama.cemit.cpp)) — so a `main` declared anywhere becomes `kama_main`
regardless of scope. Two of them therefore collide where two ordinary names would not. **Run, not assumed:**

```
$K build a/one.kama b/two.kama          # namespace a; / namespace b;  — each with fn int32 main()
kama: warning: unsupported duplicate function 'main' — kama has no overloading, so a name may be
      declared only once in its namespace (first declared at line 2) at b/two.kama:2 (not yet lowered)
exit 1, no artifact                     # correct outcome

$K build a/one.kama b/two.kama          # same two files, `helper` instead of `main`
kama: built /tmp/dupns                  # exit 0 — a__helper and b__helper are distinct
```

So the *behavior* is right: there can be one entry point, and the compiler enforces it before reaching C.
What is wrong is how it says so — and the cause is that **there is only one message**
([kama.cemit.cpp:5917](../../src/kama.cemit.cpp)), keyed on the mangled `sig.cName`, serving both the
ordinary duplicate and the entry point. That single string has to describe two different rules, so for
`main` it claims the name **"may be declared only once in its namespace"** when the two are in **different**
namespaces — blaming a rule that did not fire.

**This campaign has to touch it regardless, because the message names `namespace`, which §5 phase 2
deletes.** Both halves need new wording, and they are not the same rule:

| case | key | correct scope after this campaign |
|---|---|---|
| ordinary duplicate (`a__helper`) | project · module · symbol | **"only once in its module"** — the project root when the file is in none |
| `main` (`kama_main`) | just `kama_main`, always | **"a project has one entry point"** — `main` is scoped by nothing, which is *why* two collide where two ordinary names would not |

Two smaller repairs belong with them: the first location prints as `(first declared at line 2)` with **no
filename** — the same empty-filename wart §1a notes, and `_funcs` holds the node without its unit, so the
fix needs the unit threaded through. And it reports as `warning:` + "unlowered construct" rather than an
error, a shape `run_tests.sh:443` already treats as failure.

---

## 2. The model

### 2a. Projects and the workspace

The scope ladder is **workspace → project → module → file → declaration**, and only the top two get a file:

| file | where | mandatory | affects |
|---|---|---|---|
| `kama_workspace.json` | monorepo root | no | **tooling only** — LSP/rename scope, build & orchestration deps |
| `kama.json` | project root | **yes** | identity, resolution, the module map, visibility, the C symbol |

1. **A project requires a `kama.json`.** It is a **library** or an **executable**, stated by a new `kind`
   key. ⚠️ **Corrected while implementing:** an earlier draft said *"the kind is inferred from whether
   `entry` is present and never recorded ([kama.driver.cpp:5043](../../src/kama.driver.cpp))"*. That line
   is `seedManifest`, the seed **writer**, and `SeedKind` never leaves `kama seed`. **Nothing infers a
   kind today** — the only interpretation of `entry` anywhere is `kama run`'s hard error when it is
   absent. So `kind` records nothing implicit; it is a genuinely new fact, and is required. *(Shipped.)*
2. **`name` is the root namespace**, for both kinds, and is unique. A library's name is already required
   to be a legal kama identifier because importers write it.
3. **Projects do not nest.** `projects` is **removed from `kama.json`** and becomes the `projects` key of
   `kama_workspace.json`. Enforced as: **no `kama.json` inside `source`**, and (7c below) none beside the
   workspace file either. (A path dependency vendored under `.kama/` still has its own — that is a
   separate project, not nesting.) *(Shipped.)*
4. **`sources` → `source`**, singular, defaulting to `"src"`. A list of roots would let `src/shapes/` and
   `gen/shapes/` silently be one module. **It must name a real SUBDIRECTORY — `"."` is rejected**, along
   with `..` and absolute paths. This was added while implementing, and it is what makes rule 3
   exemption-free: with `"."` the manifest itself, `.kama/deps`, `out/` and any vendored project would all
   sit *inside* the source root, so "no `kama.json` under `source`" would need a carve-out for each. One
   level down puts all four structurally outside it. *(Shipped.)*

   ⚠️ **The default may only be applied by a reader that knows the manifest EXISTS.** `sources` was not a
   tri-state but a four-state, and the code conflated the two that matter: *"a manifest exists and
   declares nothing"* and *"there is no `kama.json` here"* were both the empty vector — and the second is
   the dominant case, because it is what makes an ordinary directory-module resolve. Defaulting
   unconditionally makes `import a::b` resolve `a/b/src/*.kama` and silently lose `a/b/*.kama`. The
   contract is now: `""` means no manifest; anything else is the source root, declared or defaulted.
5. **Unknown manifest keys are an error.**

> **The invariant that makes the split safe: a project never reads its workspace manifest for anything
> that affects compilation.** That is what keeps a project *extractable* — the property
> [packages.md:179](../packages.md) already demands and CI-checks — and it is what lets a member be absent
> at all. A project must build **identically whether or not its siblings are checked out.**

6. **Every workspace entry states `optional` explicitly.** There is no bare `{}` and no default. A project
   is the smallest shippable unit, so a checkout is routinely *partial*: sub-repos behind git submodules,
   role-scoped trees (art vs. engine), a subtree externals are not given. Which members may be absent is
   the first thing a reader of this file wants to know, so it is spelled at every entry rather than
   inferred from silence. *(Shipped.)*

```json
// kama_workspace.json
{
  "projects": {
    "libs/*":         { "optional": false },   // the glob must match at least one project
    "libs/core":      { "optional": false },   // absent is an ERROR naming the path
    "tools/codegen":  { "optional": true  },   // absent is fine, contributes nothing
    "art/pipeline":   { "optional": true  }
  },
  "dependencies": { }                           // build-time tooling, host-built — see §2d
}
```

7. **`optional` is meaningful on a glob too**, which is why it is required there rather than excused: it
   asks whether the glob may match *nothing*. `"libs/*": { "optional": false }` catches a mistyped root —
   `libz/*` expanding to zero directories is the same class of bug as a missing project (§1c).
   *(Shipped.)*

**Decided while implementing 1b, and now part of this section:**

7a. **No `name` and no `version`.** A *project* is the smallest sharable unit, so it has both; a workspace
   is only a collection organizing a workflow, with no sources, no namespace and no artifact to name or to
   version. Consequence: `kama seed --kind monorepo` **refuses** `--name`/`--version` rather than dropping
   them silently, asks for the kind *first* (it decides whether the other questions exist), and the old
   "only a monorepo root may be `my-repo`" name exemption is gone along with the name.

7b. **Workspaces do not nest either**, so there is exactly one of these files per repository and depth is
   spelled with a deeper glob (`"group/libs/*"`). This is what took "which workspace owns me?" from a
   cycle-broken tree walk over ancestor manifests to a single lookup: `collectProjectDirs` and its cycle
   set are deleted, and `collectPackageTree` no longer recurses.

7c. **A workspace root may not also be a project** — a `kama.json` beside a `kama_workspace.json` is an
   error. Members sit *beside* the file rather than under any `source` root, so §2a.3's "no `kama.json`
   inside `source`" has nothing to say about them; without this, a root that was both would be a project
   composing projects, which is the one shape the split exists to remove. Enforced where the workspace is
   READ, never on a member's build path.

7d. **A member is a directory holding a `kama.json`, in both spellings.** The plain (non-glob) form used to
   check only that the directory existed, which let a manifest-less directory be named, expand, and then
   contribute nothing — declared and silently empty.

Everything downstream — LSP file collection, `kama pkg install` across the workspace, build orchestration —
simply skips absent optional projects, which is what it already does for all of them.

*Non-goal:* the workspace does **not** police dependency edges. A mandatory project that path-depends on an
optional one is a latent break in a minimal checkout, and only the workspace layer can see it — but it is a
static lint, not a correctness rule, and the build already fails clearly for whoever hits it.

### 2b. Modules — a nested map in `kama.json`

8. **Folders are just folders.** A folder is a module **only** if it is listed in `modules`; folders
   without an entry stay invisible. **Nothing joins the API by accident.**
9. **The map is NESTED, mirroring the folder tree.** Each key is **one path segment** — a folder directly
   inside its parent — and a node may carry its own `modules` for the folders inside it.

```json
"modules": {
  ".":           { "visibility": "internal" },          // the project root: src/*.kama
  "collections": { "visibility": "public",
                   "modules": {
                     "detail": { "visibility": ["collections"] }
                   } },
  "net":         { "visibility": "public",
                   "modules": {
                     "web": { "visibility": "public" }
                   } },
  "serialization": { "visibility": "children",          // holds no .kama files TODAY — it still declares
                     "modules": {                       // an audience, and a NARROW parent does not
                       "json":   { "visibility": "public" },   // confine its children: both are public
                       "binary": { "visibility": "public" }
                     } }
}
```

10. **A module's name is the chain of keys read down to it**, joined by `::` — `net::web`,
    `serialization::json`. `name` on a node **overrides that one segment**: the escape for a folder whose
    name is not a legal identifier (`my-lib`) or simply is not the API word you want. A `::`-joined `name`
    is an error — that would smuggle hierarchy past the nesting.
11. **`visibility` is required on EVERY node** (§2c), including one whose folder holds no `.kama` files
    today. Making it conditional on file presence would mean *adding a source file invalidates the
    manifest* — the wrong coupling entirely. A folder that groups today can hold code tomorrow, and its
    answer should already be written down.
12. **`"."` is the project root** — the files under `source` in no module. It is the root's real path, not
    a reserved word, so no folder can collide with it (none can be named `.`), it needs no escape rule, and
    it is already established manifest notation (`"sources": ["."]` ships today). It carries `visibility`
    like everything else and takes no `name`: the root's identity *is* the project name.
13. **`kama_module.json` does not exist.** An earlier draft had a per-folder manifest, then a single flat
    map at the source root. Both are superseded: one nested map inside `kama.json` means one file to read
    and one place where a visibility decision can hide.

> **The invariant that makes the errors unspellable:**
> **a module's name is the path of keys you literally read down to it.** A sibling cannot change it.
> Re-parenting it is a visible restructure of this file, not a one-line addition somewhere else. Nothing is
> derived from anything you cannot see.

**Why nested rather than a flat map with `/` keys — this is the load-bearing part.** Composition is what
lets a folder tree have a name tree at all, but in a *flat* map composition is inferred from what other
entries happen to exist, and that is exactly where the errors live: with flat keys, adding or deleting an
unrelated `"serialization"` entry would silently rename `serialization/json`'s **public API** between
`std::json` and `std::serialization::json`. Nesting keeps composition and removes the hazard, because the
structure is written down rather than inferred. It also makes the shape of the manifest the shape of
`src/`, so the two can be diffed by eye.

14. **Imports match a full module name, not a segment walk.** `import std::serialization::json` resolves
    project `std`, then the module *named* `serialization::json` — one lookup. So a node holding no files
    of its own is never half-resolved into on the way past, and `import std::serialization` succeeds or
    fails purely on that node's own visibility and surface.

**Checks, all cheap and all local:**

| check | catches |
|---|---|
| `visibility` present on every node | a module with no declared audience |
| every key/`name` segment is a legal kama identifier | a folder named `my-lib` — needs an explicit `name` |
| composed names unique within the project | two paths colliding on one identity |
| a `name` is one segment, never `::`-joined | smuggling hierarchy past the nesting |
| `name` ≠ `global`; a module name ≠ the project's own name | shadowing the floor or the root (§2f) |
| `"."` carries no `name` | the root's identity *is* the project name |

> **Why flat-by-default is safe, measured.** Flatten the whole standard library — 49 files, 245 top-level
> declarations — into one namespace and you get **183 distinct names and exactly one collision**:
> `encode`, in `serialization/json/json.kama:198` and `serialization/binary/binary.kama:253`. That is
> precisely the case where a human reaches for a module. The ceremony lands on the rare case, which is
> what Python learned when PEP 420 **removed** the `__init__.py` requirement.
>
> *(The apparent duplicates in a first pass were `type intrinsic <int32> implements FromStr<This>`
> conformances — those declare no name. Worth knowing before re-measuring.)*

*Deliberately absent: a way to make a folder level contribute **no** name segment* (`src/internal/hashing/`
→ `hashing` rather than `internal::hashing`). It has **zero demand measured** — `lib/` and `prelude/` have
no namespace/path divergence at all — and every escape considered re-introduced an identity the nesting
does not show. Add it if a real case appears.

### 2c. Visibility — one source rung, one manifest key

15. **One `import` block and one `export` block per file.** Nearly free: 388 of 521 importing files
    already have exactly one `import` line, the maximum anywhere is 4, and the largest symbol list is 12.
16. **`export` has exactly one form.** `export { X }` puts X on its module's surface. Omitting the block is
    how a file exports nothing — **1,372 of 1,451 `.kama` files have no block**, every single-file fixture
    among them — so the block stays optional and the omission is the one spelling. **`export { }` remains a
    syntax error** (claim 5, §1a).
17. **`visibility` per module node**, required, with four forms:

    | `visibility` | who sees the module's surface |
    |---|---|
    | `["a", "b"]` | its own files **plus** the modules **named** `a` and `b` — **at least one entry** |
    | `"children"` | its own files plus every module **nested under it**, at any depth |
    | `"internal"` | its own files plus every module in this project |
    | `"public"` | all of the above plus **dependent projects** |

    The four are one widening ladder — *these modules* ⊂ *my subtree* ⊂ *this project* ⊂ *the world*. Each
    keyword rung exists because it names a set a list **cannot track**: `children` and `internal` both grow
    as modules are added. The list is **additive** — a module's own files always see each other — so it
    never names itself, and it may name modules in **this project only**; reaching a dependent is `public`,
    full stop.

18. **There is no `to`, in either layer.** An earlier draft had `export { X to [ … ] }` and a separate
    manifest `to` key. Both collapse into the list form of `visibility`: one key, one place. **No file is
    addressable anywhere in the model** — not in `import`, not in a grant, not in the C symbol.
19. **There is no `private` keyword and no empty list.** `private` would mean exactly `[ ]`, and the grants
    *are* the list now. `[ ]` itself is rejected for a sharper reason: **a module nobody can import is
    unreachable, so it can only be dead code** — no call path from `main` can enter it. By the same
    argument **`"children"` on a leaf node is an error**: an empty subtree is an empty audience. With `[ ]`
    gone, **the manifest alone proves there is no unreachable module in the project**, with no call-graph
    analysis.
20. **Every entry is an object** — `{ "visibility": … }`, never a bare value. A shorthand would drop the
    key that says what the value *means*, and the moment a second key is wanted the format would have to
    support both forms forever. The object is the extension point.

> **Nesting determines NAME, never ACCESS.** `visibility` does **not** nest, and this is the more
> surprising half of the model. A child does not inherit, widen, or narrow its parent's audience, and a
> parent marked narrow does **not** confine its children — `serialization` may be
> `["serialization::json"]` while `serialization::json` is `"public"`. This follows **Go**, where a
> subdirectory package has no relationship to its parent in either direction, over **Rust**, whose downward
> nesting is the implicit grant §3 rejects. `"children"` is the one place the tree touches access at all,
> and it is opt-in, one-directional, and written by hand.

**The composition.** X, declared in file F of module M, is reachable from L iff:

| L | requires |
|---|---|
| F itself | always |
| another file in M | `export` |
| another module in this project | `export` **and** M's `visibility` is `internal`, `public`, or a list naming L |
| a dependent project | `export` **and** M's `visibility` is `public` |

This changes today's semantics: privacy is currently per-**module**, so a sibling in the same
directory-module can see an unexported name (verified). Under the new rule a name must be `export`ed to
reach a sibling file. **Migration cost measured at zero**: of `lib/`'s 245 top-level declarations, 18 are
unexported and **none is referenced from another file**. (A first pass said one — `siftDown` in
`sort.kama:59` looked used by `priority_queue.kama`, but that file declares its **own** `fn void
siftDown(isize i)` at `:102` and never calls the free function. Word match, not a reference.)

**All four forms are meaningful for `"."`**, which is what makes the uniform rule safe: `"public"` is a
library's root surface, `"internal"` is project-wide plumbing, and a list is *"my root is plumbing; import
my modules instead"* — the case `kind` alone could never express. **`"public"` in an `executable` project
is allowed and means what `internal` means**: with no dependents to distinguish them it is simply inert,
and erroring would cost real churn on a `kind` flip to buy a lint.

*Per-symbol granularity is deferred, not rejected.* Narrowing one symbol below its module's level means
moving it into a module of its own — `"collections/detail": ["collections"]`, which is how Rust spells
`pub(super)`. Adding `export { X to [ … ] }` later is **source-compatible**, demand is measured at zero, and
shipping it now would re-split visibility across two places. The cost of being wrong is folder noise —
one-symbol modules — and that is exactly the signal that would justify the syntax.

### 2d. Dependencies

21. **`dependencies` / `dev-dependencies` keep their role**: kama projects, from source, key = import
    root. Their semantics are already correct — dev deps are off the import path, resolve into
    `.kama/dev-deps`, and a dependency's dev deps are never dragged into your build
    ([packages.md:433–453](../packages.md)).
22. **`link` on the project** — `"link": ["m"]`, overridable per `select.TARGET`. Native libraries linked
    into the output are part of how *this artifact* links. **There is no manifest key for this today**:
    only per-target `select.TARGET.<n>.ldflags` and the CLI `--link`, so a project needing `-lm` on every
    target has nowhere clean to say so. The key mirrors the existing flag name
    ([kama.driver.cpp:6345](../../src/kama.driver.cpp)) rather than inventing a second word.
23. **Build-time executables go in `kama_workspace.json`'s `dependencies`.** They are built for the
    **host**, not the target being cross-compiled to — the axis Cargo cites for `build-dependencies` — and
    that is a tooling/orchestration concern, exactly the layer §2a introduces. **No `build-dependencies`
    key in `kama.json`**, so nothing ships without a consumer.
24. **Only a library can be imported.** Depending on an executable for a surface is an error.

> **Why kama libraries are source-only.** `OUTPUT = EXE | SHARED | STATIC | OBJECT` already ships
> ([kama.driver.cpp:1530](../../src/kama.driver.cpp)) — but a `SHARED` artifact exports *only* `expose`d
> functions ([targets.md:191](../targets.md)), i.e. the C ABI seam, not the kama surface. It cannot be
> otherwise while **115 of 245 stdlib declarations (47%) are generic**: `Map<MyType, int32>` cannot exist
> in an archive built before `MyType` was written — the same constraint that keeps C++ templates in
> headers and puts MIR rather than object code in a Rust `rlib`. Go shipped binary-only packages in 1.7
> and **removed them in 1.13**; Swift is the only peer that made it work, and it cost library-evolution
> mode plus `.swiftinterface` files, with generics paying a runtime cost across the boundary.

### 2e. The C symbol — the old rows 1 and 2, falling out

25. **Symbol = `name` · module path · symbol**, joined by `__` exactly as `mangleNs` already joins a
    namespace path ([kama.cemit.cpp:256](../../src/kama.cemit.cpp)). **The file contributes nothing**, so
    moving a type between two files of one module is a refactor rather than a silent ABI break for anyone
    linking the emitted C.
26. **Generated `.c` filenames become path-derived** — the module-relative path with separators replaced —
    which drops the positional `_N` without reintroducing the basename collision it was guarding (§1b).
27. **Loose files** — a `.kama` with no `kama.json` — keep building and are the **only** remaining `_F<n>`
    case: basename-derived, symbols unimportable, and **two loose files sharing a basename in one build is
    an error naming both**. This is not a small population today (there is no `kama.json` anywhere in this
    repo outside 16 test fixtures), but it is the one that by definition cannot be imported. **This design
    shrinks the naming problem enormously; it does not dissolve it.**
28. **C keyword collisions** — rename **only on collision**, every other name emitted exactly as written:
    - **Reserved set:** all 44 C11 keywords + the C23 additions — the full sets, not just the 33 kama
      leaves free, since kama's own reserved list may shrink later and the cost of a wider table is zero
      (an extra word only ever fires on an actual collision). One table, following the
      single-source-of-truth shape kama already uses for its own keywords (`kamaIsKeyword`,
      [kama.l:561](../../src/kama.l), declared in [kama.forward.h:182](../../src/kama.forward.h)).
    - **Escape:** `k_` prefix. `switch` → `k_switch`.
    - **A clash is a compile error**, naming both — if the author also declares `k_switch` in the same C
      scope. Not a ladder, not a counter. *(There is no escape kama cannot spell: its identifier rule
      admits leading `_` and `__` — `type value _Hidden` compiles and emits `_F4___Hidden` — and C11
      §7.1.3 reserves exactly those forms to the implementation. Detection is the only sound answer.)*
    - **`expose` / `@extern` names are never renamed** — they are a declared C ABI, so a keyword there is
      an error, not a silent rename that would be a miscompile.
    - **Intern once**, where each name enters the emitter's tables, so all ~60 emission sites read an
      already-escaped string. The mangler is **not idempotent**, so applying it per-site is a bug.

### 2f. `global` and `main` — the two names that are not in the model

29. **`global` becomes an ordinary project `name`.** It already exists and is already reserved: `global::X`
    is the **floor**, the always-in-scope surface usable with no `import` and present under `--no-std`
    ([kama.cemit.cpp:353](../../src/kama.cemit.cpp), [FLOOR.md](../FLOOR.md),
    [SPEC:3235](../SPEC.md), `tests/global_alias.kama`, the C# spelling). Today it is held down by nothing
    but a comment and a sentence of prose — it is not even a lexer keyword.

    Give `prelude/` a `kama.json` with `"name": "global"`, and `global::X` stops being a special form: it
    is a symbol in a project, resolved by the same rule as `myapp::X`. The name is then reserved by the
    ordinary uniqueness rule (§2a.2) — **no separate reservation machinery, and no third kind of scope**.
    The floor's only special property becomes the one that genuinely is special: **it is implicitly
    imported into every file.** No other project can add to it, because claiming the name is just the
    duplicate-project error — which matters, since a dependency injecting unqualified names into every
    consumer is an unresolvable collision (it is why Rust gives only `std` a prelude).

    **One consequence.** `global::` does two jobs today and only the first survives: `global::X` (reach the
    floor where a local declaration shadows the spelling) is unchanged; `global::a::b::X` (name any
    namespace absolutely) **is gone**, since it would now mean module `a/b` of project `global`. That job
    exists only because an `as` alias can shadow a namespace name, so fix it directly: **an `import … as N`
    whose `N` collides with a known project name is an error.** `tests/global_alias.kama` tests a local
    declaration shadowing a floor symbol, so it survives intact.

    *Naming:* `base` is already a kama keyword (inheritance), and `root` is ambiguous in exactly the place
    it would be read — this model says "project root" and "root namespace" constantly. `global` is
    unambiguous, shipped, tested and documented.

30. **The prelude's `std::memory` claim resolves with it.** Today `prelude/` declares into two roots:
    `global.kama` into the floor and `prelude/std/memory/*.kama` into `std::memory`, while `lib/` is the
    project that owns `std`. There is no `lib/std/memory` on disk, so the driver hard-codes
    `providedWhole.insert("std::memory")` ([kama.driver.cpp:933](../../src/kama.driver.cpp)) to make
    `import std::memory` a satisfied no-op. Once `prelude/` is the project `global` it cannot also declare
    into `std`, so **move the triad to `lib/std/memory/`** as module `memory` of project `std`. `--no-std`
    is preserved for free: embedding is already path-parameterised (`embed_prelude.sh OUT GLOBAL MODULE...`,
    [Makefile:104](../../Makefile)), so this is a `PRELUDE_MODULES` change, not a script or driver change.
    The hard-coded line then **deletes**.

31. **`main` is the entry point, not a symbol.** It is reached *below* the visibility system: the user's
    `main` emits as `kama_main` ([kama.cemit.cpp:5779](../../src/kama.cemit.cpp)) and the emitter
    synthesizes a real C `main` that calls it directly ([:17813](../../src/kama.cemit.cpp)) after
    `kama_args_init`. That is a **C-level** call, so no visibility setting can break the entry point.

    Leaving "may you call `main`?" to `"."`'s visibility would be incoherent — `qualify()` short-circuits
    on `main` before any scope prefixing (§1c), so `main` is not in the root's surface in any sense
    visibility could gate; and a module's own files always see each other, so another root file could
    always call it regardless. State it directly instead:

    - **A call to `main` is an error**, wherever it appears. C++ forbids it outright; Rust and Go make it
      uncallable.
    - **`main` may not be exported.** It is not a surface name and no importer could reach it.
    - **`main` must be unique in a PROJECT**, not in a module or a file. Already enforced (§1c); what
      changes is that it gets **its own diagnostic** instead of borrowing the generic duplicate-function
      one, which cannot describe both rules at once. The entry-point message says a project has one entry
      point and that `main` is scoped by nothing — the reason two collide where `a::helper` and
      `b::helper` do not. The generic message it splits from moves from *namespace* to **module**, which is
      what the mangled key has always actually meant.

    Its **location** stays unconstrained — anywhere under `source`, so `cmd/`-style layouts and the
    existing `entry` key keep working. Location simply does not scope it, which is exactly why it is not
    callable. *(`spawn main()` is already impossible on two independent grounds: a `spawn` entry must
    return `void` ([:11359](../../src/kama.cemit.cpp)) and take exactly one `give`n bundle argument
    ([:11361](../../src/kama.cemit.cpp)); `fn int32 main()` fails both.)*

### 2g. The CLI contract — three build modes, spelled by the operand

Decided 2026-08-22, after auditing every CLI option against every manifest key (the matrix is §2h). The
model has three ways to name a compilation: loose files, a project, a workspace. Nothing above says how a
*command* selects between them, and today it does not — `kama build src/app.kama` inside a project walks up,
finds the manifest, and silently applies it ([kama.driver.cpp:6629](../../src/kama.driver.cpp)). So the three
modes are mixed by default, and there is no spelling that means "just these files".

32. **The operand's BASENAME is the mode.** One rule, checked at argv parse before anything else runs:

    | spelling | mode |
    |---|---|
    | `kama build a.kama src/b.kama /abs/c.kama` | **loose** — N source paths, relative or absolute |
    | `kama build kama.json` · `libs/core/kama.json` | **project** |
    | `kama build kama_workspace.json` | **workspace** |

    A manifest operand is any path whose basename is `kama.json` or `kama_workspace.json`. No `./` is
    required — naming the file is what matters, not qualifying it. **Operands are either N `.kama` files, or
    exactly one manifest.** Mixing is not rejected so much as unspellable, which is the point: there is no
    argument list that means "these files, and also that project".

33. **A loose `.kama` is NOT a project**, and that is what makes mode 1 coherent rather than a gap in
    "a project requires a `kama.json`" (§2a.1). It has no identity, no module, no namespace; it cannot
    `export`, cannot be imported, and its symbols are unaddressable — which is exactly why §2e.27 can give it
    basename-derived scopes and be done. Every peer keeps this case: `cc a.c b.c`, `rustc main.rs` with
    `mod helper;`, `zig build-exe m.zig` with `@import`, `swiftc a.swift b.swift` were all **run** and all
    build multiple files with no project file. Go is the sole exception, and only because it refuses to build
    outside a module at all.

34. **`--config` and `--project` are deleted.** `--config PATH` named a manifest; the operand *is* the
    manifest. `--project` widened `kama query` by walking up; the operand *is* the scope. Both were
    workarounds for the discovery this rule removes.

35. **Commands fall into four groups**, and the manifest means something slightly different in each:

    | group | commands | the manifest operand |
    |---|---|---|
    | **unit-set** | `build` `run` `check` `transpile` | XOR with `.kama` files — one form is required |
    | **target-addressed** | `query` | *optional*; sets SCOPE, then the file being asked about follows |
    | **project-acting** | `pkg install/add/remove/update` `publish` `agents install` `toolchain pin` | **required** — omitting it would re-introduce "act on the CWD" |
    | **none** | `seed` `lsp` `agents list/print` `toolchain list/install/uninstall/default` `update` | takes no manifest, correctly |

    `seed` CREATES the manifest, so it still takes a directory. `lsp` is handed buffers and a `rootUri` by an
    editor and never a path, so discovery there is inherent rather than convenient — it is the one place it
    survives.

    `query` gains something it cannot express today: `kama query kama_workspace.json libs/core/src/a.kama
    --refs 9:11` asks a whole workspace, where `--project` could only ever reach the file's own project.

36. **A workspace operand fans out; it never means "one big program".**

    | command | `kama_workspace.json` means |
    |---|---|
    | `build` · `check` · `pkg install` | every referenced project, each on its own |
    | `run` · `publish` | **an error naming the members** — pick one |

    `cargo` was **run** to check this, not recalled: `cargo build` builds every member, while `cargo run`
    refuses with *"could not determine which binary to run … available binaries: a, b"*. Running a workspace
    would mean supervising N processes — restart policy, log multiplexing, shutdown order — which is a
    process supervisor, not a compiler. And running one member is already spellable with no new concept:
    `kama run apps/server/kama.json`.

37. **The toolchain selector reads the named manifest instead of searching for one.** This is a
    simplification, and it is also the one place the new spelling would otherwise introduce a **silent**
    regression, so it is load-bearing.

    `maybeReExec` runs *before* argument parsing — it must, since its job is choosing which binary does the
    parsing — so it finds the project by scanning raw argv with a heuristic:
    `selectorInputFile` ([:5472](../../src/kama.driver.cpp)) matches only *an existing file whose name ends
    in `.kama`*, and `resolvePin` then walks **up** from it. Hand it `kama build ../legacy/kama.json` and
    nothing matches, so the pin resolves from the **current directory** — building `../legacy` with whatever
    compiler the CWD pins, saying nothing. That is the same failure the comment at
    [:5487](../../src/kama.driver.cpp) records having already been fixed once for `.kama` inputs.

    The replacement has no walk and no heuristic:

    | operand | pin |
    |---|---|
    | `…/kama.json` | that file's `toolchain` (+ its sibling `kama.local.json`) — **read, not searched for** |
    | `…/kama_workspace.json` | none: run in place, and re-exec **per member** (below) |
    | loose files, or none | `--toolchain <v>` → `KAMA_VERSION` → the global default |

    A loose build therefore inherits **no** project's pin, which follows from rule 33 and is a behavior
    change: today it walks up from the first `.kama` and adopts whatever it finds.

38. **A workspace build re-execs per member**, so each member gets its own pin and there is nothing to
    reconcile. This is why no `toolchain` key belongs in `kama_workspace.json`: §2a's invariant — *a project
    never reads its workspace manifest for anything that affects compilation* — plainly covers which compiler
    runs. Driving each member's own build is exactly the "build & orchestration" job §2a already assigns the
    workspace file, and every member still builds identically standalone. Recursion terminates because a
    member's operand is always a `kama.json`.

    > ⚠️ **"Run in place" for a workspace is LOAD-BEARING, not an optimization.** The selector exports
    > `KAMA_NO_SELECT=1` immediately before re-exec, as its loop-stopper
    > ([:5651](../../src/kama.driver.cpp)), and a child inherits it. Because a workspace operand never
    > execs, the driver never sets it and each member starts clean and resolves its own pin. Make the
    > driver select a version *for itself* and that inverts: every member silently skips selection and
    > builds with the driver's compiler. (A user who exports the variable themselves is using the
    > documented escape hatch, and having it reach members is then correct.)

    > **Testing this is already possible — do not conclude otherwise from a failed probe.**
    > `maybeReExec` returns immediately unless the running binary IS the installed `~/.kama/bin/kama`
    > ([:5637](../../src/kama.driver.cpp)), so an ordinary probe against a dev build exercises none of it
    > and a "successful" build there proves nothing. But
    > [tools/check-toolchain.sh](../../tools/check-toolchain.sh) already sets a throwaway `HOME`, copies the
    > compiler to `$HOME/.kama/bin/kama` so it genuinely is the selector, installs stubs that announce which
    > version ran, and unsets `KAMA_NO_SELECT`. Its §4c asserts the pin follows the INPUT FILE when building
    > from outside a project — the same claim, one spelling earlier. The manifest-operand case is a few
    > lines in that guard, not new infrastructure.

### 2h. CLI ↔ manifest coverage — what the audit found

Every CLI option checked against every manifest key, both read from the code rather than the usage text
(which drifts). Only build-affecting options are listed; command modes (`--each`, `--json`, the `query`
verbs) and hidden instruments (`--strict-numeric`, `--probe-templates`) have no manifest business.

| CLI | manifest | |
|---|---|---|
| `--target N` | `select.TARGET.<N>` + `"default": true` | ✅ |
| `--release` / `--debug` | `select.BUILD_TYPE.<v>.default` | ✅ *verified by build* |
| `--shared` | `select.OUTPUT.SHARED.default` | ✅ *verified — emits a real `.dylib`* |
| `--select G=V` · `--define` / `--undefine` | `select.<G>.<V>.default` · `flags.<N>.default` | ✅ |
| `--cc` · `--dynamic-runtime` | `select.TARGET.<n>.cc` · `.runtime` | ✅ per-target |
| `--link L` | `link` + `select.TARGET.<n>.link` | ✅ shipped in phase 1a |
| `-o P` | `out` | ◐ different jobs — `out` is the output ROOT, `-o` an exact path |
| **`--webgpu`** | — | ❌ **gap** |
| **`--no-heap`** | — | ❌ **gap** |
| `--keep-c` · `--no-line` · `--dev` · `-j` | — | correct as CLI-only (inspection knobs; a per-run choice; a machine property) |

39. **`--webgpu` and `--no-heap` gain manifest keys.** Both are permanent properties of a project rather than
    per-invocation choices, and `--no-heap` fails **silently** when forgotten: the build simply succeeds with
    allocation allowed. `--webgpu` is also a *linking* decision, which is the class `link` just gained a key
    for. (ROADMAP row 14's `subsystem` key is the same shape and already scheduled.)

    Going the other way, one apparent gap is not one: **`log` has no `kama` flag because `--log` belongs to
    the BUILT PROGRAM** ([SPEC.md:1271](../SPEC.md), `./app --log warn,audio=debug`, parsed by
    `include/kama_log.h`). The manifest's `log` is a baked runtime default, and its overrides are the
    produced binary's own flag and `KAMA_LOG`.

---

## 3. Prior art

Consulted and verified rather than recalled; the sources are worth re-reading before revisiting a choice.

**Who names a module**

| | module identity | must it match the path? | public surface |
|---|---|---|---|
| TypeScript / ES modules | the path; no declaration exists | n/a | `export` per declaration |
| Python | the path | n/a | `__all__` (advisory) |
| Zig | none — a file *is* a struct, `@import`ed by path | n/a | `pub` per declaration |
| Go | per-file `package` | free, but **all files in a directory must agree — hard error** | capitalized identifier |
| Java | per-file `package` | yes, via the classpath layout | `public` per declaration |
| C# | per-file `namespace` | **no** — only the opt-in IDE0130 analyzer flags divergence | `public`/`internal` |
| Swift | the build target; folders create **no** namespaces | n/a | `private`/`internal`/`public` |
| **kama today** | per-file `namespace` | **no, and unchecked** | `export { … }` manifest |
| **kama proposed** | project `name` + a nested module map | the tree supplies the default; `name` overrides one segment | `visibility` per module (§2c) |

**Is folder-derived naming a mistake? No — it is the majority answer.** Five of the seven tie identity to
the folder: TypeScript/ES, Python and Zig make the path *be* the identity with no declaration at all; Java
requires the match; Go declares per file but hard-errors when a directory disagrees. Only two decouple —
C#, which leaves it free and **unchecked** and then shipped an analyzer to flag the divergence, and Swift,
which sidesteps the question by giving folders no namespace meaning. kama lands between Java and Go, with
both failure modes closed: a module is **opt-in** (nothing becomes one by accident, unlike Python before
PEP 420 or TS), the name is **checkable** (unlike C#), and a per-segment `name` is the escape (unlike Java).

Two details that decided §2b and §2c:

- **Rust** nests visibility downward: a private item is visible to its module *and all descendants*, so a
  child reaches into its parent's privates. That is an implicit grant nobody wrote — rejected here, and
  `"children"` is the explicit version of it.
- **Go** gives a subdirectory package **no** relationship to its parent in either direction; the only
  hierarchy-aware rule is `internal/`. That is the answer §2c takes for access.

**C keyword collisions**

| project | locals / params | C-visible names |
|---|---|---|
| **Vala** (readable-C, closest analogue) | auto-escape to `_switch_` | **hard error** — "use the `cname` attribute" |
| Nim | mangled regardless | `mangleField` appends `_0` |
| rust-bindgen | — | trailing `_` (`type` → `type_`) |
| KaRaMeL (F*→readable C) | — | avoids C **and C++** keywords, for consumers compiling as C++ |
| Cython | blanket `__pyx_v_` prefix | `__pyx_f_<module>_<name>` |

We take Vala's *universal* rename rather than its field/method rejection — Vala can reject because it
offers `[CCode (cname = …)]` as an escape hatch and kama has none, so rejecting would amount to reserving
27 words. Vala's `_name_` form is also not injective and puts a leading `_` on file-scope names.

**Determinism.** Rust's legacy mangling embedded a build-dependent hash and v0 exists to make symbols
reproducible, deriving from the crate's own identity rather than its position — the same lesson as §1b.

---

## 4. Scale

Source-breaking and pre-1.0, so it lands before the tag or waits for 2.0. Re-measured against the working
tree (the numbers below replace an earlier set that had drifted):

| | count |
|---|---|
| `.kama` files | **1,451** |
| declaring a `namespace` | **91** — all deleted |
| with an `export` block | **79** (so 1,372 have none) |
| carrying `import` statements | **521** files, **692** statements |
| `kama.json` | **16**, all inside test fixtures |

It also touches the resolver, the emitter, the LSP, `kama query`, `kama seed`, and the docs.

One encouraging measurement, still true: `lib/` and `prelude/` have **zero** namespace/path mismatches — the
stdlib is already congruent with the model, so its migration is mechanical. Its whole map is 18 module
nodes plus `"."`, every module `public`, and **not one `name`**, because every stdlib folder is already its
API word. Any module that turns out to want something narrower is a finding about the stdlib, not about
this design.

---

## 5. Phases

**1 — the manifest.** Split in three; each lands independently green.

**1a — `kama.json`. SHIPPED 2026-08-21** (`0.9.46`–`0.9.50`, six commits): unknown keys become an error and
the pre-1.0 `main` key is rejected by name; `kind` required; `sources` → `source`, one subdirectory
defaulting to `"src"` with `"."` refused; `link`; no `kama.json` under `source`; the flat-listing fallback
gated; `lspEvictManifestCache`. `kama seed` emits `kind` and no `source`. New guard
`tools/check-manifest.sh` owns every manifest-schema rejection — **`tests/xfail/` cannot host them**, since
that leg builds one `.kama` file and a manifest error needs a directory tree.

**1b — `kama_workspace.json`. SHIPPED 2026-08-22** (`0.9.51`): parsed by `ManifestReader` in a workspace
mode with its own closed key set (`projects` map with a **required** `optional`, plus `dependencies`); a
missing mandatory member — or a glob matching nothing where `optional` is `false` — errors by path; wired
to the LSP ownership path and to `kama pkg install`'s path-dep gate. `projects` is now a rejection in
`kama.json` naming the new file, in the same commit that added the file to migrate to. `kama seed --kind
monorepo` emits a workspace file and refuses `--name`/`--version`; `tests/query/mono/` migrated.

Four things were decided while implementing and are recorded in §2a above: **no `name`/`version`** in the
file · **workspaces do not nest** (`collectProjectDirs` and its cycle break deleted; one file per repo,
depth by a deeper glob) · **`dependencies` parsed now**, consumed by nothing yet · **a workspace root may
not also be a project**. The twelve workspace rejections and their passing twins live in
`tools/check-manifest.sh`; the errors surface from `kama pkg install`, never from a build, since **no
build path reads the file** — verified by diffing the emitted C of a member with and without it.

**1c — the CLI contract (§2g).** The three-mode operand rule; `--config` and `--project` deleted; the
selector rewritten to READ the named manifest rather than search for one, with a guard that can actually
reach it (§2g.38's second warning); workspace fan-out for `build`/`check`/`pkg install` and a
member-listing error for `run`/`publish`; per-member re-exec with `KAMA_NO_SELECT` cleared; `--webgpu` and
`--no-heap` gaining manifest keys (§2h.39). Migration is ~68 call sites across five guards plus
`run_tests.sh`'s `.d` leg, all of them "build a `.kama` that happens to sit in a project".

**2 — identity and resolution.** `name` as the root namespace; the nested `modules` map with §2b's checks;
names composed from the key chain, never inferred from what other entries exist; `visibility` required on
every node; imports resolved by **full module name**, not a segment walk; nearest-ancestor file→module
attribution via the existing `projectManifestDir` walk ([kama.driver.cpp:854](../../src/kama.driver.cpp));
**delete the `namespace` declaration** (91 files). **This is where claims 1, 2 and 4 of §1a become
unrepresentable** rather than merely checked. §2f lands here too: `prelude/kama.json` named `global`,
`prelude/std/memory/` → `lib/std/memory/` with `PRELUDE_MODULES` repointed
([Makefile:104](../../Makefile)), the `providedWhole` line deleted, `global::a::b::X` dropped, and an
aliasing `import … as N` that collides with a project name rejected.

> **Deleting `namespace` strands the word wherever the compiler says it out loud.** Swept, so the list is
> not re-derived — five sites, and one of them is not a wording change:
>
> | site | today | after |
> |---|---|---|
> | [cemit:2943](../../src/kama.cemit.cpp) | ``` `::` resolves namespaces and types ``` | *modules* and types |
> | [cemit:17410](../../src/kama.cemit.cpp) | ``` `::` is scope resolution (static functions, enum variants, namespaces) ``` | … *modules* |
> | [cemit:5917](../../src/kama.cemit.cpp) | *"only once in its namespace"* | **splits in two** — see phase 3 |
> | [query.cpp:103](../../src/kama.query.cpp) | `CompletionKind::Namespace` prints `"namespace"` | `"module"` — and this merely makes the two front ends **agree**, since [lsp.cpp:337](../../src/kama.lsp.cpp) already maps that kind to LSP `Module` (9) |
> | [driver:627](../../src/kama.driver.cpp) | `bail("mixed namespaces")` | **a premise to re-derive, not a string to edit** |
>
> That last one matters. The closure-pruning path bails when a package's files are not namespace-homogeneous,
> because *"a package manifest's `sources` can span several directories and namespaces … which breaks the
> shared-namespace premise the reference closure rests on. No fixture exhibits it today, which is exactly why
> it would land silently later."* Under this model that premise is simply false by design: one `source` holds
> many modules on purpose. `source` becoming singular (§2a.4) removes half the hazard it was guarding; what
> the closure actually needs is to key on **module**, not on a single namespace for the whole package.
> Re-derive it here rather than renaming the bail.

**3 — visibility.** One import block, one export block (single form; `export { }` stays a syntax error),
and required `visibility` per node — list or keyword — enforced as §2c's composition table. **No `to`
grammar**: the `export` rule is unchanged apart from being made singular, so this phase is almost entirely
resolver work. Closes claim 3 — the hole where `geo::secretFn()` bypasses the manifest. §2f.31 lands here:
`main` uncallable, unexportable, unique-per-project — which means **splitting the one duplicate-function
diagnostic in two** ([kama.cemit.cpp:5917](../../src/kama.cemit.cpp)): an entry-point message scoped to the
**project**, and a generic one re-worded from *namespace* to **module**, since `namespace` no longer exists
after phase 2. Both gain the missing filename on the "first declared at" location.

**4 — the C symbol.** §2e: identity-derived symbols; path-derived `.c` filenames; the `k_` escape interned
once where each name enters the emitter's tables (`ClassInfo` fields, `_paramNames`, `Scope::locals`,
`VSlot::name`, variant records); the loose-file rule.

**5 — corpus and docs.** SPEC's *Modules / namespaces* section rewritten; `docs/packages.md` (the
`sources`/`projects` section, the monorepo walkthrough, the command table); `docs/targets.md` for `link`;
[FLOOR.md](../FLOOR.md)'s "the empty namespace" / "no browsable namespace" wording replaced by §2f.29; the
ROADMAP row and the §10 "C SYMBOL NAMING" entry deleted; the `_F4__` references updated in
`tools/check-ecs-zero-dispatch.sh` (4 lines), the comment in `tools/check-slot.sh`,
`docs/ENGINE_READINESS.md`, and seven fixtures.

**6 — delete this file**, per its own header and the ROADMAP_DETAIL maintenance table.

---

## 6. Verification

- **`tools/check-c-reproducible.sh`** — the guard this campaign exists for. Build one program twice with
  different argument orders into two temp dirs; `diff` the emitted `.c`/`.h` **and their filenames**:
  byte-identical, and `_F[0-9]` appears nowhere. Shape follows `tools/check-opaque-leak.sh:67`.
- **`tools/check-c-keywords.sh`** — transpile a fixture using every legal-in-kama C11/C23 keyword as a
  field, a parameter, a local, a contract method and an enum case; assert none appears as a declarator in
  the emitted C. Shape follows `tools/check-ecs-zero-dispatch.sh`.
- **`tools/check-module-visibility.sh`** — assert every rung of §2c, including the **call and
  construction** positions the current model lets through.
- **`xfail` fixtures**, each `.msg` matching the rule's own wording:
  - §1a claims 1, 3 and 4; the `k_*` escape clash; a C keyword on an `expose`d name; a `kama.json` under
    `source`; an unknown manifest key; a non-exported symbol reached in call position; duplicate loose
    basenames.
  - **Workspace**: a missing mandatory project; a `projects` entry with no `optional`; a glob matching
    nothing where `optional` is `false` — each with its optional twin as a **passing** fixture, since
    silence is the claim.
  - **Manifest** (§2b): a node missing `visibility`; a non-identifier segment; colliding composed names; a
    `::`-joined `name`; a `name` on `"."`.
  - **Visibility** (§2c): a listed-visibility module imported from a module *not* on its list; an
    `internal` module imported from a dependent project; an empty `visibility` list; `"children"` on a leaf
    node; a list naming a module that does not exist. Plus a **passing** fixture proving a narrow parent
    does **not** confine a `public` child.
  - **§2f**: a project or module claiming the name `global`; an `import … as N` colliding with a project
    name; a call to `main`; `export { main }`. Plus **two fixtures for the diagnostic split**, both already
    rejected today, so each pins the **corrected wording** — two `main`s in one project (the entry-point
    message, scoped to the project) and two `helper`s in one module (the generic message, which must say
    *module* and no longer *namespace*). Their `.msg` files are the guard that phase 2's deletion of
    `namespace` did not leave the word behind in a diagnostic.
- Guards run in parallel: private `mktemp -d`, no writes to the worktree, no `cd` outside a subshell, and
  the compiler reached through `$KAMA` (`. "$ROOT/tools/kama-bin.sh"`), never the `./kama` symlink.
- `./dev matrix > /tmp/matrix.log 2>&1; tail -5 /tmp/matrix.log` — once, into a file — then `./dev test
  san` and `./dev test wasm`. The wasm leg emits one `.c` per unit, so it exercises the naming change.
- **`VERSION` bumps on phases 1–5**; phase 6 is docs-only.

---

## 7. Traps, so they are not re-derived

- **A doc is not evidence** — six documented behaviors in §1a do not hold; the old roadmap entry's
  exposure list was wrong in a way that hid two whole families (vtable slots, variant cases); and its
  keyword count was off (25 stated, 27 measured). Compile the snippet; read the emitted C.
- **Two numbers in an earlier draft of this doc measured nothing.** "81 declarations would need a `to`" was
  `lib`+`prelude` decl-lines minus export-list entries — and `prelude` has no `export` block at all,
  contributing 70 of the 81. The real figure is **zero**. A follow-up "1" was a word match on `siftDown`,
  not a reference. Derive, then check the derivation.
- **`comm` is locale-sensitive on macOS**, so a keyword-set diff silently reported nonsense until it was
  redone with `grep -Fxv`. Any set comparison in a guard wants `LC_ALL=C` or no `comm` at all.
- **`qualify()` is not the site for the keyword fix.** It scope-prefixes *declared* names — exactly the
  set that already cannot collide. The exposed names are emitted by their own paths, and there is no
  single chokepoint. *(It is, however, where `main` escapes scoping entirely — §1c.)*
- **The keyword escape is not idempotent.** `k_switch` would become `k_k_switch`. Intern once, at the
  table, never at the emission site.
- **A partial fix is worse than none** — a use must agree with its declaration, so renaming a declaration
  without its uses trades a keyword error for an undeclared-identifier error.
- **`demangleForDisplay`** ([kama.cemit.cpp:108–173](../../src/kama.cemit.cpp)) strips a leading
  `_F<digits>::` so diagnostics never leak a mangled name (`tests/xfail/diag_no_mangled_name.kama` guards
  it). Changing the scope form means changing that strip — from a *registry* of known scopes, not a
  pattern.
- **The `_N` on generated `.c` filenames is load-bearing** until something path-derived replaces it: two
  source files may share a basename.
- **Measure with a hidden instrument first** if any step needs sizing: `--strict-numeric` and
  `--probe-templates` are the precedent (TSV on stdout, never `warning:` — `run_tests.sh:443` fails any
  fixture whose stderr matches `/warning/i`; full-row dedupe; and a bucket for the blind spot, because a
  measurement hiding its own blind spot is worse than no measurement).
