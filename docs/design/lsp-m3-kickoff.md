# LSP M3 — find-references + rename — kickoff / handoff

> **Status update (2026-07-27): M3.1–M3.3 are SHIPPED.** The rest of this brief is preserved as the design
> of record; the as-shipped summary lives in [lsp.md](lsp.md) "Progress". What changed against the plan
> below, and what a follow-on session needs to know:
> - **Option (A) was taken** and cost less than framed. The natural capture points turned out to be just
>   **five** call sites: `cType` ×3 (the single choke point for *every* type spelling — local decls,
>   fields, `new T`, casts, nested generic args) and `emitInvocation` ×2 (calls). `exprClass` /
>   `invocationReturnsPlace` / `scanExprForGenerics` were left uninstrumented on purpose: they re-walk the
>   same expressions, and identifier-node dedup absorbs them anyway.
> - **`_refIndex` is not a parallel structure.** Body refs merge into the existing `_positions`, and a
>   resolve-fill sweep at the tail of `buildPositions()` gives *every* entry its `declKey`. Consequences
>   worth keeping: `definitionAt`/`typeAtPosition` are now **`const` and do no resolution replay** (the
>   "non-const query" gotcha below is retired), and **hover + go-to-def now work inside bodies**.
> - **Gating:** a new `_analysis` flag (analysis ctor only) plus `_refUnit` (set in `emitModuleContent`).
>   `_out == &_analysisSink` is NOT a usable proxy — `emitProgram` reassigns `_out` per module.
> - **Two stale cites in this brief:** `resolveFunc` is at **kama.cemit.cpp:298**, not 253. And
>   `bin_search` in `kama.l` returns **`IDENTIFIER`**, not `-1`, when a word isn't a keyword (which is why
>   `getToken`'s `-1` check is dead code) — `kamaIsKeyword` must compare against `IDENTIFIER`.
> - **Scope decision 1 was settled the conservative way and is NOT permanent:** rename is guarded to the
>   open file, and **M3.5 (workspace indexing) is now part of this campaign**, not deferred to M5 — it is
>   what makes cross-file rename safe. M3.4 (locals/params/fields) comes first, since locals are the
>   commonest reference and are always single-file.

**Status: M3.1–M3.3 SHIPPED; M3.4/M3.5 remain (original cold-start brief follows).** M0 (query index + spans), M1 (server + diagnostics), and
M2 (hover / go-to-definition / document symbols) are complete on `dev` (`4a74dfe`, `e74a0bd`, `77709d4`;
see [lsp.md](lsp.md) "Progress" and the `next-lsp` memory). This is the self-contained handoff for **M3 —
find-references + rename**, the features that need the piece M0/M2 deliberately deferred: the **body
use-site index** (every *use* of a symbol → its definition). Read [GOALS.md](../GOALS.md) (favor
simplicity, few deps, one way to do a thing) before building.

**Build-environment changes since this brief was written** (dev-infra spike, 2026-07-27 — nothing about
M3's design changed, but the mechanics did):
- Build output is **platform-scoped**: `build/<os>-<arch>/kama`, with root `./kama` a symlink to whichever
  platform built last. A host `make` and a `tools/cdev make` no longer clobber each other — **no
  `make clean` when switching**. Shell scripts get the binary by sourcing `tools/kama-bin.sh`; the VS Code
  extension's `findKama()` prefers the native path, so a container build can't hand it a Linux binary any
  more (just reload the window after a rebuild).
- `tools/lspref.sh` (the byte-identical-emission oracle M3.1 must run) now resolves the binary the same
  way; its usage lines still write to `build/`, which is still gitignored.
- **A macOS host build now passes 793/793**, same as the container — so M3 can be developed and tested
  natively on this Mac, which is also what the VS Code extension exercises.
- **Semantic diagnostics got stronger.** M1 recorded that an undeclared type in a body (`Nonexistent x;`)
  produced NO diagnostic, so `check-lsp.sh` could only assert on parse errors. `checkTypeResolves`
  (kama.cemit.cpp, called from the local-decl path) now reports it, and the harness asserts the live
  squiggle. Two things follow for M3: the harness is a usable oracle for semantic diagnostics now, and
  **an unresolved name in a body is a reported error rather than a silent pass** — so `_refIndex` will
  simply have no entry for it, which is the right behaviour for find-references. Parameter/return/field
  positions are still silent (tracked in ROADMAP §2); if M3 wants them, the fix is a single-visit
  declaration pass, not a check at each of the 8+ emission sites.

## Goal & acceptance

In VS Code (and any LSP editor): **find-references** (Shift+F12) on a type/function decl or a reference to
it lists every occurrence in the document; **rename** (F2) on the same rewrites all of them atomically
(with a validity check). Hover/def/outline/diagnostics (M1/M2) stay live and unregressed. A
`tools/check-lsp.sh` case per feature asserts the responses over stdio.

## The one real new thing M3 must build: a use-site reference index

M0's position index (`buildPositions`, [kama.query.cpp:208](../kama.query.cpp)) indexes only **decl names
+ signature/type references** — it explicitly **never descends into `block` bodies** (that comment is at
`kama.query.cpp:190`). So today `definitionAt` can jump *from* a signature type ref *to* a decl, but there
is **no reverse map** (decl → all its uses) and **no body coverage** (a `Point m;` local decl, a
`Widget.make(...)` call, a `midpoint(...)` call inside a function body are invisible). M3 adds both.

**Data structure:** a reverse index keyed by the same resolved `DefSite` key the rest of the facade uses:
```cpp
std::map<std::string /*declKey*/, std::vector<SrcRange>> _refIndex;   // every USE site of a symbol
```
`referencesAt(uri,l,c)` = resolve the cursor to a `declKey` (exactly as `definitionAt` already does) →
return `_refIndex[declKey]` (+ optionally the decl's own `selectionRange`). Rename reuses the same set.

## How to populate `_refIndex` — the key design decision (settle FIRST)

Body identifiers resolve through **local scope** (locals, params, `this`, member access, overloads) — not
just the unit's namespace context. Re-implementing that scope resolution in a standalone walker (the way
`buildPositions` does the *signature* walk) would **duplicate the resolver** and rot. Two options:

- **(A) Piggyback on the real resolver during `analyze()`.** The full emit-into-sink walk already resolves
  every name correctly (locals, members, overload/contract dispatch). Add a lightweight recording hook at
  the handful of resolution sites so each resolved use appends `(declKey, SrcRange)` to `_refIndex`. Reuses
  the actual resolver — always correct, no scope re-implementation. The cost: threading a
  `recordRef(id, key)` call into a few functions, guarded so it perturbs nothing (append-only to a member
  vector; ideally gated to analysis mode via an existing flag so `kama build` pays nothing).
- **(B) A separate post-analysis body walker** re-implementing scope. Rejected — duplicates 15 K lines of
  resolution logic; the exact trap M0 avoided.

**Lean: (A).** The natural capture points (all in `kama.cemit.cpp`) are where a *source* identifier is
turned into a resolved key:
- **`resolveFunc`** ([kama.cemit.cpp:253](../kama.cemit.cpp)) — call sites (`midpoint(...)`,
  `Widget.make(...)`). ~20 call sites already exist; the resolver itself is the single choke point to
  instrument (record `(callIdent range, returned key)` when the key is a user `DefSite`).
- **`resolveUserName`** ([kama.cemit.h:644](../kama.cemit.h)) — type mentions in bodies (`Point m;`,
  `Widget w;`), the analogue for types.
- **Member/field access** (`.x`, `.origin`) and **locals/params** — these have **no `DefSite` node yet**
  (M0 deferred per-field/member/local nodes). Cover them only in **M3.3** (below); v1 targets types + free
  functions, which already have `DefSite` keys.

Because `resolveUserName`/`resolveFunc` are the exact functions `definitionAt` replays, instrumenting them
means find-references and go-to-def agree by construction.

## Staging (each `tools/cdev make && tools/cdev test` green)

- **M3.1 — reference index for types + free/generic functions.** Add `_refIndex` + `recordRef` hook in
  `resolveUserName`/`resolveFunc` (analysis-mode-gated), built during `analyze()`. Add facade
  `std::vector<Location> referencesAt(const std::string& uri, int line, int col, bool includeDecl)` to
  `kama.query.h`/`kama.cemit.{h,cpp}` (resolve cursor → declKey → `_refIndex` ranges, each as a `Location`
  in this unit). Emission must stay byte-identical (run the `tools/lspref.sh` --no-line oracle).
- **M3.2 — LSP `textDocument/references`.** Driver seam `lspReferences(handle, path, l, c, includeDecl)`
  (global scope, next to the M2 seams); dispatch case in `kama.lsp.cpp`; capability
  `referencesProvider: true`; read `context.includeDeclaration` from params; map results via
  `srcRangeToJson` + `pathToUri` (both from M2). Convert LSP pos → kama first (`kamaLine = lspLine + 1`).
- **M3.3 — rename.** `textDocument/rename` → a `WorkspaceEdit` `{changes: {<uri>: TextEdit[]}}`, one
  `TextEdit` (new name) per reference range + the decl's `selectionRange`. Add
  `textDocument/prepareRename` → the identifier range at the cursor or `null` (reject on
  builtins/keywords/unresolved so the editor greys out F2). **Validate** the new name is a legal kama
  identifier before emitting edits (reuse the lexer's identifier rule or a small `[A-Za-z_][A-Za-z0-9_]*`
  check). Capability `renameProvider: {prepareProvider: true}`.
- **M3.4 (stretch / could defer) — locals, params, fields, enum members.** Needs per-local/param/field
  **def-site nodes** (M0 deferred — the tables keep none) + scope-lifetime handling. High value (most body
  references are locals) but its own sub-project; land after M3.1–M3.3 or split to M3.5.

## Scope decisions to settle FIRST (with leans)

1. **Single-file vs. workspace.** ⚠️ **UPDATED (post-M2, commit `9a7c15e`):** `lspAnalyze` now
   **loadProgramUnits() the open file's transitive imports** (to kill false cross-module diagnostics), so
   the analyzed index already spans **all imported units**, not just the open buffer — `_refIndex` built
   during `analyze()` would therefore capture use-sites **across every loaded module for free**. BUT this
   is still not full workspace coverage: only modules reachable via the open file's import graph are loaded
   (a file that imports *this* symbol but isn't itself imported is invisible), and only the open file's
   buffer is live (imports are read from disk). *Lean:* **references may span the loaded units** (read-only,
   safe, and a nice win); **rename stays conservatively guarded** — safe only when every use is in the open
   file, OR restrict to file-private symbols; renaming into a std/imported file on disk is out of scope
   (and prelude/std are `DefSite.unit`-filtered anyway). Full reverse coverage (find a symbol's users that
   the open file doesn't import) still needs real **workspace indexing** (`workspace/didChangeWatchedFiles`
   + a project-wide unit set) — a separate infra milestone (M5 or a dedicated M3.6). Re-confirm what the
   loaded-unit set actually contains before designing M3.3's rename guard.
2. **Which symbols are renameable.** User `DefSite`s only — never prelude/std/builtins (`DefSite.unit ==
   nullptr` filters them, same guard the outline uses). `prepareRename` returns `null` for those.
3. **Include the declaration in references?** LSP passes `context.includeDeclaration`; honor it (add the
   decl's `selectionRange` when true). Rename always includes the decl name.

## Reuse inventory (all on `dev`)

- **Cursor→declKey resolution:** `definitionAt` ([kama.query.cpp:288](../kama.query.cpp)) already does the
  exact save/restore `_nsCtx = _unitCtx[unit]` + `resolveUserName` replay. `referencesAt` factors the same
  resolution, then indexes forward into `_refIndex` instead of `_defSites`.
- **`_defSites` / `DefSite`** (`kama.query.h`) — keys, `selectionRange` (the rename target range), owning
  unit filter. `_positions`/`posAt` — cursor hit-testing (smallest-span-wins), reuse verbatim.
- **M2 server plumbing** (`kama.lsp.cpp`): `srcRangeToJson`, `pathToUri`, `lspRange`, `Json::getStr2`, the
  LSP↔kama coord map, the dispatch/capability pattern, the opaque `SharedLspIndex` handle + query seams.
  M3 adds two seams (`lspReferences`, `lspRename`) and 2–3 dispatch cases in the same shape.
- **Harness:** `tools/check-lsp.sh` — reuse the decl-rich `shapes` buffer + `frame`/`expect`; add
  `references` (assert N ranges incl. the decl) and `rename` (assert the `WorkspaceEdit` TextEdits) cases.
- **Oracle:** `tools/lspref.sh` (in `build/`, gitignored) — regenerate + diff to prove the `analyze()`
  instrumentation left emission byte-identical.

## Gotchas (carried from M0–M2)

- **Anon-namespace seam rule** — every LSP-facing driver function at **global scope after `} //
  namespace`** ([kama.driver.cpp:2900](../kama.driver.cpp), where `lspAnalyze` and the M2 seams already
  live); the biggest recurring trap.
- **Coord convention** — kama line 1-based/col 0-based ↔ LSP 0/0. Convert on the way in, `srcRangeToJson`
  on the way out.
- ~~**`definitionAt`/`typeAtPosition` are non-const**~~ — **no longer true.** M3.1's resolve-fill sweep
  precomputes every position's `declKey`, so the whole query facade is `const` and touches no `_nsCtx`.
- **Don't call `cType`** in query paths — its `unsupported()` side effect pollutes `_diagnostics` (the M0
  rule). The `recordRef` hook must be a pure append.

---

# M3.4 — locals, params, fields, enum members (cold-start brief)

> **Status: ✅ SHIPPED 2026-07-27 (`91e27a3`, dev).** Emission byte-identical across all 534 lspref
> fixtures; native 794/794, container 794/794, ASan/UBSan 765/765. The brief below is kept as the
> reconnaissance of record — **read the "As shipped" section at the end of it first**, which records
> where the plan changed under contact.

This is the milestone that makes find-references/rename useful on the symbols users touch most.
M3.1–M3.3 cover types, contracts, enums (the type), methods, ctors and free/generic functions;
everything below was still invisible to the index.

The four kinds are **not equally hard** — do them in this order.

## (a) Enum members — EASY, no new retention

`EnumMemberDeclarationNode` ([kama.ast.h:1030](../kama.ast.h)) already carries its own
`SharedIdentifier identifier`, and `EnumDeclarationNode` ([kama.ast.h:1008](../kama.ast.h)) carries the
member list as `body`. `buildDefSites` already reaches the decl node via `_enumDeclNodes`
([kama.query.cpp:167-175](../kama.query.cpp)) — so def-sites are one loop over `en->body` away, keyed
`<enumKey>_<Member>` to match what the emitter spells. `SymKind::EnumMember` already exists and is unused.

**Uses** are recorded at the identifier arm below — enum member references (`Enum.Member`) are handled at
[kama.cemit.cpp:1052-1058](../kama.cemit.cpp), which already calls `resolveUserName` on the enum name and
then looks up `_enums`; the member name is `nm` on the same node.

## (b) Locals + params — MEDIUM; the scope plumbing exists, the def-sites don't

What already exists and should be reused:
- **`findScopeDeclaring(name)`** ([kama.cemit.cpp:1467](../kama.cemit.cpp)) — innermost-out scope search;
  its comment states `declaredNames` records EVERY local, so it is **exact for locals**, and `-1` means
  "a parameter (params outlive any scope) or unknown".
- **Scope registration** at [kama.cemit.cpp:2053-2060](../kama.cemit.cpp)
  (`_scopes.back().declaredNames.push_back(nm)`) — the natural def-site creation point.
- **THE USE CHOKE POINT: [kama.cemit.cpp:1049](../kama.cemit.cpp)** — `if (auto* v =
  dynamic_cast<IdentifierNode*>(n))`, commented *"Variable/parameter reference."* This is the arm every
  bare identifier reference flows through, and it holds the source `IdentifierNode` (position included).
  It is the locals analogue of what `cType`/`emitInvocation` were for M3.1 — one site, not a long tail.

What must be built:
1. **`Scope::declaredNames` is `std::vector<std::string>` — strings only, no position and no key.** It has
   to carry the declaring `IdentifierNode*` (or a synthesized declKey) so a use can be tied to *which*
   declaration. This is the one intrusive change; `struct Scope` is at
   [kama.cemit.h:673](../kama.cemit.h), the stack `_scopes` at [kama.cemit.h:685](../kama.cemit.h).
   ⚠️ `declaredNames` is read by `findScopeDeclaring` and by the enclosing-scope walk at
   [kama.cemit.cpp:2053](../kama.cemit.cpp) — update both readers with the type.
2. **Synthetic keys must handle SHADOWING.** Two locals named `i` in sibling scopes are different symbols
   and must not merge. Key on the declaration site, e.g. `local:<fnKey>:<name>@<line>:<col>` — unique by
   construction, and the scope entry carries it so uses resolve to the right one.
3. **Params need a grammar fix** (below).

## (c) Fields — MEDIUM; needs new retention

`FieldInfo` ([kama.cemit.h:51](../kama.cemit.h)) holds `name` as a **plain `std::string`** and a `type`
identifier — there is **no name IdentifierNode and no decl node**, so a field name has **no source span
today**. Add a `SharedIdentifier nameId` (or the `ClassFieldDeclarationNode*`) to `FieldInfo` and populate
it in `collectClasses`; then def-sites follow. Field **uses** are `MemberAccessNode`, which does carry the
member name as its own `SharedIdentifier identifier` ([kama.ast.h:534](../kama.ast.h)) — good — but see
the grammar fix.

## ⚠️ The grammar prerequisite: STAMP_LOC (do this FIRST, it is load-bearing)

`YYLLOC_DEFAULT` ([kama.y:22-45](../kama.y)) stamps each rule's `@$` into the CodeGenContext, and the
`ASTNode(context)` ctor picks it up. So **an IdentifierNode built mid-action from a raw `IDENTIFIER` token
inherits its WHOLE PRODUCTION's span, not the name's own** — the header comment says exactly this, and
`STAMP_LOC` ([kama.y:48](../kama.y)) is the fix. Today STAMP_LOC is applied **only at function / method /
ctor name sites** (kama.y:635, 642, 648, 665, 684, 1359-1362, 1443, 1445).

**MEASURED, not reasoned** (2026-07-27, via a throwaway probe over a fixture covering all four kinds).
The rule that falls out: *a name's span is correct only when its production is the bare `IDENTIFIER`
token.* It gets a **wrong end** when the name LEADS a multi-symbol rule, and a **wrong start** when it
TRAILS one:

| kind | production | measured | needs STAMP_LOC |
|---|---|---|---|
| enum member, no value | `IDENTIFIER` | `Red` → 3:13..3:16 ✅ exact | no |
| enum member `= v` | `IDENTIFIER EQ const_expr` | `Ok` → 3:12..**3:18** (swallows `= 1`) | **yes, @1** |
| field, no initializer | `variable_declarator: IDENTIFIER` | `limit` → 6:17..6:22 ✅ exact | no |
| field/local `= init` | `IDENTIFIER EQ initializer` | `seeded` → 13:10..**13:20** (swallows `= 7`) | **yes, @1** |
| local, no initializer | `IDENTIFIER` | `plain` → 12:10..12:15 ✅ exact | no |
| **parameter** | `… type IDENTIFIER` | `bias` → **8:24**..8:34 (starts at the TYPE) | **yes, @5** |
| **member access** | `primary DOT IDENTIFIER` | `limit` → **14:28**..14:35 (starts at the receiver) | **yes, @3** |

🔴 **This is a data-loss bug if M3.4 ships without the fix, not a cosmetic one.** Rename REPLACES the
range, so renaming a local declared `int32 seeded = 7;` would rewrite `seeded = 7` → `newName`, silently
eating the initializer. Land the STAMP_LOC changes and assert the ranges BEFORE wiring rename to any of
these symbol kinds.

Exact sites to fix (verified 2026-07-27):

| kama.y | rule | fix |
|---|---|---|
| **773** | `variable_declarator : IDENTIFIER EQ variable_initializer` | `STAMP_LOC(<name>, @1)` |
| **791** | `constant_declarator : IDENTIFIER EQ constant_expression` | `STAMP_LOC(<name>, @1)` |
| **1531** | `enum_member_declaration : IDENTIFIER EQ constant_expression` | `STAMP_LOC(m->identifier, @1)` |
| **1532** | `enum_member_declaration : IDENTIFIER LPAREN parameter_list RPAREN` (tagged variant) | `STAMP_LOC(m->identifier, @1)` |
| **744** | `parameter : const_opt hardware_opt parameter_modifier_opt type IDENTIFIER` | `STAMP_LOC(p->identifier, @5)` |
| **430, 1034, 1035, 1036** | the `MemberAccessNode` productions | `STAMP_LOC(<name>, @3)` |

Lines 772 (`variable_declarator : IDENTIFIER`), 792 (`constant_declarator : IDENTIFIER`) and 1530
(`enum_member_declaration : IDENTIFIER`) are the single-token arms — already correct, leave them alone.
Note 773/791 build the node inline as a ctor argument, so the STAMP_LOC needs a named local (or reach it
back off the constructed declarator, e.g. `STAMP_LOC($$->name, @1)`).

⚠️ Recall the grammar trap from the MCU campaign: **every `_opt` rule MUST set `$$`** (an empty rule with
no `$$ =` yields garbage). Touching these rules re-runs bison; `tools/check-syntax-drift.sh` guards the
keyword/highlighter side.

## Fixture is already in place

**`tests/query/scopes.kama`** was written for this milestone and compiles clean today. It covers every
row of the table above — each kind BOTH with and without an initializer/value — plus **two `shadow`
locals in sibling scopes**, so the per-declaration keying requirement is exercised (find-references on
one must not return the other's uses, and renaming one must not touch the other). ⚠️ `out` is a RESERVED
KEYWORD (it broke the first draft of this fixture); the shadowing accumulator is named `acc`.

## Rename note

A local, param or field can never be referenced from another file, so **the M3.3 open-file guard never
fires for them** — M3.4 delivers full-fidelity rename for the commonest case without waiting on M3.5.

## As shipped (2026-07-27, `91e27a3`) — where the plan changed under contact

The recon above held up: the choke points were where it said, the ordering (enum members → locals/params
→ fields) was right, and the grammar prerequisite was real. Four things came out differently.

1. **`Scope::declaredNames` did NOT need its type changed** — the brief's one intrusive change was
   avoidable. `Scope` gained an **analysis-only `std::vector<IndexDecl> indexDecls` parallel to it**
   instead. Two reasons this is strictly better: `declaredNames` drives the shadowing *rules*
   (`findScopeDeclaring` + the enclosing-scope walk), so leaving it alone means zero behavioural risk;
   and it deliberately excludes `foreach` and `match` bindings, which the index *does* want. Scope pop
   is still what makes sibling-scope `shadow` locals distinct.
2. **Binding declarations are recorded during the walk, not reconstructed after it.** Locals/params have
   no table entry to key on, so `recordDef` / `_localDefs` mirror M3.1's `recordRef` / `_bodyRefs`, and
   `buildDefSites` materialises them at the tail. One helper, `registerBinding(declSite, kind)`, is the
   single entry point used by all five declaration sites.
3. **Keys are prefixed** (`local:<file>:<line>:<col>:<name>`, `field:<Owner>::<name>`,
   `enum:<Enum>::<name>`) — an index-only namespace that cannot collide with a `resolveUserName` /
   `resolveFunc` result. ⚠️ Do **not** key an enum member by its emitted C spelling `Enum_Member`.
4. **`documentSymbols` needs an explicit Local/Param filter**, or the outline floods with every local in
   the file. Fields and enum members stay in. A side effect worth knowing: driving enum def-sites off
   `_enumDeclNodes` instead of `_enums` means a **tagged** enum's TYPE finally gets a def-site — the
   `_classes` loop skips variant backings, so it had none before.

**Two more spans turned out to be the same data-loss bug and were fixed here rather than deferred**
(both were load-bearing for symbols M3.1 *already* renamed):
`qualified_identifier_no_generic : qualifier IDENTIFIER` (kama.y:485) — the production an `Enum::Member`
read or a `mod::fn` call reduces through, whose whole-production span would have eaten the qualifier —
and the generic `basic_identifier : IDENTIFIER LT type_arg_list GT` arm (kama.y:464), whose span would
have eaten `<A, B, …>`. Note `qualified_identifier : qualifier basic_identifier` does **not** need one:
it reuses `$2` rather than constructing a node.

**Still not covered, by decision:** the tail segments of a dotted chain `obj.a.b`
([kama.cemit.cpp:1080](../kama.cemit.cpp) / [1089](../kama.cemit.cpp)) are string-concatenated without
ever resolving a `FieldInfo`, so `b` is unindexed. That needs a per-segment type walk that does not exist.

**Perf watch:** every local in the open file's whole import closure is now indexed, on top of the
per-keystroke reparse. A real multi-import stdlib file measures ~0.25 s end to end — the same order as
before, since parsing dominates. This is M5 (incremental) territory; if it ever needs a quick lever, skip
recording when `_refUnit == _preludeUnit`.

---

## ⚠️ Campaign-exit checklist — the remaining STAMP_LOC sites

**User requirement (2026-07-27): these must be closed before the LSP campaign ends.** Each is a
production that builds an `IdentifierNode` mid-action and therefore inherits its whole production's span.
None is reachable by rename *today*, which is why they were not part of M3.4 — but every one of them
becomes a silent bad-rewrite the moment its symbol kind is indexed, so treat this as a correctness debt,
not a cosmetic one. The fix is mechanical (`STAMP_LOC(<the name node>, @N)`; where `%type` is a base
handle, restructure to `auto v = …; STAMP_LOC(v->name, @N); $$ = v;`).

| kama.y | production | name token |
|---|---|---|
| 351 | `using_declaration : IDENTIFIER AS IDENTIFIER` | `@1` (name) **and** `@3` (alias) |
| 578 | `type_decl_head : IDENTIFIER LT type_param_list GT` (rule head 576) | `@1` |
| 620 | `friend_member_list COMMA IDENTIFIER` | `@3` |
| 721-723 | `type_param`, three arms (rule head 720) | `@1`, `@1`, `@2` |
| 1067 / 1073 / 1083 / 1164 | turbofish + `new`-turbofish method names | `@3` / `@3` / `@3` / `@4` |
| 1092 | `generic_turbofish_name : IDENTIFIER COLONCOLON LT … GT` (head 1091) | `@1` |
| 1107-1109 | named-argument labels (`ArgumentNode::name`) | `@1` |
| 1129 / 1131 | attribute-argument labels | `@1` |
| 1157 / 1159 / 1160 / 1166 | `ObjectCreationNode::ctorName` (named ctors) | `@5` / `@4` / `@7` / `@4` |
| 1318+ | `when_cond_list` params and bounds (rule head 1314) | various |
| 1422 / 1424 / 1425 | operator-declarator parameter names (head 1414/1421) | `@8` / `@6` / `@6`+`@9` |
| 1461 | `constructor_declarator : IDENTIFIER LPAREN …` (head 1460) | `@1` |
| 1475 | `destructor_declaration : modifiers_opt TILDE IDENTIFIER …` (head 1474) | `@3` |

Line numbers verified against `91e27a3`. Two entries are **not** just span polish: named-argument labels
(1107-1109) name the callee's *parameter*, so indexing them is what would let renaming a parameter update
its call sites — real M4/M5 functionality, impossible today. `when_cond_list` (1318+) pushes identifiers
straight into a list with no field to reach, so it needs restructuring, not just a stamp.

---

# M3.5 — workspace indexing (cold-start brief)

**Status: NOT STARTED.** This is what makes cross-file rename safe and **lifts the M3.3 guard** — it is
part of this campaign, not deferred to M5.

The problem it solves: `lspAnalyze` ([kama.driver.cpp:2913](../kama.driver.cpp)) loads the open file plus
its **transitive imports**. References across that set are already correct, but the set is *reachability
from the open file* — a file that imports THIS symbol without being imported back is invisible. So rename
can see some users and not others, which is why M3.3 refuses rather than half-rewriting.

Shape:
1. A **project-wide unit set** rather than an import-graph closure — enumerate the project's `.kama`
   sources (the `kama.json` manifest already declares the package; reuse `loadProgramUnits`' resolver
   rather than a new file walker).
2. `workspace/didChangeWatchedFiles` to keep it fresh, plus the `workspaceFolders` capability.
3. Then `referencesAt` spans the project and `handleRename`
   ([kama.lsp.cpp](../kama.lsp.cpp), the `r.uri != path` loop) drops its refusal and emits a multi-file
   `WorkspaceEdit` — still refusing to write into std/prelude (`DefSite.unit == nullptr` already filters
   those, and loaded std MODULES should be excluded explicitly since their units are non-null).
4. Watch the perf budget: whole-project analysis per keystroke is the thing M5 (incremental) exists for;
   M3.5 should index once and update per file change, not per edit.

## After M3

**M4** — completion + signature-help (the quality-hard one; delivers the `global::` floor-completion
payoff). **M5** — error-recovery + incremental/perf. **M6** — broadened editor clients + `docs/editors.md`
(Neovim `nvim-lspconfig`, Vim, Emacs eglot) — one server, so each editor is a few lines of client glue;
the user wants all major IDEs ASAP.
