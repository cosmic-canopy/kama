# `slot` — restoring its intended scope (cold-start brief)

*In-flight campaign doc. **Delete this file when the campaign ships**, once SPEC carries the record — see
the maintenance table at the top of [ROADMAP.md](../ROADMAP.md).*

## Why this campaign exists

`slot` was designed for **one** job: name the storage that is about to receive an `out` parameter, declared
externally so the reader can see the scope it lives in. It now does two jobs, and the second one arrived by
accident.

Two commits, and only the first was the design:

| commit | what it did |
| --- | --- |
| `1f84af9` | `slot T x;` — a declared hole, with its drop proven away. **The design.** |
| `c2ae0c8` | *"a local with no initializer must say `slot`"* — **the drift.** |

`c2ae0c8` removed implicit default construction, and its own message says what `T x;` had meant:

> The implicit default-ctor call goes with it. **`T x;` used to silently run the type's zero-arg `default`
> ctor**, which was a second piece of implicit construction and, worse, incoherent with `slot`: a hole that
> quietly constructs itself is not a hole.

Removing that was right. Making all 524 swept sites say `slot` was not: those sites were not holes awaiting
an `out`, they were *construction*. `slot` was simply the nearest keyword to hand.

## The evidence

Every `slot` in the tree, classified by what it is actually doing:

| what the `slot` is | count |
| --- | ---: |
| the **constructed type, inside its own ctor** — `resource` | 290 |
| the **constructed type, inside its own ctor** — `value` | 284 |
| the constructed type, inside its own ctor — `view` | 3 |
| a hole outside any ctor (resource / free fn / value) | 164 |
| a hole of *another* type inside a ctor | 46 |
| **total** | **~787** |

**~73% is construction, not holes.** And note resources outnumber values in that group — which matters,
because `@generate(zero)` is gated on `isTransparentValue` (`kind == Value` **and** every field public), so
a `resource` can never obtain a compiler-generated `zero()`. There is no existing spelling that covers
those 290 sites.

**The compiler already models the split.** `checkDefiniteAssignment` keeps two sets with different rules:

- `slotDecls` — a **hole**: strictly read-before-assign, no destructor emitted while unassigned.
- `slotClassDecls` — **valid-but-empty storage**: zero-initialized, field defaults applied, each field's
  `default` ctor run, vptr set. Safe to hand to a callee, safe to read a non-owning field of.

When one keyword needs two sets to model it, it is carrying two meanings.

## The three concepts

| # | concept | spelling | sites | status |
| --- | --- | --- | ---: | --- |
| 1 | a **hole an `out` parameter fills** | `slot T x;` | see below | ships — the original design |
| 2 | bag / zero construction of a transparent value | `T x = T.zero()` / `T.of(…)` | ≤284 | ships |
| 3 | **storage a ctor is obliged to complete** | `This x;` — legal only in a ctor | 577 | **this campaign** |
| — | call a type's elected default ctor | `T x = T.default()` | — | ✅ **SHIPPED** `5edb4d9` |

Concept 3 is irreducible: to build a `resource` field by field, the ctor needs storage that does not exist
yet, and it cannot come from another ctor without infinite regress. It needs *a* spelling; the argument for
`This x;` is that a `ctor` is a special function whose whole job is to produce the type before it
returns, and **`checkNamedCtorComplete` already proves every field is assigned there**. `slot` adds no proof
in that position — it is pure ceremony in 73% of its uses and load-bearing in the rest.

## What ships

1. **`This x;` declares a ctor's result** — exactly one per ctor, legal only there. Same lowering as
   today's class slot (zero-init + field-default fill + vptr); only the keyword goes away. `slot This x;`
   already compiles, so this is largely dropping the requirement for the keyword.
2. **`slot` becomes an error in that position** — one way to say each thing (GOALS #4). Source-breaking,
   ~577 sites, mechanical, and the compiler names every one.
3. **`slot` narrows to `out` only** (D2). A hole filled by assignment takes an explicit initializer instead.
4. **A `slot` never filled on any path becomes an error.** A hole declared and never filled is a dead
   declaration; silently eliding its drop is the wrong answer.

After this, `slot` means exactly one thing: *the storage an `out` parameter is about to fill.*

**Drop behaviour does not change** — it is already correct, and this is why:

| slot state at scope exit | drop |
| --- | --- |
| never filled on any path | **error** (new, item 4) |
| filled on **some** paths | legal; drop **guarded by liveness** — nothing dropped if unassigned |
| filled on **all** paths | unconditional drop, statically known |

Row 2 is exactly why the `fd >= 0` runtime guards did not retire with the original `slot` campaign, and it
is why they should not be retired now. A conditionally-filled owning value *must* still drop when it was
filled, or it leaks.

## Decisions — ALL SETTLED (user, 2026-08-01)

### D2 — `slot` narrows to `out` ONLY. ✅ DECIDED

`slot` names the storage an `out` parameter fills. That is all it does. A hole filled by **assignment** is
not a slot — it takes an explicit initializer, because `T x;` is precisely the habit being removed:

```kama
slot float64 area;                                  // ✗ no `out` anywhere
if (c) { area = w * h; } else { area = 0.0; }

float64 area = (c) ? w * h : 0.0;                   // ✓ explicit, and the branch disappears
```

**This is well-supported, and better than a placeholder — verified.** kama's `match` and ternary are
value-producing and can build a `resource`, so branch-initialization has an explicit spelling that is
*stronger* than a hole (the value arrives as an expression rather than being assembled through storage):

```kama
Conn c = match (k) { case A: Conn.tcp(fd: 3); case B: Conn.udp(fd: 3); };   // builds a resource
Conn d = (n > 0) ? Conn.tcp(fd: 1) : Conn.udp(fd: 1);
```

The earlier worry — that this would force throwaway initializers and lose the read-before-assign guarantee —
does not survive contact with those two forms. Where neither fits, a helper taking an `out` parameter is the
sanctioned shape, which returns `slot` to its own job.

### D1 — **exactly one** bare declaration, of the ENCLOSING type. ✅ DECIDED

Not any local in a ctor body. **D2 forces this**: if `T x;` is the habit being removed, it cannot be
re-admitted for arbitrary locals merely because they sit inside a ctor. The blessing exists for exactly one
reason — a ctor must materialize its own return value, and no expression can do that without infinite
regress. That reason does not extend to a `Ptr<Ctrl>` local.

So the **46** "hole of another type inside a ctor" sites take an explicit initializer like everything else:

```kama
public ctor adopt(Ptr<T> raw) {
    Ptr<Ctrl> k = null;                  // was: slot Ptr<Ctrl> k;
    unsafe { k = cast<Ptr<Ctrl>>(…); }
```

**At most ONE per ctor.** A ctor produces one value; declaring several uninitialized ones is not a shape the
language should allow. This is **free to enforce — 0 ctors in the corpus declare two**, so the rule codifies
existing practice rather than forcing any rewrite. A second value of the same type is still fine when it is
*initialized* (`Point other = Point.make(…);`) — the restriction is on uninitialized storage, not on the type.

**Say "the enclosing type (`This`)", not "the returned type".** A fallible ctor returns `Result<This, E>`
while the thing it builds is a `This`. All 42 fallible ctors in the corpus delegate to an infallible named
ctor (`return Result::Ok(value: File.make(fd: fd));`) and so need no blessing today, but the rule must be
worded for the first one that builds in place.

**Do NOT mandate the variable's name.** There is a de-facto convention — `r`, 422 of ~570 uses (74%) — but a
23-name tail, and `result` itself only 22. Mandating one costs ~150 sites for no semantic gain, and an
implicit `result` binding would introduce implicit behaviour immediately after this campaign removes it.
Convention, not a compiler rule.

### D5 — the blessed declaration is spelled **`This x;`**. ✅ DECIDED

Not `TypeName x;`, and not an implicit `this`.

```kama
public ctor make(int32 n) { This r; r.x = n; return give r; }
```

**`slot This r;` already compiles today** — verified, including on a generic, where it also removes the
need to restate the type arguments (`slot This b;` inside `Box<T>`, no `<T>` to get wrong). So step 1 is
close to free: allow the same declaration without the keyword.

Why `This` beats the plain `TypeName` blessing — it makes the rule **lexical rather than contextual**:

- `This x;` is legal **only** inside a ctor, so "exactly one" (D1) is trivially checkable, and a bare
  `Point p;` stays an error *everywhere* with the message it already has. No context-sensitive diagnostic
  to write, and no rule of the form "…except here".
- `grep 'This '` finds every construction site in the language.
- Rename-safe, and on a generic type the arguments cannot be restated wrongly.
- **Free in the sweep** — step 2 already rewrites all 577 lines to delete `slot `; changing the type name
  in the same pass costs nothing.
- `This` is *already* the enclosing type's name in kama (`Result<This, E>` on a fallible ctor), so it gains
  no new meaning — unlike the alternatives below.

**Rejected — an implicit `this` (`this.x = 0; this.y = 0;`).** Tempting, and it is the C++/C#/Java shape,
but:

1. **`this` in a ctor is an error today** — *"a `static` method has no `this`"* — because a kama ctor IS a
   static factory with no receiver. That is the basis of the construction model, not an incidental
   restriction.
2. It would give `this` a **second meaning** (storage under construction, alongside instance-method
   receiver) — the exact dual-meaning pattern this campaign exists to undo.
3. It erodes a real safety property. In C++/C#/Java `this` is live mid-constructor: you can call methods on
   it, call virtuals on it, and leak it — the half-constructed-object bug family. kama's build-a-local-and
   -`give`-it-away model means no half-built object is ever observable *as the object*. An implicit `this`
   reintroduces that shape.
4. It would make the move implicit, against GOALS #5, immediately after a campaign whose whole point is
   removing implicit construction.

### D3 — warn-first, in five steps. ✅ DECIDED

`c2ae0c8` landed its breaking half warn-first and swept in between, which is why it had no red intermediate
commit. Same shape:

| step | change | breaking |
| ---: | --- | --- |
| 1 | accept `This x;` in a ctor without `slot` | no — additive; both spellings briefly legal |
| 2 | sweep the 577 sites (drop `slot`, and normalize the type name to `This` in the same pass) | no |
| 3 | flip `slot`-of-the-constructed-type-in-a-ctor to an error | yes (corpus already clean) |
| 4 | warn on an assignment-filled `slot`, sweep ~164 + fixtures, then error | yes |
| 5 | error on a never-filled `slot` | yes |

### D4 — `view` types follow the same rule. ✅ DECIDED

A `view` has ctors and must materialize its return value like anything else; its own rules (borrows, owns
nothing, no destructor) are orthogonal to how its storage is spelled. 3 sites, no special case.

## ⚠️ D5 is REOPENED — implicit `this` is the current lean

The user leans to an implicit `this` in a ctor over `This x;`, and the objections recorded below did not
survive checking:

- *"`this` in a ctor is an error today"* — circular. It is an error because nobody blessed it.
- *"it gives `this` a second meaning"* — overstated. `slot`'s two meanings had different lowering and two
  analysis sets; `this`-in-a-ctor vs `this`-in-a-method is the same storage and type at two lifecycle
  stages.
- *"half-constructed objects"* — **collapses.** kama has **no constructor chaining** (M8 Phase E), so the
  C++ hazard (a base ctor running while the vptr is not yet the derived one) cannot occur.
- *"the move becomes implicit"* — separable; an explicit `return give this;` can coexist with implicit
  `this`.

It also reads better against the base fix, which is the deciding factor: `this.base = Base.createIt();`
sits naturally beside `this.x = 0;` in a way `r.base = …` does not.

**One question left before D5 can be re-decided: what does a ctor RETURN?** *Lean: keep it explicit* —
`return give this;` / `return Result::Ok(value: give this);`. Fallible ctors force the issue: they must
write `return Result::Err(…)` on failure, so an implicit success return would leave one path returning and
the other not.

**A `Base`/`base` parallel is wanted too** — `Base` the type, `base` the object, mirroring `This`/`this`.
It makes `this.base = Base.createIt()` rename-safe and means a derived author never types the concrete base
type name, which is the encapsulation point the base-ctor hole is about. Only meaningful in a type that has
a base; note `base` in a type WITHOUT one currently reports the bare fragment "base access" (ROADMAP §1).

## Constructor reuse — what works today (measured)

Relevant because the base fix and D5 both depend on it:

| form | status |
| --- | --- |
| `ctor origin() { return P.make(x: 0, y: 0); }` — delegate wholesale | ✅ legal |
| `ctor make(…) { … P::helper(…) … }` — private static helper | ✅ legal |
| `slot P r; r = P.make(…); r.y = 5; return give r;` — delegate **then tweak** | ❌ *"'x' is never assigned"* |

**`checkNamedCtorComplete` credits only FIELD-BY-FIELD assignment.** A whole-value assignment does not
count. That blocks "build via another ctor, then adjust", which looks like an oversight rather than a
decision — and it is a **prerequisite for the base fix**, since `this.base = Base.createIt()` is exactly a
whole-value assignment to a sub-object. Extending the check to credit whole-value assignment (to a
sub-object and to the value itself) is therefore shared work between this campaign and ROADMAP §1's
base-constructor hole.

## Two traps for the sweep

- **`tests/slot_drop_elided.kama:16` becomes illegal.** It proves the no-destructor payoff with
  `slot Tracker unfilled;` — *never assigned*, which step 5 makes an error. After step 5 the only reachable
  elision case is a **partial** fill, so the fixture must be restructured to prove it that way, and SPEC's
  "an unassigned slot has no destructor emitted" needs the same qualification.
- **The `match`-join fixtures added in `9968d34` fill by assignment** (`slot_match_assign` and its three
  `xfail` siblings), so step 4 rewrites them. The join rule they pin is unaffected — only the spelling of
  the hole changes. Their `out`-parameter case (`pickInto`) is already in the durable form and should stay
  the fixture's lead.

## Mechanics you will want to know

- **The bare-local rejection is emitter-side, not grammar-side.** `T x;` parses today; `c2ae0c8` made it a
  hard error in the emitter. So the blessing is a rule change, not a grammar change — no parser, tree-sitter
  or LSP-query work, which is most of what would otherwise make this expensive.
- **⚠️ Sweep with `kama query`, not with grep.** The 164-site "hole" group still has to be split into
  *out-filled* (stays `slot`) and *assignment-filled* (step 4 rewrites it), and that split **cannot be
  measured by regex** — this was attempted and produced garbage. A one-line ctor
  (`public ctor make() { slot Animal r; return give r; }`) puts the declaration, the fill and the return on
  the same line, so line-oriented matching mis-files construction as a hole, and a fixed lookahead window
  misses fills further down the function. The repo already has the right tool: the front end is a reusable
  query API with real source spans ([editors.md](../editors.md)). Drive the sweep from that, or from the
  compiler's own warning output in step 4 — the warn-first phase names every site precisely, which is the
  cheapest correct enumeration available and is exactly how `c2ae0c8` swept 524 of them.
- **`checkNamedCtorComplete`** is what makes the blessing sound: it already proves a named ctor assigns
  every field before returning. Confirm it cannot be evaded (ROADMAP §2 notes a legacy self-returning
  `static fn` factory that it cannot see through — that residual should close with this campaign, since it
  is the same "construction without proof" hole).
- **`slotDecls` vs `slotClassDecls`** in `checkDefiniteAssignment` is the existing seam; item 1 is largely
  moving `slotClassDecls` off the keyword and onto "declared in a ctor, of the constructed type".
- **LSP fixtures encode buffer coordinates.** Removing a 5-character `slot ` prefix from swept fixtures
  shifts them, exactly as adding it did in `c2ae0c8` — that commit's notes on re-deriving `check-query` /
  `check-lsp` expectations are the playbook.

## Verification

```sh
tools/cdev make && tools/cdev test                     # fixtures, xfail, trap, check≡build agreement, warnings
tools/cdev exec env KAMA_SAN=1 ./run_tests.sh          # ASan + UBSan
tools/cdev exec env KAMA_WASM=1 ./run_tests.sh         # wasm
tools/cdev exec sh tools/check-noheap.sh
tools/cdev exec sh tools/check-embedded.sh
tools/cdev exec sh tools/check-query.sh                # coordinates shift with the sweep
tools/cdev exec sh tools/check-lsp.sh
```

Baseline entering this campaign: **native 873 / ASan 838 / wasm 806, all 0 failed.**

The audit `c2ae0c8` ran is worth repeating in reverse: confirm no `xfail` fixture starts being rejected by
a *different* rule than its own once the bare-local rule relaxes inside ctors.

## Sequencing

**Source-breaking, so it lands before the 1.0 tag or waits for 2.0** — ROADMAP §1's own rule. It belongs
with the docs/naming reconcile that is the last gate before the tag.
