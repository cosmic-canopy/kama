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
| 1 | a **hole** that will be filled before use | `slot T x;` | ~210 | ships — the original design |
| 2 | bag / zero construction of a transparent value | `T x = T.zero()` / `T.of(…)` | ≤284 | ships |
| 3 | **storage a ctor is obliged to complete** | `T x;` blessed inside a ctor | 577 | **this campaign** |
| — | call a type's elected default ctor | `T x = T.default()` | — | ✅ **SHIPPED** `5edb4d9` |

Concept 3 is irreducible: to build a `resource` field by field, the ctor needs storage that does not exist
yet, and it cannot come from another ctor without infinite regress. It needs *a* spelling; the argument for
a bare `T x;` is that a `ctor` is a special function whose whole job is to produce the type before it
returns, and **`checkNamedCtorComplete` already proves every field is assigned there**. `slot` adds no proof
in that position — it is pure ceremony in 73% of its uses and load-bearing in the rest.

## What ships

1. **Bless a bare `T x;` inside a `ctor`** for the constructed type. Same lowering as today's class slot
   (zero-init + field-default fill + vptr); only the keyword goes away.
2. **`slot` becomes an error in that position** — one way to say each thing (GOALS #4). Source-breaking,
   ~577 sites, mechanical, and the compiler names every one.
3. **A `slot` never filled on any path becomes an error.** A hole declared and never filled is a dead
   declaration; silently eliding its drop is the wrong answer.

**Drop behaviour does not change** — it is already correct, and this is why:

| slot state at scope exit | drop |
| --- | --- |
| never filled on any path | **error** (new, item 3) |
| filled on **some** paths | legal; drop **guarded by liveness** — nothing dropped if unassigned |
| filled on **all** paths | unconditional drop, statically known |

Row 2 is exactly why the `fd >= 0` runtime guards did not retire with the original `slot` campaign, and it
is why they should not be retired now. A conditionally-filled owning value *must* still drop when it was
filled, or it leaks.

## Decisions needed before any code

### D1 — how wide is the ctor blessing? *(lean: the constructed type only)*

Only `This` inside its own ctor, or any local in a ctor body? The wider form re-admits silently
uninitialized locals of arbitrary type, fenced to one function kind — the exact hole `slot` closed. The
narrow form reads as the ctor's named return slot and is the argument the user actually made. It leaves the
**46** "hole of another type inside a ctor" sites still saying `slot`, which is correct: they are holes.

### D2 — ⚠️ does `slot` narrow to `out` ONLY, or to *holes*? **This is the load-bearing decision.**

The stated goal is "`slot` is only for `out` params — that is ALL a slot can do." Taken literally that also
outlaws the ~164 sites that are holes filled by **assignment** rather than through an `out`:

```kama
slot float64 area;                       // no `out` anywhere
if (c) { area = w * h; } else { area = 0.0; }
```

Forbidding that pushes those sites back to a throwaway initializer (`float64 area = 0.0;` then overwrite),
which **loses the read-before-assign guarantee** and re-introduces exactly the habit `slot` was built to
remove. The distinction that carries weight is not *out-vs-assignment* — both fill a hole, and the analysis
treats them identically — it is **hole vs. storage-a-ctor-completes**, which is the split the compiler
already models.

*Lean: `slot` = a hole, filled by an `out` **or** an assignment.* `out` is the motivating case, not the
only legal one. Deciding otherwise is defensible but should be deliberate, since it is a second narrowing
of the same kind as the original over-broadening.

### D3 — warn-first, or straight to an error?

`c2ae0c8` landed its breaking half warn-first and swept the corpus in between, which kept every
intermediate commit green. Same shape is available here and is recommended for items 2 and 3.

### D4 — do `view` types (3 sites) follow the `value`/`resource` rule?

Almost certainly yes; called out only so it is not discovered mid-sweep.

## Mechanics you will want to know

- **The bare-local rejection is emitter-side, not grammar-side.** `T x;` parses today; `c2ae0c8` made it a
  hard error in the emitter. So the blessing is a rule change, not a grammar change — no parser, tree-sitter
  or LSP-query work, which is most of what would otherwise make this expensive.
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
