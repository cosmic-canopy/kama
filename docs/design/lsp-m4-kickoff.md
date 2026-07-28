# LSP M4 — completion + signature help (cold-start brief)

**Status: NOT STARTED.** Written 2026-07-28, immediately after M3.5 shipped (`469da24`), with line
numbers verified against that commit. Read [lsp.md](lsp.md) first for campaign context, then this.

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
   the `--refs` pattern exactly (flags at kama.driver.cpp:3268-3272, parsed 3295-3299, dispatched
   3577-3605 — and **update the fallthrough usage string at :3604**).
3. **Member completion** after `.` and `::` — the highest value per line of code, and testable purely
   through the CLI seam.
4. **Names in scope** (locals + params + file-visible decls + floor names).
5. **Signature help** — `textDocument/signatureHelp`, trigger characters `(` and `,`.
6. **Keywords + import paths**, then the **`global::` resolver alias** (seam: `resolveUserName` /
   `resolveFunc`, kama.cemit.h:695-700 — both already take the optional `const IdentifierNode* site`).

## Protocol wiring (mirrors M3.5 exactly)

Capabilities at kama.lsp.cpp:535-542 — `completionProvider` with `triggerCharacters: [".", ":"]`,
`signatureHelpProvider` with `["(", ","]`. Dispatch at :876-889, handlers shaped like `handleHover`
(:614-660); `kamaPos()` (:601) converts LSP 0-based → kama 1-based line. Facade decls at
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
