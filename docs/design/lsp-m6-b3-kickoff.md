# LSP M6 B3 — the member reference index (cold-start brief)

**Status: Stage 0 + B3a + B3b + B3d + B3e SHIPPED 2026-07-30** (`2a9ba07`, `95fd0f2`, `18ee951`), and
both LSP harnesses are ASan/UBSan-clean on macOS AND in the container (the leak check only exists there).
**Remaining: B3c, B3d's `::` form, B3f** — enumerated below, not waiting to be thought of.
Everything here was verified by running it, not reasoned about.
Parent brief: [lsp-m6-kickoff.md](lsp-m6-kickoff.md). Campaign status: [lsp.md](lsp.md).

> ## Stage 0 — how the rest of B3 gets found, instead of thought of
>
> B3a existed for the whole campaign because the reference index is built by *instrumenting the emitter*,
> and an instrumented emitter is only as complete as the set of sites someone remembered. Every test we
> had asserted a spelling somebody had thought of, so a spelling nobody thought of failed nothing.
>
> **`kama query <file> --coverage`** asks the other question. `sourceIdentifiers()` ([kama.query.h](../kama.query.h))
> enumerates every identifier token the SOURCE spells — reusing completion's own literal/comment scan, and
> asking the lexer's `kamaIsKeyword` rather than carrying a keyword list — and `CEmitter::coverageAt` says
> what the index knows at each one:
>
> | status | meaning |
> |---|---|
> | `decl:<kind>` / `ref:<kind>` | indexed and resolved to a def-site |
> | `unresolved` | indexed, but the key names no def-site (a builtin, a type parameter, **or a key shape with no def-site behind it**) |
> | `-` | nothing indexed here at all — **the gap signal** |
>
> `tests/query/coverage/*.kama` spell every naming construct the language has; the checked-in
> `*.coverage` tables beside them are compared WHOLE by `tools/check-query.sh`. A gap is now a diff.
>
> ### What it found on its first run (240 identifiers, 64 gaps)
>
> The brief below named two of these. The oracle named nine:
>
> | # | gap | status |
> |---|---|---|
> | 1 | **method CALL SITES** — `p.area()`, `base.kind()`, `this.kind()`, `Point::origin()`, `Derived.make()`, `xs.add()`, `new Cat.loud()` | ✅ B3a |
> | 2 | **generic BODIES** — `Box<T>`'s `v`/`get` decls and uses; `firstOr<T>`'s params and body | ✅ B3b |
> | 3 | **a generic instance's member** — `bi.v` indexes as `field:Box_int32::v`, a key with no def-site (`unresolved`) | ✅ B3b |
> | 4 | **the TYPE QUALIFIER of a member call** — `Point` in `Point.at(…)`, `Color` in `Color::Green`, `Shape` in `Shape::Circle(…)`, `DynamicArray` in `DynamicArray.empty()`. The same spelling as a type ANNOTATION indexes fine, so these paths resolve without passing a `site` | ✅ the `.` form (B3d); ⚠️ the `::` form is BLOCKED — see below |
> | 5 | **contract METHOD declarations** — `fn int32 speak();` inside a `type contract`. `InterfaceMethod` carries no decl node, so neither the declaration nor any fat-pointer call site can be indexed | B3c |
> | 6 | **enum payload fields** — `r` in `Circle(int32 r)`, at both its declaration and the `Shape::Circle(r: 7)` label | ✅ B3e |
> | 7 | **argument labels to a library or generic callee** — `item:`, `xs:`, `fallback:`. M6 A2 works; these callees' `ParamSig::declSite` is null | ✅ B3b (they were generic-instance callees all along) |
> | 8 | **a generic free function's CALL SITE** — `firstOr(xs: …)` | ✅ B3d |
> | 9 | **import paths and namespace names** — `std`, `collections`, `DynamicArray` in an `import`; the `namespace` name itself | ✅ the imported SYMBOL (B3g); the PATH segments are blocked, same as `::` |
>
> Permanently `-`, and correctly so: the contextual type-kind words (`value`, `resource`, `contract`,
> `both`) are not lexer keywords and never name a symbol. A type PARAMETER (`T`) reads `unresolved` rather
> than `-`: it is indexed, but inside an instance it substitutes to a concrete type, so there is no
> template-parameter symbol to resolve to. That is correct, not a residual gap.
>
> ### As shipped (B3a/B3b)
>
> 64 gaps -> 35. What it took, beyond the design below:
>
> - `_labelRefs` generalized into ONE node-keyed store, `_nodeRefs` + `recordNodeRef` — a struct and a
>   function deleted rather than a second mechanism added. `keyOfDeclNode` in `buildPositions` (3b) is the
>   A2 resolve pass widened from Param to Param/Field/Method/Ctor. ⚠️ Functions are deliberately EXCLUDED:
>   a generic free fn's instances share the template's `sig.node`, so several keys would collapse onto one
>   node and the last walked would win.
> - The 5 field record sites were **converted**, not added to. `buildPositions` dedups by identifier node
>   and first-wins, so a leftover key-based record would have shadowed the node-based one and left the
>   generic case broken while every non-generic test still passed.
> - `emitDispatch` / `emitSmartPtrCall` took a defaulted `const IdentifierNode* site`, the idiom
>   `resolveUserName`/`resolveFunc` already use. One record inside `emitDispatch` covers the virtual,
>   devirtualized and auto-deref paths; `emitInterfaceDispatch` was deliberately NOT threaded, since a
>   contract method has no def-site to point at (that is B3c).
> - ⚠️ **A latent nondeterminism surfaced and was fixed.** A type that declares a `ctor` gets TWO
>   `_positions` entries over one range (the ctor's implicit result type resolves through the class's own
>   decl identifier), and `posAt` broke the tie by unstable sort order. Both carry the same key, so only
>   the decl/ref marker differed — invisible until the coverage table made the marker an assertion. `posAt`
>   now prefers the declaration on an exact span tie.
> - Cost: **+3 ms per keystroke** (86 -> 89 ms median, measured A/B against the parent commit), because
>   generic bodies are now walked for records at all. Inside the 100 ms budget.
>
> ### As shipped (B3d/B3e) — 35 gaps -> 26
>
> All small, all the same shape: a path that resolved a real symbol and returned before reaching any
> recorder. `Type.name(…)`'s receiver (recorded as `typeName`, the TEMPLATE key — `tn` is the mangled
> instance and has no def-site); a generic free function's call, which `_callInst` routes past the
> `resolveFunc` that records every other call; `Union::Variant(args)`, where only the payload-LESS read had
> a recorder; and enum payload fields, which needed def-sites first — they live on the variant BACKING
> ClassInfo, which `buildDefSites` skips as compiler-synthesized, so they had none anywhere.
>
> ### ⚠️ B3g — a TENTH gap, found by pulling on #9, and it is another silent edit
>
> An `import`'s symbol list is a REFERENCE, and it was not indexed. Renaming a type rewrote its declaration
> and every use and left `import lib::{Box, …}` spelling the old name — so the rename broke a file it had
> just edited. Same class as B3a, now across units. **Fixed** for the import side: the symbols carry real
> `UsingDeclarationNode::identifier` nodes, and `_refUnit` just had to be pointed at the importing unit for
> the export-check loop (it is null throughout `collectProgram`). The module-qualified name that check
> already builds IS the key the def-site is registered under.
>
> **The matching `export { Box, … };` is still missing**, and it is blocked on the same thing as the two
> below: `CompilationUnit::exportList` is a `SharedStringList`. `tools/check-query.sh` pins that as a
> `reject`, so closing it cannot be silent.
>
> ### ⚠️ ONE blocker is now behind THREE remaining gaps, and it is the one thing here that is not a small fix
>
> Three separate gaps turn out to be one missing thing: **a `::`-separated name list keeps its spellings as
> plain strings, with no line or column**, so there is nothing to anchor a `PosEntry` to and no amount of
> passing a `site` reaches them.
>
> | gap | the list |
> |---|---|
> | `Point::origin()`, `Color::Green`, `Shape::Circle(…)` — the QUALIFIER (B3d's `::` form) | `IdentifierNode::qualifier`, [kama.ast.h:244](../kama.ast.h#L244) |
> | `std`, `collections` — an import PATH's segments (B3f) | `ImportDeclarationNode::modulePath`, [kama.ast.h:107](../kama.ast.h#L107) |
> | `export { Box, … }` — the export surface (B3g's other half) | `CompilationUnit::exportList`, [kama.ast.h:59](../kama.ast.h#L59) |
>
> **Suggested shape, additive rather than invasive.** Do NOT retype these to identifier lists: dozens of
> consumers read `id->qualifier` as strings, and the grammar's `%type <strings>` would have to change with
> them. Instead carry a PARALLEL `std::vector<SrcRange>` filled by `STAMP_LOC`-style stamping in `kama.y`
> (the macro already exists, [kama.y:66](../kama.y#L66)), and have `buildPositions` emit a
> `PosEntry{range, nullptr, false, key}` per segment — a null `id` is already a supported shape (every
> declaration entry uses it). Nothing else has to change.
>
> ⚠️ This touches `kama.y`, so it is under the campaign's lspref before/after rule, and ⚠️ **every `_opt`
> rule must set `$$`** (the uninitialized-`$$` trap recorded in the MCU step-2 notes). Its own commit.

---

*Everything below is the original B3a/B3b design brief. It SHIPPED as written — the seam map and the
warnings in it are accurate, and they are the map for B3c-B3f too, which sit on the same seams.*

**One judgement call the brief raised was DECIDED: rename does NOT refuse a symbol with zero recorded
references** (user, 2026-07-29). A genuinely-unused private method, or an uncalled public library API, is
a real thing to rename; refusing would be a worse regression than the bug. The durable guard is the rename
assertion in `tools/check-lsp.sh` and the coverage table — not a refusal.

B3 closes the last correctness holes in the reference index. It is **two gaps with one fix**, and the
first one is a silent data-loss bug rather than a missing feature:

| | Gap | Severity |
|---|---|---|
| **B3a** | **A method's CALL SITES are not indexed — for every type, generic or not.** F2 on a method is *offered*, and renames only the declaration. | **Breaks the user's code.** |
| **B3b** | Nothing inside a generic type's or generic function's BODY is indexed. Covers all of `lib/std`'s containers. | Answers nothing (rename correctly refuses). |

Both are the same underlying thing — a member reference that never reaches `_bodyRefs` — and both are
fixed by the same inversion. Do them together; doing B3b alone would register def-sites that nothing
references.

---

## B3a — the one that loses work

Reproduce it in thirty seconds:

```sh
printf 'type value P {\n    public int32 x;\n    public fn int32 get() { return this.x; }\n}\nfn int32 main() { P p; p.x = 1; return p.get(); }\n' > /tmp/m.kama
./kama query /tmp/m.kama --refs 2:17    # FIELD x  -> decl + BOTH use sites. correct.
./kama query /tmp/m.kama --refs 3:20    # METHOD get -> its own declaration and nothing else.
```

Then drive `textDocument/prepareRename` + `textDocument/rename` at 2:20 (0-based) on the same buffer:
`prepareRename` returns the range — so the editor **offers F2** — and `rename` returns exactly one edit,
rewriting the declaration `get` → `fetch` and leaving `p.get()` untouched. The buffer no longer compiles.

**Why:** there are 13 `recordRef` call sites in `kama.cemit.cpp` and **none of them is a method
invocation**. They are two generic resolvers ([:189](../kama.cemit.cpp#L189) `resolveUserName`,
[:325](../kama.cemit.cpp#L325) `resolveFunc` — types and FREE functions), five `fieldKey` sites, four
`enumMemberKey` sites, and the binding sites. Method calls resolve through `findMethod` and emit
`mi.cName` directly, recording nothing. The method still gets a *def-site* (from the `_classes` loop at
[kama.query.cpp:409-436](../kama.query.cpp#L409-L436)), which is precisely why rename offers itself and
then under-applies — a def-site with no references reads to the rename path as "a symbol used nowhere".

> ⚠️ **Consider whether rename should refuse a symbol whose references were never indexed**, as a
> belt-and-braces guard independent of this fix. A def-site with zero recorded uses is currently
> indistinguishable from a genuinely unused symbol, and that ambiguity is what turned a missing feature
> into a silent edit. This is a judgement call for the implementer; it is not obviously right, because a
> genuinely-unused private method is a real thing a user may want to rename.

## B3b — the generic-body gap

`_refUnit` is null while generic instances are emitted, and `recordRef`/`recordDef` drop everything when
it is. The instances are emitted from `emitHeaderContent`, which runs before the per-unit
`emitModuleContent` loop that sets it.

---

## The fix: record the declaration NODE, not a key

Do **not** canonicalize instance keys onto the template (`field:Box_int32::v` → `field:Box::v`). That is
string surgery over two key shapes that a third will outgrow, and it must apply globally, since `b.v` in
ordinary code also resolves to the instance key.

Use the inversion **M6 A2 already paid for**. A key built at record time embeds ambient context that is
wrong — for argument labels the *caller's* unit, for a generic body the *instance's* mangled name. A2
stored the parameter's declaration node and resolved node → key in `buildPositions`, after
`buildDefSites`. That transfers here for a reason the emitter already depends on: **every instantiation
walks the SAME template AST nodes** (see the comment at
[kama.query.cpp:427-428](../kama.query.cpp#L427-L428) — "a generic INSTANCE shares the template's field
nodes"). So `Box<int32>` and `Box<string>` arrive at one node and collapse onto one key with no mapping
table, and the result cannot drift when a new key shape appears.

**The working precedent to copy is `_labelRefs` + `paramKeyOf`**
([kama.query.cpp:663-682](../kama.query.cpp#L663-L682)): a `RecordedLabelRef` holds
`{unit, label, paramDecl}`, and after `buildDefSites` one pass builds `ASTNode* → key` from `_defSites`
and rewrites. Generalize it from parameters to members. ⚠️ Match by **upcasting the recorded node to
`ASTNode*`**, never by downcasting the DefSite's — an upcast is unconditionally safe and identity is all
this needs. A2's comment says so; it is the same trap here.

### Three pieces

**(1) Record member references by node.** Every field-ref site already has `ClassInfo* owner` in hand
([kama.cemit.cpp:1124](../kama.cemit.cpp#L1124), [:1156](../kama.cemit.cpp#L1156),
[:12867](../kama.cemit.cpp#L12867), [:12874](../kama.cemit.cpp#L12874),
[:12885](../kama.cemit.cpp#L12885)), and `FieldInfo::nameId` ([kama.cemit.h:57](../kama.cemit.h#L57)) is
the declaration node. `MethodInfo::node` ([kama.cemit.h:85](../kama.cemit.h#L85)) is the same for a
method. So a `recordMemberRef(owner, name, site)` covers both, and the method-invocation path needs the
call *added* (B3a), not converted.

**(2) Register the generic template's MEMBERS as def-sites.** The `_genericTypes` loop
([kama.query.cpp:444-451](../kama.query.cpp#L444-L451)) registers only the template's own NAME. The loop
that registers fields/methods/ctors is the `_classes` one, which skips `ci.isGenericInst` — and the
template is deliberately kept OUT of `_classes`. Mirror the member loops there, keyed under the template
key. Verified available: the template's `ClassInfo` carries everything (`methods` with `cName`
`_F4__Box__get`, `fields` with a live `nameId`).

**(3) Attribute the generic body to the template's unit.** ⚠️ **This part is already written and
verified — then reverted**, because it is inert on its own. Redo it exactly:

- Split the `_declUnit` fill out of `buildDefSites` into a `buildDeclUnits()`; it depends on nothing but
  `_units`. Call it from `collectProgram` **after** [kama.cemit.cpp:14041](../kama.cemit.cpp#L14041)
  (`pruneInactiveDecls`) — that ordering is the one real constraint, since pruning rewrites the decl list
  in place. Gate on `_analysis`.
- Wrap `emitGenericInst` ([:6103](../kama.cemit.cpp#L6103)) and `emitGenericTypeInst`
  ([:12475](../kama.cemit.cpp#L12475)) in a save/restore of `_refUnit` set to `unitOfDecl(<template
  node>)` — the TEMPLATE's unit, never the instantiation's use site. For the type case the template
  ClassInfo is `_genericTypes[gi.templateKey]`, whose `.node` is the template decl.
- A shared RAII `RefUnitScope` beside `_refUnit` in `kama.cemit.h` lets `emitModuleContent`
  ([:14798](../kama.cemit.cpp#L14798)) drop its own hand-rolled guard struct.

**Measured when this was done:** records start flowing (`recordRef` fires for `T` and for
`field:_F4__Box_int32::v`), and **emission stays byte-identical across all 537 `lspref` fixtures**. The
duplicate-per-instantiation worry is already handled upstream — `buildPositions` dedupes `_bodyRefs` by
(unit, `IdentifierNode*`) and `buildDefSites` overwrites `_defSites[key]`; its comment already says
"a generic template body re-emitted per instantiation" out loud.

---

## Testing

**⚠️ Use TWO instantiations from the start.** A single-instantiation fixture passes under several wrong
designs — including the key-canonicalization one this brief rejects. `Box<int32>` *and* `Box<string>` in
one program is what distinguishes them, and a cross-unit pair (as `tests/query/labels/` does for A2) is
what catches attribution mistakes.

What must become true:

- `--refs` on a method declaration returns its call sites, and F2 rewrites them. **A test for B3a belongs
  in `tools/check-lsp.sh` as a rename, not only a `--refs` query** — the query returning nothing was
  survivable; the rename half-applying is the bug.
- `--refs` / `--def` / hover answer inside a generic body, and one `v` in `Box<T>` yields ONE def-site
  however many instantiations exist.
- `tests/syntax/` and every existing fixture stay untouched.

**Gates:**

```sh
make && ./run_tests.sh                                    # 803/803 before you start
tools/lspref.sh | diff build/lspref-before.txt -          # MUST be byte-identical, 537 fixtures
sh tools/check-lsp.sh && sh tools/check-query.sh          # 119 / 180 assertions, and they must GROW
tools/lsp-bench.sh --lsp                                  # 84-89 ms, 12 timing lines
```

⚠️ **`tools/check-lsp.sh` pins the current gap as an exact array** — request id 56, the
`"a generic type's BODY yields no tokens"` assertion. **It must change**, and that is deliberate: it is
what makes closing the gap visible rather than silent. Recompute it, do not delete it.

⚠️ Fixture variable names in `check-lsp.sh` are ONE FLAT SHELL SCOPE. Taken through B2: `TOKB TOKG
TOKGURI TOKURI` and the list in the parent brief; request ids 1-30, 32-49, 53-56 are used (**free: 31,
50-52, 57+**).

**Campaign non-negotiables** (unchanged): `tools/lspref.sh` before/after anything touching
`kama.driver.cpp`/`kama.cemit.cpp`/`kama.y`; never call `cType` from a query path; positions are kama line
1-based / column 0-based below the protocol layer, and `kamaPos`/`lspRange` are the only conversion
points; and the sanitized run of both LSP harnesses is a MANUAL step —
`make EXTRA_CXXFLAGS="-fsanitize=address,undefined"`, then both harnesses, **in the container as well**
(macOS ASan has no LeakSanitizer).
