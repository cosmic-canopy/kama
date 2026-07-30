# LSP M6 B3 — the member reference index (cold-start brief)

**Status: NOT STARTED. Everything below was verified by running it against `2c20be9`, not reasoned about.**
Parent brief: [lsp-m6-kickoff.md](lsp-m6-kickoff.md). Campaign status: [lsp.md](lsp.md).

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
