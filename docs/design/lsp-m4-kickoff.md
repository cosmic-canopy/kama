# LSP M4 — completion + signature help (cold-start brief)

**Status: ✅ SHIPPED 2026-07-28** (M4.0–M4.9). The brief below is preserved as written; see
**"As shipped"** at the end for where it was wrong, which was in three load-bearing places. Written immediately after M3.5 (`469da24`); **line numbers
re-verified against `a99436d`**, which is where the `kama.driver.cpp` references below now point. Read
[lsp.md](lsp.md) first for campaign context, then this.

### What moved under this brief since it was written

The workspace-deps campaign ([workspace-deps-kickoff.md](workspace-deps-kickoff.md), shipped 2026-07-28)
rewrote large parts of `kama.driver.cpp`. Nothing it did blocks M4, but three things touch the LSP:

- **`kama.driver.cpp` line numbers drifted ~300 lines.** The staging step 2 references below are
  corrected; `kama.lsp.cpp`, `kama.cemit.*`, `kama.query.cpp` and `kama.y` were not touched by it and
  their references still stand.
- **`resolveModuleFiles` gained an optional `matchedRoot` out-param**, and now expands a dependency's
  declared `sources` — so `lspAnalyze`'s import loading resolves a `src/`-layout package, which it could
  not before. Strictly more files reachable; nothing to undo.
- **`loadProgramUnits` can now print a warning to stderr** (a package importing something its own
  manifest does not declare). It is warn-once per process, so it will not repeat per keystroke — but it
  *is* a real diagnostic going to the editor's log channel rather than to the Problems pane. Promoting it
  to a `textDocument/publishDiagnostics` entry against the offending `kama.json` is a natural small win
  once M4's plumbing exists; it is not a prerequisite.

M3 is complete: the index answers *"what is at this cursor, and where else is it used"* across a whole
project. M4 asks the harder question — *"what could go here"* — which the index was never built to
answer. The milestone table calls it **`L`, "the quality-hard one"**, and that is the right expectation:
the plumbing is a day, the *quality* of the suggestions is the milestone.

Scope, per [lsp.md](lsp.md):186 and [ROADMAP.md](../ROADMAP.md):542 — four completion contexts
(**members after `.`/`::`**, **names in scope**, **import paths**, **keywords**), plus **signature help**,
plus the **`global::` resolver alias** whose completion payoff is why it was deferred to an LSP existing
([logging.md](logging.md) Part E, [FLOOR.md](../FLOOR.md)).

---

## ⚠️ Two hard prerequisites — schedule these FIRST, they are not part of "wire up the protocol"

### (1) The scope stack is DEAD by the time the index is built

`CEmitter::Scope` ([kama.cemit.h:705-720](../kama.cemit.h)) and `_scopes` (:724) are **cleared at every
function/method/ctor/dtor entry** (kama.cemit.cpp:10881, 11314, 11392, …) and are empty by the time
`analyze()` runs `buildDefSites()`/`buildPositions()` (kama.cemit.cpp:46-65). M3.4's
`Scope::IndexDecl` holds **only `{name, key}`** — deliberately no range.

So after analysis you have every local/param/foreach/match binding as a `DefSite` with its *declaration*
position and `SymKind::Local|Param`, but **no scope extent** — you cannot answer "is this local visible
at line L, col C" from the index at all. Names-in-scope completion is impossible until that changes.

Three options; **(b) is the smallest**:
- (a) record a scope *extent* + parent link at `popScope()` into a retained per-unit scope tree;
- **(b) store the enclosing-function key and a `[startLine, endLine)` on each local's `DefSite`** —
  `RecordedDef` ([kama.cemit.h:504-505](../kama.cemit.h)) already has the shape, and
  `DefSite::container` is currently passed `""` for bindings ([kama.query.cpp:291](../kama.query.cpp)),
  so it is a free field. `popScope()` (kama.cemit.cpp:1365) is the one place that knows a scope is ending;
- (c) a re-walk at query time.

Note (b) approximates: a scope's end line is enough for "visible here?", but sibling scopes that share a
line would blur. M3.4 keyed bindings by declaration site precisely so sibling-scope shadowing works —
don't regress that. Verify against `tests/query/scopes.kama`, which already has two `shadow` locals in
sibling scopes.

### (2) Named-argument label spans are still whole-production

kama uses **named parameters** (`f(a: 1)`), so signature help's *active parameter* is determined by which
label the cursor is in or after. `ArgumentNode::name` ([kama.ast.h:549-559](../kama.ast.h)) is built
mid-action at **kama.y:1107-1109** and therefore inherits its whole production's span — the
campaign-exit checklist in [lsp-m3-kickoff.md](lsp-m3-kickoff.md):347-380 already flags this entry as
*"real M4/M5 functionality, impossible today"*, not span polish.

Fix it with the rest of that checklist (a user requirement for campaign exit anyway), and do it **before**
signature help, not after — M3.4's lesson was that a wrong span is a data-loss bug, not a cosmetic one.

---

## What the index already gives you for free

**Member completion after `.` / `::` is nearly a solved problem** — the containers are plain and the
inheritance walks already exist:

| need | where |
|---|---|
| expression → mangled class name | `exprClass` ([kama.cemit.cpp:12546](../kama.cemit.cpp)) — handles `this`, locals, member chains, smart-ptr and user-`Deref<T>` auto-deref, `ElementAccess` |
| is the receiver a TYPE or an instance? | `isTypeReceiver` (kama.cemit.cpp:13400) — separates `Type.ctor(…)` from `x.method(…)`, and resolves generic templates + bound type-params |
| fields, incl. inherited | `ClassInfo::fields` (`FieldInfo` carries `nameId`, type, visibility); walk the `base` chain the way `findFieldOwner` (kama.cemit.cpp:7832) does |
| methods, incl. inherited | `ClassInfo::methods`; invert `findMethod` (kama.cemit.cpp:7840) |
| constructors | `ClassInfo::ctors` / `primaryCtor` |
| enum variants after `Enum::` | `ClassInfo::variants` (`VariantCase{name, payload}`) |
| contract members | `InterfaceInfo::methods` — **`linkContracts()` already merged parent `refines`**, so it is the full slot set |
| static members after `Type::` | the path at kama.cemit.cpp:10600-10646 (`_classes` / `retroTargetInfo` → `findMethod` → `isStatic`) |
| visibility filtering | `MethodInfo::visibility` / `FieldInfo::visibility` + `canAccess` / `friendGrants` — reuse, don't reinvent |
| smart-pointer members | mirror `emitDispatch`'s `Deref<T>` fallback (kama.cemit.cpp:13462-13475) so `x.` on an `Owned<T>` offers the pointee's methods |

**Import-path completion** has an exact source: `_exported` ([kama.cemit.h:684](../kama.cemit.h)) is the
set of mangled names from each unit's `export { … }` manifest (populated kama.cemit.cpp:14012-14019,
enforced 14315-14328). It is flat with no module back-pointer — recover the module from the `mod + "__" +
sym` mangling convention used at :14325. Per-file visibility context is `_unitCtx` → `NsCtx`
(kama.cemit.h:126-132): `isPublic == false` means a file-private `_F<idx>` scope, unreachable elsewhere.

**Signature help** parameter names are uniformly `std::vector<ParamSig>` (`ParamSig{name, byRef,
className, isConst, isHardware}`, kama.cemit.h:22-28) for every callable — `FuncSig::params` (free fns,
`_funcs`), `MethodInfo::params`, `CtorInfo::params`, `SigInfo::params` (fn-ptr typedefs) — **except**
contract methods, where `InterfaceMethod::params` is a raw `SharedParameterList` (names via
`p->identifier->value`). `emitReorderedCall` (kama.cemit.cpp:8242) is the single named-argument matcher
for *every* call form; read it before designing the active-parameter logic.

**M3.5's workspace index is the right substrate for names-in-scope and import paths** — both want
project-wide symbols, which is exactly what `lspAnalyzeWorkspace` already builds. But note it is built
**lazily, per gesture**; completion fires on every keystroke, so it must NOT trigger a workspace rebuild.
Use the per-document index for member/local completion and only consult the workspace index when it is
already warm (`wsIndex && !wsDirty`), or accept a stale one for import-path suggestions.

---

## Suggested staging

1. **Close the STAMP_LOC campaign-exit checklist** ([lsp-m3-kickoff.md](lsp-m3-kickoff.md):347-380).
   Required for signature help; required for campaign exit regardless.
2. **Scope extents** (prerequisite 1) + `kama query --complete L:C` as the debuggable CLI seam, following
   the `--refs` pattern exactly (flags declared at kama.driver.cpp:3713-3717, parsed 3740-3744,
   dispatched 4025-4048 — and **update the fallthrough usage string at :4049**).
3. **Member completion** after `.` and `::` — the highest value per line of code, and testable purely
   through the CLI seam.
4. **Names in scope** (locals + params + file-visible decls + floor names).
5. **Signature help** — `textDocument/signatureHelp`, trigger characters `(` and `,`.
6. **Keywords + import paths**, then the **`global::` resolver alias** (seam: `resolveUserName` /
   `resolveFunc`, kama.cemit.h:695-700 — both already take the optional `const IdentifierNode* site`).

## Protocol wiring (mirrors M3.5 exactly)

Capabilities at kama.lsp.cpp:535-542 — `completionProvider` with `triggerCharacters: [".", ":"]`,
`signatureHelpProvider` with `["(", ","]`. Dispatch at :876-889, handlers shaped like `handleHover`
(:647-660); `kamaPos()` (:601) converts LSP 0-based → kama 1-based line. Facade decls at
kama.cemit.h:461-473, impls in kama.query.cpp, one thin driver seam each at global scope in
kama.driver.cpp (after the anon namespace closes — the M1 linkage rule still applies).

## Gotchas carried from M3.5

- **Never call `cType` from a query path** — its `unsupported()` side effect pollutes `_diagnostics`.
- **Do not compare paths by string prefix** — module resolution names the stdlib relative to the compiler
  binary (`<exeDir>/../../lib/std/…`), which in a dev tree literally has the project root as a prefix. Use
  `lspRealPath`.
- **`unitForUri` is an exact `*unit->name == uri` match** — ask with the same spelling the units were
  parsed with.
- **Regenerate `build/lspref-before.txt` BEFORE touching `analyze()`** (`tools/lspref.sh`; `build/` is
  gitignored). Everything in this campaign has been byte-identical in emission and M4 must stay so.
- **Anything keyed off "the first open document" must not depend on URI sort order** — `docs` is a
  `std::map` keyed by URI, and that ordering differs between macOS and the container. It cost M3.5 a
  container-only failure.


---

# As shipped (2026-07-28) — where the brief was wrong

Ten commits, M4.0–M4.9. `kama query --complete L:C` / `--sighelp L:C` are the CLI seams; check-query.sh
grew 100 → 170 assertions and check-lsp.sh 58 → 75. 799/799 native, emission byte-identical across all
535 lspref fixtures, both LSP harnesses green under `make EXTRA_CXXFLAGS="-fsanitize=address,undefined"`.

## The three claims that did not survive contact with the code

**1. The three helpers in "what the index already gives you for free" are all UNUSABLE from a query path.**
`exprClass` calls `cType` on six paths — the one thing the campaign's own rule forbids, since
`unsupported()` pollutes `_diagnostics` and `diagnosticsFor` filters by file, so a completion request would
inject phantom squiggles into the open file — and it reads `_localTypes`, which is cleared at every
function entry and after `analyze()` holds *the last emitted function's* locals. `isTypeReceiver` calls
both. `canAccess` calls `unsupported()` on denial and decides from `_currentClass`/`_currentFunc`, both
dead by index time. Substitutes, each verified clean through its transitive closure: **`mangleElem`** (the
exact "type node → mangled key" function, generic instances included), `genericTypeMangle`, `findMethod`,
and a fresh pure `visibleFrom` predicate.

**2. Prerequisite (1), "the scope stack is dead by index time", did not need solving.** kama **forbids
shadowing** (`kama.cemit.cpp`, `emitDeclarator`: a local may not shadow a parameter, an enclosing-scope
local, or a field), so within one callable a name is unique *except across sibling scopes* — and
completion emits **labels**, so two sibling bindings collapse to one entry with identical insert text.
Rename needed per-declaration identity; completion does not. No `popScope()` edit, no scope tree, no
`RecordedDef` extension: **M4.0–M4.7 touch no emitter code at all**, so byte-identical emission held by
construction rather than by testing.

**3. Prerequisite (2), "named-argument spans block signature help", was false.** `kama.y` has **no error
productions**. A half-typed call `f(a: 1, ` therefore has no AST *anywhere* — not in the last-good index,
not in a fresh parse — so the active parameter must come from a textual scan of the live buffer regardless
of any span work. The checklist remained a campaign-exit requirement and shipped last, as M4.9.

## What the brief omitted

**Argument-label completion.** `kama.y:1106-1109` shows the only `argument` productions are
`IDENTIFIER COLON …` — every kama argument is named — so an argument slot with no label yet is the
language's most-used completion context. It falls straight out of the callee resolution signature help
needs, and it outranks import paths and keywords.

## The design that resulted

**Two layers.** A **lexical** layer (`completionContextAt`, in `kama.query.cpp` so the CLI and the server
share one scan) recovers `{trigger, receiver, callee, prefix, filled, activeParam}` from raw buffer text;
a **semantic** layer answers from the post-`analyze()` tables. What crosses the seam is that lexical
context, never a bare cursor position — the receiver the user just typed exists in no AST, so asking the
index to find it at a position is asking it to find something provably absent.

**The stale index is usually CORRECT, not merely tolerable.** Press Enter (still parses, index refreshes),
then type `p.` (breaks the parse, adds no lines): the last good index stays geometry-accurate for the
whole file. Every single-line statement lands there. M4.6 covers the two cases that do not — a buffer that
has never parsed, and one whose line count moved — by blanking the cursor's line and re-analyzing.
Blanking, not a placeholder: it needs no grammar knowledge and preserves geometry exactly.

## Findings worth carrying to M5/M6

- **A pre-existing out-of-bounds read in `lspAnalyzeWorkspace`**, fixed in M4.5. `units` and `paths` are
  parallel; the overlay loop bounded a `paths` index by `units.size()` and then pushed a unit without a
  path. Fires with two buffers open when one is outside the project set. Present since the workspace-deps
  campaign; a plain build reads adjacent memory and carries on. **The sanitized-compiler run of the LSP
  harnesses is what caught it, and it is a manual step — run it at the end of every LSP milestone.**
- **A type may carry a FIELD and a METHOD under one name.** `std::process::Command` has both spellings of
  `args`, so field-vs-method precedence must follow whether the source CALLED the segment. Only real
  stdlib code exposed this; the fixture could not have.
- **Bare-name completion is a filtering problem, not an enumeration one.** `bareNameOf` is the exact
  inverse of `resolveUserNameImpl`'s lookup order, and three leaks got past the first cut: the C-ABI
  plumbing behind the floor (`kama_args_at`, `malloc`, `free`) is genuinely spellable and buried `print`
  under 27 lines of shims; `main`'s table key is `kama_main`; and ranking by enum order put keywords above
  locals. Note `free` is declared in the prelude **and** in `std::collections::allocator`, so "not in the
  prelude" is not the test — an `extern fn` is offered only when declared in the file being edited.
- **Backticks inside a double-quoted `expect` description are shell command substitution.** The harness
  was silently running `.` and `p.` while building its own text, and one instance was a hard syntax error.
- **M4.9 was measured, not assumed.** Without the `type_decl_head` stamp, `prepareRename` on `Box` in
  `type value Box<T>` returned characters 11-17 — covering `Box<T>` — so a rename would have deleted the
  type-parameter list. A span is invisible in every other test: nothing fails, the rewrite is just wrong.

## Deferred, deliberately

- **Indexing argument labels** so renaming a parameter rewrites its call sites. The spans are now correct
  (M4.9), which turns this from a data-loss risk into an ordinary feature. M5.
- **Promoting the undeclared-import warning** to a `publishDiagnostics` entry against the offending
  `kama.json`. Still stderr-only; the plumbing it was waiting on now exists.
- **A parse cache** (`path → (mtime, unit)`). Completion on a never-parsed buffer costs one extra analysis;
  `didChange` already costs one per keystroke. M5 (incremental) territory.
