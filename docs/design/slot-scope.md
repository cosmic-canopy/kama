# `slot` — restoring its intended scope (cold-start brief)

*In-flight campaign doc. **Delete this file when the campaign ships**, once SPEC carries the record — see
the maintenance table at the top of [ROADMAP.md](../ROADMAP.md).*

---

## ►► STATUS — steps 1-5 SHIPPED (2026-08-03), start at step 6

`e1edf1f` → `3098b21`, then `0c126fa` → `c7988d5` on `dev`. **native 898 / ASan 862 / wasm 836, 0 failed** —
that is the baseline to hold. **`slot` now means exactly one thing**: the storage an `out` parameter is
about to fill. SPEC's *Uninitialized storage* section carries the record (three rules, per-exit-point drop
elision); this file keeps only what is not written down there.

**What is left: step 6 (inheritance hole 1) and step 7 (closeout).** Everything below the horizontal rule
is the ORIGINAL brief and is now history — read it for the reasoning, not for instructions.

### What steps 4-5 actually took, versus what this brief predicted

- **The step-4 helper is TWO shapes, not one.** The brief proposed a single `relocate` for all nine
  `addr(of:)` sites. Wrong for atomic/channel: those five wrap five DIFFERENT extern C calls, so there is
  no shared operation — they got per-type `out` wrappers. Only the four collection sites are one operation,
  and they share `std::ptr::relocate`. `std::memory` could not host it (`import std::memory` is a satisfied
  no-op, so nothing under `lib/std/memory/` is ever read) and the prelude would have put a raw unsafe
  primitive in every program's global scope, so `std::ptr` is a new importable module.
- **Two compiler holes had to close first**, neither of them in this brief:
  - a generic free fn could not bind its type parameter from an ENCLOSING generic type's parameter, which
    every `relocate` call site needs (`0c126fa`). The fix mirrors `registerInstColls`: re-walk a generic
    type's members once per instantiation with `_typeSubst` bound. ⚠️ Walk the SPECIALIZED method set
    (`_classes[mangled].methods`), not the template AST — the raw AST still carries `when [V: Copyable]`
    members that do not hold for the instantiation, and walking them registers code the program never uses.
    ⚠️ `_callInst` had to become (call site → substitution → instantiation): one AST node inside a generic
    type serves every instantiation.
  - an `out` argument did not make a slot live, so an out-filled slot's value LEAKED (`d4b544b`). The
    mirror half: the callee dropped the incoming `out` value, which the analysis already treats as
    not-live. Both fixed; `tests/out_arg_drops` is differential over them.
- **`emitDtorDefinition` reset no move state** — a destructor inherited the previously-emitted function's
  `_moveState`/`_slotLocals`. Invisible until `~Sender`/`~Receiver` reused a name another body had left
  moved-from. Fixed in `3f40103`.
- **`isConcreteTypeArg` rejected every generic type argument**, so `Deque<DynamicArray<int32>>` could not
  instantiate a generic fn. A fully-resolved instance is concrete; an unregistered one still is not.
- **The rule-3 seam is NOT `topLevel`.** It looks like one, but every call site passes `true` and
  `walkSkippable` does so deliberately. Rule 3 needed a separate `condDepth`, compared against the
  declaration's depth — relative, not absolute.
- **A warn-first commit is impossible now**: `run_tests.sh:352` fails any fixture whose stderr contains
  `warning`. The warn phase was a throwaway build used only to ENUMERATE sites; the sweep and the flip are
  separate commits, both green.

### Traps for step 6, learned here

- **The xfail audit is worth doing properly.** All 20 slot-touching xfails still emitted their own
  diagnostic under the new rules, so none was re-blessed — but 20 of them then reported TWO errors, one
  irrelevant. "Still rejected" is not the bar; each xfail should pin one thing.
- **Nothing in `tests/query/` was constructible.** `Cell`, `Point`, `Box<T>`, `Leak` are transparent bags
  with no `ctor`, so `slot` was the only way to get one. Each needed a ctor appended to an existing field
  line to keep the line COUNT stable, which is what those fixtures pin.
- **`tools/check-lsp.sh` carries six inline `slot` buffers** no corpus sweep can see, plus pinned character
  offsets and a delta-encoded semanticTokens array. Re-deriving it took 14 probe positions, two rename edit
  ranges and a 16-token array decoded by hand against the new buffer. `check-query.sh` needed five
  coordinate updates and a deliberate coverage-table regeneration.
- **`kama check <file>` attributes a lib/prelude diagnostic to the file being checked**, at the LIB file's
  line number. That makes enumeration across the corpus produce nonsense unless you split by whether the
  reported line exceeds the checked file's length. Worth fixing on its own someday.
- **Left in place deliberately**: `emitScopeCleanup`'s MaybeMoved-slot arm and `checkNotMoved`'s slot case.
  This brief expects rule 3 to make them dead. It does not — rule 3 constrains the FILL, not a later
  conditional `give`, so they are not provably unreachable.

### Step 6 — inheritance hole 1 (the next session starts here)

Unchanged from [inheritance.md](inheritance.md): `this.base = Base.make(…)` plus a `Base`/`base` alias pair
mirroring `This`/`this`. `This` is contextual, resolved in `cType` (`kama.cemit.cpp` ~`:694`) and bound by
`ScopedStr _thisType` at 11 sites — that is the recipe. `Base` must fail cleanly when there is no base, and
sit inside `#if KAMA_INHERITANCE` or `check-no-inheritance` breaks. Extend `checkNamedCtorComplete` with one
narrow rule: `this.base` assigned exactly once, from a ctor call on the base type. Its repro is parked at
`tests/pending/base_ctor_not_run.kama`. **The shallow fix is a trap — read that section of inheritance.md
first.**

### Step 7 — closeout

Migrate anything still worth keeping to SPEC, delete this file and `inheritance.md`, strip the shipped items
from ROADMAP §1.

### Still deferred, agreed as its own campaign

The **unsafe-seam campaign**: rename `Ptr<T>` → `UnsafePtr<T>`, and deliver "no null in safe kama" (the
`= null` initializers this campaign's sweep just added to eight buffer-realloc sites are new customers for
it), via an `unsafe UnsafePtr<T> p = null;` field-declaration modifier, `Optional`-returning FFI wrappers,
`unsafe { }` around the teardown guards, then banning the `null` token outside `unsafe`. Plus a
compiler-emitted debug null trap at the two `_inUnsafe` deref gates. `lib/std/ptr/` is its natural home.

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
