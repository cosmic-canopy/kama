# LSP M3 — find-references + rename — kickoff / handoff

**Status: READY TO BUILD (cold-start brief).** M0 (query index + spans), M1 (server + diagnostics), and
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
- **`definitionAt`/`typeAtPosition` are non-const** (mutate `_nsCtx`, save/restored); `referencesAt` will
  be too. Single-threaded dispatch keeps it safe — don't share a handle across threads.
- **Don't call `cType`** in query paths — its `unsupported()` side effect pollutes `_diagnostics` (the M0
  rule). The `recordRef` hook must be a pure append.

## After M3

**M4** — completion + signature-help (the quality-hard one; delivers the `global::` floor-completion
payoff). **M5** — error-recovery + incremental/perf (+ workspace indexing if not done in M3). **M6** —
broadened editor clients + `docs/editors.md` (Neovim `nvim-lspconfig`, Vim, Emacs eglot) — one server, so
each editor is a few lines of client glue; the user wants all major IDEs ASAP.
