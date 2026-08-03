# `slot` — restoring its intended scope (cold-start brief)

*In-flight campaign doc. **Delete this file when the campaign ships**, once SPEC carries the record — see
the maintenance table at the top of [ROADMAP.md](../ROADMAP.md).*

---

## ►► STATUS — steps 1–3 SHIPPED (2026-08-03), start at step 4

`e1edf1f` → `3098b21` on `dev`. **native 890 / ASan 854 / wasm 828, 0 failed** — that is the baseline to
hold. The ctor half is done: a ctor's value is an implicit `this`, the 652 sites are swept, and declaring
the constructed type inside its own ctor is an error. **`slot` itself has not moved yet** — 175
declarations remain, and narrowing it to `out` is step 5.

### What the rest of this brief gets WRONG — read before trusting any of it

- **Site counts are off.** Real totals, from a structural scan of 975 files: **762** declarations, not 787.
  Bucket A (the constructed type in its own ctor) is **652**, not 577. The C bucket splits further than
  recorded — see the table below.
- **"No compiler change, just a rule change" is wrong**, and it was the single biggest mis-estimate. A
  `ctor` is emitted as a static C factory with **no `self` at all** (`mi.isStatic = true`), so blessing
  `this` needed a genuinely new lowering: synthesize the storage at ctor entry, run the class-slot fill on
  it, point `self` at it. It is `emitAggregateFill` + the prologue buffer in `emitMethodOrCtorBody`.
- **"Two analyses must change in lockstep" undercounts — it was FOUR**, and every one had the same shape:
  it knew about a *declared local* and nothing else, so it silently stopped working when the value lost its
  declaration. `checkDefiniteAssignment`, `checkNamedCtorComplete`, `checkViewCtorEscape`, and the
  `isNamedValue`/`moveOnlySource` pair. **Assume the same of anything step 5 touches.**
- **The drop-elision fixture guidance is unusable as written** (see step 5 below).

### The bucket table, re-measured

| bucket | n | status |
| --- | ---: | --- |
| A — constructed type inside its own ctor | 652 | ✅ swept (step 2), now an error (step 3) |
| B — `slot Ptr<Ctrl>` inside an `Rc`/`Shared` ctor | 11 | ✅ now `Ptr<Ctrl> ctrl = null;` |
| C1 — filled by an `out` argument | 9 | **stays `slot`** — the only surviving use |
| C2 — filled by assignment | 43 | ☐ step 5 |
| C3a — filled by a mutating method call | 11 | ☐ step 5 → `T x = T.empty();` |
| C3b — filled via `addr(of:)` in `unsafe` | 9 | ☐ step 4 (the boundary helpers) |
| C3c — filled through a `ref` parameter | 3 | ☐ step 5 — a `ref` needs a LIVE value, so these were always wrong |
| C3d — never filled | 23 | ☐ step 5 — 13 in `tests/query/complete.kama`, 9 xfail, 1 `slot_drop_elided` |

### Traps for steps 4–7, learned the hard way

- **A drop that "cannot happen" closes fd 0.** `lvalueMoveKey` returned `""` for `this.field`, so the
  first `this.f = give …` dropped the zeroed field; for a raw handle whose zero is `fd = 0` that is a
  `close(0)`, which shut the *test harness's own stdin*. Every `proc_*` fixture then hung forever on a
  child that could not answer, with no error anywhere. **A hanging suite is a symptom of this class.**
  Move-state keys are spelled the way the SOURCE names a value (`this`), not the way C does (`__self`).
- **The xfail `.msg` guards are the real safety net** — 271 of 278 carry one, and they caught two rules
  that had silently stopped firing. Never "fix" an xfail by re-blessing; find out which rule now fires.
- **Do not run two `run_tests.sh` at once.** They share `/work` and clobber each other's temp state.
  Kill the container, not just the wrapper: `podman kill -a`.
- **Inline kama lives inside the guard scripts** (`check-noheap`, `check-no-inheritance`, `check-packages`,
  `check-query`, `check-lsp`) and no corpus sweep can see it. `check-lsp` also pins an exact delta-encoded
  semanticTokens array, which moves whenever a fixture's tokens do.
- **Seven `slot` keywords sit at the END of the preceding line**, ahead of a trailing comment (`c2ae0c8`
  put them there). Two were in ctors and are fixed; **five remain** and step 5 will hit them:
  `tests/assign_match.kama:6`, `tests/give_ptr_local.kama:16`, `tests/upcast_shared_contract.kama:19`,
  `tests/query/coverage/spellings.kama:63` and `:65`. A line-oriented rewrite corrupts them.

### Step 5's fixture problem — DECIDED, and not what this brief says

`tests/slot_drop_elided.kama` proves the payoff with a slot that is *never* filled, which step 5 makes an
error. This brief says to restructure it around a partial fill — **that does not work**: a partially-filled
slot drops unconditionally (`emitScopeCleanup` treats a slot's `MaybeMoved` as "drop it", relying on the
zero value being drop-safe), so nothing is elided and there is no property left to assert.

The agreed replacement (user, 2026-08-03) is an **exit point that PRECEDES the fill**, which is the one
case that still elides statically, since the emitter tracks move state in emission order:

```kama
slot Tracker t;
if (c) { return 0; }              // provably empty here -> no dtor emitted
makeInto(id: 1, dst: out t);      // same-scope `out` fill
return t.value();                 // live here          -> dtor emitted
```

⚠️ **Verify that against the emitted C before committing.** If the emitter does not in fact elide per exit
point, stop and report rather than quietly weakening SPEC's "an unassigned slot has no destructor emitted".

### The three rules step 5 must land

Agreed with the user 2026-08-03, and **rule 3 is tighter than D2 as written below**:

1. A `slot` is filled by an **`out` argument, and nothing else** — no assignment fill, no method-call fill,
   no `addr(of:)` fill, no `ref` fill.
2. A `slot` with no `out` fill anywhere in the function is an error.
3. The fill must be on the **same unconditional path** as the declaration — a statement of the declaring
   block, or of a nested block that always runs (`unsafe`). Not inside `if` / `match` / a loop.

Rule 3's reason, in the user's words: a conditionally-filled slot cannot be tested before use, and slot
validity must be a compile error, never a runtime test. It also makes the drop statically decidable at
every exit, so a slot never reaches `MaybeMoved`. **Every non-fixture site in the corpus already obeys it**
— the only cross-scope fill anywhere is `tests/slot_match_assign.kama` case (4), a fixture written to prove
the match join. That join rule still holds for `out` **parameters** (`pickInto`), which stays its lead.

### Step 3.5 — reserve `self` (one commit, do it first; independent of everything else)

**Decided (user, 2026-08-03): reserve it.** `self` is not a kama keyword — the lexer table has `base` and
`this` only — but it IS the emitted C name for the receiver pointer, so a local or parameter named `self`
inside a type body collides and the user gets clang's words, not kama's:

```kama
public fn int32 get() { int32 self = 1; return this.x + self; }
// error: redefinition of 'self' with a different type: 'int32_t' vs '_F4__P *'
```

**Pre-existing for methods** — that has always been broken. What this campaign changed is that a *ctor*
now emits a `self` too, so the same collision reaches constructors, where it previously compiled. Five
corpus ctors named the value they were building `self`; the step-2 sweep removed all five, so nothing is
broken today and the fix is purely about the next person to write one.

Reject a local/param named `self` inside a type body with a real kama diagnostic pointing at `this`. Cover
methods and ctors together, and pin both with xfail fixtures. The alternative — mangling the emitted
receiver to something unspellable so `self` becomes an ordinary identifier — is cleaner in principle but
touches every hardcoded `"self"` in the emitter; **rejected for now, not forever.** If the 1.0 naming
reconcile wants no compiler-reserved names, that is where it belongs.

### Step 4 — the boundary helpers (step 5's rule 1 depends on it)

Two seams hide in the 9 `addr(of:)` sites, and naming them is what lets rule 1 have no exceptions.

**Seam 1 — an extern C callee fills it** (`atomic.kama:79,95`; `channel.kama:79,100,115`). The C function
*is* an out-parameter, just type-erased to `void*`. Move the raw cast into a wrapper whose parameter is
already `out`; an `out` param has its own rule and `addr(of:)` legitimately satisfies it.

**Seam 2 — no callee at all** (`dynamic_array.kama:206`, `deque.kama:108`, `map.kama:316`,
`slot_map.kama:166`): a raw relocate, open-coded four times. One name in `std::memory`:

```kama
// Bitwise move out of raw storage; the source is left stale.
fn void relocate<T>(Ptr<T> from, int32 at, out T into) {
    debugAssert(cond: from != null);
    unsafe { Ptr<T> d = addr(of: into); d[0] = from[at]; }
}
```

Both are ordinary kama — no compiler change.

### After step 5

**Step 6 — inheritance hole 1**, unchanged from [inheritance.md](inheritance.md): `this.base = Base.make(…)`
plus a `Base`/`base` alias pair mirroring `This`/`this`. `This` is contextual, resolved in `cType`
(`kama.cemit.cpp` ~`:694`) and bound by `ScopedStr _thisType` at 11 sites — that is the recipe. `Base` must
fail cleanly when there is no base, and sit inside `#if KAMA_INHERITANCE` or `check-no-inheritance` breaks.
Extend `checkNamedCtorComplete` with one narrow rule: `this.base` assigned exactly once, from a ctor call on
the base type. **The shallow fix is a trap — read that section of inheritance.md first.**

**Step 7 — closeout**: migrate to SPEC, delete this file and `inheritance.md`, strip the shipped items from
ROADMAP §1.

### Deferred, agreed as its own campaign

The **unsafe-seam campaign**: rename `Ptr<T>` → `UnsafePtr<T>`, and deliver "no null in safe kama" (21
sites in lib+prelude), via an `unsafe UnsafePtr<T> p = null;` field-declaration modifier, `Optional`-returning
FFI wrappers, `unsafe { }` around the teardown guards, then banning the `null` token outside `unsafe`. Plus
a **compiler-emitted debug null trap** at the two `_inUnsafe` deref gates. Runs AFTER this campaign: its
field-initializer piece changes ctor-completeness rules, which is what steps 1–3 just rewrote.

---

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
| 3 | **storage a ctor is obliged to complete** | implicit `this` inside a ctor — no declaration at all | 577 | **this campaign** |
| — | call a type's elected default ctor | `T x = T.default()` | — | ✅ **SHIPPED** `5edb4d9` |

Concept 3 is irreducible: to build a `resource` field by field, the ctor needs storage that does not exist
yet, and it cannot come from another ctor without infinite regress. But it needs no *declaration* — a `ctor`
is a special function whose whole job is to produce the type before it returns, so the storage is implied by
the function itself, and **`checkNamedCtorComplete` already proves every field is assigned there**. `slot`
adds no proof in that position: it is pure ceremony in 73% of its uses and load-bearing in the rest.

## What ships

1. **A ctor's value is an implicit `this`** (D5) — no declaration. Same lowering as today's class slot
   (zero-init + field-default fill + vptr) applied to the implied storage; the return is implicit too.
   `this` in a ctor is an error today, so this is the one genuinely new piece of surface.
2. **A declaration of the constructed type in a ctor becomes an error** — `slot Point r;` and a bare
   `Point r;` alike, since `this` is now the only way to name the value under construction (GOALS #4).
   Source-breaking, ~577 sites, mechanical, and the compiler names every one.
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

## Decisions — ALL SETTLED (user, 2026-08-01 / D5 2026-08-02)

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

### D1 — one value per ctor, and D5 makes it STRUCTURAL. ✅ DECIDED

*(Recorded before D5 landed. Its conclusion holds and is now enforced by construction: with an implicit
`this` there is no declaration to duplicate, so `This x; This y;` is not merely rejected — it is
unrepresentable. That was the user's original argument for implicit `this`, and it is why this decision
needs no check to enforce it.)*


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

### D3 — warn-first, in five steps. ✅ DECIDED

`c2ae0c8` landed its breaking half warn-first and swept in between, which is why it had no red intermediate
commit. Same shape:

| step | change | breaking |
| ---: | --- | --- |
| 1 | accept an implicit `this` (and an implicit return) inside a ctor | no — additive; both forms briefly legal |
| 2 | sweep the 577 sites: drop the declaration, rewrite `r.field` -> `this.field`, drop `return give r;` | no |
| 3 | flip a declaration of the constructed type in a ctor to an error | yes (corpus already clean) |
| 4 | warn on an assignment-filled `slot`, sweep ~164 + fixtures, then error | yes |
| 5 | error on a never-filled `slot` | yes |

### D4 — `view` types follow the same rule. ✅ DECIDED

A `view` has ctors and must materialize its return value like anything else; its own rules (borrows, owns
nothing, no destructor) are orthogonal to how its storage is spelled. 3 sites, no special case.

### D5 — the ctor's value is an implicit `this`. ✅ DECIDED (user, 2026-08-02)

```kama
public ctor make(int32 x, int32 y) {
    this.base = Base.createIt();      // reads beside the field assignments, which `r.base` does not
    this.x = x;
    this.y = y;
}                                     // implicit return of the constructed value
```

Not `This x;`. The objections recorded against implicit `this` did not survive checking:

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

**The return is IMPLICIT** — an infallible ctor spells no return type, so it need spell no return either.
`return give this;` stays legal, and this is NOT two ways to say one thing: it is the **early-return** form,
exactly as `return;` is in a `void` function, while falling off the end is the normal path.

```kama
public ctor make(int32 x) {
    this.x = x;
    if (x < 0) { return give this; }   // early exit — needs the explicit form
    this.y = 1;
}                                      // normal path — implicit
```

**A fallible ctor follows the same rule**, which is what keeps it from being a special case: it writes
`return Result::Err(…)` on a failure path, and falling off the end means `Ok(give this)`. Say so in SPEC, so
a missing `Ok` does not read as an omission.

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
| `slot P r; r = P.make(…); r.y = 5; return give r;` — delegate **then tweak** | ❌ *"'x' is never assigned"* — **and this is BY DESIGN** |

**`checkNamedCtorComplete` credits only FIELD-BY-FIELD assignment**, and the third row is the intended
behaviour, not an oversight (user, 2026-08-02): delegation exists to hand off to a *more specialized* ctor,
not to build-then-adjust.

So the base fix needs far less than "credit whole-value assignment" generally. It needs one narrow rule:
**`this.base` must be assigned exactly once, from a ctor call on the base type.** No general whole-value
support, no tweak path — base fields are private, so there is nothing to tweak through anyway. A derived
ctor's completeness rule becomes: *every own field assigned, plus `base` assigned from a `Base` ctor.*

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

Baseline entering this campaign: **native 887 / ASan 851 / wasm 825, all 0 failed** (re-measured
2026-08-02, after the inheritance campaign; the 873/838/806 figure this file was written with is stale).

⚠️ **The inheritance campaign lands first and changes what this one sweeps.** Every `extends` fixture and
the last parked repro (`tests/pending/base_ctor_not_run.kama`) still use the old
`slot D r; … return give r;` form, and the fixture set grew — `tests/inherit_abstract_base`,
`tests/inherit_private_name_reuse`, `tests/poly_in_collection`, `tests/vtable_depth2` are new, and four
more were restructured. Re-count before trusting any site number in this brief.

The audit `c2ae0c8` ran is worth repeating in reverse: confirm no `xfail` fixture starts being rejected by
a *different* rule than its own once the bare-local rule relaxes inside ctors.

## Sequencing

**Source-breaking, so it lands before the 1.0 tag or waits for 2.0** — ROADMAP §1's own rule. It belongs
with the docs/naming reconcile that is the last gate before the tag.

**This campaign is now the immediate next one, and it BLOCKS the last inheritance hole.** Hole 1 of
[inheritance.md](inheritance.md) — a derived type never runs its base's constructor — is fixed by
`this.base = Base.make(…)`, which needs D5's implicit `this`. Since this campaign is already rewriting
every ctor body, the two sweeps must not interleave: finish here, then close hole 1, then delete
`inheritance.md`. Everything else in that campaign has shipped (`0f8b2a9` → `f9c0e59`).
