# The module system — identity, visibility, and the C symbol

*In-flight design doc. **Delete this file when the work ships**, once SPEC + ROADMAP carry the record — see
the maintenance table at the top of [ROADMAP_DETAIL.md](../ROADMAP_DETAIL.md).*

**→ START HERE.** One ROADMAP row points here. It replaces the former rows 1 and 2 (*C keyword
collisions* and *file-private C symbols are positional*), which are **not shipped** — they are folded in,
because both are downstream of the model this doc replaces.

The short version: kama had **two ways to name a thing** — a `namespace` declaration and a filesystem
path — and nothing kept them in agreement. **Phase 2 closed that: the declaration is deleted and a file's
identity is where it sits.** Six separate claims in SPEC.md turn out to be
prose with no enforcement behind them (§1). The C symbol defects are what that ambiguity looks like once
it reaches the emitter. Fixing the naming without fixing the model would bake the ambiguity into the C
ABI, so this is one campaign: **replace the model, and the symbol rule falls out of it.**

The model in one paragraph: **five scopes — workspace → project → module → file → declaration — and only
the top two get a manifest.** A project is a `kama.json`; its `name` is its root module. Modules are a
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
([kama.driver.cpp:2568](../../src/kama.driver.cpp)), so `"sourses": ["src"]` is accepted and ignored; and
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
**filenames** carry the same index ([kama.driver.cpp:7968](../../src/kama.driver.cpp)). Note also that
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
`!fileExists(manifest)` with no diagnostic ([kama.driver.cpp:479](../../src/kama.driver.cpp)), and
`collectPackageTree` does the same ([:5599](../../src/kama.driver.cpp)). So a **typo'd member path and a
deliberately-absent one are indistinguishable**. §2a makes absence a declared state.

**Two `main`s are rejected for a reason the diagnostic misstates.** `qualify()` short-circuits on `main`
*before* any scope prefixing — `if (name == "main") return "kama_main";` precedes the `_nsCtx.scope` check
([kama.cemit.cpp:311](../../src/kama.cemit.cpp)) — so a `main` declared anywhere becomes `kama_main`
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
   `entry` is present and never recorded ([kama.driver.cpp:5420](../../src/kama.driver.cpp))"*. That line
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

   ⚠️ **A module is a FOLDER, and the file-module is deleted** (decided 2026-08-22). Today
   `resolveModuleFiles` ([kama.driver.cpp:611](../../src/kama.driver.cpp)) resolves `<root>/a/b/c.kama`
   as module `a::b::c` before it tries the directory, so a module has two spellings. A file has no key
   in a folder-keyed map, so keeping it would need a *second*, unwritten naming rule — basename to
   module name — which is exactly what this section's invariant forbids. It also re-opens §1a claim 1 in
   a new place: `src/math.kama` and `src/math/` could both exist and the file would silently win.
   *"When it comes to modules they span multiple files… part of the design was to keep file changes out
   of the API."* Two lines delete; `tests/mod_basic.d` gains folders.

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
10a. **The `modules` key itself is REQUIRED, and non-empty at every depth** (shipped `0.9.70`). It was
    optional through 2a–2d while `visibility` was only form-checked and a file could still name itself
    with a `namespace` declaration; with the declaration gone the map is the only way to name a module, so
    a project without one has no API to speak of. `{}` is refused for the same reason an empty
    `visibility` list is: a map that lists nothing decides nothing, which is what omitting the key meant —
    and accepting it would leave the requirement satisfiable by a decorative key, the rot 1c hit with
    `entry`. Enforced in `resolveBuildConfig` beside `kind`, the one place a manifest is validated as
    THIS project's; not on a dependency's, for `kind`'s reason.
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

15. **One `import` block and one `export` block per file. SHIPPED `0.9.78`.** ⚠️ The original note here
    said this was "nearly free: 388 of 521 importing files already have exactly one `import` line" — which
    measured the wrong thing. `export` was ALREADY singular (`export_manifest_opt` is not a list), but
    `import` was a **repeat of directives**: 136 files carried more than one line, max 4. Making it one
    block moved the scope INSIDE the braces, and that is what the entry syntax below is for.

    **Every import entry names a SYMBOL**, so `a::b::X` is symbol `X` of module `a::b`, always:

    ```kama
    import {
        std::collections::Map,      // a symbol of another module
        other::Thing as T,          // `as` renames
        Sibling,                    // a symbol of THIS file's own module — no scope to name
    };
    ```

    **There is no whole-module import**, and dropping it is what makes an entry unambiguous — otherwise
    `a::b::X` could be module `a::b::X` or symbol `X` of `a::b`, and only a module map could say. It also
    retires the module ALIAS, which gave `m::anything`, a qualified glob of the kind rule 16's successor
    rejects unqualified. The one real use of the bare form was a **capability gate** — 22 files wrote
    `import std::concurrent;` naming nothing from it, because `spawn` checks `externsHeader`, and loading
    the module is what registers that `extern` block. Importing any symbol of a module still loads it, so
    the gate survives with a name attached. Both blocks take a trailing comma.
16. **VISIBILITY IS PER FILE, and it is symmetric. SHIPPED `0.9.75`–`0.9.76`.** A file may name only what
    it **declares** or **imports**. `export { X }` is what lets a name LEAVE its file; an import entry is
    what lets one ENTER. That holds for a **sibling in the same module** too — privacy used to stop at the
    module, which is the rung Go never had and the reason a large Go package becomes a soup where any file
    reaches any unexported identifier. Omitting the `export` block is how a file exports nothing, and
    **`export { }` remains a syntax error** (claim 5, §1a).

    ⚠️ **Migration cost was measured at ZERO for the outbound half** — all 52 `lib/` files already carried
    an `export` block and no cross-file reference named an unexported name — and at **7 files** for the
    inbound half. A static estimate said 18; it over-predicted 2.5x because most of the stdlib already
    self-imported by full path, which was the long spelling of the same act.

    **An exported symbol may not name an unexported type of its own file** (Rust's `private_interfaces`).
    A project's API is DERIVED — the `public` modules of `kama.json` crossed with its files' `export`
    blocks — and that enumeration is only usable if every name in it can be spelled. Publicly reachable
    positions only: a `private` field leaks nothing. Measured 0 in `lib/`.

    **The compiler-owned sources are outside all of it**: the prelude floor and the built-in `std::memory`
    triad are intrinsics — always in scope, never imported, exempt from `export`. So is an **`extern`**
    name, which keeps its literal C spelling, is never scope-prefixed, and therefore collapses onto one
    table entry no matter how many files declare it.
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
| another file of M | `export` **and** L imports it (no scope in the entry — the module is implied) |
| another module of this project | `export` **and** L imports it **and** M's `visibility` is `internal`, `public`, or a list naming L's module |
| a dependent project | `export` **and** L imports it **and** M's `visibility` is `public` |

**Rows 1 and 2 SHIPPED (`0.9.75`–`0.9.78`); the `visibility` clause in rows 3 and 4 is phase 3c and is
still unenforced** — `ModuleVis`/`visibleTo` are parsed and validated for self-consistency and consulted
by no access decision.

This changed today's semantics, and shipped: privacy WAS per-**module**, so a sibling in the same
directory-module could see an unexported name (verified). A name must now be `export`ed AND imported to
reach a sibling file. **Migration cost measured at zero for the export half**: of `lib/`'s 245 top-level declarations, 18 are
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
    ([kama.driver.cpp:7004](../../src/kama.driver.cpp)) rather than inventing a second word.
23. **Build-time executables go in `kama_workspace.json`'s `dependencies`.** They are built for the
    **host**, not the target being cross-compiled to — the axis Cargo cites for `build-dependencies` — and
    that is a tooling/orchestration concern, exactly the layer §2a introduces. **No `build-dependencies`
    key in `kama.json`**, so nothing ships without a consumer.
24. **Only a library can be imported.** Depending on an executable for a surface is an error.

> **Why kama libraries are source-only.** `OUTPUT = EXE | SHARED | STATIC | OBJECT` already ships
> ([kama.driver.cpp:1663](../../src/kama.driver.cpp)) — but a `SHARED` artifact exports *only* `expose`d
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
27. **Loose files** — a `.kama` with no `kama.json` — keep building. A file in the loose ROOT (§2i) is the
    **only** remaining `_F<n>` case: basename-derived, symbols unimportable, and **two such files sharing a
    basename in one build is an error naming both**. This is not a small population today (there is no
    `kama.json` anywhere in this repo outside 16 test fixtures), but it is the one that by definition
    cannot be imported. **This design shrinks the naming problem enormously; it does not dissolve it.**
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

    ⚠️ **CORRECTED 2026-08-22 — an earlier draft of this rule said "give `prelude/` a `kama.json` with
    `"name": "global"`". That was an analogy, and taking it literally does not work.** The prelude is
    **embedded**: `preludeUnit()` and `preludeModuleUnits()`
    ([kama.driver.cpp:1424](../../src/kama.driver.cpp), [:1436](../../src/kama.driver.cpp)) parse it from
    string literals with the synthetic names `<prelude>` and `<prelude>/std/…`, and it is never resolved
    from disk — so **nothing would read that manifest**, which is the decorative-key trap §5/1c already
    hit with `entry`. Worse, `source` must name a real subdirectory (`"."` is rejected, §2a.4), and
    §2b's own check — *a `name` may not be `global`* — would reject the prelude's own manifest the moment
    an editor opened a file under it and `owningPackageDir` walked up to it.

    **What actually lands.** `global` joins `std` and `core` in the reserved-project-name set
    ([kama.driver.cpp:5377](../../src/kama.driver.cpp)), promoted from a seed-time check to one the
    manifest reader applies. That is §2a.2's uniqueness rule reaching the same conclusion by the same
    mechanism it already uses for the other two reserved roots — **no separate reservation machinery, and
    no third kind of scope**, which were the goals of the sentence this replaces. "No third kind of
    scope" is already true today: the floor is an *empty* scope, and `qualify`
    ([cemit:312](../../src/kama.cemit.cpp)) and `resolveUserNameImpl`
    ([:353](../../src/kama.cemit.cpp)) already treat it as the degenerate case of the ordinary one.
    `prelude/` therefore has **no `kama.json`**, and that is correct: it is compiler source that happens
    to be written in kama, shipped inside `bin/kama` rather than as a tree.
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
    `providedWhole.insert("std::memory")` ([kama.driver.cpp:1048](../../src/kama.driver.cpp)) to make
    `import std::memory` a satisfied no-op. Once `prelude/` is the project `global` it cannot also declare
    into `std`, so **move the triad to `lib/std/memory/`** as module `memory` of project `std`. `--no-std`
    is preserved for free: embedding is already path-parameterised (`embed_prelude.sh OUT GLOBAL MODULE...`,
    [Makefile:98](../../Makefile)), so this is a `PRELUDE_MODULES` change, not a script or driver change.
    The hard-coded line then **deletes**.

    ⚠️ **PROBED 2026-08-22. One claim above survived; one did not.**

    - *"The hard-coded line then deletes."* **Run, and the prediction against it was WRONG.** Built a
      compiler with the line removed, staged an installed payload with `lib/std/memory/` on disk beside
      the still-embedded triad, and compiled `import std::memory::{Owned}` through it: it **builds,
      links and runs correctly**. The reason the two copies do not collide is that the embedded ones are
      **collect-only** — `analyze()` excludes them from `_units` and `checkDeclaredTypes` treats them as
      compiler-owned ([cemit.cpp:1714](../../src/kama.cemit.cpp)) — while `Owned<T>` is **generic**, so
      its code is emitted at each instantiation rather than at its declaration. The disk copy's
      translation unit comes out **three lines long and empty**.

      So the line *can* go. It should still not simply vanish, for a reason the original never gave:
      deleting it makes every build that names `import std::memory` parse a module the compiler already
      has embedded and hand the C compiler an empty translation unit. **Re-derive it rather than delete
      or keep it** — the rule is not "`std::memory` is special" but *"a module already embedded in the
      compiler is already provided"*, which reads off `preludeModuleUnits()` and names no module at all.
      That removes the hard-coded exception, which is what this campaign objected to.
    - *"not a script or driver change."* `tools/embed_prelude.sh:35` builds each synthetic name as
      `"<prelude>/${m#prelude/}"` — a literal prefix strip that becomes a no-op once `PRELUDE_MODULES`
      points at `lib/std/memory/`, yielding `<prelude>/lib/std/memory/owned.kama`. And the deeper
      version: the `<` prefix marks a unit **synthetic**, and `setPackageResolver`
      ([driver:1783](../../src/kama.driver.cpp)) deliberately returns no manifest for such a unit — so
      under derived identity (§5 phase 2) the embedded triad would have no project and no module, and
      `std__memory__Owned` would silently become `_F<i>__Owned`. The embedded units must be handed their
      identity explicitly.

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
finds the manifest, and silently applies it. So the three
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

### 2i. Loose mode — the operands are the compilation

Decided 2026-08-22. §2g gave the three build modes; nothing above said how a module gets its *name* in the
one that has no manifest to name it.

40. **A loose build does no filesystem searching. You pass every source file.** ⚠️ This is a **behavior
    change, not a formalization** — loose mode searches today, and it was measured rather than assumed:
    one operand importing `thing::{answer}` pulled `thing/t.kama` off disk, because `here = dirName(paths[i])`
    was the first entry on the import root list. **DONE in 2d** — that entry is now the project this
    invocation is for, which for a loose build is nothing at all, and the operands **are** the
    compilation.
    `std` and `global` still resolve from the stdlib — they are a dependency, not a source — and a name
    no operand provides is an error naming `kama.json`.

    ⚠️ **The same-directory sibling scan was the last exception, and 2e closed it** (`0.9.71`). It was
    kept through 2d on the argument that it cannot change a module's NAME, only add files to one — but
    adding files to a compilation is enough: `kama build a.kama` compiled `b.kama` beside it and nothing
    said so. It is gated on `g_looseBuild`, not deleted, so a PROJECT build still loads its module's other
    files (§2g already says a project operand's unit set is every file under `source`) and `kama lsp`
    keeps it, since the server never goes through the operand path that sets the flag. That is §2g.35's
    asymmetry exactly: the CLI takes its operand at its word, the editor walks. The consequence to expect
    is that `kama check <one member file>` no longer resolves that file's module — for a member that
    self-imports OR one that names a sibling with no import at all. Both are `check-self-import`'s now.

41. **Module names are derived from the RESOLVED operand paths.** Operands may be relative or absolute
    and may sit anywhere on disk, so: resolve each to an absolute path, take their **deepest common
    ancestor** as the loose ROOT, and a file's module is its directory relative to that root with `/`
    replaced by `::`. Files directly in the root are in the root module and are unimportable (§2e.27).
    No common ancestor — different Windows drives — means no modules at all.

    This is **order-independent**: `kama build a.kama b.kama` and `b.kama a.kama` produce identical
    symbols, which is the invariant §1b actually wants. It *is* set-dependent — adding a file from a
    sibling tree raises the root and renames modules — and that is accepted rather than mitigated. A
    different file set is a different program in loose mode, and unrelated trees yielding long module
    names is precisely the signal that the build wants a `kama.json`.

42. **Adding a `kama.json` listing those same folders is a NO-OP.** That is the property that makes loose
    mode a genuine subset of project mode rather than a second dialect, and it is where the deviations
    become nameable: a folder you do *not* list, a `name` override, a visibility narrower than public, or
    a `source` root that is not the common ancestor.

> **Why kama can afford Go's answer where Rust and Zig cannot.** kama's `import a::b` is a **name** — not
> a path the way Zig's `@import("foo.zig")` is, and not preceded by a declaration the way Rust's
> `mod foo;` is. A path or a declaration can be followed with nothing else to consult; a name cannot. So
> the choice is between a manifest answering it and an implicit search answering it, and the implicit
> search is the half §3 rejects. See §3's *"must you list every source"* table: every language that
> requires listing all sources is a separate-compilation AOT language where the command line already
> **is** the unit definition — which is exactly `kama build a.kama b.kama -o app`, one `.c` per unit.

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

**Must you list every source? — the survey behind §2i**

Checked 2026-08-22, and it is a different question from "does it search": it asks who decides what is in
the compilation when there is no manifest.

*Must list every source — no search:*

| | |
|---|---|
| **C / C++** | every translation unit on the command line; `#include` reaches headers only, never another TU. **This is kama's own shape** — `kama build a.kama b.kama -o app`, one `.c` per unit |
| **Swift** | `swiftc a.swift b.swift -module-name M`; every file in a compilation unit is one module, and no file-search mechanism exists |
| **C#** | `csc a.cs b.cs`; wildcards are a shell convenience, not a search |
| **Go**, loose | `go build a.go b.go` → the synthetic package `command-line-arguments`; a local package import **fails** without `go.mod` (1.16+) |

*Pass a root and follow imports — each has a path-spelled or declaration-driven import:*

| | |
|---|---|
| **Rust** `rustc x.rs` | an explicit `mod foo;` loads `foo.rs` / `foo/mod.rs`. `use` never loads a file, and **`Cargo.toml` does not participate in module resolution at all** |
| **Zig** `zig build-exe x.zig` | `@import("foo.zig")` spells the path. `build.zig.zon` is for *external* packages only |
| **TypeScript / Python / Nim** | follow the import graph; Python's is a bare-name search on `sys.path` |

*The directory is the unit:* **Odin** — `odin build <dir>` compiles every `.odin` in it as one package,
and `-file` is the explicit opt-out meaning "this file alone, with no access to its siblings". The
instructive near-miss: directory-as-package is kama's *project* mode with `source`, and Odin still needed
a flag to spell "this one file", a distinction §2g's operand rule already makes structurally.

**kama takes discovery from the first group and the manifest's role from the second** — the operands are
the compilation, and adding a `kama.json` does not change how modules are named (§2i.42). The half it
avoids is Python's: a bare name answered by an implicit search.

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

⚠️ **Counted over GIT-TRACKED files.** The file count below read **1,451** until 2026-08-22, which was
`find`ing the whole worktree — 120 of those were probe files in the gitignored `.scratch/`, which exist on
one machine. Every other row was already tracked-only and is unchanged.

| | count |
|---|---|
| `.kama` files | **1,331** |
| declaring a `namespace` | **91** — all deleted |
| with an `export` block | **79** (so 1,252 have none) |
| carrying `import` statements | **521** files, **692** statements |
| `kama.json` | **14**, all inside test fixtures, plus **1** `kama_workspace.json` |

It also touches the resolver, the emitter, the LSP, `kama query`, `kama seed`, and the docs.

One encouraging measurement, still true: `lib/` and `prelude/` have **zero** namespace/path mismatches — the
stdlib is already congruent with the model, so its migration is mechanical. Its whole map is 18 module
nodes plus `"."`, every module `public`, and **not one `name`**, because every stdlib folder is already its
API word. (Re-verified 2026-08-22: **17** directories under `lib/std/` hold `.kama` files, and
`std/serialization` is the 18th — a pure grouping node holding only `binary/` and `json/`. Counting only
file-bearing directories gives 17 and is the wrong count for a NESTED map, which needs the intermediate
node to hang the two children off.) Any module that turns out to want something narrower is a finding about the stdlib, not about
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

**1c — the CLI contract (§2g). SHIPPED 2026-08-22** (`0.9.52`–`0.9.54`, three commits): the three-mode
operand rule; `--config` and `--project` deleted; the selector reading the named manifest; workspace
fan-out for `build`/`check`/`pkg install` with a member-listing error for `run`/`publish`/`transpile`;
per-member re-exec, asserted in `check-toolchain.sh` with two members pinned to different stub versions;
`--webgpu` and `--no-heap` gaining manifest keys (§2h.39). Migration was ~40 call sites across 9 guards
plus `run_tests.sh`'s `.d` leg.

Four things fell out that §2g did not name, each recorded here because the next reader will wonder:

- **A bare `kama run` is now an error.** It meant "read ./kama.json", which is the same implicit gesture
  the rule removes everywhere else. §2g.35's "one form is required" is read strictly.
- **The project-acting group takes the manifest FIRST**, then its own operand: `kama pkg add kama.json
  geo --path ../geo`, `kama toolchain pin 0.9.54 kama.json`. Same order as `query <manifest> <file>`.
- **A library's default OUTPUT is STATIC.** Without it `kama build kama_workspace.json` built the app and
  then failed on the first library, which is not what "build every member" can mean. `kind` picking the
  default is the first consumer of that key on the build path; `entry` gets the second, checked before
  the compile instead of discovered at the link as `Undefined symbols: _main`.
- **A project operand's unit set is every file under `source`**, which is not a change: building ONE file
  of a project already pulled in every file of that package, because they are one package. Probed before
  it was written down.

⚠️ **`projectManifestDir`'s walk is now a THREE-state.** `main` installs the answer from the operand, and
"the CLI says there is no project" has to be distinguishable from "nobody has said anything yet" — with a
two-state, a loose build falls through to the walk and picks up the manifest above it, which is exactly
the silent behavior the rule exists to remove. The walk itself survives for one caller: `kama lsp`.

**2 — identity and resolution.** `name` as the root namespace; the nested `modules` map with §2b's checks;
names composed from the key chain, never inferred from what other entries exist; `visibility` required on
every node; imports resolved by **full module name**, not a segment walk; nearest-ancestor file→module
attribution via the existing `projectManifestDir` walk ([kama.driver.cpp:934](../../src/kama.driver.cpp));
**delete the `namespace` declaration** (91 files). **This is where claims 1, 2 and 4 of §1a become
unrepresentable** rather than merely checked. **COMPLETE at `0.9.73`.** §2f lands here too: `global` reserved as a project name
(§2f.29 as corrected — *not* a `prelude/kama.json`), `prelude/std/memory/` → `lib/std/memory/` with
`PRELUDE_MODULES` repointed ([Makefile:98](../../Makefile)), `global::a::b::X` dropped, and an aliasing
`import … as N` that collides with a project name rejected. §2i's loose-mode rule lands here too, and
§2b.9's file-module deletion.

**Split in five**, planned 2026-08-22 on the same principle 1a/1b/1c used — each lands green on all three
legs and bumps `VERSION`:

| | | changes |
|---|---|---|
| **2a** | the `modules` map parsed + §2b/§2c form checks, plus `--probe-modules` and `tools/check-modules.sh` | **SHIPPED** `0.9.55`–`0.9.58` |
| **2b** | the corpus migration — `lib/kama.json`, the `std::memory` move, fixtures and guards become projects | **SHIPPED** `0.9.59`–`0.9.62` |
| **2c** | identity — `ctxOf` derives the scope from path + manifest instead of the declaration | **SHIPPED** `0.9.65` — see below |
| **2d** | resolution — imports by full module name; loose mode stops searching | **SHIPPED** `0.9.66`–`0.9.69` |
| **2e** | the deletion — 91 files, the grammar, `modules` required, the stranded sites, §2f | **SHIPPED** `0.9.70`–`0.9.73` |

> **The ordering rule that matters: the corpus migration (2b) lands BEFORE the cutovers, not with them.**
> It is a pure tree edit that is green under *today's* resolver — a folder that gains a `kama.json` still
> resolves through `packageSourceFiles` ([:476](../../src/kama.driver.cpp)), and the declared `namespace`
> is untouched. That keeps 2c and 2d C++-only, so a red suite names which half broke instead of leaving
> two candidates.

> **2a's key is not decorative, and that is deliberate.** 1c learned that an unconsumed manifest key rots
> (`entry` drew an unused-function warning the moment `kama run` stopped reading it). `--probe-modules` is
> the consumer: a hidden instrument on **stdout** — never `warning:`, since
> [run_tests.sh:443](../../run_tests.sh) fails any fixture whose stderr matches `/warning/i` — emitting
> one TSV row per unit comparing the **derived** identity against the still-present **declared** one,
> with `no-module` and `synthetic` buckets so the measurement does not hide its own blind spot (§7).
> `tools/check-modules.sh` reports through 2a–2b and **fails on any mismatch from 2c**, which is what
> turns the identity cutover from a leap into a measurement. Both are deleted when phase 2 closes.
>
> ⚠️ **What that instrument cannot see, learned in 2c:** a probe row exists only for a unit that *loads*,
> so a project's `src/main.kama` — which cannot resolve its own imports when handed to the compiler alone —
> emits no row under the `--each` corpus sweep. The 12 files the cutover actually moved were invisible to
> it. Drive each project **by its manifest** (`kama check <dir>/kama.json --probe-modules`) to see them.

**2c — identity. SHIPPED 2026-08-23** (`0.9.65`): `setModuleResolver` on the emitter, mirroring
`setPackageResolver` and installed in the same `configureEmitter` so all six emitters get it; `ctxOf`
([kama.cemit.cpp:265](../../src/kama.cemit.cpp)) scopes a file by the module its PATH puts it in.
**The emitted C for all 49 `lib/` files is byte-identical across the commit** — the property 2b existed to
create, measured rather than asserted. What moves is the **12** files that sit in a project and declare
nothing (`tests/mod_basic.d/src/main.kama`: `_F7__` → `modbasic__`), which is the cutover working.

Three things worth carrying into 2d/2e:

- **`ctxOf` has three rungs, and the middle one is temporary**: derived module → declared `namespace` →
  `_F<idx>`. The declaration is still read for a file with no project above it, because 11 fixtures and
  **17** declarations inside guard heredocs live in manifest-less trees and would lose their `export` and
  their qualified self-references. All 91 go in 2e together — the rule 2b learned by breaking the seed
  template.
- **`tools/check-modules.sh` gained a §3**, because §1 and §2 read identically whether or not the emitter
  consumes the derivation — which is exactly what they did through 2a and 2b. §3 reads the derived module
  back out of the emitted C. Verified to fail: a compiler built without the derivation reports eight
  failures and names the files still carrying `_F<n>`.
- ⚠️ **A claim in [kama.prelude.h:29](../../src/kama.prelude.h) is a prediction about 2e, not a fact
  today.** It warns that without the triad's stated `module`, `std__memory__Owned` becomes `_F<n>__Owned`.
  Built a compiler with that arm disabled: **entirely green**, because `lib/std/memory/*.kama` still
  declare `namespace std::memory` and the middle rung catches them. The arm is what carries their identity
  across 2e, which is also when that warning becomes true and when §3's triad assertion can fail.

#### 2d — resolution. SHIPPED 2026-08-23 (`0.9.66`–`0.9.69`)

Four commits, each green on all three legs (**1209 / 1192 / 1164**, 40 guards), and the emitted C
byte-identical across the first three — measured with `--keep-c` + `diff -r` over a loose stdlib-heavy
build (examples/httpd) and a project build, not asserted.

| | what landed |
|---|---|
| `0.9.66` | **the loose arm.** `ModuleId` had none: §2i.41's derivation did not exist in any form. The deepest common ancestor of the operands' directories is the root, established **per program** in `loadProgramUnits` (so `--each` is N loose builds, each with its own root) and read through the `setModuleResolver` seam 2c installed |
| `0.9.67` | **the loader keys on the module.** `unitNsKey` answered four questions — what a CLI input provides, which siblings belong with it, what a loaded module contributes, package homogeneity — all four now go through `moduleKeyOf`, whose rungs are `ctxOf`'s own: derived, else the declaration (until 2e) |
| `0.9.68` | **manifest-driven resolution.** `resolveModuleFiles` is the inverse of `moduleIdForFile`; the file-module arm and the `here` search are deleted |
| `0.9.69` | **the diagnostic**, three arms — stdlib, project, loose |

**The rule that took two tries, and the guards that caught it.** A loose build overrides a `kama.json`
for an **operand only**. Naming everything under the loose root that way is wrong, and wrong in a way
that is easy to miss by reasoning: `check-runtime-dir` stages a stdlib payload at `$tmp/payload/lib/kama`
and compiles `$tmp/s.kama`, `check-manifest` vendors a path dep inside the project it builds — both sit
under the root, and both turned into `payload::lib::kama::std::collections`. A dependency is **reached,
not named**; §2i says so in as many words ("`std` and `global` still resolve from the stdlib — they are a
dependency, not a source"). What a loose build refuses to read is a manifest over the files it was handed.

**⚠️ `check-modules` §2 went vacuous the moment the loose arm landed, and that is the shape of hazard to
watch for in 2e.** It swept the corpus with `kama check --each`, which is now a *loose* invocation — so it
measured the loose derivation and would have reported all 52 stdlib files as `declared-only` while
printing *"every file that declares a namespace derives exactly that namespace"*. It now drives every
project **by its manifest** and sweeps only the files no project owns, with a per-file assertion that
maintains itself: for each of the **91** files declaring a `namespace`, if a project owns it the derived
module must BE that declaration. **80 are owned and verified; 11 are not** — the 2e population, counted
and never asserted on. Five projects emit no rows (their dependency view is not materialized and
installing would write into the worktree): **named out loud**, never silently skipped.

**Four more guards were about to pass while testing nothing**, which is most of what 2d's diff is:

| guard | why it stopped meaning anything | what it says now |
|---|---|---|
| `check-manifest` ×3 | the `source` gate and the broken-manifest fallback were asserted on a LOOSE file importing a sibling directory — every one of which now fails for a reason unrelated to `source` | re-homed onto a project with a path dependency. The broken-manifest case **inverts**: an unparseable manifest costs a package its *name*, so it cannot be imported — while a file that does not import it still checks (the editor-leniency half) |
| `check-query` M4.7 | completed `module shapes` for a sibling FILE | a **reject**: a file is not a module, and completing a name that cannot resolve is the worst kind of completion |
| `check-query` M3.5 | *"app.kama is invisible to a query on widget.kama"* — true only because widget declared a namespace and app did not. **A module is a folder**, so they are one module and each sees the other | the fixture gained `src/parts/`, a second module. A member-file query sees its own folder; `--project` reaches the other module |
| `check-runtime-dir` | asserted a manifest-less payload merely derived no module | it cannot resolve `import std::…` **at all** — the real reason `release.yml` must stage `lib/kama.json` |

And three that simply moved to the §2i spelling: `check-build-jobs` (sixteen flat `src/wN.kama` reached
by the file-module rule → one module per directory, every file named), `check-diag-file` (each imported
module's file passed as an operand), `check-self-import` (the project spelling, plus a loose build that
names every source).

**New coverage, because the campaign should not have taken it on trust:** a file importing another
**module of its own project** analyzes clean in the **server** — measured over real JSON-RPC in
`check-lsp`, empty diagnostics — while the same file as a loose `kama check` operand cannot resolve it.
The second half is the negative control; without it the first proves nothing. This is the asymmetry
§2g.35 designed: the CLI takes the operand at its word, the editor walks.

**What 2e inherits from here** — each measured, none predicted:

1. **`kama check <one member file>` still half-works, through the declaration rung.** `a.kama` declaring
   `namespace my::mod` checks clean alone: the declaration names it and the same-directory sibling scan
   pulls its module's other files. A file that declares **nothing** already fails today (`cannot resolve
   module 'my::mod'`), which is what `check-self-import` asserts. 2e is when the first case joins it.
2. **The sibling scan is the one disk read left in a loose build**, and it was deliberately kept: it is
   bounded to the operand's own directory and can only pull files that derive the *same* module as
   something you named, so it cannot change a module's NAME. It is also what `lspAnalyze` (which passes
   ONE file) rests on. Whether §2i should forbid it is a 2e question, not a 2d one.
3. **`tests/query` holds five programs in one directory, two of them declaring `main`.** They stay apart
   today only because each declares its own namespace. After 2e they are one module by definition and
   have to become five directories. The warning is written beside the sibling scan in the driver.
4. `check-argv-env` and `check-panic-multitu` are **still** 2e's, unchanged and green: both pass two
   operands from one directory, so their files are in the loose ROOT and §2e.27 will make them
   unimportable — `lib.kama` moves into a subdirectory then.
5. A project's entry file now HAS a module (its root), so an editor session on `src/app.kama` loads its
   folder's other files. That is what a module means; it is noted because it is a behaviour change nobody
   asked for and nothing failed on.

#### 2e — the deletion. SHIPPED 2026-08-23 (`0.9.70`–`0.9.73`)

Four commits plus a corpus-prep one, each green on all three legs. Baseline **1209 / 1192 / 1164, 40
guards** throughout, rising to **1212 / 1195 / 1167** when the three §2f `xfail` fixtures landed.

| | what landed |
|---|---|
| *(prep)* | the fixtures whose identity came from a declaration get one from their FOLDER — `check-argv-env` and `check-panic-multitu`'s `lib.kama`, and `check-query`'s M4.2 scope head |
| `0.9.70` | **`modules` required**, and non-empty at every depth; 13 corpus manifests and 45 guard heredocs migrated |
| `0.9.71` | **a loose build reads only its operands** — the sibling scan is gated off for `g_looseBuild` |
| `0.9.72` | **the deletion**: 91 corpus declarations, 52 guard lines, the grammar, the AST node, `unitNsKey`, both middle rungs, the stranded diagnostics, `--probe-modules` |
| `0.9.73` | **§2f**: `global` reserved as a project name, `global::a::b::X` deleted, an `import … as` alias refused when it claims a project root |

**The method that made it a worklist rather than a leap**, and it is the one to reuse: before touching a
single file, build a compiler with `ctxOf`'s declaration rung and `moduleKeyOf`'s fallback **disabled**,
and run the matrix once. The whole corpus failed in **four places** — `tests/global_alias.kama`,
`check-argv-env`, `check-panic-multitu`, and eight assertions in `check-query`. Everything else, including
all 52 `lib/std` files and `check-packages`' 21 heredocs, was already green with the rung gone. The
prediction in this doc had been that `tests/query`'s five programs would have to become five directories;
they did not, because the sibling scan is skipped for a file with no module, so five loose one-file
programs in one directory never merge.

**Five claims this doc carried that phase 2e falsified.** Each was measured, not re-reasoned:

1. ⚠️ **`bail("mixed namespaces")` was not "a premise to re-derive". It was DEAD.** `files` comes from
   `resolveModuleFiles`, which builds the set by asking `moduleIdForFile` about each candidate — so the
   homogeneity it tested is guaranteed by construction. Proven with a tripwire on the branch and a full
   matrix on all three legs: it never fired once. Deleted rather than reworded.
2. ⚠️ **`kama.prelude.h:29`'s warning is now TRUE**, where 2c measured it false. With the driver's
   synthetic arm returning `""`, a one-line `new int32()` program no longer builds: `Owned` loses its
   module and stops satisfying the bounds written against it. That arm is now the ONLY thing naming the
   embedded modules, and `check-modules`' triad assertion is load-bearing for the first time.
3. ⚠️ **The self-import defect DISSOLVED rather than moving.** A stdlib file whose sibling reference is an
   `import` — `net/net.kama`, `io/streams.kama`, five in `collections` — now checks **clean alone**. The
   bug was that a CLI input registered its own DECLARED namespace as already provided, so the self-import
   was skipped and the sibling never loaded; with no module to claim, the same line is an ordinary foreign
   import and resolves the module whole. Only the no-import shape (`math/mat.kama`) still cannot be
   checked alone, and it is `check-self-import`'s control now.
4. ⚠️ **`global` cannot be reserved alongside `std`/`core` on the manifest-reader rung.** Doing so refuses
   `lib/kama.json`, whose `name` IS `"std"`, and the entire standard library stops resolving. The reader
   holds down `global` alone — safe precisely because nothing legitimate is ever called that, the prelude
   being embedded with no manifest at all. `seedValidName` refuses all three. `check-manifest` asserts
   both halves so the asymmetry cannot be "fixed" later.
5. **The guard heredoc count of 52 was right** (this doc's third figure, and the second correction). The
   anchored regex that produced "17" misses `printf 'namespace geo;\n…'` one-liners.

**And the population was 17 unowned, not 11.** The doc's 11 excluded the six under `tests/xfail/*.d/`,
which `check-modules` §2 skipped because an xfail fixture is meant not to compile. All six were no-ops:
`run_tests.sh` passes every file of an xfail `.d` as an operand, so `cola/cola.kama` derives `cola` from
its folder.

**What the deletion did NOT move: the emitted C.** Identical symbol sets over a stdlib-heavy loose build
(451 symbols) and a project build (237). The only textual differences are `#line` directives and the line
numbers baked into `assert`/`panic` messages, both shifting by exactly one because the sources are one
line shorter.

**Two mechanical rules the corpus edit needed**, both worth knowing before a similar sweep:

- **Under `tests/query/` the declaration was REPLACED, not removed.** `check-query` pins `line:column` and
  `check-lsp` pins LSP `{"line":N}` over real JSON-RPC, so those 13 files keep their length and the line
  says instead where the module comes from. Everywhere else the line simply goes. `check-lsp`'s own
  heredoc fixtures could not do that — they are the guard's text — so **43 request positions and 19
  assertions were decremented**. That is safe here in a way it usually is not: a wrong renumber fails
  loudly against real server output rather than passing on a different token.
- **`--probe-modules` and `check-modules` §1/§2 are deleted, §3/§4 stay.** §1 asserted the same seven
  shapes over the same fixture as §3 — one through the TSV, one through the emitted C — so §3 subsumed it
  the moment the emitter consumed the derivation (2c). §2 had nothing left to compare. `check-runtime-dir`,
  the probe's only other reader, asserts the stdlib's identity through a **C symbol** now, which the probe
  version had already been bitten by once: its first cut matched the DECLARED column and reported a
  deleted manifest as read.

⚠️ **The trap that nearly hid the whole thing.** `./dev build` after a `git stash` round-trip did **not**
regenerate the lexer, so `namespace foo;` still parsed against a binary carrying every other change. The
stale-binary trap in a generated-file disguise: `out/<plat>/kama.lexer.cpp` is an artifact, and a stash
that restores `src/kama.l` with an unchanged mtime leaves it stale. `./dev rebuild`, then assert the thing
itself — `namespace foo;` is a syntax error.

**3 — visibility.** `visibility` is parsed and form-checked; it is **enforced nowhere**. Closes claim 3.
§2f.31 lands here too: `main` uncallable, unexportable, unique-per-project.

#### 3a/3b — the file rung. SHIPPED 2026-08-24/25 (`0.9.75`–`0.9.78`)

Five commits: **`export` gated at every position** · **a sibling must be imported** · SPEC · **one
spelling for a sibling + `namespace` out of the other grammars** · **one `import { … };` block, entries
name symbols**. All three holes reproduced at `0.9.74` first — each BUILT, LINKED AND RAN — and the
`import { X };` form was a parse error, so none of it was recalled.

⚠️ **The `export` half needed no corpus migration and the `import` half needed 7 files.** Both numbers
came from running the compiler with the rung on. A static estimate for the second said 18 and
over-predicted 2.5x, carrying the exact `siftDown` word-match trap `.scratch`'s probe README warns about.

**Four things the matrix found that reasoning did not:**

1. A **substituted type argument** is not a reference in the file being walked. Generic instantiation
   restores the TEMPLATE's context while the argument was written at the call site, so `View<Transform>`
   blamed a single-file fixture for not exporting its own type. Three signals separate them: `_refUnit`
   (whose text is being recorded), `synthesized` (an emitter-built node has no source text to attribute),
   and `_typeSubst` (this name is a type parameter's binding, already judged where it was written). The
   last surfaced only as 26 `kama check`/`kama build` DISAGREEMENTS, not as a failure.
2. An **`extern` name must be exempt** — it keeps its literal C spelling, is never scope-prefixed, and so
   every file declaring `extern fn memset` collapses onto one entry whose `declFile` is whichever file
   lost the race. SPEC prescribes that repetition. 31 fs/net/proc fixtures.
3. The declaration walk named **no file at all** (`at :17`) — it runs after `_collectingUnitPath` unwinds.
   The wart the probe README logged and nobody had chased.
4. **Two other grammars and a BNF doc** track `kama.y`: tree-sitter, the VS Code TextMate grammar, and
   `docs/grammar.bnf`. The suite names all three. `namespace` had survived its own deletion in the first
   two since 2e, invisible because `check-treesitter` only checks the compiler-accepts/tree-sitter-rejects
   direction — a grammar that accepts MORE than the compiler is not tested.

**§2f.29 had to MOVE or retire in silence.** "An `as` may not claim a name that roots a project this file
can reach" was checked on the MODULE alias; dropping that form leaves `moduleAlias` permanently null, so
the rule would have become dead code with its two xfails still passing. It rides on the symbol alias now.

#### 3c/3d — the `visibility` rung and `main`. SHIPPED 2026-08-25 (`0.9.79`–`0.9.80`)

**3c.** All four forms decide now, at the import entry AND at a qualified reference (which reaches a
module without importing it — one position without the other is §1a claim 3 again). The driver answers a
PREDICATE rather than exporting `ModuleVis`, installed beside `setModuleResolver`; it needed a module-name
→ manifest index, which did not exist, so `moduleIdForFile` records one as each unit passes.

⚠️ **THE CHECK CANNOT LIVE IN `ctxOf`, AND THE FIRST VERSION DID.** That index fills as each unit is
attributed — through the resolver callback `ctxOf` itself makes — so asking there consults a half-built
index and the answer depends on unit order. It read as "internal is enforced, a list is not"; both were
coin-flips. It runs in the import-privacy pass, where every unit's context already exists.

⚠️ **THE FAIL-CHECK CAUGHT TWO CANARIES IN MY OWN GUARD.** With the rung disabled, two of four assertions
still passed: `["app"]` named a module the project did not have and `"children"` sat on a leaf, so both
were refused by MANIFEST VALIDATION and never reached visibility. Rewritten against a manifest that is
valid, they fail as they should. **Disable the predicate and re-run before trusting any of this.**

The within-a-project rungs are fixtures, which needed a four-line fix: a `.d` xfail was built by naming
its `.kama` files — a LOOSE build, applying no manifest — so a visibility rejection could not fire and the
fixture failed on "cannot resolve module" instead. The POSITIVE `.d` leg has read the manifest since 1c;
the xfail leg simply never got that arm. Cross-project rungs live in `tools/check-module-visibility.sh`
because they need `kama pkg install`.

**3d.** `main` is not callable, not exportable, unique per PROJECT. The export check runs BEFORE
`qualify`, which maps `main` to `kama_main` and made `export { main }` pass validation while the import
side looked for `m__main`. The duplicate diagnostic splits, both arms now printing
`first declared at <file>:<line>`. ⚠️ The entry-point message's claim — that two collide *where
`a::helper` and `b::helper` would not* — is pinned by `tests/mod_two_modules_same_name.d`; without it the
message is the duplicate-name message with different words.

**Phase 3 is COMPLETE.** What remains of the campaign: 4 (the C symbol), 5 (corpus + docs), 6 (delete
this file).

**4 — the C symbol.** §2e: identity-derived symbols; path-derived `.c` filenames; the `k_` escape interned
once where each name enters the emitter's tables (`ClassInfo` fields, `_paramNames`, `Scope::locals`,
`VSlot::name`, variant records); the loose-file rule.

**Both defects reproduced at `0.9.74`**, so the phase opens on measurement rather than recall. One loose
program, three files, two of them sharing the basename `x.kama`, built twice with the operands in
different orders:

```
order 1:  main_0.c  x_1.c  x_2.c      _F4__helper
order 2:  x_0.c     x_1.c  main_2.c   _F6__helper
```

Both the generated **filenames** and the file-private **scope** are positional, so `--keep-c` is not
reproducible — which is what the README's "drops into an existing C codebase" rests on. Note what phase 2
already fixed and what it deliberately did not: every symbol with a MODULE is stable now (measured across
the 2e deletion — identical symbol sets, 451 loose / 237 project), and `_F<n>` survives only for a file in
the loose ROOT, which §2e.27 says is the one case that cannot be imported anyway. The `_N` on filenames is
**load-bearing until something path-derived replaces it**: the two `x.kama` above are exactly the basename
collision it guards.

And the keyword half, same build:

```kama
type value Cfg { public int32 switch; ... }   ->  clang: 1 warning and 6 errors
```

**5 — the `native` module: one place for the FFI surface. DESIGN NOT STARTED.**

An `extern` is the one thing in the language that sits **outside** the file rung, and it has to today: it
keeps its **literal C spelling** and is therefore never scope-prefixed, so every file declaring
`extern fn memset` collapses onto ONE emitter table entry whose `declFile` is whichever file was collected
last. Judging that entry by file would reject the losers of a race. That exemption is now the only hole in
an otherwise total rule — everything else a file names is governed by `export` and `import`.

**Measured at `0.9.80`, so the design starts from the corpus and not from the idea:**

| | |
|---|---|
| files declaring at least one `extern fn` | **109** — but only **25** are in `lib/`; **84** are elsewhere |
| distinct extern names | 211, of which **47 are declared in more than one file** |
| the worst | `malloc` and `free`: **39 files each** |
| `extern "<stdlib.h>";` seam declarations | **50** |

⚠️ **THE 84 IS THE CONSTRAINT, AND IT IS EASY TO MISS.** Most declaring files are single-file fixtures and
examples with no project, no `source` root and no folder to put a `native/` in. So **`native/` cannot be
mandatory**: a loose one-file program must still be able to declare an `extern`. That makes this a way to
*organize and share* a project's FFI surface, not a way to forbid the seam elsewhere — which is a smaller
and much safer change than "externs move".

**Why it belongs after phase 4.** The hard part is not the folder; it is separating the **kama-side name**
from the **emitted C name**, which are identical for an extern today. The language already does that split
twice — `expose` gives a kama function a bare unmangled C name at the host boundary, and phase 4's keyword
escape gives `switch` the C name `k_switch`. Phase 4 builds the interning that makes a third case cheap,
so doing this first would mean building it twice.

⚠️ **The crux, and the thing a design has to answer before anything else: MANY KAMA NAMES, ONE C SYMBOL.**
`malloc` is genuinely one symbol in libc, and two unrelated projects both needing it is normal, not a
collision. Today's accidental collapse handles that correctly. Any model that gives each declaration a
module identity must keep it — `std::native::malloc` and `otherproj::native::malloc` must still be the
same `malloc` at link time, and must not be a duplicate-symbol error.

**Open, for the design session — none of these is decided:**

1. Is `native` the right reserved word? `extern` is already a keyword; `ffi`, `sys` and `c` are the other
   candidates. Whatever it is, it joins `global`/`std`/`core` in the reserved set — with §2f.29's
   asymmetry in mind, which reserves `global` in the manifest reader ALONE.
2. Is `native/` an ordinary entry in `modules`, or reserved and implicit? An ordinary entry costs nothing
   and keeps one rule; implicit means one fewer thing to write and one more thing to know.
3. Does an `extern` in `native/` need an `export` to leave its file, like everything else? Consistency
   says yes. That is also what makes the surface auditable rather than merely co-located.
4. What does an `extern` *outside* `native/` mean once the folder exists — still allowed (and still
   exempt), or an error in a project that has one? The 84 files above argue strongly for "allowed".
5. **The `extern "header.h";` seam.** It is per-FILE today, and it is what makes `spawn` work at all:
   `externsHeader("kama_isolate.h")` is a capability gate, and importing a symbol of a module is what
   loads the file carrying that seam (§3b retired the bare module import over exactly this). If externs
   move into `native/`, does the header association move with them, and does the gate still key on a
   loaded seam or on an imported symbol?
6. Does a dependency's `native` module obey `visibility` like any other module — i.e. must a library
   mark it `public` for a consumer to reach its FFI surface?

**6 — corpus and docs.** ⚠️ **Partly done in 2e's close-out, because a doc that contradicts a shipped
compiler is the exact failure this repo's house rule is about**: SPEC's *Modules* section is rewritten
(file-modules, the search, the declaration and `global::a::b::X` all described things that no longer
exist), `docs/packages.md` gained the `modules` section and its seeded manifests now match what `kama
seed` writes, and **`agents/AGENTS.md` — which SHIPS inside the binary — stopped telling users to write a
`namespace`**. FLOOR.md's `global::a::b::X`
paragraph went with it (§2f.29), and its "no browsable namespace" wording with that.

**What remains, re-counted at `0.9.80`:** `docs/packages.md`'s monorepo walkthrough and command table ·
`docs/targets.md` for `link` · the ROADMAP row and ROADMAP_DETAIL's §10 *C symbol naming* pointer, both
deleted when the campaign closes (phase 7) · and the **16 files that still spell `_F<n>`**, which cannot
be touched before phase 4 changes what it is: `tools/check-ecs-zero-dispatch.sh`, `tools/check-slot.sh`,
`tools/check-modules.sh`, `tools/embed_prelude.sh`, `run_tests.sh`, `docs/ENGINE_READINESS.md`,
`docs/ROADMAP.md`, `docs/ROADMAP_DETAIL.md`, this file, and **7 fixtures** (`tests/ctor_generic.kama`,
`poly_in_collection`, `enum_payload_unconstructed`, `constgen_value_widths`, `xfail/diag_no_mangled_name`,
`xfail/generate_serialize_generic`, `xfail/dup_fn`).

**7 — delete this file**, per its own header and the ROADMAP_DETAIL maintenance table.

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
- **`xfail` fixtures**, each `.msg` matching the rule's own wording. ⚠️ **Audited at `0.9.74` — some of
  this list is DONE and some of it was never an xfail's job:**
  - **Shipped:** §1a claims 1 and 4 became unrepresentable in phase 2 rather than checked, so there is
    nothing to reject; §2f's three are in (`global_absolute_path`, `import_alias_shadows_project`,
    `import_alias_claims_global`), and `mod_collision.d` / `mod_export_private.d` /
    `mod_export_undefined` predate them.
  - **Not xfail's:** every *manifest* rejection lives in `tools/check-manifest.sh` (44 assertions now) —
    that leg builds one `.kama` file and a manifest error needs a directory tree. That covers the
    `kama.json`-under-`source`, unknown-key, workspace and `modules`/`visibility` rows below.
  - **Still owed, and each blocked on the phase that creates its rule:** §1a claim 3 and a non-exported
    symbol in call position (**phase 3**); the `k_*` escape clash, a C keyword on an `expose`d name, and
    duplicate loose basenames (**phase 4**).
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
- **`VERSION` bumps on phases 1–5**; phase 6 is docs-only and phase 7 deletes this file.

---

## 7. Traps, so they are not re-derived

- **A doc is not evidence** — six documented behaviors in §1a do not hold; the old roadmap entry's
  exposure list was wrong in a way that hid two whole families (vtable slots, variant cases); and its
  keyword count was off (25 stated, 27 measured). Compile the snippet; read the emitted C.
- **Two numbers in an earlier draft of this doc measured nothing.** "81 declarations would need a `to`" was
  `lib`+`prelude` decl-lines minus export-list entries — and **`prelude/global.kama`**, the implicit
  prelude, has no `export` block at all, contributing 70 of the 81. The real figure is **zero**.
  (Said precisely on 2026-08-22, because "prelude has no export block" invites a re-measure that finds
  **three**: `prelude/std/memory/{owned,shared,weak}.kama`. Those are the namespaced built-in modules,
  which do export; the file the arithmetic was about is `global.kama`.) A follow-up "1" was a word match on `siftDown`,
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
