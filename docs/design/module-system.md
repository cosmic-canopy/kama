# The module system — identity, visibility, and the C symbol

*In-flight design doc. **Delete this file when the work ships**, once SPEC + ROADMAP carry the record — see
the maintenance table at the top of [ROADMAP_DETAIL.md](../ROADMAP_DETAIL.md).*

**→ START HERE.** One ROADMAP row points here. It replaces the former rows 1 and 2 (*C keyword
collisions* and *file-private C symbols are positional*), which are **not shipped** — they are folded in,
because both are downstream of the model this doc replaces.

The short version: kama today has **two ways to name a thing** — a `namespace` declaration and a
filesystem path — and nothing keeps them in agreement. Five separate claims in SPEC.md turn out to be
prose with no enforcement behind them (§1). The C symbol defects are what that ambiguity looks like once
it reaches the emitter. Fixing the naming without fixing the model would bake the ambiguity into the C
ABI, so this is one campaign: **replace the model, and the symbol rule falls out of it.**

---

## 1. Why — what was measured, not assumed

Every row below was **run** against `0.9.45+g84c7c58`. The house rule is that a doc is not evidence; that
rule is what produced this section, since all five were documented behaviors that do not hold.

### 1a. Five unenforced claims

| # | the claim | what actually happens |
|---|---|---|
| 1 | every `*.kama` in `a/b/c/` shares `namespace a::b::c` — [SPEC:3173](../SPEC.md) | two files in one directory may declare **different** namespaces. Accepted. A sibling reference then fails downstream with `unsupported method call on unresolved receiver` — never naming the real cause. Go rejects the same shape by name: `found packages bad (a.go) and sad (b.go) in .` |
| 2 | a namespace corresponds to its path | a flat `zzz.kama` may declare `namespace deeply::nested::thing;` and it builds. Resolution by path is only the *disk-search* rule — a file already in the compilation satisfies an import from anywhere |
| 3 | *"a qualified spelling reaches no further than an `import` would"* — [SPEC:3164](../SPEC.md) | enforced in **type positions only**. `import geo;` then `geo::secretFn()` on a **non-exported** function **builds, links and runs (exit 0)**. Same for `geo::Secret.make(…)`. The per-symbol import of the same name *is* rejected, so the two disagree |
| 4 | — | `export { … }` in a file with **no** `namespace` is silently accepted and entirely inert: nothing can import such a file |
| 5 | — | `export { };` is a **syntax error** — there is no way to spell an explicitly empty public surface |

Reproductions for 1, 3 and 4 are the smallest cases that show it; keep them as `xfail` fixtures when the
rules land (§7), because a prose claim that something is rejected is unguarded without one.

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

---

## 2. The model

### 2a. Projects

1. **A project requires a `kama.json`.** It is a **library** or an **executable**, stated by a new `kind`
   key. *Today the kind is inferred from whether `entry` is present and never recorded*
   ([kama.driver.cpp:5043](../../src/kama.driver.cpp)) — so this records something that already exists
   implicitly.
2. **`name` is the root namespace**, for both kinds, and is unique. A library's name is already required
   to be a legal kama identifier because importers write it.
3. **Projects do not nest.** `projects` is removed; a monorepo is sibling projects under a non-kama
   parent. Enforced as: **no `kama.json` inside `source`**. (A path dependency vendored under `.kama/`
   still has its own — that is a separate project, not nesting.)
4. **`sources` → `source`**, singular, defaulting to `"src"`. A list of roots would let `src/shapes/` and
   `gen/shapes/` silently be one module.
5. **Unknown manifest keys are an error.**

### 2b. Modules

6. **Folders are just folders.** Files and folders under `source` are project-global at any depth.
7. **A folder becomes a module by containing a `kama_module.json`**, which names it (defaulting to the
   folder name) and configures it. Nothing is a module by accident, and **folder layout never leaks into
   the API** — the goal that rules out every derive-from-path scheme.
8. **Modules do not nest**, by symmetry with projects. A file belongs to its **nearest ancestor module**,
   else to the project root — the same nearest-manifest walk the driver already does
   (`projectManifestDir`, [kama.driver.cpp:854](../../src/kama.driver.cpp)), reused rather than reinvented.
9. **`kama_module.json` carries no `version`.** A project is the smallest shareable unit.

> **Why flat-by-default is safe, measured.** Flatten the whole standard library — 49 files, 245 top-level
> declarations — into one namespace and you get **183 distinct names and exactly one collision**:
> `encode`, in `serialization/json/json.kama:198` and `serialization/binary/binary.kama:253`. That is
> precisely the case where a human reaches for a module. The ceremony lands on the rare case, which is
> what Python learned when PEP 420 **removed** the `__init__.py` requirement.
>
> *(The apparent duplicates in a first pass were `type intrinsic <int32> implements FromStr<This>`
> conformances — those declare no name. Worth knowing before re-measuring.)*

### 2c. Visibility

10. **One `import` block and one `export` block per file.** Nearly free: 388 of 521 importing files
    already have exactly one `import` line, the maximum anywhere is 4, and the largest symbol list is 12.
11. **The ladder mirrors type member access** — the analogy is deliberate, and it is the language's
    existing private-by-default instinct applied one level up:

    | spelling | meaning |
    |---|---|
    | *(nothing)* | private to the **file** |
    | `export { X }` | public to the world |
    | `export { X to [ targets ] }` | friend — visible only to the listed targets |

    A target is a **module** or a **file**. Note this changes today's semantics: privacy is currently
    per-**module**, so a sibling in the same directory-module can see an unexported name (verified). Under
    the new rule that access becomes an explicit `to` grant. **81 stdlib declarations** are the
    candidates (245 top-level, 164 currently exported).
12. **A file may export only what it declares.** **No `as` on export** — under folders-are-just-folders
    the motivating case (surfacing a nested private type at module root) evaporates, since the type is
    already in the module. `as` on **import** stays: it is the disambiguation escape when two modules
    export one name, and the compiler already reports that collision.
13. **`import { … }`** — listing a *module* name imports it for qualified use; listing a *symbol* brings
    it into scope. One spelling, one block, and the 49 bare-`import geo;` sites survive.

### 2d. Dependencies

14. **`dependencies` / `dev-dependencies` keep their role**: kama projects, from source, key = import
    root. Their semantics are already correct — dev deps are off the import path, resolve into
    `.kama/dev-deps`, and a dependency's dev deps are never dragged into your build
    ([packages.md:433–453](../packages.md)).
15. **Two additive keys, each with a dev counterpart**, for the kinds that contribute no import surface:
    - **executables run at build time** — built for the **host**, not the target being cross-compiled to.
      That axis is why Cargo keeps `build-dependencies` separate from `dependencies`, and it is a real
      difference, not a flag on an existing entry.
    - **native libraries linked into the output** — C ABI, reached through `extern fn`. **There is no
      manifest key for this today**: only per-target `select.TARGET.<n>.ldflags` and the CLI `--link`, so
      a project needing `-lm` on every target has nowhere clean to say so.
16. **Only a library can be imported.** Depending on an executable for a surface is an error.

> **Why kama libraries are source-only.** `OUTPUT = EXE | SHARED | STATIC | OBJECT` already ships
> ([kama.driver.cpp:1530](../../src/kama.driver.cpp)) — but a `SHARED` artifact exports *only* `expose`d
> functions ([targets.md:191](../targets.md)), i.e. the C ABI seam, not the kama surface. It cannot be
> otherwise while **115 of 245 stdlib declarations (47%) are generic**: `Map<MyType, int32>` cannot exist
> in an archive built before `MyType` was written — the same constraint that keeps C++ templates in
> headers and puts MIR rather than object code in a Rust `rlib`. Go shipped binary-only packages in 1.7
> and **removed them in 1.13**; Swift is the only peer that made it work, and it cost library-evolution
> mode plus `.swiftinterface` files, with generics paying a runtime cost across the boundary.

### 2e. The C symbol — rows 1 and 2, falling out

17. **Symbol = `name` · module (if any) · symbol**, joined by `__` exactly as `mangleNs` already joins a
    namespace path ([kama.cemit.cpp:256](../../src/kama.cemit.cpp)). **The file contributes nothing**, so
    moving a type between two files of one module is a refactor rather than a silent ABI break for anyone
    linking the emitted C.
18. **Loose files** — a `.kama` with no manifest — keep building and are the **only** remaining `_F<n>`
    case: basename-derived, symbols unimportable. This is not a small population today (there is no
    `kama.json` anywhere in this repo outside 16 test fixtures), but it is the one that by definition
    cannot be imported, so a basename is enough. **This design shrinks the naming problem enormously; it
    does not dissolve it.**
19. **C keyword collisions** — rename **only on collision**, every other name emitted exactly as written:
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
| **kama proposed** | project `name` + opt-in module folders | n/a — nothing is derived | the ladder in §2c |

Two details that decided §2b and §2c:

- **Rust** nests visibility downward: a private item is visible to its module *and all descendants*, so a
  child reaches into its parent's privates. That is an implicit grant nobody wrote — rejected here in
  favour of an explicit `to`.
- **Go** gives a subdirectory package **no** relationship to its parent in either direction; the only
  hierarchy-aware rule is `internal/`. That is the answer §2b takes.

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
25 words. Vala's `_name_` form is also not injective and puts a leading `_` on file-scope names.

**Determinism.** Rust's legacy mangling embedded a build-dependent hash and v0 exists to make symbols
reproducible, deriving from the crate's own identity rather than its position — the same lesson as §1b.

---

## 4. Open decisions

These are **not** settled. Each changes what gets built.

1. **`to [...]` semantics — friend list or scope boundary?** The §2c analogy is C++ `friend`: the
   exporter names its consumers. That inverts the dependency direction — a low-level file lists the
   high-level files permitted to use it, and the list is edited whenever a consumer appears. Rust faced
   the identical choice and picked a **boundary** (`pub(in path)`) that names nobody. Both defensible;
   they scale very differently. 81 declarations are the test set.
2. **Naming of the two new dependency keys**, and whether build-tools and native libraries share an
   umbrella key or take one each.
3. **Is a file addressable?** For `to [file]` targets only, or also for direct import? Note it must stay
   out of the **symbol** either way (§2e.17).
4. **The loose-file rule** in detail — basename as root, symbols unimportable.

---

## 5. Scale

Source-breaking and pre-1.0, so it lands before the tag or waits for 2.0. In this repo: **1,329 `.kama`
files** — 90 declaring a `namespace`, 78 with an `export` block, 521 carrying 715 `import` statements,
and 16 `kama.json`, all inside test fixtures. It also touches the resolver, the emitter, the LSP,
`kama query`, `kama seed`, and the docs.

One encouraging measurement: `lib/` and `prelude/` have **zero** namespace/path mismatches — the stdlib
is already congruent with the model, so its migration is mechanical.

---

## 6. Phases

**1 — the manifest.** `kind`; `sources` → `source` defaulting to `"src"`; drop `projects`; unknown keys
become an error; the new dependency keys; reject a `kama.json` under `source`. `kama seed` updated.

**2 — identity and resolution.** `name` as the root namespace; `kama_module.json`; nearest-ancestor
module resolution; delete the `namespace` declaration. **This is where claims 1, 2 and 4 of §1a become
unrepresentable** rather than merely checked.

**3 — visibility.** One import block, one export block, the three rungs, `to [...]`. Closes claim 3 — the
hole where `geo::secretFn()` bypasses the manifest — and gives claim 5 a spelling.

**4 — the C symbol.** §2e: identity-derived symbols; the `k_` escape interned once where each name enters
the emitter's tables (`ClassInfo` fields, `_paramNames`, `Scope::locals`, `VSlot::name`, variant records);
the loose-file rule; drop the `_N` suffix from generated `.c` filenames.

**5 — corpus and docs.** SPEC's *Modules / namespaces* section rewritten; the ROADMAP row and the §10
"C SYMBOL NAMING" entry deleted; the `_F4__` references updated in `tools/check-ecs-zero-dispatch.sh`
(4 lines), the comment in `tools/check-slot.sh`, and seven fixtures.

---

## 7. Verification

- **`tools/check-c-reproducible.sh`** — the guard this campaign exists for. Build one program twice with
  different argument orders into two temp dirs; `diff` the emitted `.c`/`.h` **and their filenames**:
  byte-identical, and `_F[0-9]` appears nowhere. Shape follows `tools/check-opaque-leak.sh:67`.
- **`tools/check-c-keywords.sh`** — transpile a fixture using every legal-in-kama C11/C23 keyword as a
  field, a parameter, a local, a contract method and an enum case; assert none appears as a declarator in
  the emitted C. Shape follows `tools/check-ecs-zero-dispatch.sh`.
- **`tools/check-module-visibility.sh`** — assert every rung of §2c, including the **call and
  construction** positions the current model lets through.
- **`xfail` fixtures**, each `.msg` matching the rule's own wording: one per claim in §1a, plus the `k_*`
  escape clash, a C keyword on an `expose`d name, a `kama.json` under `source`, an unknown manifest key,
  and a non-exported symbol reached in call position.
- Guards run in parallel: private `mktemp -d`, no writes to the worktree, no `cd` outside a subshell, and
  the compiler reached through `$KAMA` (`. "$ROOT/tools/kama-bin.sh"`), never the `./kama` symlink.
- `./dev matrix > /tmp/matrix.log 2>&1; tail -5 /tmp/matrix.log` — once, into a file — then `./dev test
  san` and `./dev test wasm`. The wasm leg emits one `.c` per unit, so it exercises the naming change.
- **`VERSION` bumps on every phase** — all of them touch `src/`.

---

## 8. Traps, so they are not re-derived

- **A doc is not evidence** — five documented behaviors in §1a do not hold; the old roadmap entry's
  exposure list was wrong in a way that hid two whole families (vtable slots, variant cases); and its
  keyword count was off (25 stated, 27 measured). Compile the snippet; read the emitted C.
- **`comm` is locale-sensitive on macOS**, so a keyword-set diff silently reported nonsense until it was
  redone with `grep -Fxv`. Any set comparison in a guard wants `LC_ALL=C` or no `comm` at all.
- **`qualify()` is not the site for the keyword fix.** It scope-prefixes *declared* names — exactly the
  set that already cannot collide. The exposed names are emitted by their own paths, and there is no
  single chokepoint.
- **The keyword escape is not idempotent.** `k_switch` would become `k_k_switch`. Intern once, at the
  table, never at the emission site.
- **A partial fix is worse than none** — a use must agree with its declaration, so renaming a declaration
  without its uses trades a keyword error for an undeclared-identifier error.
- **`demangleForDisplay`** ([kama.cemit.cpp:108–173](../../src/kama.cemit.cpp)) strips a leading
  `_F<digits>::` so diagnostics never leak a mangled name (`tests/xfail/diag_no_mangled_name.kama` guards
  it). Changing the scope form means changing that strip — from a *registry* of known scopes, not a
  pattern.
- **Measure with a hidden instrument first** if any step needs sizing: `--strict-numeric` and
  `--probe-templates` are the precedent (TSV on stdout, never `warning:` — `run_tests.sh:443` fails any
  fixture whose stderr matches `/warning/i`; full-row dedupe; and a bucket for the blind spot, because a
  measurement hiding its own blind spot is worse than no measurement).
