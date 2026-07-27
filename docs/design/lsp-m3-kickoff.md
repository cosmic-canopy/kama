# LSP M3 — find-references + rename — kickoff / handoff

**Status: READY TO BUILD (cold-start brief).** M0 (query index + spans), M1 (server + diagnostics), and
M2 (hover / go-to-definition / document symbols) are complete on `dev` (`4a74dfe`, `e74a0bd`, `77709d4`;
see [lsp.md](lsp.md) "Progress" and the `next-lsp` memory). This is the self-contained handoff for **M3 —
find-references + rename**, the features that need the piece M0/M2 deliberately deferred: the **body
use-site index** (every *use* of a symbol → its definition). Read [GOALS.md](../GOALS.md) (favor
simplicity, few deps, one way to do a thing) before building.

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

1. **Single-file vs. workspace.** The LSP path (`lspAnalyze`, [kama.driver.cpp:2894](../kama.driver.cpp))
   analyzes **one buffer + prelude** (`analyze({ pr.unit })`) — it does **not** load imported units. So M3
   sees only the **current document**. *Lean:* **v1 = single-file references + rename**, and **guard
   rename**: it is only safe for a symbol whose every use is in-file. For an exported/importable symbol,
   either (a) restrict rename to file-private symbols, or (b) ship it single-file but **document the
   limitation loudly** (the harness note pattern from M2). True cross-file rename needs **workspace
   indexing** (load all program units, `workspace/didChangeWatchedFiles`) — a separate infra milestone
   (fold into M5 or a dedicated M3.6). Settle this before M3.3, since it bounds rename safety.
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
  namespace`** ([kama.driver.cpp:2882](../kama.driver.cpp)); the biggest recurring trap.
- **Coord convention** — kama line 1-based/col 0-based ↔ LSP 0/0. Convert on the way in, `srcRangeToJson`
  on the way out.
- **`definitionAt`/`typeAtPosition` are non-const** (mutate `_nsCtx`, save/restored); `referencesAt` will
  be too. Single-threaded dispatch keeps it safe — don't share a handle across threads.
- **Don't call `cType`** in query paths — its `unsupported()` side effect pollutes `_diagnostics` (the M0
  rule). The `recordRef` hook must be a pure append.

## After M3

**M4** — completion + signature-help (the quality-hard one; delivers the `global::` floor-completion
payoff). **M5** — error-recovery + incremental/perf (+ workspace indexing if not done in M3). **M6** —
broadened editor clients + `docs/editors.md` (Neovim `nvim-lspconfig`, Vim, Emacs eglot) — one server, so
each editor is a few lines of client glue; the user wants all major IDEs ASAP.
